// C ABI for the browser demo. Runs real Sessions over the deterministic SimNet
// with a virtual clock, so a multi-second lossy transfer completes in real
// milliseconds. One call = one full experiment; the result is a JSON string
// (valid until the next call) with a delivery timeline and latency percentiles.

#include <algorithm>
#include <cstdint>
#include <cstring>
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
