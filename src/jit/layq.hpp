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
#include <cstdio>
#include <cstdlib>
#include <utility>

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
        // Unrecognized values used to fall through silently. Warn once so
        // a typo (e.g. XYO_YIELD_QMODE=OF) does not run an unintended
        // policy for a whole benchmark session.
        std::fprintf(stderr,
                     "[layq-config] ignoring malformed XYO_YIELD_QMODE='%s' "
                     "(want off|adaptive|fixed; using adaptive)\n",
                     v);
        return LayqMode::Adaptive;
    }();
    return mode;
}

inline uint64_t layqEnvU64(const char* name, uint64_t dflt) {
    const char* v = std::getenv(name);
    if (!v || !*v) return dflt;
    return std::strtoull(v, nullptr, 0);
}

// Checked variant: reports whether the variable was present and parsed
// cleanly (whole string consumed by strtoull). LAYQ normalization is
// silent by default; layqConfig() warns once for malformed or
// contradictory input so a typo does not silently change poll cadence.
inline std::pair<uint64_t, bool> layqParseU64(const char* name,
                                              uint64_t dflt) {
    const char* v = std::getenv(name);
    if (!v || !*v) return {dflt, true};
    char* end = nullptr;
    const unsigned long long r = std::strtoull(v, &end, 0);
    if (!end || end == v || *end != '\0') return {dflt, false};
    return {static_cast<uint64_t>(r), true};
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
        // Parse errors fall back to defaults (with a one-time warning
        // below); every other value is rounded down to a power of two
        // and clamped so the resulting quantum is always a valid pow2
        // in [1, 2^62] regardless of input. Contradictory settings are
        // resolved qmin-wins (the floor is the safety-critical bound).
        auto [rawMin, okMin] = layqParseU64("XYO_YIELD_QMIN", 64);
        auto [rawMaxPure, okMax] = layqParseU64("XYO_YIELD_QMAX", 16384);
        auto [rawMaxCall, okCall] = layqParseU64("XYO_YIELD_QMAX_CALL", 256);
        auto [rawFixed, okFixed] = layqParseU64("XYO_YIELD_QFIXED", 64);
        c.qmin = layqRoundDownPow2(rawMin);
        if (c.qmin < 1) c.qmin = 1;
        c.qmaxPure = layqRoundDownPow2(rawMaxPure);
        c.qmaxCall = layqRoundDownPow2(rawMaxCall);
        if (c.qmaxCall < c.qmin) c.qmaxCall = c.qmin;
        if (c.qmaxCall > c.qmaxPure) c.qmaxCall = c.qmaxPure;
        c.qfixed = layqRoundDownPow2(rawFixed);
        if (c.qmaxPure > (1ULL << 62)) c.qmaxPure = 1ULL << 62;
        if (c.qmaxCall > (1ULL << 62)) c.qmaxCall = 1ULL << 62;
        if (c.qfixed > (1ULL << 62)) c.qfixed = 1ULL << 62;
        if (!okMin)
            std::fprintf(stderr,
                         "[layq-config] ignoring malformed XYO_YIELD_QMIN "
                         "(using default 64)\n");
        else if (rawMin >= 2 && c.qmin != rawMin)
            std::fprintf(stderr,
                         "[layq-config] XYO_YIELD_QMIN=%llu is not a power "
                         "of two (using %llu)\n",
                         (unsigned long long)rawMin,
                         (unsigned long long)c.qmin);
        if (!okMax)
            std::fprintf(stderr,
                         "[layq-config] ignoring malformed XYO_YIELD_QMAX "
                         "(using default 16384)\n");
        else if (rawMaxPure >= 2 &&
                 layqRoundDownPow2(rawMaxPure) != rawMaxPure)
            std::fprintf(stderr,
                         "[layq-config] XYO_YIELD_QMAX=%llu is not a power "
                         "of two (using %llu)\n",
                         (unsigned long long)rawMaxPure,
                         (unsigned long long)c.qmaxPure);
        if (!okCall)
            std::fprintf(stderr,
                         "[layq-config] ignoring malformed "
                         "XYO_YIELD_QMAX_CALL (using default 256)\n");
        else if (rawMaxCall >= 2 &&
                 layqRoundDownPow2(rawMaxCall) != rawMaxCall)
            std::fprintf(stderr,
                         "[layq-config] XYO_YIELD_QMAX_CALL=%llu is not a "
                         "power of two (rounded down to %llu)\n",
                         (unsigned long long)rawMaxCall,
                         (unsigned long long)layqRoundDownPow2(rawMaxCall));
        if (!okFixed)
            std::fprintf(stderr,
                         "[layq-config] ignoring malformed XYO_YIELD_QFIXED "
                         "(using default 64)\n");
        else if (rawFixed >= 2 && layqRoundDownPow2(rawFixed) != rawFixed)
            std::fprintf(stderr,
                         "[layq-config] XYO_YIELD_QFIXED=%llu is not a power "
                         "of two (rounded down to %llu)\n",
                         (unsigned long long)rawFixed,
                         (unsigned long long)layqRoundDownPow2(rawFixed));
        if (okMin && okMax && rawMin > 0 && rawMaxPure > 0 &&
            layqRoundDownPow2(rawMin) > layqRoundDownPow2(rawMaxPure))
            std::fprintf(stderr,
                         "[layq-config] XYO_YIELD_QMIN=%llu exceeds "
                         "XYO_YIELD_QMAX=%llu; the floor wins (quantum=%llu)\n",
                         (unsigned long long)rawMin,
                         (unsigned long long)rawMaxPure,
                         (unsigned long long)c.qmin);
        if (okCall && c.qmaxCall != layqRoundDownPow2(rawMaxCall))
            std::fprintf(stderr,
                         "[layq-config] XYO_YIELD_QMAX_CALL=%llu clamped to "
                         "[QMIN,QMAX] (using %llu)\n",
                         (unsigned long long)rawMaxCall,
                         (unsigned long long)c.qmaxCall);
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
