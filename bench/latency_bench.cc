// taut message-latency load generator (§7). One binary, several modes over a symmetric pair
// of taut Sessions on real UDP sockets. Roles: --role receiver binds (bind,port); --role
// sender binds (bind,port+1) and targets (addr,port). The taut Session (poll/tick) has no
// epoll driver yet, so both roles pump poll()+tick() in a tight loop - exactly what the
// future Node event loop will do per iteration.
//
// Modes:
//   rr         : closed-loop request-reply (one outstanding). Client sends a 512 B request,
//                server echoes it, client records the round-trip. This is the headline
//                latency-vs-loss probe (netperf TCP_RR style) - rate-independent, unbiased,
//                and it isolates per-message recovery latency (RTO + head-of-line) from
//                throughput throttling. See docs/BENCHMARKS.md for why this replaces open-loop
//                Poisson as the headline.
//   latency    : open-loop Poisson at --rate. Sustained-load test; under loss it shows
//                delivery ratio (received/offered) and coordinated-omission-corrected latency.
//   throughput : saturating send; receiver reports goodput (clean-link axis).
//
// Class note (2026-07): the merged Session delivers *all* classes in seq order (class-1
// unordered semantics land with feat/core). --class 1 today therefore exercises the same
// code path as class 2; the class-1 curve is re-run after feat/core merges. The CSV still
// records the requested class so the two runs stay distinguishable.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bench/common.h"
#include "taut/config.h"
#include "taut/session.h"
#include "taut/transport.h"
#include "taut/types.h"

namespace {

// Decorates a UdpTransport to count datagrams/bytes actually put on (and taken off) the
// socket - including retransmits and pure acks. Lets the taut sender report its app-level
// wire cost (UDP payload; IP/UDP headers are added by the kernel and show up in wire.csv).
class CountingTransport : public taut::UdpTransport {
  public:
    explicit CountingTransport(taut::UdpTransport& inner) : inner_(inner) {}

    std::size_t send(const taut::Endpoint& to, std::span<const std::byte> data) override {
        ++tx_datagrams_;
        tx_bytes_ += data.size();
        return inner_.send(to, data);
    }
    std::optional<taut::RecvResult> recv(std::span<std::byte> buf) override {
        auto r = inner_.recv(buf);
        if (r) {
            ++rx_datagrams_;
            rx_bytes_ += r->size;
        }
        return r;
    }
    std::chrono::steady_clock::time_point now() const override {
        return inner_.now();
    }
    int fd() const override {
        return inner_.fd();
    }

    std::uint64_t tx_datagrams() const {
        return tx_datagrams_;
    }
    std::uint64_t tx_bytes() const {
        return tx_bytes_;
    }

