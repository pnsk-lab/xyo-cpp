---
id: PROF-001
title: YJIT参考のJITコンパイル・キャッシュ統計計測基盤
status: done
owner: agent-prof-gpt6
priority: P1
updated: 2026-11-12
---

## Scope

- Implement opt-in XYO_JIT_COMPILE_STATS=1 compile-path telemetry following the YJIT observability survey.
- Count synchronous compilation, asynchronous preparation/finish, compile cache hits and async publication outcomes.
- Do not edit the currently owned codegen.cpp, rt.cpp, sequencer.cpp, thread.cpp, or renderer files.

## Acceptance criteria

- [x] Telemetry disabled by default and does not alter Scratch-visible execution.
- [x] Standalone telemetry test passes with reporting enabled.
- [x] jit.cpp syntax-only compilation passes under the existing compile configuration.
- [x] jit.cpp isolated object compilation passes.
- [ ] Full end-to-end build/parity: currently blocked by concurrent PEN-002 renderer code (unknown render::DlssOpaqueCoverage).

## Files / dependencies

- src/jit/compile_telemetry.hpp (new)
- src/jit/jit.cpp (targeted instrumentation only)
- tests/unit/jit_compile_telemetry_smoke.cpp (standalone test)
- docs/jit-compile-telemetry.md (usage and metric semantics)
- Depends on the concurrent DLSS renderer fix before full binary testing.
