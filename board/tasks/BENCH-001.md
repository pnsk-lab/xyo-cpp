---
id: BENCH-001
title: Run scratch-bench with current optimizations
status: done
owner: codex-scratch-bench
priority: P1
updated: 2026-11-08
---

## Scope

Run `scratch-bench` against the current xyo-cpp source checkout with the default full xyo-cpp variant set (hotspot, interpreter, eager, AOT). Do not change benchmark or runtime source files; generated benchmark artifacts/results may be written under `scratch-bench/`.

## Acceptance criteria

- [ ] Validation posted

## Files / dependencies

Read only: `scratch-bench/src/runtimes/xyo.ts`, benchmark docs. Generated output: `scratch-bench/.cache/`, `scratch-bench/results/`.
