#include "taut/swim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "taut/codec.h"

namespace taut {
namespace {

// Endpoint identity key (same shape SimNet uses internally). addr_be occupies the high bits,
// port_be the low 16 — collision-free for IPv4 addr + port.
std::uint64_t key(const Endpoint& e) {
    return (static_cast<std::uint64_t>(e.addr_be) << 16) | e.port_be;
}

bool is_none(const Endpoint& e) {
    return e.addr_be == 0 && e.port_be == 0;
}

// --- SWIM payload (little-endian), carried inside the packet payload (§5.9 lane note) ------
//
//   u32 probe_id
//   u32 subject_addr_be
//   u16 subject_port_be
//   u16 gossip_count
//   gossip_count x { u32 addr_be, u16 port_be, u32 incarnation, u8 state }   (11 B each)
//
constexpr std::size_t kSwimHeader = 12;
constexpr std::size_t kGossipEntry = 11;
constexpr std::size_t kMaxGossipEntries = 16; // infection-path entries per packet
// Hard ceiling: entries that fit in one datagram (used to bound a JOIN full-roster reply).
constexpr std::size_t kMaxFitEntries =
    (kMaxDatagram - kBaseHeaderSize - kSwimHeader) / kGossipEntry;

void put_u16(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(std::byte{static_cast<std::uint8_t>(v & 0xFFu)});
    b.push_back(std::byte{static_cast<std::uint8_t>((v >> 8) & 0xFFu)});
}
void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(std::byte{static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu)});
    }
}
std::uint16_t get_u16(std::span<const std::byte> b, std::size_t off) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[off]) |
                                      (std::to_integer<unsigned>(b[off + 1]) << 8));
}
std::uint32_t get_u32(std::span<const std::byte> b, std::size_t off) {
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(b[off])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[off + 1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[off + 2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(b[off + 3])) << 24);
}

// Incarnation precedence (SWIM §4.2 + v0.1.1 rejoin). Records are ordered lexicographically
// by (incarnation, state), state ranked Alive < Suspect < Dead. Within one incarnation,
// evidence only accumulates toward death: Suspect beats Alive, Dead beats both, and Dead is
// unbeatable — a suspicion sticks until the accused bumps its own incarnation. A strictly
// newer incarnation beats anything older, INCLUDING Dead: only the subject itself can mint a
// higher incarnation, so Alive@k+1 is first-hand proof of life issued after the Dead@k
// verdict's evidence. This replaces v0.1.0's "Dead is terminal at any incarnation", which
// made restart-and-rejoin impossible (the memberlist/Lifeguard ordering).
bool overrides(MemberState cur, std::uint32_t cur_inc, MemberState in, std::uint32_t in_inc) {
    // The state ranking piggybacks on the enum's wire values (types.h): Alive=0 < Suspect=1
    // < Dead=2. Keep that ordering if the enum ever changes.
    static_assert(MemberState::Alive < MemberState::Suspect &&
                  MemberState::Suspect < MemberState::Dead);
    if (in_inc != cur_inc) {
        return in_inc > cur_inc;
    }
    return in > cur;
}

MemberState state_from_wire(std::uint8_t v) {
    switch (v) {
    case 0:
        return MemberState::Alive;
    case 1:
        return MemberState::Suspect;
    default:
        return MemberState::Dead;
    }
}

} // namespace

Swim::Swim(UdpTransport& transport, Endpoint self, SwimConfig cfg, std::uint64_t seed)
    : tx_(transport), self_(self), cfg_(cfg), rng_(seed) {
    // Announce ourselves so peers learn our (initial) incarnation via ordinary gossip.
    queue_rumor(self_, MemberState::Alive, my_incarnation_);
}

void Swim::add_member(const Endpoint& peer) {
    if (peer == self_ || is_none(peer)) {
        return;
    }
    const auto k = key(peer);
    if (members_.find(k) == members_.end()) {
        members_[k] = Member{peer, MemberState::Alive, 0, {}};
    }
}

void Swim::join(const Endpoint& introducer) {
    // JOIN carries our current incarnation in the probe_id field so the introducer learns it.
    send_swim(PacketType::Join, introducer, my_incarnation_, self_, /*full_snapshot=*/false);
}

