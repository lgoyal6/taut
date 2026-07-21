// Kernel-TCP baseline for the latency matrix (§7). Same 512 B workload as latency_bench, over
// a single TCP connection with TCP_NODELAY (never benchmark latency against Nagle) and
// length-prefixed framing: [u32 little-endian payload length][payload].
//
// TCP is forced to behave like taut's class 2 for everything: strictly ordered bytes, so one
// lost segment head-of-line-blocks every later message until recovery (fast-retransmit, or the
// >=200 ms Linux RTO_min floor). Modes mirror latency_bench: rr (headline round-trip probe),
// latency (open-loop Poisson sustained load), throughput (clean-link goodput).
#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "bench/common.h"

namespace {

bool set_nodelay(int fd) {
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0;
}

bool read_full(int fd, std::byte* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::read(fd, buf + got, n - got);
        if (r > 0) {
            got += static_cast<std::size_t>(r);
        } else {
            return false; // EOF, error, or SO_RCVTIMEO expiry
        }
    }
    return true;
}

bool write_full(int fd, const std::byte* buf, std::size_t n) {
    std::size_t put = 0;
    while (put < n) {
        const ssize_t w = ::write(fd, buf + put, n - put);
        if (w <= 0) {
            return false;
        }
        put += static_cast<std::size_t>(w);
    }
    return true;
}

int listen_accept(const bench::Args& a) {
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        std::perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<std::uint16_t>(a.port));
    sa.sin_addr.s_addr = a.bind_addr == "0.0.0.0" ? INADDR_ANY : inet_addr(a.bind_addr.c_str());
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        std::perror("bind");
        ::close(lfd);
        return -1;
    }
    if (::listen(lfd, 1) != 0) {
        std::perror("listen");
        ::close(lfd);
        return -1;
    }
    const int fd =
        ::accept(lfd, nullptr, nullptr); // script starts us first; client connects promptly
    ::close(lfd);
    return fd;
}

int connect_client(const bench::Args& a) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<std::uint16_t>(a.port));
    sa.sin_addr.s_addr = inet_addr(a.addr.c_str());
    for (int attempt = 0; attempt < 100; ++attempt) { // tolerate small server-startup skew
        if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) {
            set_nodelay(fd);
            return fd;
        }
        struct timespec ts {
            0, 50'000'000
        };
        nanosleep(&ts, nullptr);
    }
    std::perror("connect");
    ::close(fd);
    return -1;
}

// ---- rr (headline) ---------------------------------------------------------------------

