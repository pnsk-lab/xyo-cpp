---
id: PROF-004
title: LLVM JIT内Guard低速経路の実行回数計測
status: done
owner: agent-prof-gpt6
priority: P1
updated: 2026-11-12
---

## Scope

Instrument selected named LLVM slow-path blocks after IR generation and
before optimization without changing Scratch codegen, runtime helper
semantics or the existing OSR/CallFrame ABI. Diagnostics are strictly
opt-in with XYO_JIT_GUARD_STATS=1.

## Acceptance criteria

- [x] Seven audited native JIT slow-path kinds, distinguishing typed SSA cache misses from true Guard failures.
- [x] Only numeric-suffixed exact-name blocks are recognized; unrelated names excluded.
- [x] No LLVM instrumentation emitted when XYO_JIT_GUARD_STATS is disabled.
- [x] Both synchronous and asynchronous JIT generation paths instrumented, including emitted OSR continuations.
- [x] Eager and lazy ORC register the native C telemetry symbol.
- [x] Standalone atomic-counter smoke test passes enabled/disabled.
- [x] LLVM IR insertion test passes enabled/disabled (eight synthetic named sites).
- [x] Real ORC execution test passes enabled/disabled (one site, two slow-path hits).
- [x] src/jit/backend_llvm.cpp syntax + isolated object compilation pass.
- [x] Bun log analyzer regression tests pass (3/3), JSON output supported.
- [x] git diff --check succeeds.
- [ ] Full xyo binary/parity/47-workload ScratchBench blocked by concurrent renderer work: sdl_renderer.hpp references render::DlssOpaqueCoverage, whose declaration is absent.

## Files / dependencies

- src/jit/guard_fallback_telemetry.hpp (new)
- src/jit/guard_fallback_instrument.hpp (new)
- src/jit/backend_llvm.cpp (limited opt-in codegen/ORC symbol registration)
- tests/unit/jit_guard_fallback_telemetry_smoke.cpp (new)
- tests/unit/jit_guard_fallback_instrument_test.cpp (new)
- tests/unit/jit_guard_fallback_native_test.cpp (new)
- tools/jit-guard-analyze.ts, tools/jit-guard-analyze.test.ts (new)
- docs/jit-guard-slowpaths.md (new)

## Limitations

This counts entries into curated native slow paths, not all LLVM guards
and not real deoptimizations. The optional counter call and preserving
LLVM IR names perturb diagnostic run performance; flag must be OFF for
throughput benchmarks. Named preoptimization sites may be optimized out.
The native numeric-CFG and AOT backends are outside scope.
