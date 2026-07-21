// Integration test for the epoll event loop + real UDP transport (Linux). Runs entirely
// in one process — a client transport and a server transport on loopback — so it exercises
// epoll_wait, socket drain, and dispatch deterministically, with no cross-process race.
#include "taut/loop.h"
#include "taut/transport.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include "taut/codec.h" // kMaxDatagram

namespace {

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

} // namespace

TEST(EventLoop, EchoesDatagramThroughEpoll) {
    taut::RealUdpTransport server;
    ASSERT_TRUE(server.bind("127.0.0.1", 0));
    const auto server_addr = server.local_endpoint();
    ASSERT_TRUE(server_addr.has_value());
    ASSERT_NE(server_addr->port_be, 0);

    taut::EventLoop loop;
    ASSERT_TRUE(loop.valid());

    int handled = 0;
    ASSERT_TRUE(loop.add_transport(
        server, [&server, &handled](const taut::Endpoint& from, std::span<const std::byte> data) {
            server.send(from, data); // echo back to sender
            ++handled;
        }));

    taut::RealUdpTransport client;
    ASSERT_TRUE(client.bind("127.0.0.1", 0));

    const std::string_view msg = "hello-loop";
    ASSERT_GT(client.send(*server_addr, as_bytes(msg)), 0u);

    // One tick: the queued datagram is readable, gets drained and echoed.
    loop.run_once(500);
    EXPECT_EQ(handled, 1);

    // The echo should come back to the client (loopback is fast; be lenient).
    std::array<std::byte, taut::kMaxDatagram> buf{};
    std::optional<taut::RecvResult> got;
    for (int i = 0; i < 200 && !got; ++i) {
        got = client.recv(buf);
        if (!got) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->size, msg.size());
    EXPECT_EQ(std::memcmp(buf.data(), msg.data(), got->size), 0);
}
