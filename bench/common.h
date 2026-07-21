// Shared load-generator + measurement plumbing for the three latency benchmarks
// (taut, TCP, ENet). See docs/BENCHMARKS.md for the method; the design decisions worth
// knowing:
//
//   * One-way latency. Every message carries an 8-byte CLOCK_MONOTONIC send timestamp;
//     the receiver computes latency = recv_now - send_ts. This is honest only because
//     both processes run on the same host (veth across two netns, §6.5) and therefore
//     read the same system-wide monotonic clock. Do NOT reuse this across two machines.
//
//   * Coordinated-omission correction (wrk2/HdrHistogram style). Each message is stamped
//     with its *intended* Poisson arrival time, not the instant we managed to hand it to
//     the transport. If the transport backs up (window full, TCP head-of-line stall), the
//     queueing delay lands in the measured latency instead of vanishing. Benchmarking a
//     transport under loss without this correction flatters whichever transport stalls,
//     which is exactly the effect we are trying to measure.
//
//   * Fixed seed -> reproducible arrival schedule (std::mt19937_64 + exponential
//     inter-arrivals). Same (seed, rate, duration) => same offered load on every run.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bench {

// ---- clock -----------------------------------------------------------------------------

// System-wide monotonic time in nanoseconds. The single time source for every binary, so
// send/receive stamps are directly comparable across processes on one host.
inline std::uint64_t now_ns() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

// Service the transport (pump) while waiting for `target_ns`. A short nanosleep keeps us
// off a 100 % busy-spin without blowing the sub-millisecond scheduling budget.
template <class Pump> void wait_until_ns(std::uint64_t target_ns, Pump&& pump) {
    for (;;) {
        pump();
        const std::uint64_t n = now_ns();
        if (n >= target_ns) {
            return;
        }
        if (target_ns - n > 150'000ULL) { // >150us out: sleep ~50us, then re-check
            struct timespec ts {
                0, 50'000
            };
            nanosleep(&ts, nullptr);
        }
    }
}

// ---- message layout --------------------------------------------------------------------

// Every message opens with {send_ts_ns, seq}; the rest is filler up to msg_size. Fixed
// offsets + memcpy so there is no struct-packing or alignment ambiguity on the wire.
inline constexpr std::size_t kMsgHeader = 16; // u64 ts + u64 seq
inline constexpr int kDefaultMsgSize = 512;

inline void write_msg(std::span<std::byte> buf, std::uint64_t ts_ns, std::uint64_t seq) {
    std::memcpy(buf.data() + 0, &ts_ns, sizeof ts_ns);
    std::memcpy(buf.data() + 8, &seq, sizeof seq);
    // Deterministic non-zero filler so on-wire bytes aren't a trivially compressible run.
    for (std::size_t i = kMsgHeader; i < buf.size(); ++i) {
        buf[i] = static_cast<std::byte>((i * 31u + 7u) & 0xFF);
    }
}

inline void read_msg(std::span<const std::byte> buf, std::uint64_t& ts_ns, std::uint64_t& seq) {
    std::memcpy(&ts_ns, buf.data() + 0, sizeof ts_ns);
    std::memcpy(&seq, buf.data() + 8, sizeof seq);
}

// A distinguished sequence value marking end-of-run. Sent (reliably, several times) by the
// sender after its send window closes so a reliable receiver can flush and exit promptly;
// the receiver also has an idle-timeout fallback for lossy/unreliable runs.
inline constexpr std::uint64_t kEndSeq = 0xFFFF'FFFF'FFFF'FFFFULL;

// ---- Poisson arrival schedule ----------------------------------------------------------

// Cumulative intended arrival offsets (seconds from t0). next_offset() advances one arrival;
// inter-arrival times are Exp(rate), i.e. a Poisson process at `rate` messages/second.
class PoissonSchedule {
  public:
    PoissonSchedule(std::uint64_t seed, double rate) : rng_(seed), gap_(rate) {}
    double next_offset_s() {
        accum_ += gap_(rng_);
        return accum_;
    }

  private:
    std::mt19937_64 rng_;
    std::exponential_distribution<double> gap_;
    double accum_ = 0.0;
};

// How many messages the open-loop schedule offers within [0, duration] for this (seed, rate).
// Both roles compute it identically, so the receiver can report an honest delivery ratio
// (received / offered) even when a saturated sender never gets to enqueue the tail.
inline std::uint64_t schedule_count(std::uint64_t seed, double rate, int duration_s) {
    PoissonSchedule s(seed, rate);
    const double dur = static_cast<double>(duration_s);
    std::uint64_t n = 0;
    while (s.next_offset_s() <= dur) {
        ++n;
    }
    return n;
}

// ---- percentiles -----------------------------------------------------------------------

struct Percentiles {
    std::size_t n = 0;
    double p50 = 0, p90 = 0, p99 = 0, p999 = 0, min = 0, max = 0, mean = 0;
};

