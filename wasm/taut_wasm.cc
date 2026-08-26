// C ABI for the browser demo. Runs real Sessions over the deterministic SimNet
// with a virtual clock, so a multi-second lossy transfer completes in real
// milliseconds. Two surfaces:
//   tt_run          - one call = one full batch experiment, JSON result
//   tt_feel_*       - a persistent pair of experiments (rto floor 25ms vs 200ms)
//                     stepped in real time from the page's animation loop
// Returned strings are valid until the next call into the same surface.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

#include "taut/config.h"
#include "taut/session.h"
#include "taut/sim_net.h"
#include "taut/types.h"

using namespace std::chrono_literals;

namespace {

std::string g_ret;

const char* ret(std::string s) {
    g_ret = std::move(s);
    return g_ret.c_str();
}

taut::Endpoint ep(std::uint16_t port) {
    taut::Endpoint e{};
    e.addr_be = 1;
    e.port_be = port;
    return e;
}

} // namespace

extern "C" {

// Transfers n_msgs reliable-ordered messages A->B over an impaired link.
// Returns JSON:
// { delivered, ordered, retransmits, virtual_ms, timeline: [[ms, delivered], ...],
//   lat_p50, lat_p99, lat_max, timed_out }
EMSCRIPTEN_KEEPALIVE
const char* tt_run(double seed, double loss_pct, int delay_ms, int jitter_ms,
                   int window_pkts, int rto_floor_ms, int n_msgs, int payload_size) {
    taut::Impairments imp;
    imp.loss = loss_pct / 100.0;
    imp.delay = std::chrono::milliseconds(delay_ms);
    imp.jitter = std::chrono::milliseconds(jitter_ms);

    taut::SimNet net(static_cast<std::uint64_t>(seed), imp);
    const auto a = ep(1);
    const auto b = ep(2);

    taut::Config cfg;
    cfg.window_pkts = static_cast<std::uint16_t>(window_pkts);
    cfg.rto_floor = std::chrono::milliseconds(rto_floor_ms);

    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    const auto t0 = net.now();
    std::vector<int> send_ms(n_msgs, -1), recv_ms(n_msgs, -1);
    int delivered = 0;
    bool ordered = true;
    std::uint32_t expect = 0;

    sb.on_message([&](taut::Class, taut::ByteSpan p) {
        std::uint32_t v = 0;
        std::memcpy(&v, p.data(), std::min(sizeof(v), p.size()));
        if (v != expect) {
            ordered = false;
        }
        expect = v + 1;
        if (v < static_cast<std::uint32_t>(n_msgs)) {
            recv_ms[v] = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(net.now() - t0).count());
        }
        delivered++;
    });

    std::vector<std::byte> payload(std::max(payload_size, 4));
    int next = 0;
    const int step_ms = 5;
    const int max_virtual_ms = 120000;
    std::string timeline = "[";
    int elapsed = 0, last_sampled = -1000;

    while (delivered < n_msgs && elapsed < max_virtual_ms) {
        // Feed as much as the window allows.
        while (next < n_msgs) {
            std::uint32_t v = static_cast<std::uint32_t>(next);
            std::memcpy(payload.data(), &v, sizeof(v));
            if (!sa.send(taut::Class::ReliableOrdered,
                         taut::ByteSpan(payload.data(), payload.size()))) {
                break; // backpressure: window full
            }
            send_ms[next] = elapsed;
            next++;
        }
        net.advance(std::chrono::milliseconds(step_ms));
        elapsed += step_ms;
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();

        if (elapsed - last_sampled >= 25) { // sample every 25 virtual ms
            timeline += (timeline.size() > 1 ? "," : "");
            timeline += "[" + std::to_string(elapsed) + "," + std::to_string(delivered) + "]";
            last_sampled = elapsed;
        }
    }
    timeline += "]";

    // Per-message delivery latency (virtual ms).
    std::vector<int> lat;
    lat.reserve(n_msgs);
    for (int i = 0; i < n_msgs; i++) {
        if (send_ms[i] >= 0 && recv_ms[i] >= 0) {
            lat.push_back(recv_ms[i] - send_ms[i]);
        }
    }
    std::sort(lat.begin(), lat.end());
    const auto pick = [&](double q) {
        return lat.empty() ? 0 : lat[std::min(lat.size() - 1,
                                              static_cast<std::size_t>(q * lat.size()))];
    };

    std::string out = "{";
    out += "\"delivered\":" + std::to_string(delivered);
    out += ",\"sent\":" + std::to_string(next);
    out += ",\"ordered\":" + std::string(ordered ? "true" : "false");
    out += ",\"retransmits\":" + std::to_string(sa.retransmits());
    out += ",\"virtual_ms\":" + std::to_string(elapsed);
    out += ",\"timed_out\":" + std::string(delivered < n_msgs ? "true" : "false");
    out += ",\"lat_p50\":" + std::to_string(pick(0.50));
    out += ",\"lat_p99\":" + std::to_string(pick(0.99));
    out += ",\"lat_max\":" + std::to_string(lat.empty() ? 0 : lat.back());
    out += ",\"timeline\":" + timeline;
    out += "}";
    return ret(std::move(out));
}

} // extern "C"

// ---------------------------------------------------------------------------
// tt_feel_*: the cursor-echo instrument. Two independent experiments share one
// virtual clock cadence; each is a real Session pair over its own SimNet. The
// nets are seeded identically, so the links are statistically identical, but
// once retransmit schedules diverge the per-packet RNG draws diverge too -
// same weather, not the same raindrops.
// ---------------------------------------------------------------------------

