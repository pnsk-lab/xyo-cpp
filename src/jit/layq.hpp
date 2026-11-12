#pragma once
// LAYQ v2 (push model): Load-Adaptive Yield Quantum for warp-loop deadline
// polling.
//
// Today's warp loops check their deadline every 64 iterations
// ((++n & 63) == 0). The check exists to hand off to other threads (or
// notice warp-budget/stop/GC pressure). When no other thread is runnable
// the check is pure overhead, so LAYQ widens the poll quantum when the
// machine is idle and snaps it back when contention appears.
//
// v2 pushes the adaptation into the scheduler: once per pass it publishes
// the recommended pure-class quantum, and only when the value changes.
// JIT poll/entry blocks just load that hint (relaxed; usually an L1-shared
// line) and clamp it to the loop's body class — no competitor math, no
// shifts, no extra atomics, no danger branch in generated code.
//
// Design notes (see docs/layq.md for measurements):
// - Per-thread quantum, always a power of two (keeps the mask form
//   `q - (n & (q-1))`). Re-synced from the ctx field at loop entry;
//   updated at every poll; never computed per iteration.
// - The scheduler publishes Runtime::yieldQuantumHint (its own thread,
//   release store, only on change). JIT loads it relaxed. Staleness is
//   bounded by one scheduler pass + one quantum and only ever costs
//   efficiency, never correctness.
// - Correctness-critical checks (deadline, stop epoch, GC budget) still
//   run at EVERY poll; only their cadence changes. Non-warp loops still
//   yield every iteration (Scratch step semantics, untouched).
// - Body classes bound the worst-case gap: call-free fast regions can
//   prove tiny per-iteration cost (no allocation possible without a
//   call, so no GC trigger mid-quantum), while regions containing opaque
//   calls use a small cap. Iteration counts are never treated as
//   wall-clock bounds; the caps below keep the worst realistic gap at
//   milliseconds against the 500 ms warp budget.
//
// Env (all read once per process, first use):
//   XYO_YIELD_QMODE=off|adaptive|fixed   (default adaptive)
//   XYO_YIELD_QMIN=<pow2>               (default 64; floor under load)
//   XYO_YIELD_QMAX=<pow2>               (default 16384; pure loops)
//   XYO_YIELD_QMAX_CALL=<pow2>          (default 256; loops with calls)
//   XYO_YIELD_QFIXED=<pow2>             (default 64; fixed mode pin)
//   XYO_YIELD_QSTATS=1                  (default off; per-thread counters)
//   XYO_YIELD_QTRACE=1                  (default off; log scheduler publishes)

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace xyo {

// Fallback scheduler-published quantum for JitCtx instances that never go
// through the scheduler attach path (hand-rolled test contexts). Never
// written; reads see the floor quantum, the safe default.
inline std::atomic<uint32_t> layqFallbackQuantum{64};
inline std::atomic<uint64_t> layqFallbackStopEpoch{0};

/// Poll quantum policy modes (XYO_YIELD_QMODE).
enum class LayqMode : int { Off = 0, Adaptive = 1, Fixed = 2 };

inline LayqMode layqMode() {
    static const LayqMode mode = [] {
        const char* v = std::getenv("XYO_YIELD_QMODE");
        if (!v || !*v) return LayqMode::Adaptive;
        if (v[0] == 'o' || v[0] == '0') return LayqMode::Off;
        if (v[0] == 'a' || v[0] == '1') return LayqMode::Adaptive;
        if (v[0] == 'f') return LayqMode::Fixed;
        return LayqMode::Adaptive;
    }();
    return mode;
}

inline uint64_t layqEnvU64(const char* name, uint64_t dflt) {
    const char* v = std::getenv(name);
    if (!v || !*v) return dflt;
    return std::strtoull(v, nullptr, 0);
}

/// Round down to a power of two (0 -> 1). Quantum math needs pow2 masks.
inline uint64_t layqRoundDownPow2(uint64_t v) {
    if (v < 2) return 1;
#if defined(__GNUC__) || defined(__clang__)
    return 1ULL << (63 - __builtin_clzll(v));
#else
    uint64_t p = 1;
    while ((p << 1) <= v) p <<= 1;
    return p;
#endif
}

struct LayqConfig {
    uint64_t qmin = 64;
    uint64_t qmaxPure = 16384;
    uint64_t qmaxCall = 256;
    uint64_t qfixed = 64;
};

inline LayqConfig layqConfig() {
    static const LayqConfig cfg = [] {
        LayqConfig c;
        c.qmin = layqRoundDownPow2(layqEnvU64("XYO_YIELD_QMIN", 64));
        if (c.qmin < 1) c.qmin = 1;
        c.qmaxPure = layqRoundDownPow2(layqEnvU64("XYO_YIELD_QMAX", 16384));
        c.qmaxCall =
            layqRoundDownPow2(layqEnvU64("XYO_YIELD_QMAX_CALL", 256));
        if (c.qmaxCall < c.qmin) c.qmaxCall = c.qmin;
        if (c.qmaxCall > c.qmaxPure) c.qmaxCall = c.qmaxPure;
        c.qfixed = layqRoundDownPow2(layqEnvU64("XYO_YIELD_QFIXED", 64));
        if (c.qmaxPure > (1ULL << 62)) c.qmaxPure = 1ULL << 62;
        if (c.qmaxCall > (1ULL << 62)) c.qmaxCall = 1ULL << 62;
        if (c.qfixed > (1ULL << 62)) c.qfixed = 1ULL << 62;
        return c;
    }();
    return cfg;
}

/// log2(qmax/qmin); both are pow2 with qmax >= qmin (see layqConfig).
inline int layqShiftRange(uint64_t qmax, uint64_t qmin) {
    int shift = 0;
    for (uint64_t v = qmin; v < qmax; v <<= 1) ++shift;
    return shift;
}

/// Target quantum for a given competitor count: halve per competitor,
/// floored at qmin. Pure function — unit tested.
inline uint64_t layqTarget(uint64_t competitors, uint64_t qmax,
                           uint64_t qmin) {
    const int shift = layqShiftRange(qmax, qmin);
    const uint64_t c =
        competitors > (uint64_t)shift ? (uint64_t)shift : competitors;
    const uint64_t t = qmax >> c;
    return t < qmin ? qmin : t;
}

/// Competitors from the scheduler-published runnable estimate (which
/// includes self): saturate at zero. Pure function — unit tested.
inline uint64_t layqCompetitors(uint64_t runnableEstimate) {
    return runnableEstimate == 0 ? 0 : runnableEstimate - 1;
}

inline bool layqStatsEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("XYO_YIELD_QSTATS");
        return v && v[0] == '1';
    }();
    return enabled;
}

} // namespace xyo
