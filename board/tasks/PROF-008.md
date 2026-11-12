---
id: PROF-008
title: CFG型ヒントの解析コスト・省略効果の実測
status: done
owner: agent-prof-gpt6
priority: P1
updated: 2026-11-12
---

## Scope

Add opt-in timing and counters for CFG type-flow analysis. Preserve Scratch semantics, concurrent OSR/IR/PEN source, and existing optimization defaults.

## Acceptance criteria

- [x] XYO_JIT_CFG_TYPE_STATS=1: eligible/skipped/analyses/truncated/promoted/nodes/analysis_ms reported.
- [x] OFF mode does not take timing samples or write diagnostic output.
- [x] C++ smoke with -Wall -Wextra -Werror passes enabled/disabled.
- [x] Dedicated Release LLVM headless binary compiled successfully.
- [x] Six full ScratchBench diagnostic runs complete: each 47/47, DONE, exit 0, deterministic checksums agree.
- [x] Full mode 8 analyses / 176 nodes / 2 promotions; selective mode 4 analyses / 112 nodes / 2 promotions.
- [x] Verified promotion identities k_n and k_x in BOTH modes.
- [x] Six isolated cases tested with two runs per mode and checksum checks.
- [x] Differential type-hint safety tests 12/12, Bun tests 6/6, git diff --check pass.
- [x] Results documented with caveats.
- [ ] Official whole-runtime parity and cross-host performance validation are future work.

## Results

- CFG analysis time full 0.328–0.481ms vs selective 0.337–0.448ms; JIT compile time fluctuates 388–653ms, not a proven speedup.
- Selection of numeric shadows is unchanged across full/ selective modes for current 47-case project.
- Log files: board/prof-008/full/, isolated/, verbose-compare.json.
- Report: docs/jit-cfg-type-analysis-cost-2026-11-09.md
- Source: src/analysis/cfg_type_hint_telemetry.hpp, src/jit/codegen.cpp, tests/unit/jit_cfg_hint_telemetry_smoke.cpp.

## Decision

Leave opt-in. Selective analysis gate saves negligible compilation time on the current suite; earlier broad speedup appears affected by noise. No commit or push.
