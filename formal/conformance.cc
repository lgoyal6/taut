#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "taut/codec.h"
#include "taut/config.h"
#include "taut/session.h"
#include "taut/transport.h"

namespace {

class ScriptTransport final : public taut::UdpTransport {
  public:
    std::size_t send(const taut::Endpoint&, std::span<const std::byte> data) override {
        return data.size();
    }

    std::optional<taut::RecvResult> recv(std::span<std::byte> out) override {
        if (inbox_.empty()) {
            return std::nullopt;
        }
        const auto bytes = std::move(inbox_.front());
        inbox_.erase(inbox_.begin());
        std::copy(bytes.begin(), bytes.end(), out.begin());
        return taut::RecvResult{bytes.size(), peer_};
    }

    std::chrono::steady_clock::time_point now() const override {
        return {};
    }

    int fd() const override {
        return -1;
    }

    void inject_ack(std::uint32_t cum_ack, std::uint32_t ack_seq, std::uint16_t window) {
        taut::Packet packet{};
        packet.type = taut::PacketType::Ack;
        packet.cls = taut::Class::Unreliable;
        packet.seq = ack_seq;
        packet.cum_ack = cum_ack;
        packet.adv_window = window;
        std::array<std::byte, taut::kBaseHeaderSize> encoded{};
        const std::size_t size = taut::encode(packet, encoded);
        inbox_.emplace_back(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(size));
    }

  private:
    taut::Endpoint peer_{2, 2};
    std::vector<std::vector<std::byte>> inbox_;
};

int run(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        std::cerr << "cannot open trace: " << path << '\n';
        return 2;
    }

    ScriptTransport transport;
    taut::Config config;
    config.window_pkts = 1;
    taut::Session sender(transport, taut::Endpoint{2, 2}, config);
    std::uint16_t expected = 0;
    std::size_t deliveries = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        if (kind == "ack") {
            std::uint32_t cum_ack = 0;
            std::uint32_t ack_seq = 0;
            std::uint16_t window = 0;
            fields >> cum_ack >> ack_seq >> window;
            transport.inject_ack(cum_ack, ack_seq, window);
            sender.poll();
            ++deliveries;
        } else if (kind == "expect_window") {
            fields >> expected;
        } else {
            std::cerr << "unknown trace operation: " << kind << '\n';
            return 2;
        }
    }

    std::cout << "replayed_deliveries=" << deliveries
              << " final_window=" << sender.peer_adv_window() << " expected_window=" << expected
              << '\n';
    if (sender.peer_adv_window() != expected) {
        std::cerr << "conformance failure: stale equal ACK clobbered reopened window\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: taut_formal_conformance formal/counterexample.trace\n";
        return 2;
    }
    return run(argv[1]);
}
