#pragma once

// PROF-008: opt-in, compile-time-only cost counters for CFG-guided scalar
// shadows. When disabled, no singleton, clocks or counters are accessed.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace xyo::analysis {

struct CfgTypeHintTelemetry final {
    using Clock = std::chrono::steady_clock;
    std::atomic<uint64_t> eligible{0};
    std::atomic<uint64_t> skippedProfitability{0};
    std::atomic<uint64_t> analyses{0};
    std::atomic<uint64_t> truncated{0};
    std::atomic<uint64_t> promoted{0};
    std::atomic<uint64_t> analyzedNodes{0};
    std::atomic<uint64_t> analysisNs{0};

    static bool enabled() noexcept {
        static const bool on = [] {
            const char* value = std::getenv("XYO_JIT_CFG_TYPE_STATS");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        return on;
    }

    static CfgTypeHintTelemetry& get() noexcept {
        static CfgTypeHintTelemetry instance;
        return instance;
    }

    void noteEligible(bool skipped) noexcept {
        eligible.fetch_add(1, std::memory_order_relaxed);
        if (skipped)
            skippedProfitability.fetch_add(1, std::memory_order_relaxed);
    }

    void noteAnalysis(Clock::time_point started,
                      std::size_t nodes, bool wasTruncated) noexcept {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started).count();
        analyses.fetch_add(1, std::memory_order_relaxed);
        analyzedNodes.fetch_add(static_cast<uint64_t>(nodes),
                                std::memory_order_relaxed);
        analysisNs.fetch_add(static_cast<uint64_t>(ns),
                             std::memory_order_relaxed);
        if (wasTruncated)
            truncated.fetch_add(1, std::memory_order_relaxed);
    }

    void notePromotion() noexcept {
        promoted.fetch_add(1, std::memory_order_relaxed);
    }

    void report() const noexcept {
        if (!enabled()) return;
        const auto count = [](const std::atomic<uint64_t>& n) {
            return static_cast<unsigned long long>(
                n.load(std::memory_order_relaxed));
        };
        std::fprintf(stderr,
            "[jit-cfg-type] eligible=%llu skipped=%llu analyses=%llu "
            "truncated=%llu promoted=%llu nodes=%llu analysis_ms=%.3f\n",
            count(eligible), count(skippedProfitability), count(analyses),
            count(truncated), count(promoted), count(analyzedNodes),
            static_cast<double>(analysisNs.load(std::memory_order_relaxed))
                / 1'000'000.0);
    }

    ~CfgTypeHintTelemetry() { report(); }
};

} // namespace xyo::analysis