int run_rr_server(const bench::Args& a) {
    const int fd = listen_accept(a);
    if (fd < 0) {
        return 1;
    }
    set_nodelay(fd);
    timeval tv{static_cast<time_t>(a.duration_s + 30), 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const std::size_t msg = static_cast<std::size_t>(a.msg_size);
    std::vector<std::byte> buf(4 + msg);
    for (;;) {
        if (!read_full(fd, buf.data(), 4)) {
            break;
        }
        std::uint32_t len = 0;
        std::memcpy(&len, buf.data(), 4);
        if (len == 0 || len > msg || !read_full(fd, buf.data() + 4, len)) {
            break;
        }
        std::uint64_t ts = 0, seq = 0;
        bench::read_msg(std::span<const std::byte>(buf.data() + 4, len), ts, seq);
        if (seq == bench::kEndSeq) {
            break;
        }
        if (!write_full(fd, buf.data(), 4 + len)) { // echo frame verbatim
            break;
        }
    }
    ::close(fd);
    std::fprintf(stderr, "tcp rr-server: done\n");
    return 0;
}

int run_rr_client(const bench::Args& a) {
    const int fd = connect_client(a);
    if (fd < 0) {
        return 1;
    }
    timeval tv{20, 0}; // generous per-reply cap; reliable stream should never hit it
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const std::size_t msg = static_cast<std::size_t>(a.msg_size);
    std::vector<std::byte> frame(4 + msg);
    const std::uint32_t len_le = static_cast<std::uint32_t>(msg);
    std::memcpy(frame.data(), &len_le, 4);
    auto payload = std::span<std::byte>(frame.data() + 4, msg);
    std::vector<std::byte> reply(4 + msg);

    std::vector<double> samples;
    samples.reserve(1u << 18);
    std::uint64_t sent = 0;
    const std::uint64_t start = bench::now_ns();
    const std::uint64_t dur_ns = static_cast<std::uint64_t>(a.duration_s) * 1'000'000'000ULL;

    while (bench::now_ns() - start < dur_ns) {
        bench::write_msg(payload, bench::now_ns(), sent);
        if (!write_full(fd, frame.data(), frame.size())) {
            break;
        }
        ++sent;
        std::uint32_t rlen = 0;
        if (!read_full(fd, reply.data(), 4)) {
            break;
        }
        std::memcpy(&rlen, reply.data(), 4);
        if (rlen == 0 || rlen > msg || !read_full(fd, reply.data() + 4, rlen)) {
            break;
        }
        std::uint64_t ts = 0, seq = 0;
        bench::read_msg(std::span<const std::byte>(reply.data() + 4, rlen), ts, seq);
        samples.push_back(static_cast<double>(bench::now_ns() - ts) / 1e6);
    }
    // END sentinel.
    bench::write_msg(payload, bench::now_ns(), bench::kEndSeq);
    write_full(fd, frame.data(), frame.size());
    struct timespec ts {
        0, 200'000'000
    };
    nanosleep(&ts, nullptr);
    ::close(fd);

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
    std::fprintf(stderr, "tcp rr[loss%.0f%%]: n=%zu p50=%.2f p99=%.2f p999=%.2f max=%.2f ms\n",
                 a.loss_pct, p.n, p.p50, p.p99, p.p999, p.max);
    return 0;
}

// ---- open-loop latency / throughput ----------------------------------------------------

int run_receiver(const bench::Args& a) {
    const int fd = listen_accept(a);
    if (fd < 0) {
        return 1;
    }
    set_nodelay(fd);
    timeval tv{0, 200'000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    const std::size_t msg = static_cast<std::size_t>(a.msg_size);
    std::vector<std::byte> buf(msg);
    std::vector<double> samples;
    samples.reserve(1u << 20);
    std::uint64_t received = 0, max_seq = 0, goodput_bytes = 0;
    std::uint64_t first_recv_ns = 0, last_recv_ns = 0;
    std::uint64_t last_activity_ns = bench::now_ns();
    const std::uint64_t idle_ns = static_cast<std::uint64_t>(a.idle_ms) * 1'000'000ULL;
    const std::uint64_t hard_cap_ns =
        bench::now_ns() + static_cast<std::uint64_t>(a.duration_s + 60) * 1'000'000'000ULL;
    bool done = false;
    while (!done) {
        std::uint32_t len = 0;
        if (!read_full(fd, reinterpret_cast<std::byte*>(&len), 4)) {
            const std::uint64_t n = bench::now_ns();
            if ((received > 0 && n - last_activity_ns > idle_ns) || n > hard_cap_ns) {
                break;
            }
            continue; // recv timeout on a healthy stream: keep waiting
        }
        if (len == 0 || len > msg || !read_full(fd, buf.data(), len)) {
            break;
        }
        std::uint64_t ts = 0, seq = 0;
        bench::read_msg(std::span<const std::byte>(buf.data(), len), ts, seq);
        const std::uint64_t recv = bench::now_ns();
        last_activity_ns = recv;
        if (seq == bench::kEndSeq) {
            done = true;
            continue;
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
    }
    ::close(fd);

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
        std::fprintf(stderr, "tcp recv[thru]: %llu msgs, %.2f Mbit/s over %.2fs\n",
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
                 "tcp recv[lat loss%.0f%%]: offered=%llu recv=%llu p50=%.2f p99=%.2f p999=%.2f "
                 "max=%.2f ms\n",
                 a.loss_pct, static_cast<unsigned long long>(offered),
                 static_cast<unsigned long long>(received), p.p50, p.p99, p.p999, p.max);
    return 0;
}

int run_sender(const bench::Args& a) {
    const int fd = connect_client(a);
    if (fd < 0) {
        return 1;
    }
    const std::size_t msg = static_cast<std::size_t>(a.msg_size);
    std::vector<std::byte> frame(4 + msg);
    const std::uint32_t len_le = static_cast<std::uint32_t>(msg);
    std::memcpy(frame.data(), &len_le, 4);
    auto payload = std::span<std::byte>(frame.data() + 4, msg);

    std::uint64_t seq = 0, sent = 0;
    const std::uint64_t start = bench::now_ns();
    if (a.mode == "throughput") {
        const std::uint64_t end =
            start + static_cast<std::uint64_t>(a.duration_s) * 1'000'000'000ULL;
        while (bench::now_ns() < end) {
            bench::write_msg(payload, bench::now_ns(), seq);
            if (!write_full(fd, frame.data(), frame.size())) {
                break;
            }
            ++seq;
            ++sent;
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
            // Saturation wall-cap (see latency_bench): bound a wedged run; un-offered tail
            // counts against delivery ratio.
            if (bench::now_ns() - start > dur_ns + 15'000'000'000ULL) {
                break;
            }
            bench::wait_until_ns(intended, [] {});
            bench::write_msg(payload, intended, seq); // stamp INTENDED time (coord-omission fix)
            if (!write_full(fd, frame.data(), frame.size())) {
                break; // saturated + peer gone; sustained-load saturation shows as low delivery
            }
            ++seq;
            ++sent;
        }
    }
    bench::write_msg(payload, bench::now_ns(), bench::kEndSeq);
    write_full(fd, frame.data(), frame.size());
    struct timespec ts {
        0, 300'000'000
    };
    nanosleep(&ts, nullptr);
    ::close(fd);

    if (!a.send_out.empty()) {
        std::string row = bench::key_row(a);
        row += "," + std::to_string(sent) + "," + std::to_string(sent) + "," +
               std::to_string(sent * msg);
        bench::append_csv(a.send_out, bench::key_header() + ",sent,tx_datagrams,tx_payload_bytes",
                          row);
    }
    std::fprintf(stderr, "tcp send[loss%.0f%%]: sent=%llu\n", a.loss_pct,
                 static_cast<unsigned long long>(sent));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE,
                SIG_IGN); // writing to a peer-closed socket must fail the write, not kill us
    const bench::Args a = bench::parse_args(argc, argv);
    if (a.mode == "rr") {
        return a.role == "receiver" ? run_rr_server(a) : run_rr_client(a);
    }
    return a.role == "receiver" ? run_receiver(a) : run_sender(a);
}
