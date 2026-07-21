#include "taut/swim.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "taut/sim_net.h"
#include "taut/transport.h"
#include "taut/types.h"

using namespace std::chrono_literals;

namespace {

taut::Endpoint ep(std::uint16_t port) {
    taut::Endpoint e{};
    e.addr_be = 1;
    e.port_be = port;
    return e;
}

std::uint64_t ekey(const taut::Endpoint& e) {
    return (static_cast<std::uint64_t>(e.addr_be) << 16) | e.port_be;
}

// A UdpTransport decorator that can sever the link to a set of peers in BOTH directions —
// how we model a network partition without touching the shared SimNet (which is owned by
// feat/core). Blocked outbound datagrams are silently dropped; blocked inbound datagrams are
// discarded on recv. Symmetric blocking on both endpoints == a bidirectional partition.
class LinkFilter : public taut::UdpTransport {
  public:
    explicit LinkFilter(taut::UdpTransport& inner) : inner_(inner) {}

    void block(const taut::Endpoint& peer) {
        blocked_.insert(ekey(peer));
    }
    void heal() {
        blocked_.clear();
    }

    std::size_t send(const taut::Endpoint& to, std::span<const std::byte> data) override {
        if (blocked_.count(ekey(to))) {
            return data.size(); // pretend it left; the wire ate it
        }
        return inner_.send(to, data);
    }
    std::optional<taut::RecvResult> recv(std::span<std::byte> buf) override {
        while (auto r = inner_.recv(buf)) {
            if (blocked_.count(ekey(r->from))) {
                continue; // drop datagrams from partitioned peers
            }
            return r;
        }
        return std::nullopt;
    }
    std::chrono::steady_clock::time_point now() const override {
        return inner_.now();
    }
    int fd() const override {
        return inner_.fd();
    }

  private:
    taut::UdpTransport& inner_;
    std::unordered_set<std::uint64_t> blocked_;
};

// N-node SWIM mesh over one SimNet, all-to-all seeded Alive at construction (the SWIM paper's
// "existing group" assumption). Each node's transport is wrapped in a LinkFilter so tests can
// partition links; a node can also be "crashed" (we simply stop stepping it, so it answers
// nothing — an exact process-death model).
struct Mesh {
    taut::SimNet net;
    std::vector<taut::Endpoint> eps;
    std::vector<std::unique_ptr<LinkFilter>> links;
    std::vector<std::unique_ptr<taut::Swim>> nodes;
    std::set<int> down;

    Mesh(std::uint64_t seed, taut::Impairments imp, int n, taut::SwimConfig cfg) : net(seed, imp) {
        for (int i = 0; i < n; ++i) {
            eps.push_back(ep(static_cast<std::uint16_t>(7000 + i)));
        }
        for (int i = 0; i < n; ++i) {
            links.push_back(std::make_unique<LinkFilter>(net.endpoint(eps[i])));
            nodes.push_back(std::make_unique<taut::Swim>(*links[i], eps[i], cfg, seed * 1000 + i));
        }
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if (i != j) {
                    nodes[i]->add_member(eps[j]);
                }
            }
        }
    }

    int size() const {
        return static_cast<int>(nodes.size());
    }

    void step(std::chrono::milliseconds dt) {
        net.advance(dt);
        for (int i = 0; i < size(); ++i) {
            if (!down.count(i)) {
                nodes[i]->poll();
            }
        }
        for (int i = 0; i < size(); ++i) {
            if (!down.count(i)) {
                nodes[i]->tick();
            }
        }
    }

    // Partition node `idx` from every other node, both directions.
    void isolate(int idx) {
        for (int j = 0; j < size(); ++j) {
            if (j == idx) {
                continue;
            }
            links[idx]->block(eps[j]);
            links[j]->block(eps[idx]);
        }
    }
    void heal_all() {
        for (auto& l : links) {
            l->heal();
        }
    }
    void crash(int idx) {
        down.insert(idx);
    }

    // Every live node sees every other seeded node as Alive (full convergence).
    bool all_alive() const {
        for (int i = 0; i < size(); ++i) {
            if (down.count(i)) {
                continue;
            }
            for (int j = 0; j < size(); ++j) {
                if (i == j) {
                    continue;
                }
                if (nodes[i]->state_of(eps[j]) != taut::MemberState::Alive) {
                    return false;
                }
            }
        }
        return true;
    }

    // Some node other than `target` believes `target` is in `st`.
    bool anyone_sees(int target, taut::MemberState st) const {
        for (int i = 0; i < size(); ++i) {
            if (i == target || down.count(i)) {
                continue;
            }
            if (nodes[i]->state_of(eps[target]) == st) {
                return true;
            }
        }
        return false;
    }
    bool everyone_sees(int target, taut::MemberState st) const {
        for (int i = 0; i < size(); ++i) {
            if (i == target || down.count(i)) {
                continue;
            }
            if (nodes[i]->state_of(eps[target]) != st) {
                return false;
            }
        }
        return true;
    }
};

