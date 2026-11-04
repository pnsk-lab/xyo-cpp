#include "warp_clock.hpp"

#include <algorithm>
#include <limits>

namespace xyo {

WarpClockWatchdog::WarpClockWatchdog() : worker_([this] { run(); }) {}

WarpClockWatchdog::~WarpClockWatchdog() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        ++generation_;
    }
    cv_.notify_one();
    worker_.join();
}

int64_t WarpClockWatchdog::toMicroseconds(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        time.time_since_epoch()).count();
}

void WarpClockWatchdog::arm(double deadlineMs, bool unlimited) {
    std::lock_guard lock(mutex_);
    unlimited_ = unlimited;
    deadline_ = unlimited
        ? Clock::time_point::max()
        : Clock::time_point(std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(deadlineMs)));
    deadlineUs.store(
        unlimited ? std::numeric_limits<int64_t>::max()
                  : toMicroseconds(deadline_),
        std::memory_order_release);
    sampledUs.store(toMicroseconds(Clock::now()), std::memory_order_relaxed);
    active_ = true;
    armed_.store(true, std::memory_order_release);
    ++generation_;
    cv_.notify_one();
}

void WarpClockWatchdog::disarm() {
    std::lock_guard lock(mutex_);
    active_ = false;
    armed_.store(false, std::memory_order_release);
    ++generation_;
    cv_.notify_one();
}

void WarpClockWatchdog::run() {
    std::unique_lock lock(mutex_);
    while (!stopped_) {
        if (!active_) {
            cv_.wait(lock, [&] { return stopped_ || active_; });
            continue;
        }

        const uint64_t generation = generation_;
        // Danger mode does not need periodic time samples: the JIT deadline
        // is +infinity and cannot expire.
        if (unlimited_) {
            cv_.wait(lock, [&] {
                return stopped_ || !active_ || generation_ != generation;
            });
            continue;
        }

        const auto wakeAt = std::min(
            deadline_, Clock::now() + std::chrono::milliseconds(1));
        const bool interrupted = cv_.wait_until(lock, wakeAt, [&] {
            return stopped_ || !active_ || generation_ != generation;
        });
        if (interrupted || !active_ || generation_ != generation)
            continue;

        const auto now = Clock::now();
        sampledUs.store(toMicroseconds(now), std::memory_order_relaxed);

        // Once expired, keep the timestamp updated no longer; the JIT's
        // monotonic comparison stays true until the next arm().
        if (now >= deadline_) {
            cv_.wait(lock, [&] {
                return stopped_ || !active_ || generation_ != generation;
            });
        }
    }
}

} // namespace xyo