std::size_t Swim::gossip_budget() const {
    const double n = static_cast<double>(members_.size() + 1); // include self
    if (n <= 1.0) {
        return 1;
    }
    const auto b = static_cast<std::size_t>(std::ceil(cfg_.gossip_factor * std::log(n)));
    return b < 1 ? 1 : b;
}

void Swim::queue_rumor(const Endpoint& subject, MemberState state, std::uint32_t inc) {
    rumors_[key(subject)] = Rumor{state, inc, 0}; // fresh budget: re-disseminate from scratch
}

void Swim::notify(const Endpoint& peer, MemberState state) {
    if (on_state_) {
        on_state_(peer, state);
    }
}

void Swim::refute(std::uint32_t suspected_inc) {
    // Only the accused refutes, by re-announcing with a strictly higher incarnation (§5.9).
    my_incarnation_ = std::max(my_incarnation_, suspected_inc + 1);
    queue_rumor(self_, MemberState::Alive, my_incarnation_);
}

void Swim::apply_rumor(const Endpoint& subject, MemberState state, std::uint32_t inc) {
    if (is_none(subject)) {
        return;
    }
    if (subject == self_) {
        // A rumor about us. If we are suspected/confirmed at an incarnation >= ours, refute.
        if ((state == MemberState::Suspect || state == MemberState::Dead) &&
            inc >= my_incarnation_) {
            refute(inc);
        }
        return;
    }

    const auto now = tx_.now();
    const auto k = key(subject);
    auto it = members_.find(k);
    if (it == members_.end()) {
        // Learn a previously-unknown member in whatever state the rumor reports.
        Member m{subject, state, inc, {}};
        if (state == MemberState::Suspect) {
            m.suspect_deadline = now + cfg_.suspicion_timeout;
        }
        members_[k] = m;
        queue_rumor(subject, state, inc);
        notify(subject, state);
        return;
    }

    Member& m = it->second;
    if (!overrides(m.state, m.incarnation, state, inc)) {
        return;
    }
    const MemberState old = m.state;
    m.state = state;
    m.incarnation = inc;
    if (state == MemberState::Suspect) {
        m.suspect_deadline = now + cfg_.suspicion_timeout;
    }
    queue_rumor(subject, state, inc);
    if (old != state) {
        notify(subject, state);
    }
}

// ---- wire path ---------------------------------------------------------------------------

void Swim::send_swim(PacketType type, const Endpoint& to, std::uint32_t probe_id,
                     const Endpoint& subject, bool full_snapshot) {
    std::vector<std::byte> body;
    put_u32(body, probe_id);
    put_u32(body, subject.addr_be);
    put_u16(body, subject.port_be);

    // Pick the gossip payload. Normal packets piggyback the least-transmitted pending rumors
    // (infection-style dissemination); a JOIN reply instead dumps a full snapshot so the new
    // node learns the whole roster in one shot.
    std::vector<std::pair<Endpoint, Rumor>> pick;
    if (full_snapshot) {
        pick.emplace_back(self_, Rumor{MemberState::Alive, my_incarnation_, 0});
        for (const auto& [mk, m] : members_) {
            (void)mk;
            pick.emplace_back(m.addr, Rumor{m.state, m.incarnation, 0});
        }
        if (pick.size() > kMaxFitEntries) {
            pick.resize(kMaxFitEntries);
        }
    } else {
        const auto budget = gossip_budget();
        for (auto& [rk, r] : rumors_) {
            (void)rk;
            if (r.tx_count < budget) {
                pick.emplace_back(Endpoint{static_cast<std::uint32_t>(rk >> 16),
                                           static_cast<std::uint16_t>(rk & 0xFFFFu)},
                                  r);
            }
        }
        std::sort(pick.begin(), pick.end(), [](const auto& a, const auto& b) {
            if (a.second.tx_count != b.second.tx_count) {
                return a.second.tx_count < b.second.tx_count; // least-transmitted first
            }
            return key(a.first) < key(b.first); // deterministic tiebreak
        });
        if (pick.size() > kMaxGossipEntries) {
            pick.resize(kMaxGossipEntries);
        }
        // Charge the send budget and drop exhausted rumors.
        for (const auto& [ep, r] : pick) {
            (void)r;
            auto rit = rumors_.find(key(ep));
            if (rit != rumors_.end() && ++rit->second.tx_count >= budget) {
                rumors_.erase(rit);
            }
        }
    }

    put_u16(body, static_cast<std::uint16_t>(pick.size()));
    for (const auto& [ep, r] : pick) {
        put_u32(body, ep.addr_be);
        put_u16(body, ep.port_be);
        put_u32(body, r.incarnation);
        body.push_back(std::byte{static_cast<std::uint8_t>(r.state)});
    }

    Packet p{};
    p.type = type;
    p.flags = 0;
    p.cls = Class::Unreliable;
    p.seq = 0;
    p.cum_ack = 0;
    p.adv_window = 0;
    p.payload = body;

    std::array<std::byte, kMaxDatagram> out{};
    const std::size_t n = encode(p, out);
    if (n > 0) {
        tx_.send(to, std::span<const std::byte>(out.data(), n));
    }
}

