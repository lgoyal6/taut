// A telemetry uplink on taut: push a run of small readings across a 20%-loss
// link and prove every one of them arrives exactly once, in order, without the
// application ever seeing the retransmits.
//
// This is the shape taut is actually for - small messages, lossy link, tail
// latency that matters - and it exercises the API an outside consumer needs:
// the send window returning false under backpressure, the tick()/poll() split
// that makes RTO recovery happen at all, and the difference between a class 0
// datagram (may vanish) and a class 2 one (may not).
//
// It runs on SimNet, the in-process simulator on a virtual clock, so it needs
// no sockets and gives the same answer on every platform. Same seed, same run.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <taut/session.h>
#include <taut/sim_net.h>
#include <taut/types.h>

using namespace std::chrono_literals;

namespace {

constexpr int kReadings = 500;

taut::ByteSpan as_bytes(const std::string& s) {
    return taut::ByteSpan(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

std::string to_string(taut::ByteSpan p) {
    return std::string(reinterpret_cast<const char*>(p.data()), p.size());
}

} // namespace

int main(int argc, char** argv) {
    const std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 7;

    // 20% loss in both directions: enough that a transport without ARQ loses
    // roughly one reading in five, and enough that acks get lost too.
    taut::SimNet net(seed, taut::Impairments{.loss = 0.20, .delay = 5ms, .jitter = 3ms});

    const taut::Endpoint sensor{1, 1};
    const taut::Endpoint collector{2, 2};

    taut::Config cfg; // rto_floor 25ms, window 64 packets - the defaults are the thesis
    taut::Session uplink(net.endpoint(sensor), collector, cfg);
    taut::Session sink(net.endpoint(collector), sensor, cfg);

    std::vector<std::string> received;
    int heartbeats = 0;
    sink.on_message([&](taut::Class cls, taut::ByteSpan payload) {
        if (cls == taut::Class::Unreliable) {
            ++heartbeats;
        } else {
            received.push_back(to_string(payload));
        }
    });

    // One pump step. tick() fires the RTO retransmits; poll() only drains what
    // is readable, so a loop that calls poll() alone never recovers a loss.
    auto pump = [&] {
        net.advance(2ms);
        uplink.tick();
        sink.tick();
        uplink.poll();
        sink.poll();
    };

    // Send every reading on class 2 (ReliableOrdered). send() returns false when
    // the window is full, which is backpressure and not an error: drain and retry
    // rather than dropping the reading on the floor.
    int sent = 0;
    int backpressure_stalls = 0;
    int heartbeats_offered = 0;
    for (int i = 0; i < kReadings; ++i) {
        const std::string reading = "reading-" + std::to_string(i);
        while (!uplink.send(taut::Class::ReliableOrdered, as_bytes(reading))) {
            ++backpressure_stalls;
            pump();
        }
        ++sent;

        // Every 50th reading also goes out as a class 0 heartbeat. Class 0 is
        // fire-and-forget: some of these are expected to vanish, and that is the
        // point of having the classes at all.
        if (i % 50 == 0) {
            ++heartbeats_offered;
            uplink.send(taut::Class::Unreliable, as_bytes("heartbeat"));
        }
    }

    // Drain. Bounded so a protocol bug fails the example instead of hanging it.
    for (int step = 0; step < 20000 && static_cast<int>(received.size()) < kReadings; ++step) {
        pump();
    }

    bool ordered = true;
    for (std::size_t i = 0; i < received.size(); ++i) {
        if (received[i] != "reading-" + std::to_string(i)) {
            ordered = false;
            break;
        }
    }

    std::printf("sent %d readings on class 2 across a 20%% loss link\n", sent);
    std::printf("delivered %zu, in order: %s, duplicates: %s\n", received.size(),
                ordered ? "yes" : "NO",
                received.size() == static_cast<std::size_t>(kReadings) ? "none" : "COUNT MISMATCH");
    std::printf("retransmits the application never saw: %u\n", uplink.retransmits());
    std::printf("backpressure stalls (send window full): %d\n", backpressure_stalls);
    std::printf("class 0 heartbeats: %d offered, %d arrived (loss here is allowed)\n",
                heartbeats_offered, heartbeats);

    const bool ok = ordered && received.size() == static_cast<std::size_t>(kReadings) &&
                    uplink.retransmits() > 0 && heartbeats <= heartbeats_offered;
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}