taut::SwimConfig default_cfg() {
    taut::SwimConfig c;
    c.period = 1000ms;
    c.ping_timeout = 300ms;
    c.suspicion_timeout = 3000ms;
    c.k = 3;
    return c;
}

} // namespace

// ---- incarnation precedence (the core correctness property) ------------------------------

// The exact stale-rumor interleaving that incarnation numbers prevent: a suspicion that was
// already refuted by a higher incarnation must not re-suspect the node when it arrives late.
TEST(Swim, IncarnationNumbersRejectStaleSuspicion) {
    taut::SimNet net(1);
    const auto self = ep(7000);
    const auto p = ep(7001);
    taut::Swim node(net.endpoint(self), self, default_cfg(), 1);
    node.add_member(p);

    ASSERT_EQ(node.state_of(p), taut::MemberState::Alive);
    ASSERT_EQ(node.incarnation_of(p), 0u);

    // A suspicion about p (incarnation 0) arrives and is adopted (suspect beats alive @ =inc).
    node.apply_rumor(p, taut::MemberState::Suspect, 0);
    EXPECT_EQ(node.state_of(p), taut::MemberState::Suspect);

    // p refutes by re-announcing itself with a higher incarnation.
    node.apply_rumor(p, taut::MemberState::Alive, 1);
    EXPECT_EQ(node.state_of(p), taut::MemberState::Alive);
    EXPECT_EQ(node.incarnation_of(p), 1u);

    // THE POINT: the original (now stale, lower-incarnation) suspicion arrives late. Without
    // incarnation numbers it would re-kill p; with them it is silently dropped.
    node.apply_rumor(p, taut::MemberState::Suspect, 0);
    EXPECT_EQ(node.state_of(p), taut::MemberState::Alive)
        << "a stale, lower-incarnation suspicion must never override a fresher refutation";

    // Contrast — a suspicion at the *current* incarnation is legitimate and does apply, which
    // is why the accused must keep bumping its incarnation to stay alive.
    node.apply_rumor(p, taut::MemberState::Suspect, 1);
    EXPECT_EQ(node.state_of(p), taut::MemberState::Suspect);
}

// Only the accused refutes, and it does so by bumping its own incarnation past the suspicion.
TEST(Swim, SelfRefutesByBumpingIncarnation) {
    taut::SimNet net(1);
    const auto self = ep(7000);
    taut::Swim node(net.endpoint(self), self, default_cfg(), 1);
    ASSERT_EQ(node.my_incarnation(), 0u);

    node.apply_rumor(self, taut::MemberState::Suspect, 0); // "you are suspected @0"
    EXPECT_EQ(node.my_incarnation(), 1u);                  // refute: 0 -> 1

    node.apply_rumor(self, taut::MemberState::Suspect, 1); // suspected again @ our new inc
    EXPECT_EQ(node.my_incarnation(), 2u);

    node.apply_rumor(self, taut::MemberState::Suspect, 0); // stale — no further bump
    EXPECT_EQ(node.my_incarnation(), 2u);
}