void Swim::poll() {
    std::array<std::byte, kMaxDatagram> buf{};
    while (auto r = tx_.recv(buf)) {
        const Endpoint from = r->from; // r is engaged here (loop guard); capture before gossip loop
        Packet p{};
        if (decode(std::span<const std::byte>(buf.data(), r->size), p) != DecodeError::Ok) {
            continue; // malformed / corrupt — drop
        }
        if (p.type != PacketType::Ping && p.type != PacketType::PingReq &&
            p.type != PacketType::Pong && p.type != PacketType::Join) {
            continue; // not a SWIM packet
        }
        const auto body = p.payload;
        if (body.size() < kSwimHeader) {
            continue; // truncated SWIM body
        }
        const std::uint32_t probe_id = get_u32(body, 0);
        Endpoint subject{};
        subject.addr_be = get_u32(body, 4);
        subject.port_be = get_u16(body, 8);
        const std::uint16_t count = get_u16(body, 10);

        // A packet from a member we believe Dead: make sure it hears the accusation — its
        // death rumor's budget is long spent, and only the accused can mint the higher
        // incarnation that resurrects it (v0.1.2, same partition-heal gap as above).
        if (auto mit = members_.find(key(from)); mit != members_.end() &&
                                                 mit->second.state == MemberState::Dead &&
                                                 rumors_.find(key(from)) == rumors_.end()) {
            queue_rumor(from, MemberState::Dead, mit->second.incarnation);
        }

        // Apply piggybacked gossip first so state is current before we act on the message.
        std::size_t off = kSwimHeader;
        for (std::uint16_t i = 0; i < count; ++i) {
            if (off + kGossipEntry > body.size()) {
                break;
            }
            Endpoint ep{};
            ep.addr_be = get_u32(body, off);
            ep.port_be = get_u16(body, off + 4);
            const std::uint32_t inc = get_u32(body, off + 6);
            const auto st = state_from_wire(std::to_integer<std::uint8_t>(body[off + 10]));
            off += kGossipEntry;
            apply_rumor(ep, st, inc);
        }

        switch (p.type) {
        case PacketType::Ping:
            handle_ping(from, probe_id);
            break;
        case PacketType::PingReq:
            handle_ping_req(from, probe_id, subject);
            break;
        case PacketType::Pong:
            handle_pong(from, probe_id, subject);
            break;
        case PacketType::Join:
            // A JOIN whose subject is its own sender is a request (join() names itself);
            // a reply names the joiner — i.e. the recipient — as subject.
            handle_join(from, probe_id, /*request=*/subject == from);
            break;
        default:
            break;
        }
    }
}

void Swim::handle_ping(const Endpoint& from, std::uint32_t probe_id) {
    // Answer any ping (direct or relayed) with a PONG naming ourselves as alive.
    send_swim(PacketType::Pong, from, probe_id, self_, /*full_snapshot=*/false);
}

void Swim::handle_ping_req(const Endpoint& from, std::uint32_t probe_id, const Endpoint& subject) {
    if (is_none(subject) || subject == self_) {
        return;
    }
    // Relay: ping the target on the requester's behalf under a fresh probe id, remembering how
    // to route the eventual PONG back to `from` with its original probe id.
    const std::uint32_t relay_probe = next_probe_id_++;
    relays_[relay_probe] = PendingRelay{from, probe_id, subject, tx_.now() + cfg_.period};
    send_swim(PacketType::Ping, subject, relay_probe, subject, /*full_snapshot=*/false);
}

