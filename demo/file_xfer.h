// Shared plumbing for the send_file / recv_file demos: "IP:PORT" parsing and an
// efficient fd wait so the drive loop sleeps between events instead of busy-spinning.
// Demo-scoped (not library code).
#pragma once

#include <poll.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "taut/transport.h"

namespace demo {

// Header framing: message 0 of the class-2 stream is the u64 little-endian file length;
// messages 1..N are the file split into <= kChunk-byte pieces. The receiver reconstructs
// the file by concatenating the data messages until it has `length` bytes.
inline constexpr std::size_t kHeaderLen = 8;
inline constexpr std::size_t kChunk = 1024; // <= kMaxDatagram - kBaseHeaderSize (1179)

struct HostPort {
    std::string ip;
    std::uint16_t port;
};

// Parse "10.9.0.2:7001" into {"10.9.0.2", 7001}. nullopt on malformed input.
inline std::optional<HostPort> parse_hostport(std::string_view s) {
    const auto colon = s.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= s.size()) {
        return std::nullopt;
    }
    const long port = std::atol(std::string(s.substr(colon + 1)).c_str());
    if (port <= 0 || port > 65535) {
        return std::nullopt;
    }
    return HostPort{std::string(s.substr(0, colon)), static_cast<std::uint16_t>(port)};
}

// Block until `fd` is readable or `timeout_ms` elapses. A short timeout bounds how long
// the loop can sleep before it next fires retransmit timers via Session::tick().
inline void wait_readable(int fd, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    ::poll(&pfd, 1, timeout_ms);
}

} // namespace demo
