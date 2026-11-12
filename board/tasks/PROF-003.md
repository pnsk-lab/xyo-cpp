---
id: PROF-003
title: 実行時Deoptの理由別カウンターと復帰成功率
status: done
owner: agent-prof-gpt6
priority: P1
updated: 2026-11-12
---

## Scope

- Add direct opt-in counters for three runtime Deopt mechanisms with five labeled source paths.
- Report attempt count, accepted interpreter continuations, refused transfers, and reasons.
- Keep OSR/RT control flow, source revision checks, CallFrame, GC roots and resume PC unchanged.
- Extend the log analyzer to handle direct counters separately from diagnostic messages.

## Acceptance criteria

- [x] Native source handoff successful/refused attempts counted, including failed capture.
- [x] Numeric CFG rollback categorized using the existing reason labels; count the actual interpreter restore result.
- [x] Nested OSR outer/inner/triple restoration and unsafe refusal categorized.
- [x] Opt-in flag XYO_JIT_DEOPT_STATS=1; disabled mode does not instantiate or update stats.
- [x] Atomic counters and no dynamic allocation/printing in deopt recording path.
- [x] Standalone C++ smoke compiles with -Wall -Wextra -Werror; enabled/disabled tests pass.
- [x] Isolated src/engine/sequencer.cpp full object build passes with the repository's compile flags.
- [x] Bun diagnostics analyzer regression tests pass (5/5).
- [x] git diff --check passes.
- [ ] Full executable build / parity suite not completed; concurrent PEN-002 renderer still references an undeclared render::DlssOpaqueCoverage type.

## Files / dependencies

- src/jit/deopt_telemetry.hpp (new)
- src/engine/sequencer.cpp (six bounded instrumentation insertions)
- tests/unit/jit_deopt_telemetry_smoke.cpp (new)
- tools/jit-deopt-analyze.ts (direct counters without double counting)
- tools/jit-deopt-analyze.test.ts (two new tests)
- docs/jit-runtime-deopt-telemetry.md (new)

## Known limitations

This only measures the named runtime restore pathways; speculative LLVM guard
fallbacks remaining inside native code and other uninstrumented side exits
are not counted. Success means the interpreter continuation was accepted,
not that a full-parity suite passed. Do not call this an exhaustive Deopt
rate or a performance improvement.
