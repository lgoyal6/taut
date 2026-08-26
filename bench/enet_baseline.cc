// ENet baseline for the latency matrix (§7). Same 512 B workload over an ENet reliable channel
// (channel 0, ENET_PACKET_FLAG_RELIABLE). ENet is the closest existing peer to taut - reliable
// messages over UDP with its own ARQ and RTT-based retransmit - so it is the more honest "did
// you actually beat a real library" comparison than TCP alone. One ENet packet == one message
// (no length prefix). Modes: rr (headline round-trip probe), latency (open-loop Poisson),
// throughput (clean-link goodput).
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "bench/common.h"

namespace {

void read_pkt(const ENetPacket* pkt, std::uint64_t& ts, std::uint64_t& seq) {
    bench::read_msg(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(pkt->data), pkt->dataLength),
        ts, seq);
}

void send_bytes(ENetPeer* peer, const std::vector<std::byte>& buf, std::uint64_t ts,
                std::uint64_t seq) {
    std::vector<std::byte> tmp = buf;
    bench::write_msg(tmp, ts, seq);
    ENetPacket* pkt = enet_packet_create(tmp.data(), tmp.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pkt);
}

ENetHost* make_server(const bench::Args& a) {
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = static_cast<enet_uint16>(a.port);
    return enet_host_create(&address, 1, 1, 0, 0);
}

ENetPeer* connect_client(ENetHost* host, const bench::Args& a) {
    ENetAddress address{};
    enet_address_set_host(&address, a.addr.c_str());
    address.port = static_cast<enet_uint16>(a.port);
    ENetPeer* peer = enet_host_connect(host, &address, 1, 0);
    if (peer == nullptr) {
        return nullptr;
    }
    ENetEvent ev;
    for (int attempt = 0; attempt < 50; ++attempt) {
        while (enet_host_service(host, &ev, 100) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                return peer;
            }
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            }
        }
    }
    return nullptr;
}

// ---- rr (headline) ---------------------------------------------------------------------

int run_rr_server(const bench::Args& a) {
    ENetHost* host = make_server(a);
    if (host == nullptr) {
        std::fprintf(stderr, "enet rr-server: host_create failed\n");
        return 1;
    }
    bool done = false;
    std::uint64_t last_activity_ns = bench::now_ns();
    const std::uint64_t idle_ns = static_cast<std::uint64_t>(a.idle_ms) * 1'000'000ULL;
    const std::uint64_t hard_cap_ns =
        bench::now_ns() + static_cast<std::uint64_t>(a.duration_s + 120) * 1'000'000'000ULL;
    while (!done) {
        ENetEvent ev;
        while (enet_host_service(host, &ev, 100) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                last_activity_ns = bench::now_ns();
                std::uint64_t ts = 0, seq = 0;
                read_pkt(ev.packet, ts, seq);
                if (seq == bench::kEndSeq) {
                    done = true;
                } else {
                    // Echo verbatim: reuse the received bytes in a fresh reliable packet.
                    ENetPacket* out = enet_packet_create(ev.packet->data, ev.packet->dataLength,
                                                         ENET_PACKET_FLAG_RELIABLE);
                    enet_peer_send(ev.peer, 0, out);
                }
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                done = true;
            }
            if (done) {
                break;
            }
        }
        const std::uint64_t n = bench::now_ns();
        if (done || n - last_activity_ns > idle_ns + 2'000'000'000ULL || n > hard_cap_ns) {
            break;
        }
    }
    enet_host_destroy(host);
    std::fprintf(stderr, "enet rr-server: done\n");
    return 0;
}

int run_rr_client(const bench::Args& a) {
    ENetHost* host = enet_host_create(nullptr, 1, 1, 0, 0);
    if (host == nullptr) {
        return 1;
    }
    ENetPeer* peer = connect_client(host, a);
    if (peer == nullptr) {
        std::fprintf(stderr, "enet rr-client: connect timeout\n");
        return 1;
    }
    const std::size_t msg = static_cast<std::size_t>(a.msg_size);
    std::vector<std::byte> buf(msg);
    std::vector<double> samples;
    samples.reserve(1u << 18);
    std::uint64_t sent = 0;
    const std::uint64_t start = bench::now_ns();
    const std::uint64_t dur_ns = static_cast<std::uint64_t>(a.duration_s) * 1'000'000'000ULL;

    while (bench::now_ns() - start < dur_ns) {
        send_bytes(peer, buf, bench::now_ns(), sent);
        ++sent;
        bool got = false;
        const std::uint64_t cap = bench::now_ns() + 15'000'000'000ULL;
        while (!got && bench::now_ns() < cap) {
            ENetEvent ev;
            while (enet_host_service(host, &ev, 1) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    std::uint64_t ts = 0, seq = 0;
                    read_pkt(ev.packet, ts, seq);
                    samples.push_back(static_cast<double>(bench::now_ns() - ts) / 1e6);
                    enet_packet_destroy(ev.packet);
                    got = true;
                } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                    got = true;
                }
                if (got) {
                    break;
                }
            }
        }
    }
    send_bytes(peer, buf, bench::now_ns(), bench::kEndSeq);
    const std::uint64_t flush = bench::now_ns() + 500'000'000ULL;
    while (bench::now_ns() < flush) {
        ENetEvent ev;
        while (enet_host_service(host, &ev, 1) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(ev.packet);
            }
        }
    }
    enet_peer_disconnect(peer, 0);
    for (int i = 0; i < 100; ++i) {
        ENetEvent ev;
        enet_host_service(host, &ev, 1);
        struct timespec ts {
            0, 1'000'000
        };
        nanosleep(&ts, nullptr);
    }
    enet_host_destroy(host);

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
    std::fprintf(stderr, "enet rr[loss%.0f%%]: n=%zu p50=%.2f p99=%.2f p999=%.2f max=%.2f ms\n",
                 a.loss_pct, p.n, p.p50, p.p99, p.p999, p.max);
    return 0;
}

