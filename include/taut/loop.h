#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "taut/transport.h"

namespace taut {

// Single-threaded, level-triggered epoll event loop (§5.7). This is the Week 1 S3
// skeleton: it drives the UDP socket and an eventfd for wakeup/shutdown; the timerfd slot
// for the timer heap is added in week 2. Per tick: epoll_wait -> drain socket -> dispatch.
class EventLoop {
  public:
    using DatagramHandler = std::function<void(const Endpoint&, std::span<const std::byte>)>;

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // True if epoll/eventfd were created successfully.
    bool valid() const {
        return epfd_ >= 0 && eventfd_ >= 0;
    }

    // Register a transport's fd (level-triggered) with a per-datagram handler.
    bool add_transport(UdpTransport& transport, DatagramHandler on_datagram);

    // Wake epoll_wait from another thread / signal handler (writes the eventfd).
    void wake();

    // Ask run() to return after the current tick.
    void stop();

    // Block, processing events until stop().
    void run();

    // One epoll_wait tick. timeout_ms < 0 blocks indefinitely.
    void run_once(int timeout_ms);

  private:
    struct Registration {
        int fd;
        UdpTransport* transport;
        DatagramHandler handler;
    };

    int epfd_ = -1;
    int eventfd_ = -1;
    bool running_ = false;
    std::vector<Registration> regs_;
};

} // namespace taut
