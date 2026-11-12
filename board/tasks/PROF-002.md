---
id: PROF-002
title: Guard拒否理由分類とDeoptログ集計
status: done
owner: agent-prof-gpt6
priority: P1
updated: 2026-11-12
---

## Scope

- Extend existing opt-in JIT telemetry with stable compile-time refusal and speculative numeric-entry guard reason counters.
- Parse the existing runtime deoptimization diagnostic logs without modifying the OSR/CallFrame implementation owned by other agents.
- Preserve behavior when XYO_JIT_COMPILE_STATS is unset or disabled.

## Acceptance criteria

- [x] Ten explicit refusal reason IDs are counted and reported independently of runtime deoptimization.
- [x] Numeric CFG slot guard rejection is differentiated from unsupported lowering/loop shape.
- [x] No non-opt-in runtime logging or allocations were added to hot paths.
- [x] C++ refusal smoke test compiles and produces expected count labels.
- [x] C++ telemetry emits nothing with XYO_JIT_COMPILE_STATS=0.
- [x] Deoptimization log analyzer passes three Bun tests.
- [x] Isolated src/jit/jit.cpp object compilation passes with the project's real compiler flags.
- [ ] Full binary/parity not rerun: independent SDL renderer DLSS type error still prevents clean build.

## Files / dependencies

- src/jit/compile_telemetry.hpp (extended PROF-001 counters)
- src/jit/jit.cpp (existing owned instrumentation; categorized refusals)
- tests/unit/jit_refusal_telemetry_smoke.cpp
- tools/jit-deopt-analyze.ts
- tools/jit-deopt-analyze.test.ts
- docs/jit-guard-deopt-telemetry.md

## Follow-up

Actual runtime side exits and all deoptimization outcomes must be instrumented at their authoritative call sites after agreement with RT-001/OSR owners. The diagnostic parser is only a partial count of already-logged experiments.