// ---- open-loop latency / throughput ----------------------------------------------------

int run_receiver(const bench::Args& a) {
    ENetHost* host = make_server(a);
    if (host == nullptr) {
        std::fprintf(stderr, "enet receiver: host_create failed\n");
        return 1;
    }
    const std::size_t msg = static_cast<std::size_t>(a.msg_size);
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
        ENetEvent ev;
        while (enet_host_service(host, &ev, 100) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                if (ev.packet->dataLength >= bench::kMsgHeader) {
                    std::uint64_t ts = 0, seq = 0;
                    read_pkt(ev.packet, ts, seq);
                    const std::uint64_t recv = bench::now_ns();
                    last_activity_ns = recv;
                    if (seq == bench::kEndSeq) {
                        done = true;
                    } else {
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
                }
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                done = true;
            }
            if (done) {
                break;
            }
        }
        const std::uint64_t n = bench::now_ns();
        if (done || (received > 0 && n - last_activity_ns > idle_ns) || n > hard_cap_ns) {
            break;
        }
    }
    enet_host_destroy(host);

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
        std::fprintf(stderr, "enet recv[thru]: %llu msgs, %.2f Mbit/s over %.2fs\n",
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
                 "enet recv[lat loss%.0f%%]: offered=%llu recv=%llu p50=%.2f p99=%.2f p999=%.2f "
                 "max=%.2f ms\n",
                 a.loss_pct, static_cast<unsigned long long>(offered),
                 static_cast<unsigned long long>(received), p.p50, p.p99, p.p999, p.max);
    return 0;
}

int run_sender(const bench::Args& a) {
    ENetHost* host = enet_host_create(nullptr, 1, 1, 0, 0);
    if (host == nullptr) {
        return 1;
    }
    ENetPeer* peer = connect_client(host, a);
    if (peer == nullptr) {
        std::fprintf(stderr, "enet sender: connect timeout\n");
        return 1;
    }
    auto pump = [&] {
        ENetEvent e;
        while (enet_host_service(host, &e, 0) > 0) {
            if (e.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(e.packet);
            }
        }
    };
    const std::size_t msg = static_cast<std::size_t>(a.msg_size);
    std::vector<std::byte> buf(msg);
    std::uint64_t seq = 0, sent = 0;
    const std::uint64_t start = bench::now_ns();

    if (a.mode == "throughput") {
        const std::uint64_t end =
            start + static_cast<std::uint64_t>(a.duration_s) * 1'000'000'000ULL;
        while (bench::now_ns() < end) {
            send_bytes(peer, buf, bench::now_ns(), seq);
            ++seq;
            ++sent;
            pump();
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
            bench::wait_until_ns(intended, pump);
            send_bytes(peer, buf, intended, seq); // stamp INTENDED time (coord-omission fix)
            ++seq;
            ++sent;
        }
    }
    for (int i = 0; i < 200; ++i) {
        pump();
        struct timespec ts {
            0, 1'000'000
        };
        nanosleep(&ts, nullptr);
    }
    send_bytes(peer, buf, bench::now_ns(), bench::kEndSeq);
    const std::uint64_t flush = bench::now_ns() + 500'000'000ULL;
    while (bench::now_ns() < flush) {
        pump();
        struct timespec ts {
            0, 1'000'000
        };
        nanosleep(&ts, nullptr);
    }
    enet_peer_disconnect(peer, 0);
    for (int i = 0; i < 100; ++i) {
        pump();
        struct timespec ts {
            0, 1'000'000
        };
        nanosleep(&ts, nullptr);
    }
    enet_host_destroy(host);

    if (!a.send_out.empty()) {
        std::string row = bench::key_row(a);
        row += "," + std::to_string(sent) + "," + std::to_string(sent) + "," +
               std::to_string(sent * msg);
        bench::append_csv(a.send_out, bench::key_header() + ",sent,tx_datagrams,tx_payload_bytes",
                          row);
    }
    std::fprintf(stderr, "enet send[loss%.0f%%]: sent=%llu\n", a.loss_pct,
                 static_cast<unsigned long long>(sent));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }
    const bench::Args a = bench::parse_args(argc, argv);
    int rc = 0;
    if (a.mode == "rr") {
        rc = a.role == "receiver" ? run_rr_server(a) : run_rr_client(a);
    } else {
        rc = a.role == "receiver" ? run_receiver(a) : run_sender(a);
    }
    enet_deinitialize();
    return rc;
}
