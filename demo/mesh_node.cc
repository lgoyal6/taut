// mesh_node - a 5-node SWIM membership demo on the deterministic in-process SimNet
// (§5.9 deliverable). No sockets/epoll: one virtual clock, five Swim instances, seeded so the
// run is byte-reproducible. It converges the mesh, partitions one node, and heals it, printing
// the live membership table and the measured time-to-detect / time-to-reconverge.
//
// Run: ./mesh_node [seed]

#include "taut/swim.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "taut/sim_net.h"
#include "taut/transport.h"
#include "taut/types.h"

using namespace std::chrono_literals;
using taut::MemberState;

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

// See tests/unit/swim_test.cc - a transport decorator that severs links in both directions to
// model a partition, without touching the shared SimNet.
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
        return blocked_.count(ekey(to)) ? data.size() : inner_.send(to, data);
    }
    std::optional<taut::RecvResult> recv(std::span<std::byte> buf) override {
        while (auto r = inner_.recv(buf)) {
            if (!blocked_.count(ekey(r->from))) {
                return r;
            }
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

const char* label(MemberState s) {
    switch (s) {
    case MemberState::Alive:
        return "ALIVE  ";
    case MemberState::Suspect:
        return "SUSPECT";
    case MemberState::Dead:
        return "DEAD   ";
    }
    return "?";
}

char name(int i) {
    return static_cast<char>('A' + i);
}

} // namespace

int main(int argc, char** argv) {
    const std::uint64_t seed = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 20260720ull;
    constexpr int N = 5;
    const int victim = N - 1; // node E

    taut::SwimConfig cfg; // T=1s, ping 300ms, suspicion 3s, k=3 (defaults, §5.9)

    taut::SimNet net(seed, taut::Impairments{.loss = 0.02, .delay = 15ms, .jitter = 5ms});
    std::vector<taut::Endpoint> eps;
    std::vector<std::unique_ptr<LinkFilter>> links;
    std::vector<std::unique_ptr<taut::Swim>> nodes;
    for (int i = 0; i < N; ++i) {
        eps.push_back(ep(static_cast<std::uint16_t>(7000 + i)));
    }
    for (int i = 0; i < N; ++i) {
        links.push_back(std::make_unique<LinkFilter>(net.endpoint(eps[i])));
        nodes.push_back(std::make_unique<taut::Swim>(*links[i], eps[i], cfg,
                                                     seed * 1000 + static_cast<std::uint64_t>(i)));
    }
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i != j) {
                nodes[i]->add_member(eps[j]);
            }
        }
    }

    const auto t0 = net.now();
    const auto ms_since = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(net.now() - t0).count();
    };
    const auto print_table = [&](const std::string& when) {
        std::cout << "\n== membership @ t=" << ms_since() << "ms  (" << when << ") ==\n";
        for (int i = 0; i < N; ++i) {
            std::cout << "  node " << name(i) << " sees: ";
            for (int j = 0; j < N; ++j) {
                if (i == j) {
                    continue;
                }
                std::cout << name(j) << "=" << label(nodes[i]->state_of(eps[j])) << "(i"
                          << nodes[i]->incarnation_of(eps[j]) << ") ";
            }
            std::cout << "\n";
        }
    };
    const auto all_alive = [&] {
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                if (i != j && nodes[i]->state_of(eps[j]) != MemberState::Alive) {
                    return false;
                }
            }
        }
        return true;
    };
    const auto anyone_sees_victim = [&](MemberState st) {
        for (int i = 0; i < N; ++i) {
            if (i != victim && nodes[i]->state_of(eps[victim]) == st) {
                return true;
            }
        }
        return false;
    };
    const auto step = [&](std::chrono::milliseconds dt) {
        net.advance(dt);
        for (auto& n : nodes) {
            n->poll();
        }
        for (auto& n : nodes) {
            n->tick();
        }
    };

    std::cout << "SWIM mesh: " << N << " nodes  (T=" << cfg.period.count()
              << "ms, ping_timeout=" << cfg.ping_timeout.count()
              << "ms, suspicion=" << cfg.suspicion_timeout.count() << "ms, k=" << cfg.k
              << ")  seed=" << seed << "\n";

    // Phase 1: warm up until fully converged (seeded, so this is quick).
    for (int i = 0; i < 100 && !all_alive(); ++i) {
        step(20ms);
    }
    print_table("initial, converged");

    // Phase 2: partition node E from the rest.
    for (int i = 0; i < N; ++i) {
        if (i != victim) {
            links[victim]->block(eps[i]);
            links[i]->block(eps[victim]);
        }
    }
    std::cout << "\n-- partitioned node " << name(victim) << " at t=" << ms_since() << "ms --\n";
    const auto t_part = net.now();

    std::optional<long> t_detect;
    for (int i = 0; i < 200 && !t_detect; ++i) { // watch inside the 3s suspicion window
        step(20ms);
        if (anyone_sees_victim(MemberState::Suspect)) {
            t_detect =
                std::chrono::duration_cast<std::chrono::milliseconds>(net.now() - t_part).count();
        }
    }
    print_table("during partition");
    if (t_detect) {
        std::cout << "\n>>> time-to-detect (partition -> first SUSPECT): " << *t_detect << "ms\n";
    } else {
        std::cout << "\n>>> victim not yet suspected (unexpected)\n";
    }

    // Phase 3: heal before the suspicion timeout elapses, then let refutations flow.
    for (auto& l : links) {
        l->heal();
    }
    std::cout << "\n-- healed partition at t=" << ms_since() << "ms --\n";
    const auto t_heal = net.now();

    std::optional<long> t_reconverge;
    bool ever_dead = false;
    for (int i = 0; i < 400 && !t_reconverge; ++i) {
        step(20ms);
        ever_dead = ever_dead || anyone_sees_victim(MemberState::Dead);
        if (all_alive()) {
            t_reconverge =
                std::chrono::duration_cast<std::chrono::milliseconds>(net.now() - t_heal).count();
        }
    }
    print_table("after heal");

    if (t_reconverge) {
        std::cout << "\n>>> time-to-reconverge (heal -> all ALIVE again): " << *t_reconverge
                  << "ms\n";
    } else {
        std::cout << "\n>>> FAILED to reconverge\n";
    }
    std::cout << ">>> node " << name(victim)
              << " confirmed DEAD during the episode: " << (ever_dead ? "yes" : "no")
              << "  (invariant 6: a reachable node must not be falsely confirmed dead)\n";

    const bool ok = t_reconverge && !ever_dead;
    std::cout << "\n" << (ok ? "OK" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
