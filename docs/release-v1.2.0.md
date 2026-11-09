# xyo-cpp v1.2.0 — verified fast defaults

v1.2.0 makes performance-validated and parity-checked optimizations the release defaults. An explicitly supplied environment variable overrides every default.

## Enabled by default

- XYO_JIT_EQ_NUMERIC_PROFILE_CACHE=1 — algo_primes +16.2% in 16 paired trials, ScratchBench 2123/2123 parity (docs/algo-primes-profiled-equality-2026-11-09.md).
- XYO_JIT_LIST_MONOTONE_FLAG=1 — algo_sieve +4.6% in 12 pairs, 2123/2123 parity (docs/algo-sieve-monotone-list-2026-11-09.md).
- XYO_JIT_REPLACE_NO_NULL=1 — guarded null-check elision for existing lists.
- XYO_JIT_NUMERIC_UNTIL_SSA=1, XYO_JIT_NUMERIC_UNTIL_FAST=1, XYO_JIT_AFFINE_UNTIL=1 — validated numeric until/affine SSA (docs/jit-numeric-until-ssa-2026-11-08.md).
- XYO_BCAST_FAST_ASCII=1 — ASCII fast path with Unicode fallback.
- XYO_SKIP_SAME_EFFECT_REDRAW=1 — omit redundant redraw requests for identical visual effects; disable for strict frame scheduling comparisons.

Default-on numeric repeat SSA, HotSpot JIT, Dragonbox, and direct SStr conversion remain in place. XYO_JIT_NUMERIC_REPEAT_SSA is intentionally left unset: forcing =1 broadens eligibility beyond the proven default.

## Experimental options excluded from defaults

Conditional i32 SSA, if/PHI, speculative reads, nested OSR/CallFrame, short-string intern, FSR/DLSS and executable-order pop-last stay opt-in. --opt-all enables experiments, not a production-ready faster profile.

CMake defaults to Release when unspecified. Use -DCMAKE_BUILD_TYPE=RelWithDebInfo for symbols. Benchmarks are host/workload dependent; no claim of universal 47/47 superiority.