// Dead is terminal: once confirmed dead a member is not resurrected by a stale Alive.
TEST(Swim, DeadIsTerminal) {
    taut::SimNet net(1);
    const auto self = ep(7000);
    const auto p = ep(7001);
    taut::Swim node(net.endpoint(self), self, default_cfg(), 1);
    node.add_member(p);

    node.apply_rumor(p, taut::MemberState::Dead, 5);
    EXPECT_EQ(node.state_of(p), taut::MemberState::Dead);
    node.apply_rumor(p, taut::MemberState::Alive, 6);
    EXPECT_EQ(node.state_of(p), taut::MemberState::Dead);
}

// ---- failure detection on SimNet ---------------------------------------------------------

// A crashed node is detected: Suspect first (indirect probes also fail), then Dead after the
// suspicion timeout, and the Dead verdict gossips to every survivor.
TEST(Swim, DetectsCrashedNode) {
    Mesh m(42, taut::Impairments{.delay = 10ms}, 5, default_cfg());
    const int victim = 4;

    // Warm up, then crash the victim (it stops answering entirely).
    for (int i = 0; i < 50; ++i) {
        m.step(20ms);
    }
    ASSERT_TRUE(m.all_alive());
    m.crash(victim);

    std::optional<std::chrono::milliseconds> t_suspect, t_dead;
    const auto t0 = m.net.now();
    for (int i = 0; i < 1200 && !m.everyone_sees(victim, taut::MemberState::Dead); ++i) {
        m.step(20ms);
        if (!t_suspect && m.anyone_sees(victim, taut::MemberState::Suspect)) {
            t_suspect = std::chrono::duration_cast<std::chrono::milliseconds>(m.net.now() - t0);
        }
        if (!t_dead && m.anyone_sees(victim, taut::MemberState::Dead)) {
            t_dead = std::chrono::duration_cast<std::chrono::milliseconds>(m.net.now() - t0);
        }
    }

    EXPECT_TRUE(m.everyone_sees(victim, taut::MemberState::Dead));
    ASSERT_TRUE(t_suspect.has_value());
    ASSERT_TRUE(t_dead.has_value());
    // Suspect precedes Dead by roughly the suspicion timeout.
    EXPECT_GE(t_dead->count(), t_suspect->count());
    EXPECT_GE((*t_dead - *t_suspect).count(), 3000 - 1000); // ~suspicion_timeout, minus a period
}