  private:
    taut::UdpTransport& inner_;
    std::uint64_t tx_datagrams_ = 0, tx_bytes_ = 0, rx_datagrams_ = 0, rx_bytes_ = 0;
};

taut::Config make_config(const bench::Args& a) {
    taut::Config cfg;
    cfg.rto_floor = std::chrono::milliseconds(a.rto_floor_ms);
    cfg.window_pkts = static_cast<std::uint16_t>(a.window_pkts);
    cfg.mtu_payload = 1200;
    return cfg;
}

void nap_50us() {
    struct timespec ts {
        0, 50'000
    };
    nanosleep(&ts, nullptr);
}

// ---- open-loop latency / throughput receiver -------------------------------------------

int run_receiver(const bench::Args& a) {
    taut::RealUdpTransport real;
    if (!real.bind(a.bind_addr, static_cast<std::uint16_t>(a.port))) {
        std::fprintf(stderr, "latency_bench receiver: bind %s:%d failed\n", a.bind_addr.c_str(),
                     a.port);
        return 1;
    }
    const auto peer = taut::make_endpoint(a.addr, static_cast<std::uint16_t>(a.port + 1));
    if (!peer) {
        std::fprintf(stderr, "latency_bench receiver: bad peer addr %s\n", a.addr.c_str());
        return 1;
    }
    taut::Session session(real, *peer, make_config(a));

    std::vector<double> samples;
    samples.reserve(1u << 20);
    std::uint64_t received = 0, max_seq = 0, goodput_bytes = 0;
    std::uint64_t first_recv_ns = 0, last_recv_ns = 0;
    bool got_end = false;
    std::uint64_t last_activity_ns = bench::now_ns();
    const std::size_t msg = static_cast<std::size_t>(a.msg_size);

    session.on_message([&](taut::Class, taut::ByteSpan payload) {
        if (payload.size() < bench::kMsgHeader) {
            return;
        }
        std::uint64_t ts = 0, seq = 0;
        bench::read_msg(payload, ts, seq);
        const std::uint64_t recv = bench::now_ns();
        last_activity_ns = recv;
        if (seq == bench::kEndSeq) {
            got_end = true;
            return;
        }
        if (first_recv_ns == 0) {
            first_recv_ns = recv;
        }
        last_recv_ns = recv;
        ++received;
        goodput_bytes += msg;
        if (seq > max_seq) {
            max_seq = seq;
        }
        samples.push_back(recv > ts ? static_cast<double>(recv - ts) / 1e6 : 0.0);
    });

    const std::uint64_t idle_ns = static_cast<std::uint64_t>(a.idle_ms) * 1'000'000ULL;
    const std::uint64_t hard_cap_ns =
        bench::now_ns() + static_cast<std::uint64_t>(a.duration_s + 60) * 1'000'000'000ULL;
    for (;;) {
        session.poll();
        session.tick();
        const std::uint64_t n = bench::now_ns();
        if (got_end && n - last_activity_ns > 200'000'000ULL) {
            break;
        }
        if (received > 0 && n - last_activity_ns > idle_ns) {
            break;
        }
        if (n > hard_cap_ns) {
            break;
        }
        nap_50us();
    }

    if (a.mode == "throughput") {
        const double secs = last_recv_ns > first_recv_ns
                                ? static_cast<double>(last_recv_ns - first_recv_ns) / 1e9
                                : 0.0;
        const double mbps = secs > 0 ? static_cast<double>(goodput_bytes) * 8.0 / secs / 1e6 : 0.0;
        std::string row = bench::key_row(a);
        row += "," + std::to_string(received) + "," + std::to_string(goodput_bytes) + "," +
               std::to_string(secs) + "," + std::to_string(mbps);
        bench::append_csv(a.out, bench::key_header() + ",received,goodput_bytes,secs,goodput_mbps",
                          row);
        std::fprintf(stderr, "taut recv[thru]: %llu msgs, %.2f Mbit/s over %.2fs\n",
                     static_cast<unsigned long long>(received), mbps, secs);
        return 0;
    }

    const std::uint64_t offered = bench::schedule_count(a.seed, a.rate, a.duration_s);
    bench::Percentiles p = bench::percentiles(samples);
    std::string row = bench::key_row(a);
    row += "," + std::to_string(offered) + "," + std::to_string(received) + "," +
           std::to_string(p.p50) + "," + std::to_string(p.p90) + "," + std::to_string(p.p99) + "," +
           std::to_string(p.p999) + "," + std::to_string(p.min) + "," + std::to_string(p.max) +
           "," + std::to_string(p.mean);
    bench::append_csv(a.out,
                      bench::key_header() +
                          ",offered,received,p50_ms,p90_ms,p99_ms,p999_ms,min_ms,max_ms,mean_ms",
                      row);
    std::fprintf(stderr,
                 "taut recv[lat cls%d loss%.0f%%]: offered=%llu recv=%llu p50=%.2f p99=%.2f "
                 "p999=%.2f max=%.2f ms\n",
                 a.taut_class, a.loss_pct, static_cast<unsigned long long>(offered),
                 static_cast<unsigned long long>(received), p.p50, p.p99, p.p999, p.max);
    return 0;
}

int run_sender(const bench::Args& a) {
    taut::RealUdpTransport real;
    if (!real.bind(a.bind_addr, static_cast<std::uint16_t>(a.port + 1))) {
        std::fprintf(stderr, "latency_bench sender: bind %s:%d failed\n", a.bind_addr.c_str(),
                     a.port + 1);
        return 1;
    }
    const auto peer = taut::make_endpoint(a.addr, static_cast<std::uint16_t>(a.port));
    if (!peer) {
        std::fprintf(stderr, "latency_bench sender: bad peer addr %s\n", a.addr.c_str());
        return 1;
    }
    CountingTransport tx(real);
    taut::Session session(tx, *peer, make_config(a));
    const auto cls = static_cast<taut::Class>(a.taut_class);
    auto pump = [&] {
        session.poll();
        session.tick();
    };

    std::vector<std::byte> buf(static_cast<std::size_t>(a.msg_size));
    const std::span<const std::byte> payload(buf.data(), buf.size());
    std::uint64_t seq = 0, sent = 0;
    const std::uint64_t start = bench::now_ns();

    if (a.mode == "throughput") {
        const std::uint64_t end =
            start + static_cast<std::uint64_t>(a.duration_s) * 1'000'000'000ULL;
        while (bench::now_ns() < end) {
            bench::write_msg(buf, bench::now_ns(), seq);
            if (session.send(cls, payload)) {
                ++seq;
                ++sent;
            } else {
                pump();
            }
        }
    } else {
        bench::PoissonSchedule sched(a.seed, a.rate);
        const std::uint64_t dur_ns = static_cast<std::uint64_t>(a.duration_s) * 1'000'000'000ULL;
        for (;;) {
            const double off_s = sched.next_offset_s();
            const std::uint64_t intended = start + static_cast<std::uint64_t>(off_s * 1e9);
            if (intended - start > dur_ns) {
                break;
            }
            // Saturation wall-cap: stop offering after duration+15s of real time so a wedged
            // transport doesn't drag the run out; the un-offered tail counts against delivery.
            if (bench::now_ns() - start > dur_ns + 15'000'000'000ULL) {
                break;
            }
            bench::wait_until_ns(intended, pump);
            bench::write_msg(buf, intended, seq); // stamp INTENDED time (coordinated-omission fix)
            while (!session.send(cls, payload)) {
                pump();
            }
            ++seq;
            ++sent;
        }
    }

    auto drain_ring = [&] {
        while (session.in_flight() > 0) {
            pump();
            nap_50us();
        }
    };
    drain_ring();
    bench::write_msg(buf, bench::now_ns(), bench::kEndSeq);
    while (!session.send(cls, payload)) {
        pump();
    }
    drain_ring();
    const std::uint64_t extra = bench::now_ns() + 300'000'000ULL;
    while (bench::now_ns() < extra) {
        pump();
    }

    if (!a.send_out.empty()) {
        std::string row = bench::key_row(a);
        row += "," + std::to_string(sent) + "," + std::to_string(tx.tx_datagrams()) + "," +
               std::to_string(tx.tx_bytes());
        bench::append_csv(a.send_out, bench::key_header() + ",sent,tx_datagrams,tx_payload_bytes",
                          row);
    }
    const double mult =
        sent > 0 ? static_cast<double>(tx.tx_datagrams()) / static_cast<double>(sent) : 0.0;
    std::fprintf(stderr, "taut send[cls%d loss%.0f%%]: sent=%llu tx_datagrams=%llu (%.2fx)\n",
                 a.taut_class, a.loss_pct, static_cast<unsigned long long>(sent),
                 static_cast<unsigned long long>(tx.tx_datagrams()), mult);
    return 0;
}

// ---- closed-loop request-reply (headline) ----------------------------------------------

int run_rr_server(const bench::Args& a) {
    taut::RealUdpTransport real;
    if (!real.bind(a.bind_addr, static_cast<std::uint16_t>(a.port))) {
        std::fprintf(stderr, "latency_bench rr-server: bind failed\n");
        return 1;
    }
    const auto peer = taut::make_endpoint(a.addr, static_cast<std::uint16_t>(a.port + 1));
    if (!peer) {
        return 1;
    }
    taut::Session session(real, *peer, make_config(a));
    bool got_end = false;
    std::uint64_t last_activity_ns = bench::now_ns();

    session.on_message([&](taut::Class cls, taut::ByteSpan payload) {
        last_activity_ns = bench::now_ns();
        if (payload.size() < bench::kMsgHeader) {
            return;
        }
        std::uint64_t ts = 0, seq = 0;
        bench::read_msg(payload, ts, seq);
        if (seq == bench::kEndSeq) {
            got_end = true;
            return;
        }
        session.send(cls, payload); // echo verbatim (1 outstanding => window never fills)
    });

    const std::uint64_t idle_ns = static_cast<std::uint64_t>(a.idle_ms) * 1'000'000ULL;
    const std::uint64_t hard_cap_ns =
        bench::now_ns() + static_cast<std::uint64_t>(a.duration_s + 120) * 1'000'000'000ULL;
    for (;;) {
        session.poll();
        session.tick();
        const std::uint64_t n = bench::now_ns();
        if (got_end && n - last_activity_ns > 300'000'000ULL) {
            break;
        }
        if (n - last_activity_ns > idle_ns + 2'000'000'000ULL || n > hard_cap_ns) {
            break;
        }
        nap_50us();
    }
    std::fprintf(stderr, "taut rr-server: done\n");
    return 0;
}

int run_rr_client(const bench::Args& a) {
    taut::RealUdpTransport real;
    if (!real.bind(a.bind_addr, static_cast<std::uint16_t>(a.port + 1))) {
        std::fprintf(stderr, "latency_bench rr-client: bind failed\n");
        return 1;
    }
    const auto peer = taut::make_endpoint(a.addr, static_cast<std::uint16_t>(a.port));
    if (!peer) {
        return 1;
    }
    CountingTransport tx(real);
    taut::Session session(tx, *peer, make_config(a));
    const auto cls = static_cast<taut::Class>(a.taut_class);
    auto pump = [&] {
        session.poll();
        session.tick();
    };

    bool got_reply = false;
    std::vector<double> samples;
    samples.reserve(1u << 18);
    session.on_message([&](taut::Class, taut::ByteSpan payload) {
        if (payload.size() < bench::kMsgHeader) {
            return;
        }
        std::uint64_t ts = 0, seq = 0;
        bench::read_msg(payload, ts, seq);
        if (seq == bench::kEndSeq) {
            return;
        }
        samples.push_back(static_cast<double>(bench::now_ns() - ts) / 1e6); // round trip
        got_reply = true;
    });

    std::vector<std::byte> buf(static_cast<std::size_t>(a.msg_size));
    const std::span<const std::byte> payload(buf.data(), buf.size());
    std::uint64_t seq = 0, sent = 0;
    const std::uint64_t start = bench::now_ns();
    const std::uint64_t dur_ns = static_cast<std::uint64_t>(a.duration_s) * 1'000'000'000ULL;

    while (bench::now_ns() - start < dur_ns) {
        bench::write_msg(buf, bench::now_ns(), seq); // stamp actual send instant (true round trip)
        got_reply = false;
        while (!session.send(cls, payload)) {
            pump();
        }
        ++seq;
        ++sent;
        // Await this reply (reliable => arrives); cap guards against a wedged run.
        const std::uint64_t cap = bench::now_ns() + 15'000'000'000ULL;
        while (!got_reply && bench::now_ns() < cap) {
            session.poll();
            session.tick();
        }
    }

    // END sentinel, then a short flush.
    bench::write_msg(buf, bench::now_ns(), bench::kEndSeq);
    while (!session.send(cls, payload)) {
        pump();
    }
    const std::uint64_t extra = bench::now_ns() + 500'000'000ULL;
    while (bench::now_ns() < extra) {
        pump();
        nap_50us();
    }

    bench::Percentiles p = bench::percentiles(samples);
    std::string row = bench::key_row(a);
    row += "," + std::to_string(sent) + "," + std::to_string(p.n) + "," + std::to_string(p.p50) +
           "," + std::to_string(p.p90) + "," + std::to_string(p.p99) + "," +
           std::to_string(p.p999) + "," + std::to_string(p.min) + "," + std::to_string(p.max) +
           "," + std::to_string(p.mean);
    bench::append_csv(a.out,
                      bench::key_header() +
                          ",sent,replies,p50_ms,p90_ms,p99_ms,p999_ms,min_ms,max_ms,mean_ms",
                      row);
    if (!a.send_out.empty()) {
        std::string s = bench::key_row(a);
        s += "," + std::to_string(sent) + "," + std::to_string(tx.tx_datagrams()) + "," +
             std::to_string(tx.tx_bytes());
        bench::append_csv(a.send_out, bench::key_header() + ",sent,tx_datagrams,tx_payload_bytes",
                          s);
    }
    std::fprintf(stderr,
                 "taut rr[cls%d loss%.0f%%]: n=%zu p50=%.2f p99=%.2f p999=%.2f max=%.2f ms "
                 "(%.2fx wire)\n",
                 a.taut_class, a.loss_pct, p.n, p.p50, p.p99, p.p999, p.max,
                 sent > 0 ? static_cast<double>(tx.tx_datagrams()) / static_cast<double>(sent)
                          : 0.0);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const bench::Args a = bench::parse_args(argc, argv);
    if (a.mode == "rr") {
        return a.role == "receiver" ? run_rr_server(a) : run_rr_client(a);
    }
    return a.role == "receiver" ? run_receiver(a) : run_sender(a);
}
