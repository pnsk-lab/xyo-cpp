// Sequencer-scoped monotonic clock for JIT warp polling.
// One worker samples steady_clock every ~1ms while a tick is active.
// JIT reads only the atomic timestamp and deadline, without a clock call.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace xyo {

class WarpClockWatchdog {
public:
    // Separate cache lines: a writer updating sampledUs must not continually
    // invalidate the read-mostly deadline cache line.
    alignas(64) std::atomic<int64_t> sampledUs{0};
    alignas(64) std::atomic<int64_t> deadlineUs{0};
    // system_clock Unix milliseconds for Scratch's days since 2000.
    // Separate cache line to avoid invalidating the warp polling clock.
    alignas(64) std::atomic<int64_t> sampledUnixMs{0};
    // The same ms sample converted to Scratch's NaN-boxed days value once.
    // Native JIT can load these bits without div/convert/boxing each iteration.
    alignas(64) std::atomic<uint64_t> sampledUnixDaysBits{0};

    explicit WarpClockWatchdog(bool sampleWallClock = false);
    ~WarpClockWatchdog();

    WarpClockWatchdog(const WarpClockWatchdog&) = delete;
    WarpClockWatchdog& operator=(const WarpClockWatchdog&) = delete;

    /// Time values use steady_clock's epoch, in milliseconds / microseconds.
    /// Calling arm again cancels the previous tick's scheduled updates.
    void arm(double deadlineMs, bool unlimited);
    void disarm();
    bool armed() const { return armed_.load(std::memory_order_acquire); }
    bool samplesWallClock() const { return sampleWallClock_; }

private:
    using Clock = std::chrono::steady_clock;

    static int64_t toMicroseconds(Clock::time_point time);
    void run();

    const bool sampleWallClock_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> armed_{false};
    bool stopped_ = false;
    bool active_ = false;
    uint64_t generation_ = 0;
    Clock::time_point deadline_ = Clock::time_point::max();
    bool unlimited_ = false;
    // Keep the thread handle last: no worker may observe state members
    // before their initialization. The constructor starts it in its body.
    std::thread worker_;
};

} // namespace xyo