void Swim::handle_pong(const Endpoint& from, std::uint32_t probe_id, const Endpoint& subject) {
    (void)from;
    // Is this a PONG for an indirect probe we are relaying? If so, forward to the requester.
    auto it = relays_.find(probe_id);
    if (it != relays_.end()) {
        const PendingRelay relay = it->second;
        relays_.erase(it);
        send_swim(PacketType::Pong, relay.requester, relay.req_probe, relay.target,
                  /*full_snapshot=*/false);
        return;
    }
    // Otherwise it acks our own probe. Match on the subject being alive == our probe target.
    if (probing_ && !is_none(probe_target_) && subject == probe_target_) {
        probe_acked_ = true;
    }
}

void Swim::handle_join(const Endpoint& from, std::uint32_t joiner_inc, bool request) {
    if (is_none(from) || from == self_) {
        return;
    }
    // The JOIN itself is first-hand proof the sender is alive at its stated incarnation, so
    // route it through the ordinary merge (v0.1.0 unconditionally adopted/gossiped Alive here,
    // disagreeing with this node's own table when the joiner was recorded Dead). A REJOINING
    // node restarts at incarnation 0, which does not yet beat its Dead@k record — the
    // full-snapshot reply shows it it is believed Dead@k, and the standard refutation path
    // re-announces it at k+1, which now wins (see overrides()).
    apply_rumor(from, MemberState::Alive, joiner_inc);
    if (request) {
        // Reply with a full roster so the newcomer converges immediately. subject = the
        // JOINER (not self_) marks this packet as a reply: v0.1.0 replied to replies too,
        // making every join exchange an infinite JOIN ping-pong.
        send_swim(PacketType::Join, from, my_incarnation_, from, /*full_snapshot=*/true);
    }
}

// ---- failure-detector state machine ------------------------------------------------------

void Swim::tick() {
    const auto now = tx_.now();
    if (!started_) {
        started_ = true;
        next_period_ = now;
    }

    expire_suspects(now);

    // Prune stale relays.
    for (auto it = relays_.begin(); it != relays_.end();) {
        it = (now >= it->second.deadline) ? relays_.erase(it) : std::next(it);
    }

    if (probing_) {
        if (!indirect_sent_ && !probe_acked_ && now >= ping_deadline_) {
            send_indirect_probes();
            indirect_sent_ = true;
        }
        if (now >= period_deadline_) {
            conclude_period(now);
        }
    }
    if (!probing_ && now >= next_period_) {
        start_period(now);
    }
}

void Swim::expire_suspects(TimePoint now) {
    std::vector<Endpoint> to_kill;
    for (const auto& [k, m] : members_) {
        (void)k;
        if (m.state == MemberState::Suspect && now >= m.suspect_deadline) {
            to_kill.push_back(m.addr);
        }
    }
    for (const auto& ep : to_kill) {
        apply_rumor(ep, MemberState::Dead, incarnation_of(ep));
    }
}

// Post-Dead refutation channel (v0.1.2). Once both sides of a healed partition hold Dead
// verdicts for each other, the base protocol goes silent forever: Dead members are never
// probed, so no packet crosses the healed link, and the accused's death rumor spent its
// gossip budget INTO the partition, so even incidental contact would not tell it to
// refute. tautq's chaos partition scenario caught exactly this as permanently-stalled
// jobs. Each period we ping one random Dead member with its own Dead rumor re-queued —
// if it is actually alive it refutes at inc+1, which resurrects it under the v0.1.1
// ordering, and normal gossip re-merges the halves.
void Swim::probe_one_dead() {
    std::vector<const Member*> dead;
    for (const auto& [k, m] : members_) {
        (void)k;
        if (m.state == MemberState::Dead) {
            dead.push_back(&m);
        }
    }
    if (dead.empty()) {
        return;
    }
    const Member& t = *dead[rng_() % dead.size()];
    if (rumors_.find(key(t.addr)) == rumors_.end()) {
        queue_rumor(t.addr, MemberState::Dead, t.incarnation); // "you are dead" — refute it
    }
    send_swim(PacketType::Ping, t.addr, next_probe_id_++, t.addr, /*full_snapshot=*/false);
}

