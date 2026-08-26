// send_file - transfer a file to recv_file over taut class 2 (ReliableOrdered).
//
// The file is read into memory, chunked into <= kChunk-byte messages, and streamed over a
// single Session. Message 0 is an 8-byte little-endian length header (see demo/file_xfer.h);
// messages 1..N are the file. We fill the send window, then poll()/tick() the Session until
// every message is acknowledged (in_flight() == 0), which - with cumulative acks - means the
// receiver has delivered the whole file in order. sha256 of the sent bytes is printed so it
// can be compared with recv_file's.
//
// Usage: send_file --bind IP:PORT --peer IP:PORT --in PATH [--timeout-ms N]
#include <algorithm>
#include <array>
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
    std::string in_path;
    int timeout_ms = 120000; // generous: RTO-only recovery under loss is slow
};

bool parse_args(int argc, char** argv, Args& a) {
    bool have_bind = false, have_peer = false, have_in = false;
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
        } else if (flag == "--in") {
            const char* v = next();
            if (!v) {
                return false;
            }
            a.in_path = v;
            have_in = true;
        } else if (flag == "--timeout-ms") {
            const char* v = next();
            if (!v) {
                return false;
            }
            a.timeout_ms = std::atoi(v);
        } else {
            std::fprintf(stderr, "send_file: unknown arg '%.*s'\n", static_cast<int>(flag.size()),
                         flag.data());
            return false;
        }
    }
    return have_bind && have_peer && have_in;
}

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return {};
    }
    const std::streamsize n = f.tellg();
    f.seekg(0);
    std::vector<std::byte> data(static_cast<std::size_t>(n));
    if (n > 0) {
        f.read(reinterpret_cast<char*>(data.data()), n);
    }
    return data;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        std::fprintf(stderr,
                     "usage: send_file --bind IP:PORT --peer IP:PORT --in PATH [--timeout-ms N]\n");
        return 1;
    }

    const std::vector<std::byte> data = read_file(args.in_path);
    {
        std::ifstream probe(args.in_path, std::ios::binary);
        if (!probe) {
            std::fprintf(stderr, "send_file: cannot open '%s'\n", args.in_path.c_str());
            return 1;
        }
    }
    std::printf("send_file: sha256=%s bytes=%zu\n", demo::Sha256::of(data).c_str(), data.size());

    // 8-byte little-endian length header (message 0 of the stream).
    std::array<std::byte, demo::kHeaderLen> header{};
    const std::uint64_t len = data.size();
    for (std::size_t i = 0; i < demo::kHeaderLen; ++i) {
        header[i] = static_cast<std::byte>((len >> (8 * i)) & 0xFF);
    }

    taut::RealUdpTransport tx;
    if (!tx.bind(args.bind.ip, args.bind.port)) {
        std::fprintf(stderr, "send_file: bind failed on %s:%u\n", args.bind.ip.c_str(),
                     args.bind.port);
        return 1;
    }
    const auto peer = taut::make_endpoint(args.peer.ip, args.peer.port);
    if (!peer) {
        std::fprintf(stderr, "send_file: bad peer address %s\n", args.peer.ip.c_str());
        return 1;
    }

    taut::Config cfg;
    taut::Session session(tx, *peer, cfg);

    bool header_sent = false;
    std::size_t off = 0;
    const auto feed = [&] {
        if (!header_sent) {
            if (!session.send(taut::Class::ReliableOrdered, header)) {
                return; // window full
            }
            header_sent = true;
        }
        while (off < data.size()) {
            const std::size_t n = std::min(demo::kChunk, data.size() - off);
            if (!session.send(taut::Class::ReliableOrdered,
                              std::span<const std::byte>(data.data() + off, n))) {
                return; // window full - drain acks and retry next iteration
            }
            off += n;
        }
    };

    const auto start = tx.now();
    const auto deadline = start + std::chrono::milliseconds(args.timeout_ms);
    while (true) {
        feed();
        session.poll();
        session.tick();
        if (header_sent && off == data.size() && session.in_flight() == 0) {
            break; // everything acknowledged
        }
        if (tx.now() > deadline) {
            std::fprintf(stderr, "send_file: TIMEOUT after %d ms (in_flight=%zu, sent=%zu/%zu)\n",
                         args.timeout_ms, session.in_flight(), off, data.size());
            return 3;
        }
        demo::wait_readable(tx.fd(), 5);
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(tx.now() - start).count();
    std::printf("send_file: done, %zu bytes in %lld ms\n", data.size(),
                static_cast<long long>(elapsed));
    return 0;
}
