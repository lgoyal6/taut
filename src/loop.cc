#include "taut/loop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <utility>

#include "taut/codec.h" // kMaxDatagram

namespace taut {

EventLoop::EventLoop() {
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (epfd_ >= 0 && eventfd_ >= 0) {
        epoll_event ev{};
        ev.events = EPOLLIN; // level-triggered (no EPOLLET) - correctness-first (D13)
        ev.data.fd = eventfd_;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, eventfd_, &ev);
    }
}

EventLoop::~EventLoop() {
    if (eventfd_ >= 0) {
        ::close(eventfd_);
    }
    if (epfd_ >= 0) {
        ::close(epfd_);
    }
}

bool EventLoop::add_transport(UdpTransport& transport, DatagramHandler on_datagram) {
    const int fd = transport.fd();
    if (epfd_ < 0 || fd < 0) {
        return false;
    }
    epoll_event ev{};
    ev.events = EPOLLIN; // level-triggered
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
        return false;
    }
    regs_.push_back(Registration{fd, &transport, std::move(on_datagram)});
    return true;
}

void EventLoop::wake() {
    if (eventfd_ < 0) {
        return;
    }
    const std::uint64_t one = 1;
    const ssize_t r = ::write(eventfd_, &one, sizeof(one));
    (void)r;
}

void EventLoop::stop() {
    running_ = false;
    wake();
}

void EventLoop::run() {
    running_ = true;
    while (running_) {
        run_once(-1);
    }
}

namespace {
// Mirrors Config::max_recv_per_poll. The loop has no Config, and level-triggered
// epoll re-reports the socket, so nothing is lost by returning early.
constexpr std::uint32_t kMaxRecvPerTick = 64;
} // namespace

void EventLoop::run_once(int timeout_ms) {
    std::array<epoll_event, 16> evs{};
    const int n = ::epoll_wait(epfd_, evs.data(), static_cast<int>(evs.size()), timeout_ms);
    if (n < 0) {
        return; // EINTR etc.; run() will loop
    }
    std::array<std::byte, kMaxDatagram> buf{};
    for (int i = 0; i < n; ++i) {
        const int fd = evs[static_cast<std::size_t>(i)].data.fd;
        if (fd == eventfd_) {
            std::uint64_t drain = 0;
            const ssize_t r = ::read(eventfd_, &drain, sizeof(drain));
            (void)r;
            continue;
        }
        for (auto& reg : regs_) {
            if (reg.fd != fd) {
                continue;
            }
            // Bounded drain (level-triggered, so anything left is re-reported on the
            // next tick). Unbounded, one busy socket holds run_once() forever and every
            // other registration - and the caller's timer tick - starves behind it.
            for (std::uint32_t drained = 0; drained < kMaxRecvPerTick; ++drained) {
                const auto res = reg.transport->recv(buf);
                if (!res) {
                    break;
                }
                reg.handler(res->from, std::span<const std::byte>(buf.data(), res->size));
            }
        }
    }
}

} // namespace taut