void Swim::start_period(TimePoint now) {
    period_deadline_ = now + cfg_.period;
    next_period_ = period_deadline_;
    reinject_suspicions(); // keep unresolved suspicions spreading until refuted/dead
    probe_one_dead();      // keep a healed-but-condemned member reachable (v0.1.2)

    const Endpoint target = pick_target();
    if (is_none(target)) {
        return; // nothing to probe this period
    }
    probing_ = true;
    indirect_sent_ = false;
    probe_acked_ = false;
    probe_target_ = target;
    probe_id_ = next_probe_id_++;
    ping_deadline_ = now + cfg_.ping_timeout;
    send_swim(PacketType::Ping, target, probe_id_, target, /*full_snapshot=*/false);
}

void Swim::send_indirect_probes() {
    for (const Endpoint& r : pick_k_others(probe_target_)) {
        send_swim(PacketType::PingReq, r, probe_id_, probe_target_, /*full_snapshot=*/false);
    }
}

void Swim::conclude_period(TimePoint now) {
    if (!probe_acked_) {
        auto it = members_.find(key(probe_target_));
        if (it != members_.end() && it->second.state == MemberState::Alive) {
            apply_rumor(probe_target_, MemberState::Suspect, it->second.incarnation);
        }
    }
    probing_ = false;
    (void)now;
}

void Swim::reinject_suspicions() {
    for (const auto& [k, m] : members_) {
        (void)k;
        if (m.state == MemberState::Suspect) {
            queue_rumor(m.addr, MemberState::Suspect, m.incarnation);
        }
    }
}

void Swim::rebuild_probe_order() {
    probe_order_.clear();
    for (const auto& [k, m] : members_) {
        (void)k;
        if (m.state != MemberState::Dead) {
            probe_order_.push_back(m.addr);
        }
    }
    std::shuffle(probe_order_.begin(), probe_order_.end(), rng_);
}

Endpoint Swim::pick_target() {
    // Randomized round-robin: walk a shuffled order, reshuffle on wrap. Guarantees every live
    // member is probed once per (N-1) periods; the reshuffle avoids a fixed skip pattern.
    for (std::size_t tries = 0; tries <= probe_order_.size() + 1; ++tries) {
        if (probe_index_ >= probe_order_.size()) {
            rebuild_probe_order();
            probe_index_ = 0;
            if (probe_order_.empty()) {
                return Endpoint{};
            }
        }
        const Endpoint cand = probe_order_[probe_index_++];
        auto it = members_.find(key(cand));
        if (it != members_.end() && it->second.state != MemberState::Dead) {
            return cand;
        }
    }
    return Endpoint{};
}

std::vector<Endpoint> Swim::pick_k_others(const Endpoint& target) {
    std::vector<Endpoint> pool;
    for (const auto& [k, m] : members_) {
        (void)k;
        if (m.state == MemberState::Alive && m.addr != target) {
            pool.push_back(m.addr);
        }
    }
    std::shuffle(pool.begin(), pool.end(), rng_);
    if (pool.size() > cfg_.k) {
        pool.resize(cfg_.k);
    }
    return pool;
}

// ---- introspection -----------------------------------------------------------------------

MemberState Swim::state_of(const Endpoint& peer) const {
    if (peer == self_) {
        return MemberState::Alive;
    }
    auto it = members_.find(key(peer));
    return it == members_.end() ? MemberState::Dead : it->second.state;
}

std::uint32_t Swim::incarnation_of(const Endpoint& peer) const {
    if (peer == self_) {
        return my_incarnation_;
    }
    auto it = members_.find(key(peer));
    return it == members_.end() ? 0 : it->second.incarnation;
}

std::size_t Swim::alive_count() const {
    std::size_t n = 0;
    for (const auto& [k, m] : members_) {
        (void)k;
        if (m.state == MemberState::Alive) {
            ++n;
        }
    }
    return n;
}

std::vector<std::pair<Endpoint, MemberState>> Swim::snapshot() const {
    std::vector<std::pair<Endpoint, MemberState>> out;
    out.reserve(members_.size());
    for (const auto& [k, m] : members_) {
        (void)k;
        out.emplace_back(m.addr, m.state);
    }
    return out;
}

} // namespace taut
