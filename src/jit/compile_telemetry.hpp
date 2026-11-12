#pragma once

// Opt-in compile-path telemetry. No counters or clocks are updated unless
// XYO_JIT_COMPILE_STATS=1. These metrics never change Scratch execution.
#include <array>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <cstdlib>

namespace xyo {

// Compile-time rejections and speculative-entry guard outcomes only.
// These events are NOT runtime deoptimizations (which occur in Sequencer).
enum class JitRefusalReason : uint8_t {
    EdgeActivatedHat,
    InvalidSource,
    SyncBackendFailure,
    NumericRejectedCached,
    NumericSourceInvalid,
    NumericNotLowered,
    NumericLoopShape,
    NumericSlotGuard,
    NumericBackendFailure,
    NumericAttachFailure,
    Count
};

struct JitCompileTelemetry {
    using Clock = std::chrono::steady_clock;

    struct Sample {
        std::atomic<uint64_t> attempts{0};
        std::atomic<uint64_t> successes{0};
        std::atomic<uint64_t> elapsedNs{0};

        void record(bool succeeded, Clock::time_point started) noexcept {
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::nanoseconds>(Clock::now() - started).count();
            attempts.fetch_add(1, std::memory_order_relaxed);
            if (succeeded)
                successes.fetch_add(1, std::memory_order_relaxed);
            elapsedNs.fetch_add(static_cast<uint64_t>(elapsed),
                                std::memory_order_relaxed);
        }
    };

    Sample syncCompile;  // backend compile(), excludes source capture
    Sample asyncPrepare; // prepareAsync on the runtime thread
    Sample asyncFinish;  // finish() on worker or synchronous prefetch
    std::atomic<uint64_t> compileCacheHits{0};
    std::atomic<uint64_t> failedCacheHits{0};
    std::atomic<uint64_t> asyncBackpressure{0};
    std::atomic<uint64_t> asyncPublished{0};
    std::atomic<uint64_t> asyncStaleDiscarded{0};
    std::atomic<uint64_t> asyncFailed{0};
    std::array<std::atomic<uint64_t>,
               static_cast<size_t>(JitRefusalReason::Count)> refusals{};

    void noteRefusal(JitRefusalReason reason) noexcept {
        refusals[static_cast<size_t>(reason)].fetch_add(
            1, std::memory_order_relaxed);
    }

    static bool enabled() noexcept {
        static const bool on = [] {
            const char* value = std::getenv("XYO_JIT_COMPILE_STATS");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        return on;
    }

    void report() const noexcept {
        if (!enabled()) return;
        printSample("sync", syncCompile);
        printSample("prepare", asyncPrepare);
        printSample("finish", asyncFinish);
        std::fprintf(stderr,
            "[jit-compile] lookup cache_hit=%llu failed_hit=%llu queue_full=%llu\n",
            value(compileCacheHits), value(failedCacheHits),
            value(asyncBackpressure));
        std::fprintf(stderr,
            "[jit-compile] publish accepted=%llu stale=%llu failed=%llu\n",
            value(asyncPublished), value(asyncStaleDiscarded),
            value(asyncFailed));
        constexpr const char* kReasons[] = {
            "edge_activated_hat", "invalid_source", "sync_backend_failure",
            "numeric_rejected_cached", "numeric_source_invalid",
            "numeric_not_lowered", "numeric_loop_shape", "numeric_slot_guard",
            "numeric_backend_failure", "numeric_attach_failure"
        };
        static_assert(std::size(kReasons) ==
                      static_cast<size_t>(JitRefusalReason::Count));
        for (size_t i = 0; i < std::size(kReasons); ++i) {
            const auto count = refusals[i].load(std::memory_order_relaxed);
            if (count != 0)
                std::fprintf(stderr, "[jit-refusal] reason=%s count=%llu\n",
                             kReasons[i], static_cast<unsigned long long>(count));
        }
    }

private:
    static unsigned long long value(const std::atomic<uint64_t>& counter) noexcept {
        return static_cast<unsigned long long>(
            counter.load(std::memory_order_relaxed));
    }

    static void printSample(const char* label, const Sample& sample) noexcept {
        const auto nanoseconds = sample.elapsedNs.load(std::memory_order_relaxed);
        std::fprintf(stderr,
            "[jit-compile] %s attempts=%llu success=%llu total_ms=%.3f\n",
            label, value(sample.attempts), value(sample.successes),
            static_cast<double>(nanoseconds) / 1'000'000.0);
    }
};

} // namespace xyo