namespace {

struct FeelDelivery {
    std::uint32_t seq;
    float x, y;
    int lat_ms;
};

struct FeelChannel {
    std::unique_ptr<taut::SimNet> net;
    std::unique_ptr<taut::Session> tx;
    std::unique_ptr<taut::Session> rx;
    taut::SimNet::TimePoint t0{};
    std::uint32_t next_msg = 0;
    std::vector<int> send_ms;            // per-seq virtual send time
    std::vector<FeelDelivery> delivered; // drained by tt_feel_step

    int now_ms() const {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(net->now() - t0).count());
    }
};

FeelChannel g_feel[2];
bool g_feel_ready = false;
std::string g_feel_ret;

const char* feel_ret(std::string s) {
    g_feel_ret = std::move(s);
    return g_feel_ret.c_str();
}

void feel_make(FeelChannel& ch, double seed, double loss_pct, int delay_ms, int jitter_ms,
               int rto_floor_ms) {
    taut::Impairments imp;
    imp.loss = loss_pct / 100.0;
    imp.delay = std::chrono::milliseconds(delay_ms);
    imp.jitter = std::chrono::milliseconds(jitter_ms);

    ch.net = std::make_unique<taut::SimNet>(static_cast<std::uint64_t>(seed), imp);
    const auto a = ep(1);
    const auto b = ep(2);

    taut::Config cfg;
    cfg.window_pkts = 64;
    cfg.rto_floor = std::chrono::milliseconds(rto_floor_ms);

    ch.tx = std::make_unique<taut::Session>(ch.net->endpoint(a), b, cfg);
    ch.rx = std::make_unique<taut::Session>(ch.net->endpoint(b), a, cfg);
    ch.t0 = ch.net->now();
    ch.next_msg = 0;
    ch.send_ms.clear();
    ch.delivered.clear();

    FeelChannel* chp = &ch;
    ch.rx->on_message([chp](taut::Class, taut::ByteSpan p) {
        if (p.size() < 12) {
            return;
        }
        FeelDelivery d{};
        std::memcpy(&d.seq, p.data(), 4);
        std::memcpy(&d.x, p.data() + 4, 4);
        std::memcpy(&d.y, p.data() + 8, 4);
        d.lat_ms = (d.seq < chp->send_ms.size()) ? chp->now_ms() - chp->send_ms[d.seq] : 0;
        chp->delivered.push_back(d);
    });
}

} // namespace

extern "C" {

// (Re)build both experiments. Floors are fixed: channel a = 25ms, channel b = 200ms.
EMSCRIPTEN_KEEPALIVE
int tt_feel_init(double seed, double loss_pct, int delay_ms, int jitter_ms) {
    feel_make(g_feel[0], seed, loss_pct, delay_ms, jitter_ms, 25);
    feel_make(g_feel[1], seed, loss_pct, delay_ms, jitter_ms, 200);
    g_feel_ready = true;
    return 0;
}

// Send one cursor sample (12-byte payload: seq, x, y) on both channels.
// Returns {"a":seq,"b":seq}; -1 where the send queue pushed back.
EMSCRIPTEN_KEEPALIVE
const char* tt_feel_send(double x, double y) {
    if (!g_feel_ready) {
        return feel_ret("{\"a\":-1,\"b\":-1}");
    }
    long long seqs[2];
    for (int i = 0; i < 2; i++) {
        FeelChannel& ch = g_feel[i];
        std::byte payload[12];
        const std::uint32_t seq = ch.next_msg;
        const float fx = static_cast<float>(x);
        const float fy = static_cast<float>(y);
        std::memcpy(payload, &seq, 4);
        std::memcpy(payload + 4, &fx, 4);
        std::memcpy(payload + 8, &fy, 4);
        if (ch.tx->send(taut::Class::ReliableOrdered, taut::ByteSpan(payload, 12))) {
            ch.send_ms.push_back(ch.now_ms());
            ch.next_msg++;
            seqs[i] = seq;
        } else {
            seqs[i] = -1;
        }
    }
    return feel_ret("{\"a\":" + std::to_string(seqs[0]) + ",\"b\":" + std::to_string(seqs[1]) +
                    "}");
}

// Advance both experiments dt virtual ms (5ms sub-steps, same cadence as tt_run)
// and drain deliveries. Returns
// {"a":{"msgs":[[seq,x,y,lat],...],"retx":n},"b":{...}}
EMSCRIPTEN_KEEPALIVE
const char* tt_feel_step(int dt_ms) {
    if (!g_feel_ready) {
        return feel_ret("{}");
    }
    dt_ms = std::clamp(dt_ms, 0, 250);
    for (int done = 0; done < dt_ms; done += 5) {
        const auto step = std::chrono::milliseconds(std::min(5, dt_ms - done));
        for (FeelChannel& ch : g_feel) {
            ch.net->advance(step);
            ch.tx->poll();
            ch.rx->poll();
            ch.tx->tick();
            ch.rx->tick();
        }
    }
    std::string out = "{";
    const char* names[2] = {"\"a\":", "\"b\":"};
    for (int i = 0; i < 2; i++) {
        FeelChannel& ch = g_feel[i];
        out += names[i];
        out += "{\"msgs\":[";
        for (std::size_t m = 0; m < ch.delivered.size(); m++) {
            const FeelDelivery& d = ch.delivered[m];
            out += (m ? "," : "");
            out += "[" + std::to_string(d.seq) + "," + std::to_string(d.x) + "," +
                   std::to_string(d.y) + "," + std::to_string(d.lat_ms) + "]";
        }
        out += "],\"retx\":" + std::to_string(ch.tx->retransmits()) + "}";
        out += (i == 0 ? "," : "");
        ch.delivered.clear();
    }
    out += "}";
    return feel_ret(std::move(out));
}

} // extern "C"
