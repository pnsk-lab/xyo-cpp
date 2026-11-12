#pragma once

// PROF-004: opt-in, native-only counters for entry into selected LLVM
// fallback basic blocks. These are NOT deoptimizations or side exits.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace xyo {

enum class JitGuardSlowKind : uint32_t {
    NumericProfile,
    TruthyProfile,
    NumericList,
    NumericEquality,
    PlainComparison,
    Induction,
    TypedSsaCacheMiss,
    Count,
};

struct JitGuardSlowStats final {
    static constexpr size_t kCount =
        static_cast<size_t>(JitGuardSlowKind::Count);
    std::array<std::atomic<uint64_t>, kCount> sites{};
    std::array<std::atomic<uint64_t>, kCount> hits{};

    static bool enabled() noexcept {
        static const bool on = [] {
            const char* v = std::getenv("XYO_JIT_GUARD_STATS");
            return v && v[0] == '1' && v[1] == '\0';
        }();
        return on;
    }

    static JitGuardSlowStats& get() noexcept {
        static JitGuardSlowStats instance;
        return instance;
    }

    void noteSite(JitGuardSlowKind kind) noexcept {
        sites[static_cast<size_t>(kind)].fetch_add(
            1, std::memory_order_relaxed);
    }

    void noteHit(uint32_t kind) noexcept {
        if (kind >= kCount) return;
        hits[kind].fetch_add(1, std::memory_order_relaxed);
    }

    ~JitGuardSlowStats() {
        constexpr const char* kNames[] = {
            "numeric_profile", "truthy_profile", "numeric_list",
            "numeric_equality", "plain_comparison", "induction",
            "typed_ssa_cache_miss"
        };
        static_assert(std::size(kNames) == kCount);
        for (size_t i = 0; i < kCount; ++i) {
            const auto n = sites[i].load(std::memory_order_relaxed);
            if (!n) continue;
            const auto calls = hits[i].load(std::memory_order_relaxed);
            std::fprintf(stderr, "[jit-guard] kind=%s sites=%llu hits=%llu\n",
                kNames[i], static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(calls));
        }
    }
};

} // namespace xyo
