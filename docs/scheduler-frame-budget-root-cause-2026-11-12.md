# VM scheduler 25ms budget: source analysis and controlled work-cap experiment

Date: 2026-11-12
Repository: xyo-cpp
Status: opt-in diagnostic only; defaults unchanged.

## Root cause of the 25ms frame floor

`Sequencer::stepThreads()` sets
`WORK_TIME = 0.75 * runtime.currentStepTime` (around 25ms when
`currentStepTime` is 33.333ms). Its outer loop re-runs
`runSchedulerPass()` while there is an active Thread, the work-time
budget is not exhausted, and redraw policy permits it.

The CLI's `xyo play --fps 60` changes the presentation/pacing
interval but does **not** write `runtime.currentStepTime`. Thus,
for projects that retain the default 30fps Scratch runtime step time,
60fps presentation still allocates ~25ms *before* drawing. The source
SB3 may also override `currentStepTime` through TurboWarp metadata;
the relevant source is `src/load/sb3.cpp`.

This explains the prior measured `vm_ms` near 25ms on projects with
active long-lived scripts. The scheduler is intentionally using its
allocated per-frame CPU time; it is not proof that Ready Index
traversal itself consumes 25ms.

## Explicit diagnostic cap (default unchanged)

`src/engine/sequencer.cpp` now accepts a *strictly optional*
`XYO_SCHED_WORK_BUDGET_MS` for A/B measurement:

```
XYO_SCHED_WORK_BUDGET_MS=12.5
```

Only valid finite real values in [0.5, 250] ms apply. If absent or
invalid, the original `0.75 * currentStepTime` computation runs.
This is **not an optimization in compute efficiency**: it means fewer
scheduler passes and potentially less Scratch work per host frame.
It should not be enabled globally without additional semantic testing.

## Controlled measurements

A separate SDL2-enabled build (software rendering using SDL dummy
driver), CPU affinity 16, eager JIT, `XYO_YIELD_QMODE=off`,
`XYO_WAIT_WAKE_REGISTRY=1`,
`XYO_SCHED_READY_INDEX=1`, `XYO_WAIT_READY_SLEEP=1`.

Two existing/derived projects:

1. `debug/projects/rubikscube.sb3` (original 3D/pen renderer)
2. `rubik-with-waiters.sb3` (same renderer plus 24 independent
   `wait until` scripts and a Scratch broadcast receiver)

Each measured in 4 alternating 25.0ms/12.5ms pairs, 50 frames per
trial, first 10 removed. Identical frozen executable and input stream
within all pairs. The derived project injects
`25:bc:UNLOCK`; 24 native waits must finish and count once.
The shell prints `XYO_DUMPVARS=json` and saves a final framebuffer
PPM at frame 50.

Raw data and per-trial images, CSVs and source:
`build-scheduler-rewrite-20261111/wait-real-frame-ab-20261112/frame-budget-ab/`

| Project | Work budget | Median frame step p50 | Median frame step p95 |
| --- | ---: | ---: | ---: |
| rubikscube | 25.0ms | 30.478ms | 31.256ms |
| rubikscube | 12.5ms | 17.276ms | 18.418ms |
| rubik-with-waiters | 25.0ms | 30.298ms | 31.060ms |
| rubik-with-waiters | 12.5ms | 17.121ms | 17.915ms |

Across all 8 trials per project (both budgets), the script
recorded *one distinct* framebuffer hash and *one distinct*
normalized variable-state hash for each project. In the derived
project `wait_ready_done=24`, `wait_ready_gate=1`
with both work budgets. These limited fixtures happen to
produce the same final state at frame 50; **this does not
prove that reducing work budget preserves arbitrary Scratch
semantics, instruction count, or input-event timing**.

All 40 analyzed frames per run exceed 16.67ms for these
software-rendered CPU-limited trials. Even the 12.5ms scheduler
budget cannot guarantee physically sustained 60fps: rendering,
OS overhead and occasional long JIT work must fit too.

## Next engineering work

Treat these as **two separate** performance topics:

1. **Frame pacing and workload budgeting:** make the
   user-requested play FPS and the Scratch work-time policy
   consistent while respecting project-specific TurboWarp
   framerate overrides; consider reserving headroom for
   rendering with explicit frame deadlines, not blind
   per-frame 25ms busy filling. This changes scheduling
   semantics, and requires end-to-end tests.
2. **Actual scheduler efficiency:** measure cost per live
   dispatch, number of scheduler passes, per-pass status-change
   work, JIT Fiber transitions, and watcher bookkeeping
   under a fixed total amount of Scratch computation.
   Throughput benchmarks (~9x improvement on wait-heavy
   microbenchmark) are evidence of this separate topic.
3. **Correctness:** cross-engine wait/notify coverage, event
   ordering, source edits, and uninstrumented host variable
   writes must remain safe before the experimental ready
   index can be enabled by default.

No default change to `currentStepTime` or `xyo play --fps`
has been made. All source changes remain uncommitted.
