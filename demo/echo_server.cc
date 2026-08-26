// Minimal echo server over the taut event loop (Week 1 S3 skeleton). Binds a UDP socket
// and echoes every datagram back to its sender. No reliability yet - under loss, drops
// are expected. Ctrl-C to stop.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include "taut/loop.h"
#include "taut/transport.h"

int main(int argc, char** argv) {
    std::uint16_t port = 7000;
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    }

    taut::RealUdpTransport transport;
    if (!transport.bind("0.0.0.0", port)) {
        std::fprintf(stderr, "echo_server: bind failed on port %u\n", port);
        return 1;
    }

    taut::EventLoop loop;
    if (!loop.valid()) {
        std::fprintf(stderr, "echo_server: event loop init failed\n");
        return 1;
    }

    loop.add_transport(transport,
                       [&transport](const taut::Endpoint& from, std::span<const std::byte> data) {
                           transport.send(from, data); // echo it back
                       });

    std::fprintf(stderr, "echo_server: listening on udp/%u\n", port);
    loop.run();
    return 0;
}
