// Runs matched native TCP and Taut workloads using bench/common.h framing.
// Fixed-cadence offers and bounded admission expose rejected or missing requests.
// Raw timestamps feed analyze_paced_study.py; bulk uses the same echo contract.
#include "bench/common.h"
#include "bench/resource.h"
#include "taut/session.h"
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <linux/tcp.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr std::size_t kLimit = 64, kSize = 512;
using Bytes = std::vector<std::byte>;
using Receive = std::function<void(std::span<const std::byte>)>;
void nap() {
    timespec ts{0, 100'000};
    nanosleep(&ts, nullptr);
}
struct RetransmissionStats {
    std::uint64_t packets = 0, bytes = 0;
    const char* scope = "unavailable";
    bool available = false;
};
struct Channel {
    virtual ~Channel() = default;
    virtual bool send(std::span<const std::byte>) = 0;
    virtual void poll(const Receive&) = 0;
    virtual RetransmissionStats retransmissions() const = 0;
};
struct Udp final : Channel {
    taut::RealUdpTransport tx;
    std::unique_ptr<taut::Session> session;
    Udp(const bench::Args& a, bool paced) {
        if (!tx.bind(a.bind_addr,
                     static_cast<std::uint16_t>(a.port + (a.role == "sender" ? 1 : 0))))
            throw std::runtime_error("UDP bind failed");
        auto peer = taut::make_endpoint(
            a.addr, static_cast<std::uint16_t>(a.port + (a.role == "sender" ? 0 : 1)));
        if (!peer)
            throw std::runtime_error("bad UDP address");
        taut::Config cfg;
        cfg.congestion =
            paced ? taut::CongestionMode::PacedAimd : taut::CongestionMode::FixedWindow;
        session = std::make_unique<taut::Session>(tx, *peer, cfg);
    }
    bool send(std::span<const std::byte> b) override {
        return session->send(taut::Class::ReliableOrdered, b);
    }
    void poll(const Receive& receive) override {
        session->on_message([&](taut::Class, taut::ByteSpan b) { receive(b); });
        session->poll();
        session->tick();
        session->on_message({});
    }
    RetransmissionStats retransmissions() const override {
        return {session->retransmits(), session->retransmitted_bytes(), "udp_payload_accepted",
                true};
    }
};
struct Tcp final : Channel {
    int fd = -1;
    std::deque<Bytes> output;
    std::size_t offset = 0, got = 0;
    std::array<std::byte, kSize + 4> input{};
    explicit Tcp(const bench::Args& a) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::runtime_error("TCP socket failed");
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(a.port));
        addr.sin_addr.s_addr = inet_addr((a.role == "receiver" ? a.bind_addr : a.addr).c_str());
        if (a.role == "receiver") {
            if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) || ::listen(fd, 1))
                throw std::runtime_error("TCP listen failed");
            const int accepted = ::accept(fd, nullptr, nullptr);
            ::close(fd);
            fd = accepted;
            if (fd < 0)
                throw std::runtime_error("TCP accept failed");
        } else if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr)) {
            throw std::runtime_error("TCP connect failed");
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
            throw std::runtime_error("TCP nonblock failed");
    }
    RetransmissionStats retransmissions() const override {
        tcp_info info{};
        socklen_t size = sizeof info;
        if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &size) != 0 ||
            size < offsetof(tcp_info, tcpi_bytes_retrans) + sizeof info.tcpi_bytes_retrans)
            return {};
        return {info.tcpi_total_retrans, info.tcpi_bytes_retrans, "tcp_stream_kernel", true};
    }
    ~Tcp() override {
        if (fd >= 0)
            ::close(fd);
    }
    bool send(std::span<const std::byte> b) override {
        if (output.size() == kLimit)
            return false;
        Bytes frame(kSize + 4);
        const std::uint32_t n = kSize;
        std::memcpy(frame.data(), &n, 4);
        std::copy(b.begin(), b.end(), frame.begin() + 4);
        output.push_back(std::move(frame));
        return true;
    }
    void poll(const Receive& receive) override {
        for (std::size_t i = 0; i < kLimit && !output.empty(); ++i) {
            auto& b = output.front();
            const auto n = ::send(fd, b.data() + offset, b.size() - offset, MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (n <= 0)
                throw std::runtime_error("TCP write failed");
            offset += static_cast<std::size_t>(n);
            if (offset == b.size()) {
                output.pop_front();
                offset = 0;
            }
        }
        for (std::size_t i = 0; i < kLimit; ++i) {
            const auto n = ::recv(fd, input.data() + got, input.size() - got, 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (n == 0)
                break;
            if (n < 0)
                throw std::runtime_error("TCP read failed");
            got += static_cast<std::size_t>(n);
            if (got == input.size()) {
                std::uint32_t length = 0;
                std::memcpy(&length, input.data(), 4);
                if (length != kSize)
                    throw std::runtime_error("invalid TCP frame length");
                receive(std::span<const std::byte>(input.data() + 4, kSize));
                got = 0;
            }
        }
    }
};
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    try {
        auto a = bench::parse_args(argc, argv);
        std::uint64_t start = 0;
        int drain_s = 15;
        bool paced = false;
        for (int i = 1; i + 1 < argc; ++i) {
            std::string k = argv[i];
            if (k == "--start-ns")
                start = std::stoull(argv[i + 1]);
            if (k == "--drain")
                drain_s = std::stoi(argv[i + 1]);
            if (k == "--cc")
                paced = std::string(argv[i + 1]) == "paced";
        }
        if (a.rate <= 0 || a.rate > 10000 || a.duration_s <= 0 || a.duration_s > 30 ||
            drain_s < 0 || drain_s > 30)
            throw std::runtime_error("rate/duration/drain outside bounded protocol");
        const bool bulk = a.mode == "bulk";
        const auto count = static_cast<std::uint64_t>(a.rate * a.duration_s);
        if (count > 100000)
            throw std::runtime_error("study offer limit exceeded");
        const auto interval = static_cast<std::uint64_t>(1e9 / a.rate);
        std::unique_ptr<Channel> ch;
        if (a.transport == "tcp")
            ch = std::make_unique<Tcp>(a);
        else if (a.transport == "taut")
            ch = std::make_unique<Udp>(a, paced);
        else
            throw std::runtime_error("unsupported native transport");
        if (!start)
            start = bench::now_ns() + 200'000'000ULL;
        const auto end = start + static_cast<std::uint64_t>(a.duration_s) * 1'000'000'000ULL;
        const auto cap = end + static_cast<std::uint64_t>(drain_s) * 1'000'000'000ULL;
        std::uint64_t offered = 0, accepted = 0, completed = 0, in_window = 0, rejected = 0,
                      errors = 0, late = 0, peak = 0;
        std::map<std::uint64_t, std::uint64_t> pending;
        std::vector<double> samples;
        std::ofstream offers_file, samples_file;
        if (a.role == "sender") {
            offers_file.open(a.out + ".offers.csv");
            samples_file.open(a.out + ".samples.csv");
            if (!offers_file || !samples_file)
                throw std::runtime_error("cannot open raw observation files");
            offers_file << "seq,intended_ns,observed_ns,accepted\n";
            samples_file << "seq,intended_ns,completed_ns\n";
        }
        std::deque<Bytes> echoes;
        bool server_done = false;
        Receive receive = [&](std::span<const std::byte> b) {
            if (b.size() != kSize) {
                ++errors;
                return;
            }
            if (a.role == "receiver") {
                std::uint64_t ts = 0, seq = 0;
                bench::read_msg(b, ts, seq);
                if (seq == bench::kEndSeq) {
                    server_done = true;
                    return;
                }
                if (echoes.size() == kLimit) {
                    ++errors;
                    return;
                }
                echoes.emplace_back(b.begin(), b.end());
                return;
            }
            std::uint64_t ts = 0, seq = 0;
            bench::read_msg(b, ts, seq);
            auto it = pending.find(seq);
            Bytes expected(kSize);
            bench::write_msg(expected, ts, seq);
            if (it == pending.end() || it->second != ts ||
                !std::equal(expected.begin(), expected.end(), b.begin())) {
                ++errors;
                return;
            }
            const auto now = bench::now_ns();
            if (now >= cap)
                return; // retain this request as missing at the frozen boundary
            samples.push_back(static_cast<double>(now - ts) / 1e6);
            samples_file << seq << "," << ts << "," << now << "\n";
            pending.erase(it);
            ++completed;
            if (now < end)
                ++in_window;
        };
        std::fprintf(stderr, "ready transport=%s role=%s start=%llu\n", a.transport.c_str(),
                     a.role.c_str(), static_cast<unsigned long long>(start));
        while (bench::now_ns() < cap) {
            if (bulk && offered >= 100000) {
                ++errors;
                break;
            }
            ch->poll(receive);
            while (!echoes.empty() && ch->send(echoes.front()))
                echoes.pop_front();
            auto now = bench::now_ns();
            if (server_done)
                break;
            if (a.role == "sender") {
                while ((!bulk && offered < count && start + offered * interval <= now) ||
                       (bulk && now >= start && now < end && pending.size() < kLimit)) {
                    const auto seq = offered++, intended = bulk ? now : start + seq * interval;
                    if (now > intended + 1'000'000ULL)
                        ++late;
                    Bytes b(kSize);
                    bench::write_msg(b, intended, seq);
                    // Do not inject an overdue burst after host scheduling stalls.
                    const bool accept = now < end && now <= intended + interval &&
                                        pending.size() < kLimit && ch->send(b);
                    offers_file << seq << "," << intended << "," << now << "," << accept << "\n";
                    if (!accept)
                        ++rejected;
                    else {
                        pending.emplace(seq, intended);
                        ++accepted;
                        peak = std::max<std::uint64_t>(peak, pending.size());
                    }
                    now = bench::now_ns();
                    if (bulk && !accept)
                        break;
                }
                if ((bulk || offered == count) && pending.empty() && now >= end) {
                    Bytes sentinel(kSize);
                    bench::write_msg(sentinel, now, bench::kEndSeq);
                    if (ch->send(sentinel)) {
                        const auto flush = bench::now_ns() + 300'000'000ULL;
                        while (bench::now_ns() < flush) {
                            ch->poll(receive);
                            nap();
                        }
                        break;
                    }
                }
            }
            nap();
        }
        if (a.role == "receiver") {
            auto r = bench::sample_resources();
            const auto retransmissions = ch->retransmissions();
            std::fprintf(stderr,
                         "server errors=%llu cpu_s=%.6f retransmits=%llu retx_bytes=%llu "
                         "retx_available=%d retx_scope=%s\n",
                         static_cast<unsigned long long>(errors), r.cpu_s,
                         static_cast<unsigned long long>(retransmissions.packets),
                         static_cast<unsigned long long>(retransmissions.bytes),
                         retransmissions.available, retransmissions.scope);
            return errors ? 1 : 0;
        }
        errors += pending.size();
        const auto p = bench::percentiles(samples);
        const auto r = bench::sample_resources();
        const auto retransmissions = ch->retransmissions();
        const auto label = a.transport == "tcp" ? "tcp" : (paced ? "taut-paced" : "taut-fixed");
        const std::string header =
            "transport,offered,accepted,rejected,completed,completed_in_window,missing,errors,late_"
            "offers,peak_inflight,p50_ms,p99_ms,max_ms,cpu_s,retransmits,goodput_mbps,"
            "retransmitted_bytes,retransmission_scope";
        const std::string row =
            std::string(label) + "," + std::to_string(offered) + "," + std::to_string(accepted) +
            "," + std::to_string(rejected) + "," + std::to_string(completed) + "," +
            std::to_string(in_window) + "," + std::to_string(pending.size()) + "," +
            std::to_string(errors) + "," + std::to_string(late) + "," + std::to_string(peak) + "," +
            std::to_string(p.p50) + "," + std::to_string(p.p99) + "," + std::to_string(p.max) +
            "," + std::to_string(r.cpu_s) + "," + std::to_string(retransmissions.packets) + "," +
            std::to_string(static_cast<double>(in_window * kSize * 8) / a.duration_s / 1e6) + "," +
            (retransmissions.available ? std::to_string(retransmissions.bytes) : "") + "," +
            retransmissions.scope;
        bench::append_csv(a.out, header, row);
        std::fprintf(stderr, "%s\n", row.c_str());
        return errors || offered != accepted + rejected || accepted != completed ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "paced_bench: %s\n", e.what());
        return 1;
    }
}
