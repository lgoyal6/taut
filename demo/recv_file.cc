// recv_file - receive a file from send_file over taut class 2 (ReliableOrdered).
//
// The first class-2 message is an 8-byte little-endian length header; the rest are the file,
// delivered in order (class 2 guarantees exactly-once, in-order delivery), so we simply
// concatenate them until we have `length` bytes. sha256 of the received bytes is printed for
// comparison with send_file's.
//
// Last-ack-loss handling: once the file is complete we do NOT exit immediately. We linger,
// still answering the sender's retransmissions with acks, so that a dropped final ack (which
// would otherwise make the sender retransmit into a closed socket and time out) is re-acked.
// Without this the 20x checkpoint would flake under loss.
//
// Usage: recv_file --bind IP:PORT --peer IP:PORT --out PATH [--timeout-ms N] [--linger-ms N]
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "file_xfer.h"
#include "sha256.h"
#include "taut/config.h"
#include "taut/session.h"
#include "taut/transport.h"
#include "taut/types.h"

namespace {

struct Args {
    demo::HostPort bind;
    demo::HostPort peer;
    std::string out_path;
    int timeout_ms = 120000; // no-progress deadline for the whole transfer
    int linger_ms = 2000;    // keep acking retransmits after completion
};

bool parse_args(int argc, char** argv, Args& a) {
    bool have_bind = false, have_peer = false, have_out = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (flag == "--bind") {
            const char* v = next();
            auto hp = v ? demo::parse_hostport(v) : std::nullopt;
            if (!hp) {
                return false;
            }
            a.bind = *hp;
            have_bind = true;
        } else if (flag == "--peer") {
            const char* v = next();
            auto hp = v ? demo::parse_hostport(v) : std::nullopt;
            if (!hp) {
                return false;
            }
            a.peer = *hp;
            have_peer = true;
        } else if (flag == "--out") {
            const char* v = next();
            if (!v) {
                return false;
            }
            a.out_path = v;
            have_out = true;
        } else if (flag == "--timeout-ms") {
            const char* v = next();
            if (!v) {
                return false;
            }
            a.timeout_ms = std::atoi(v);
        } else if (flag == "--linger-ms") {
            const char* v = next();
            if (!v) {
                return false;
            }
            a.linger_ms = std::atoi(v);
        } else {
            std::fprintf(stderr, "recv_file: unknown arg '%.*s'\n", static_cast<int>(flag.size()),
                         flag.data());
            return false;
        }
    }
    return have_bind && have_peer && have_out;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        std::fprintf(stderr, "usage: recv_file --bind IP:PORT --peer IP:PORT --out PATH "
                             "[--timeout-ms N] [--linger-ms N]\n");
        return 1;
    }

    taut::RealUdpTransport tx;
    if (!tx.bind(args.bind.ip, args.bind.port)) {
        std::fprintf(stderr, "recv_file: bind failed on %s:%u\n", args.bind.ip.c_str(),
                     args.bind.port);
        return 1;
    }
    const auto peer = taut::make_endpoint(args.peer.ip, args.peer.port);
    if (!peer) {
        std::fprintf(stderr, "recv_file: bad peer address %s\n", args.peer.ip.c_str());
        return 1;
    }

    taut::Config cfg;
    taut::Session session(tx, *peer, cfg);

    bool have_len = false;
    std::uint64_t expected = 0;
    std::vector<std::byte> body;
    session.on_message([&](taut::Class, taut::ByteSpan p) {
        if (!have_len) {
            std::uint64_t v = 0;
            for (std::size_t i = 0; i < demo::kHeaderLen && i < p.size(); ++i) {
                v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
            }
            expected = v;
            have_len = true;
            body.reserve(static_cast<std::size_t>(expected));
        } else {
            body.insert(body.end(), p.begin(), p.end());
        }
    });

    const auto complete = [&] { return have_len && body.size() >= expected; };

    const auto start = tx.now();
    const auto deadline = start + std::chrono::milliseconds(args.timeout_ms);
    while (!complete()) {
        session.poll();
        session.tick();
        if (complete()) {
            break;
        }
        if (tx.now() > deadline) {
            std::fprintf(stderr, "recv_file: TIMEOUT after %d ms (have_len=%d, got=%zu/%llu)\n",
                         args.timeout_ms, static_cast<int>(have_len), body.size(),
                         static_cast<unsigned long long>(expected));
            return 3;
        }
        demo::wait_readable(tx.fd(), 5);
    }

    // Linger: keep servicing retransmits so a lost final ack still gets re-acked.
    const auto linger_until = tx.now() + std::chrono::milliseconds(args.linger_ms);
    while (tx.now() < linger_until) {
        session.poll();
        session.tick();
        demo::wait_readable(tx.fd(), 5);
    }

    body.resize(static_cast<std::size_t>(expected)); // trim any overshoot from a partial chunk
    std::ofstream out(args.out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "recv_file: cannot open '%s' for writing\n", args.out_path.c_str());
        return 1;
    }
    if (!body.empty()) {
        out.write(reinterpret_cast<const char*>(body.data()),
                  static_cast<std::streamsize>(body.size()));
    }
    out.close();

    std::printf("recv_file: sha256=%s bytes=%zu\n", demo::Sha256::of(body).c_str(), body.size());
    return 0;
}
