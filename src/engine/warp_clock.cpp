#include "warp_clock.hpp"
#include "../core/value.hpp"

#include <algorithm>
#include <limits>

namespace {
int64_t unixMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

namespace xyo {

WarpClockWatchdog::WarpClockWatchdog(bool sampleWallClock)
    : sampleWallClock_(sampleWallClock) {
    // All state members must finish initialization before run() can access
    // them. Launching worker_ from its initializer races against subsequent
    // default member initializers (TSan: stopped_/active_/generation_).
    worker_ = std::thread([this] { run(); });
}

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
    if (sampleWallClock_) {
        const auto ms = unixMilliseconds();
        sampledUnixMs.store(ms, std::memory_order_relaxed);
        sampledUnixDaysBits.store(
            Value::num((double(ms) - 946684800000.0) / 86400000.0).bits,
            std::memory_order_relaxed);
    }
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
        // No extra wakeups in danger mode unless a wall-clock sample is used.
        if (unlimited_ && !sampleWallClock_) {
            cv_.wait(lock, [&] {
                return stopped_ || !active_ || generation_ != generation;
            });
            continue;
        }

        // With wall-clock reporting enabled, continue sampling after the
        // deadline expires. Otherwise retain the original watchdog behavior.
        const auto nextSample = Clock::now() + std::chrono::milliseconds(1);
        const auto wakeAt = (sampleWallClock_ && (unlimited_ || deadline_ <= Clock::now()))
            ? nextSample : std::min(deadline_, nextSample);
        const bool interrupted = cv_.wait_until(lock, wakeAt, [&] {
            return stopped_ || !active_ || generation_ != generation;
        });
        if (interrupted || !active_ || generation_ != generation)
            continue;

        const auto now = Clock::now();
        sampledUs.store(toMicroseconds(now), std::memory_order_relaxed);
        if (sampleWallClock_) {
            const auto ms = unixMilliseconds();
            sampledUnixMs.store(ms, std::memory_order_relaxed);
            sampledUnixDaysBits.store(
                Value::num((double(ms) - 946684800000.0) / 86400000.0).bits,
                std::memory_order_relaxed);
        } else if (now >= deadline_) {
            cv_.wait(lock, [&] {
                return stopped_ || !active_ || generation_ != generation;
            });
        }
    }
}

} // namespace xyo
