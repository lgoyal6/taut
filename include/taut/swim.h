#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "taut/codec.h"
#include "taut/transport.h"
#include "taut/types.h"

namespace taut {

// SWIM failure-detection tunables (§5.9 / SWIM paper, Das/Gupta/Motivala 2002).
struct SwimConfig {
    std::chrono::milliseconds period{1000};            // T: one probe target per period
    std::chrono::milliseconds ping_timeout{300};       // direct-ping wait before indirect probing
    std::chrono::milliseconds suspicion_timeout{3000}; // Suspect -> Dead
    std::uint32_t k = 3;                               // indirect-probe fan-out
    double gossip_factor = 3.0;                        // per-rumor send budget = ceil(factor*ln N)
};

// One node's SWIM membership + failure detector over a UdpTransport (§5.9). Single peer set,
// single-threaded, no exceptions across the API.
//
// Membership gossip rides in the packet PAYLOAD for now (no header/flags changes); the §5.2
// header-piggyback path is a later merge step. Uses PacketType::{Ping,PingReq,Pong,Join} with
// Class::Unreliable. Sender identity comes from the transport (`RecvResult::from`), mirroring
// real SWIM's use of the UDP source address - it is never carried in the payload.
//
// Driven exactly like Session: poll() drains inbound datagrams, tick() advances the
// time-based state machine off the transport clock. The event loop and the deterministic sim
// harness both just call those.
class Swim {
  public:
    // (peer, new state) whenever this node's belief about a peer changes.
    using StateHandler = std::function<void(const Endpoint&, MemberState)>;

    // `self` is this node's identity/address. `seed` seeds the protocol RNG (shuffled
    // round-robin target order + indirect-probe peer choice) so runs repro from a seed,
    // independently of the SimNet RNG.
    Swim(UdpTransport& transport, Endpoint self, SwimConfig cfg, std::uint64_t seed);

    void on_state_change(StateHandler h) {
        on_state_ = std::move(h);
    }

    // Seed a known peer as Alive@0 (cluster bootstrap without a JOIN round).
    void add_member(const Endpoint& peer);

    // Announce ourselves to a known introducer (sends JOIN). Bootstrap path for new nodes AND
    // for a restarted node rejoining under an endpoint the cluster believes Dead: the reply
    // snapshot carries the Dead@k belief, we refute at k+1, and Alive@k+1 outranks Dead@k
    // (v0.1.1 precedence; see docs/DESIGN-swim.md "Rejoin").
    void join(const Endpoint& introducer);

    // Drain and process every datagram currently readable from the transport.
    void poll();

    // Advance the failure detector: start/conclude periods, escalate to indirect probes,
    // expire suspicions to Dead. Uses tx_.now().
    void tick();

    // --- introspection -----------------------------------------------------------------

    MemberState state_of(const Endpoint& peer) const; // Dead if unknown; Alive for self
    std::uint32_t incarnation_of(const Endpoint& peer) const;
    std::uint32_t my_incarnation() const {
        return my_incarnation_;
    }
    std::size_t alive_count() const; // members believed Alive (excludes self)
    std::vector<std::pair<Endpoint, MemberState>> snapshot() const; // all known peers (not self)

    // The state-merge entry point: apply a membership rumor as if received via gossip.
    // Public so precedence can be tested deterministically without staging wire interleavings,
    // and so the future header-piggyback path has a single merge implementation to call.
    void apply_rumor(const Endpoint& subject, MemberState state, std::uint32_t incarnation);

  private:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Member {
        Endpoint addr{};
        MemberState state = MemberState::Alive;
        std::uint32_t incarnation = 0;
        TimePoint suspect_deadline{}; // when state==Suspect: when to escalate to Dead
    };
    // One pending dissemination per subject; overwritten (budget reset) when a newer update
    // for that subject is adopted. Gossiped least-transmitted-first, dropped past budget.
    struct Rumor {
        MemberState state;
        std::uint32_t incarnation;
        std::uint32_t tx_count = 0;
    };
    // An in-flight indirect probe we are relaying for another node.
    struct PendingRelay {
        Endpoint requester;      // who asked us to probe (A)
        std::uint32_t req_probe; // A's probe id, echoed back to A on success
        Endpoint target;         // the node we ping on A's behalf (T)
        TimePoint deadline;      // give up relaying after this
    };

    // wire (payload) path
    void send_swim(PacketType type, const Endpoint& to, std::uint32_t probe_id,
                   const Endpoint& subject, bool full_snapshot);
    void handle_ping(const Endpoint& from, std::uint32_t probe_id);
    void handle_ping_req(const Endpoint& from, std::uint32_t probe_id, const Endpoint& subject);
    void handle_pong(const Endpoint& from, std::uint32_t probe_id, const Endpoint& subject);
    // `request` = the JOIN names its sender as subject (a reply names the joiner). Only
    // requests are answered with a full-snapshot reply, so a join exchange terminates.
    void handle_join(const Endpoint& from, std::uint32_t joiner_inc, bool request);

    // failure-detector state machine
    void start_period(TimePoint now);
    void probe_one_dead(); // post-Dead refutation channel (v0.1.2, partition heal)
    void send_indirect_probes();
    void conclude_period(TimePoint now);
    void expire_suspects(TimePoint now);
    void reinject_suspicions();
    Endpoint pick_target();
    void rebuild_probe_order();
    std::vector<Endpoint> pick_k_others(const Endpoint& target);

    // membership merge / dissemination
    void notify(const Endpoint& peer, MemberState state);
    void refute(std::uint32_t suspected_inc);
    void queue_rumor(const Endpoint& subject, MemberState state, std::uint32_t inc);
    std::size_t gossip_budget() const;

    UdpTransport& tx_;
    Endpoint self_;
    SwimConfig cfg_;
    std::mt19937_64 rng_;
    StateHandler on_state_;

    std::uint32_t my_incarnation_ = 0;
    std::unordered_map<std::uint64_t, Member> members_;      // keyed by endpoint; excludes self
    std::unordered_map<std::uint64_t, Rumor> rumors_;        // pending dissemination, one/subject
    std::unordered_map<std::uint32_t, PendingRelay> relays_; // relay_probe_id -> relay

    std::vector<Endpoint> probe_order_; // shuffled round-robin order
    std::size_t probe_index_ = 0;

    // current period
    bool started_ = false;
    bool probing_ = false;
    bool indirect_sent_ = false;
    bool probe_acked_ = false;
    Endpoint probe_target_{};
    std::uint32_t probe_id_ = 0;
    TimePoint ping_deadline_{};   // direct-ping timeout -> indirect probes
    TimePoint period_deadline_{}; // end of period -> suspect if unacked
    TimePoint next_period_{};     // when the next period may begin

    std::uint32_t next_probe_id_ = 1;
};

} // namespace taut
