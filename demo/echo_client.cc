// Echo client (Week 1 S3 skeleton): sends N datagrams to the echo server and counts how
// many come back within a deadline. Exits 0 iff all N were echoed.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <thread>

#include "taut/codec.h" // kMaxDatagram
#include "taut/transport.h"

int main(int argc, char** argv) {
    std::uint16_t port = 7000;
    int count = 10;
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        count = std::atoi(argv[2]);
    }

    taut::RealUdpTransport transport;
    if (!transport.bind("0.0.0.0", 0)) { // ephemeral port
        std::fprintf(stderr, "echo_client: bind failed\n");
        return 1;
    }
    const auto server = taut::make_endpoint("127.0.0.1", port);
    if (!server) {
        std::fprintf(stderr, "echo_client: bad server address\n");
        return 1;
    }

    for (int i = 0; i < count; ++i) {
        std::array<char, 32> msg{};
        const int len = std::snprintf(msg.data(), msg.size(), "ping-%d", i);
        transport.send(*server,
                       std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg.data()),
                                                  static_cast<std::size_t>(len)));
    }

    int echoed = 0;
    std::array<std::byte, taut::kMaxDatagram> buf{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (echoed < count && std::chrono::steady_clock::now() < deadline) {
        if (transport.recv(buf)) {
            ++echoed;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::printf("echo_client: echoed %d/%d\n", echoed, count);
    return echoed == count ? 0 : 2;
}
