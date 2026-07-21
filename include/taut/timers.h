#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace taut {

using TimerId = std::uint64_t;

// Hand-built binary min-heap of deadlines with lazy deletion (§5.7). `cancel` just records
// the id as dead; the heap entry is discarded when it surfaces at the root (so cancel is
// O(1) and we never pay to locate the entry). Used by the ARQ to track per-packet RTO
// deadlines and by the flow-control persist timer.
class TimerHeap {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;

    // Schedule a timer to fire at `deadline`; returns an id usable with cancel().
    TimerId schedule(TimePoint deadline);

    // Mark a timer dead. Safe to call with an unknown/already-fired id (no-op).
    void cancel(TimerId id);

    // Earliest live deadline, pruning any cancelled entries at the root. nullopt if none.
    std::optional<TimePoint> next_deadline();

    // If the earliest live timer is due at or before `now`, remove and return its id.
    std::optional<TimerId> pop_due(TimePoint now);

    // True if no live timers remain (prunes cancelled entries at the root).
    bool empty();

  private:
    struct Entry {
        TimePoint deadline;
        TimerId id;
    };

    void sift_up(std::size_t i);
    void sift_down(std::size_t i);
    void remove_root();
    void prune_root(); // drop cancelled entries sitting at the root

    std::vector<Entry> heap_;
    std::unordered_set<TimerId> cancelled_;
    TimerId next_id_ = 1;
};

} // namespace taut
