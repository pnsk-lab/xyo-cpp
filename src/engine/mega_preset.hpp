// xyo-cpp: one-switch preset for the source-certified Mega scheduler.
//
// The Mega family is a set of independent opt-in experiments (see
// docs/scheduler-mega-*.md). This preset turns the coherent set on in one
// shot so the system can be exercised without spelling out ~30 variables.
//
// Contract:
//   * `XYO_JIT_MEGA=1` (or `--mega`) enables every compatible Mega feature
//     that the caller has not set explicitly.
//   * `run`/`test` default to the preset (the strongest headless profile);
//     `--no-mega` or an explicit non-Eager JIT mode keeps the old path.
//   * An explicit `XYO_JIT_MEGA_<NAME>=0` always wins; the preset never
//     overwrites a variable that already exists.
//   * `XYO_JIT_MEGA=0` disables the preset.
//   * Mega lowering exists only in the Eager LLVM tier, so the preset also
//     selects `XYO_JIT_MODE=eager` when neither XYO_JIT_MODE nor
//     XYO_JIT_DISABLE is set. An explicit mode still wins.
//
// The preset is applied during static initialization *and* from main() so
// every process (CLI, tests, embedders) sees the same flags before the first
// JIT/scheduler flag read.
#pragma once

#include <cstdlib>
#include <cstring>

namespace xyo {

namespace detail {

inline bool megaPresetTruthy(const char* value) {
    if (!value || !*value) return false;
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
        std::strcmp(value, "false") == 0)
        return false;
    return true;
}

inline bool setMegaEnvIfAbsent(const char* name, const char* value) {
#if defined(_WIN32)
    if (std::getenv(name)) return false;
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 0) == 0;
#endif
}

} // namespace detail

/// True when the environment asks for the Mega preset.
inline bool megaPresetRequested() {
    return detail::megaPresetTruthy(std::getenv("XYO_JIT_MEGA"));
}

/// Request the preset (used by the `--mega` command-line switch).
inline void requestMegaPreset() {
#if defined(_WIN32)
    _putenv_s("XYO_JIT_MEGA", "1");
#else
    setenv("XYO_JIT_MEGA", "1", 1);
#endif
}

/// Disable the preset even when the environment asked for it (`--no-mega`).
inline void disableMegaPreset() {
#if defined(_WIN32)
    _putenv_s("XYO_JIT_MEGA", "0");
#else
    setenv("XYO_JIT_MEGA", "0", 1);
#endif
}

/// Enable every compatible Mega feature that is not already configured.
/// Returns true when the preset is active. Safe to call repeatedly.
inline bool applyMegaPreset() {
    if (!megaPresetRequested()) return false;
    // Keep this list in sync with docs/notes/scheduler/mega/scheduler-mega-complete-2026-11-19.md.
    // Diagnostics flags (TRACE / BULK_STATS / JOIN_PROFILE / ...) are
    // deliberately excluded: the preset changes behaviour, not verbosity.
    static const char* const kFeatures[] = {
        "XYO_JIT_MEGA_BATCH",
        "XYO_JIT_MEGA_BATCH_YIELD",
        "XYO_JIT_MEGA_SINGLE_SENDER",
        "XYO_JIT_MEGA_BCAST_RECEIVERS",
        "XYO_JIT_MEGA_SINGLE_RECEIVER",
        "XYO_JIT_MEGA_SCALAR_BATCH",
        "XYO_JIT_MEGA_DIRECT_SCALAR",
        "XYO_JIT_MEGA_MULTI_PC_BATCH",
        "XYO_JIT_MEGA_SINGLE_DISPATCH",
        "XYO_JIT_MEGA_SINGLE_FAST_SCAN",
        "XYO_JIT_MEGA_SINGLE_PREPROVE",
        "XYO_JIT_MEGA_SINGLE_SPECIALIZE",
        "XYO_JIT_MEGA_REPEAT_BURST",
        "XYO_JIT_MEGA_MULTI_REPEAT_BURST",
        "XYO_JIT_MEGA_IF_ELSE_BATCH",
        "XYO_JIT_MEGA_BRANCH_PAIR",
        "XYO_JIT_MEGA_UNTIL_BCAST",
        "XYO_JIT_MEGA_UNTIL_OUTER",
        "XYO_JIT_MEGA_UNTIL_POST_SET",
        "XYO_JIT_MEGA_UNTIL_OUTER_POST_SET",
        "XYO_JIT_MEGA_UNTIL_BENCH_ROUND",
        "XYO_JIT_MEGA_UNTIL_BENCH_BEGIN",
        "XYO_JIT_MEGA_UNTIL_BENCH_END",
        "XYO_JIT_MEGA_UNTIL_VAR_PAIR",
        "XYO_JIT_MEGA_NORMALIZE_YIELD",
        "XYO_JIT_MEGA_FAST_BCAST_LATCH",
        "XYO_JIT_MEGA_FAST_VAR_PAIR_JOIN",
        "XYO_JIT_MEGA_DISPATCH",
        "XYO_JIT_CORO_GENERAL",
        "XYO_JIT_MEGA_PARTIAL",
        "XYO_JIT_MEGA_SCHED_PREFILTER",
    };
    for (const char* name : kFeatures)
        detail::setMegaEnvIfAbsent(name, "1");
    if (!std::getenv("XYO_JIT_MODE") && !std::getenv("XYO_JIT_DISABLE"))
        detail::setMegaEnvIfAbsent("XYO_JIT_MODE", "eager");
    return true;
}

} // namespace xyo
