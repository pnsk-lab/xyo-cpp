#pragma once
// PROF-003: opt-in runtime deoptimization outcome counters.
// Only called after the authoritative OSR/RT checks have selected an outcome.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace xyo {
enum class JitDeoptPath : uint8_t {
    NativeHandoff, NumericCfg, NestedOuter, NestedInner, NestedTriple, Count
};
enum class JitDeoptReason : uint8_t {
    SourceChanged, InvalidState, InvalidBindings, BackendFailure,
    InvalidContinuation, CommitConflict, UnspecifiedNumeric,
    HandoffRefused, UnsafeResume, ExplicitHandoff, Count
};
inline JitDeoptReason numericDeoptReason(const char* name) noexcept {
    if (!name) return JitDeoptReason::UnspecifiedNumeric;
    if (std::strcmp(name, "source-changed") == 0)
        return JitDeoptReason::SourceChanged;
    if (std::strcmp(name, "invalid-state") == 0)
        return JitDeoptReason::InvalidState;
    if (std::strcmp(name, "invalid-bindings") == 0)
        return JitDeoptReason::InvalidBindings;
    if (std::strcmp(name, "backend-failure") == 0)
        return JitDeoptReason::BackendFailure;
    if (std::strcmp(name, "invalid-continuation") == 0)
        return JitDeoptReason::InvalidContinuation;
    if (std::strcmp(name, "commit-conflict") == 0)
        return JitDeoptReason::CommitConflict;
    return JitDeoptReason::UnspecifiedNumeric;
}

struct JitDeoptTelemetry {
    static constexpr size_t kPaths = static_cast<size_t>(JitDeoptPath::Count);
    static constexpr size_t kReasons = static_cast<size_t>(JitDeoptReason::Count);
    struct Counts {
        std::atomic<uint64_t> attempts{0};
        std::atomic<uint64_t> resumed{0};
        std::atomic<uint64_t> refused{0};
    };
    std::array<Counts, kPaths> paths{};
    std::array<std::atomic<uint64_t>, kReasons> reasons{};

    static bool enabled() noexcept {
        static const bool on = [] {
            const char* s = std::getenv("XYO_JIT_DEOPT_STATS");
            return s && s[0] == '1' && s[1] == '\0';
        }();
        return on;
    }
    static JitDeoptTelemetry& get() noexcept {
        static JitDeoptTelemetry result;
        return result;
    }
    void record(JitDeoptPath path, JitDeoptReason reason, bool resumed) noexcept {
        auto& c = paths[static_cast<size_t>(path)];
        c.attempts.fetch_add(1, std::memory_order_relaxed);
        (resumed ? c.resumed : c.refused).fetch_add(1, std::memory_order_relaxed);
        reasons[static_cast<size_t>(reason)].fetch_add(1,
            std::memory_order_relaxed);
    }
    ~JitDeoptTelemetry() {
        constexpr const char* names[] = {"native_handoff", "numeric_cfg",
            "nested_outer", "nested_inner", "nested_triple"};
        constexpr const char* reasonNames[] = {"source_changed", "invalid_state",
            "invalid_bindings", "backend_failure", "invalid_continuation",
            "commit_conflict", "unspecified_numeric", "handoff_refused",
            "unsafe_resume", "explicit_handoff"};
        static_assert(std::size(names) == kPaths);
        static_assert(std::size(reasonNames) == kReasons);
        for (size_t i = 0; i < kPaths; ++i) {
            const auto n = paths[i].attempts.load(std::memory_order_relaxed);
            if (!n) continue;
            const auto yes = paths[i].resumed.load(std::memory_order_relaxed);
            const auto no = paths[i].refused.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                "[jit-deopt] path=%s attempts=%llu resumed=%llu refused=%llu rate=%.2f%%\n",
                names[i], (unsigned long long)n, (unsigned long long)yes,
                (unsigned long long)no, double(yes) * 100.0 / double(n));
        }
        for (size_t i = 0; i < kReasons; ++i) {
            const auto n = reasons[i].load(std::memory_order_relaxed);
            if (n)
                std::fprintf(stderr, "[jit-deopt] reason=%s count=%llu\n",
                    reasonNames[i], (unsigned long long)n);
        }
    }
};
inline void noteJitDeopt(JitDeoptPath path, JitDeoptReason reason,
                         bool resumed) noexcept {
    if (JitDeoptTelemetry::enabled())
        JitDeoptTelemetry::get().record(path, reason, resumed);
}
} // namespace xyo
