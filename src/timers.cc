#include "taut/timers.h"

#include <utility>

namespace taut {

TimerId TimerHeap::schedule(TimePoint deadline) {
    const TimerId id = next_id_++;
    heap_.push_back(Entry{deadline, id});
    sift_up(heap_.size() - 1);
    return id;
}

void TimerHeap::cancel(TimerId id) {
    cancelled_.insert(id);
}

std::optional<TimerHeap::TimePoint> TimerHeap::next_deadline() {
    prune_root();
    if (heap_.empty()) {
        return std::nullopt;
    }
    return heap_.front().deadline;
}

std::optional<TimerId> TimerHeap::pop_due(TimePoint now) {
    prune_root();
    if (heap_.empty() || heap_.front().deadline > now) {
        return std::nullopt;
    }
    const TimerId id = heap_.front().id;
    remove_root();
    return id;
}

bool TimerHeap::empty() {
    prune_root();
    return heap_.empty();
}

void TimerHeap::prune_root() {
    while (!heap_.empty()) {
        const auto it = cancelled_.find(heap_.front().id);
        if (it == cancelled_.end()) {
            return;
        }
        cancelled_.erase(it);
        remove_root();
    }
}

void TimerHeap::remove_root() {
    heap_.front() = heap_.back();
    heap_.pop_back();
    if (!heap_.empty()) {
        sift_down(0);
    }
}

void TimerHeap::sift_up(std::size_t i) {
    while (i > 0) {
        const std::size_t parent = (i - 1) / 2;
        if (heap_[i].deadline < heap_[parent].deadline) {
            std::swap(heap_[i], heap_[parent]);
            i = parent;
        } else {
            break;
        }
    }
}

void TimerHeap::sift_down(std::size_t i) {
    const std::size_t n = heap_.size();
    while (true) {
        const std::size_t left = 2 * i + 1;
        const std::size_t right = 2 * i + 2;
        std::size_t smallest = i;
        if (left < n && heap_[left].deadline < heap_[smallest].deadline) {
            smallest = left;
        }
        if (right < n && heap_[right].deadline < heap_[smallest].deadline) {
            smallest = right;
        }
        if (smallest == i) {
            break;
        }
        std::swap(heap_[i], heap_[smallest]);
        i = smallest;
    }
}

} // namespace taut