// Partition one node, detect it (Suspect), then heal: once refutations can flow again,
// membership reconverges to all-Alive with the node NEVER wrongly confirmed Dead.
//
// Timings are scaled from §5.9's defaults (shorter period, wider suspicion window) purely for
// determinism: at N=5 the worst-case round-robin detection is ~(N-1)*period, which only
// reliably precedes a Dead verdict (at ~period + suspicion_timeout) when suspicion_timeout is
// comfortably above (N-2)*period. The mechanism under test — suspect, refute, reconverge,
// never confirm-dead — is identical to the T=1 s / 3 s demo; only the clock is compressed.
TEST(Swim, PartitionHealReconverges) {
    taut::SwimConfig cfg;
    cfg.period = 500ms;
    cfg.ping_timeout = 150ms;
    cfg.suspicion_timeout = 4000ms;
    cfg.k = 3;

    for (std::uint64_t seed : {1u, 7u, 42u, 100u}) {
        Mesh m(seed, taut::Impairments{.delay = 10ms}, 5, cfg);
        const int victim = 2;

        for (int i = 0; i < 60; ++i) {
            m.step(20ms);
        }
        ASSERT_TRUE(m.all_alive()) << "seed " << seed;

        // Sever the victim from everyone, then heal the instant it is first suspected — so the
        // earliest Dead deadline (relative to the first suspicion) is never reached.
        m.isolate(victim);
        bool ever_dead = false;
        std::optional<std::chrono::milliseconds> detect;
        const auto t_part = m.net.now();
        for (int i = 0; i < 250 && !detect; ++i) {
            m.step(20ms);
            ever_dead = ever_dead || m.anyone_sees(victim, taut::MemberState::Dead);
            if (m.anyone_sees(victim, taut::MemberState::Suspect)) {
                detect =
                    std::chrono::duration_cast<std::chrono::milliseconds>(m.net.now() - t_part);
            }
        }
        ASSERT_TRUE(detect.has_value()) << "seed " << seed << ": partition should be detected";
        ASSERT_FALSE(ever_dead) << "seed " << seed;

        m.heal_all();
        const auto t_heal = m.net.now();
        std::optional<std::chrono::milliseconds> reconverge;
        for (int i = 0; i < 800 && !reconverge; ++i) {
            m.step(20ms);
            ever_dead = ever_dead || m.anyone_sees(victim, taut::MemberState::Dead);
            if (m.all_alive()) {
                reconverge =
                    std::chrono::duration_cast<std::chrono::milliseconds>(m.net.now() - t_heal);
            }
        }
        EXPECT_TRUE(reconverge.has_value()) << "seed " << seed << ": should reconverge after heal";
        EXPECT_FALSE(ever_dead) << "seed " << seed
                                << ": a reachable node was confirmed dead — invariant 6 violated";
        EXPECT_TRUE(m.all_alive()) << "seed " << seed;
    }
}

// Invariant 6: with steady loss (no partition), refutations always flow, so a live reachable
// node is NEVER confirmed Dead — even though transient suspicions do occur and get cleared.
// Run several seeds so it isn't a single-seed accident.
TEST(Swim, LiveReachableNodeNeverConfirmedDead) {
    for (std::uint64_t seed : {1u, 2u, 3u, 7u, 99u}) {
        Mesh m(seed, taut::Impairments{.loss = 0.10, .delay = 10ms, .jitter = 5ms}, 5,
               default_cfg());
        bool any_dead = false;
        for (auto& n : m.nodes) {
            n->on_state_change([&](const taut::Endpoint&, taut::MemberState s) {
                if (s == taut::MemberState::Dead) {
                    any_dead = true;
                }
            });
        }
        for (int i = 0; i < 1500 && !any_dead; ++i) { // 30 s of virtual time
            m.step(20ms);
        }
        EXPECT_FALSE(any_dead) << "seed " << seed
                               << ": a reachable node was confirmed dead while refutations flow";
    }
}

// A node that knows only an introducer learns the whole roster via JOIN, and the cluster
// learns it.
TEST(Swim, JoinLearnsRoster) {
    taut::Impairments imp{.delay = 10ms};
    taut::SimNet net(5, imp);
    const auto a = ep(7000);
    const auto b = ep(7001);
    const auto c = ep(7002); // the newcomer

    taut::Swim sa(net.endpoint(a), a, default_cfg(), 1);
    taut::Swim sb(net.endpoint(b), b, default_cfg(), 2);
    taut::Swim sc(net.endpoint(c), c, default_cfg(), 3);
    sa.add_member(b);
    sb.add_member(a);

    sc.join(a); // C only knows A

    for (int i = 0; i < 200; ++i) {
        net.advance(20ms);
        sa.poll();
        sb.poll();
        sc.poll();
        sa.tick();
        sb.tick();
        sc.tick();
    }

    EXPECT_EQ(sc.state_of(a), taut::MemberState::Alive);
    EXPECT_EQ(sc.state_of(b), taut::MemberState::Alive); // learned transitively from A's snapshot
    EXPECT_EQ(sa.state_of(c), taut::MemberState::Alive);
    EXPECT_EQ(sb.state_of(c), taut::MemberState::Alive); // learned via gossip
}