// Nearest-rank percentiles over `xs` (sorted in place). Nearest-rank (no interpolation) is
// the defensible choice for tail latency: p999 reports an actually-observed sample.
inline Percentiles percentiles(std::vector<double>& xs) {
    Percentiles r;
    r.n = xs.size();
    if (xs.empty()) {
        return r;
    }
    std::sort(xs.begin(), xs.end());
    auto rank = [&](double p) -> double {
        // 1-indexed nearest rank: ceil(p * n), clamped to [1, n].
        std::size_t idx = static_cast<std::size_t>(std::ceil(p * static_cast<double>(xs.size())));
        if (idx < 1) {
            idx = 1;
        }
        if (idx > xs.size()) {
            idx = xs.size();
        }
        return xs[idx - 1];
    };
    r.min = xs.front();
    r.max = xs.back();
    r.p50 = rank(0.50);
    r.p90 = rank(0.90);
    r.p99 = rank(0.99);
    r.p999 = rank(0.999);
    double sum = 0;
    for (double x : xs) {
        sum += x;
    }
    r.mean = sum / static_cast<double>(xs.size());
    return r;
}

// ---- CSV sink --------------------------------------------------------------------------

// Append one row; write `header` first iff the file is empty/new. Runs are sequential
// (run_matrix.sh) so no locking is needed.
inline void append_csv(const std::string& path, std::string_view header, std::string_view row) {
    bool need_header = true;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (in && in.tellg() > 0) {
            need_header = false;
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        return;
    }
    if (need_header) {
        out << header << '\n';
    }
    out << row << '\n';
}

// ---- args ------------------------------------------------------------------------------

struct Args {
    std::string role = "sender";       // sender | receiver
    std::string transport = "taut";    // CSV label: taut | tcp | enet
    std::string mode = "latency";      // latency | throughput
    std::string addr = "127.0.0.1";    // remote peer IP (sender: target; receiver: sender)
    std::string bind_addr = "0.0.0.0"; // local bind IP
    int port = 8000;                   // receiver's port; taut sender binds port+1
    double rate = 2000.0;              // messages/second (latency mode)
    int duration_s = 60;               // send duration
    std::uint64_t seed = 1;
    int run = 0;           // run index, CSV label only
    double loss_pct = 0.0; // netem loss, CSV label only
    double rtt_ms = 0.0;   // netem RTT, CSV label only
    int msg_size = kDefaultMsgSize;
    int taut_class = 2;   // 1 | 2 (taut only)
    int idle_ms = 3000;   // receiver: stop after this much silence
    std::string out;      // receiver latency/throughput CSV
    std::string send_out; // sender send-side CSV
    // taut Config knobs (taut only)
    int rto_floor_ms = 25;
    int window_pkts = 64;
};

inline Args parse_args(int argc, char** argv) {
    Args a;
    auto next = [&](int& i) -> std::string {
        return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--role") {
            a.role = next(i);
        } else if (k == "--transport") {
            a.transport = next(i);
        } else if (k == "--mode") {
            a.mode = next(i);
        } else if (k == "--addr") {
            a.addr = next(i);
        } else if (k == "--bind") {
            a.bind_addr = next(i);
        } else if (k == "--port") {
            a.port = std::stoi(next(i));
        } else if (k == "--rate") {
            a.rate = std::stod(next(i));
        } else if (k == "--duration") {
            a.duration_s = std::stoi(next(i));
        } else if (k == "--seed") {
            a.seed = std::stoull(next(i));
        } else if (k == "--run") {
            a.run = std::stoi(next(i));
        } else if (k == "--loss") {
            a.loss_pct = std::stod(next(i));
        } else if (k == "--rtt") {
            a.rtt_ms = std::stod(next(i));
        } else if (k == "--msg-size") {
            a.msg_size = std::stoi(next(i));
        } else if (k == "--class") {
            a.taut_class = std::stoi(next(i));
        } else if (k == "--idle") {
            a.idle_ms = std::stoi(next(i));
        } else if (k == "--out") {
            a.out = next(i);
        } else if (k == "--send-out") {
            a.send_out = next(i);
        } else if (k == "--rto-floor") {
            a.rto_floor_ms = std::stoi(next(i));
        } else if (k == "--window") {
            a.window_pkts = std::stoi(next(i));
        }
    }
    return a;
}

// The shared leading CSV columns, identical across all three binaries so the per-CSV rows
// join cleanly in plot.py. `cls` is the taut reliability class (blank for tcp/enet).
inline std::string key_header() {
    return "transport,cls,mode,loss_pct,rtt_ms,rate,run,seed";
}
inline std::string key_row(const Args& a) {
    std::string cls = (a.transport == "taut") ? std::to_string(a.taut_class) : std::string();
    std::string s;
    s += a.transport;
    s += ',';
    s += cls;
    s += ',';
    s += a.mode;
    s += ',';
    s += std::to_string(a.loss_pct);
    s += ',';
    s += std::to_string(a.rtt_ms);
    s += ',';
    s += std::to_string(a.rate);
    s += ',';
    s += std::to_string(a.run);
    s += ',';
    s += std::to_string(a.seed);
    return s;
}

} // namespace bench
