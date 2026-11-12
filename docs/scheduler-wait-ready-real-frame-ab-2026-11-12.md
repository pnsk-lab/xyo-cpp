# Wait-ready suppression: SDL play frame-latency A/B and visual parity

Date: 2026-11-12
Repository: xyo-cpp
Feature: `XYO_WAIT_READY_SLEEP` (opt-in only)

## Scope

Earlier scheduler microbenchmarks measured `sched_waituntil` throughput:
approximately 9.15x with `XYO_WAIT_READY_SLEEP=1` on a CPU-pinned
eager-JIT microbenchmark. This report checks whether the same opt-in
optimization materially improves **drawn frames** and preserves game
output on existing Scratch projects.

## Rendering build

The existing `build-scheduler-rewrite-20261111/xyo` binary has
`XYO_SDL2=OFF`, so it cannot execute `xyo play`. A *separate*
SDL2-enabled Release/LLVM JIT configuration was created at
`build-scheduler-play-20261112`:

```sh
cmake -S . -B build-scheduler-play-20261112 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DXYO_SDL2=ON \
  -DXYO_REQUIRE_SDL2=ON -DXYO_JIT=ON \
  -DXYO_JIT_BACKEND=llvm -DXYO_BUILD_TESTS=OFF -DXYO_RSVG=ON
cmake --build build-scheduler-play-20261112 --target xyo -j4
```

SDL2 version 2.32.74; librsvg version 2.63.4.
The server has no desktop display, so real rendering is exercised via
`SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software
SDL_AUDIODRIVER=dummy`. This is SDL CPU rendering, **not** GPU/DLSS or
a true display-server present-time measurement.

The program's pre-existing `XYO_PLAY_PROFILE` records a CSV per frame
of `step_ms`, `vm_ms`, `draw_ms`, `frame_interval_ms`, etc.
No rendering-time instrumentation was invented for the benchmark.

## A/B harness

New tool: `tests/bench/algos/bench_wait_ready_play.py`.

- Freeze executable and original SB3 archives before starting.
- Pin the benchmark process and children to CPU 16.
- Keep `XYO_JIT_MODE=eager`, `XYO_WAIT_WAKE_REGISTRY=1`,
  `XYO_SCHED_READY_INDEX=1`, `XYO_YIELD_QMODE=off` identical.
- Toggle **only** `XYO_WAIT_READY_SLEEP` between OFF (0) and ON (1).
- Alternate OFF→ON and ON→OFF in consecutive pairs.
- Four pairs per project, 70 presented SDL frames per trial, first
  10 omitted as warmup.
- `--fps 60 --turbo --scale 1` with the same deterministic frame-indexed
  mouse and key injection in both variants.
- Persist CSV data, stderr/stdout, SHA256 of executable and SB3,
  and final framebuffer PPM image for every trial.
- Report within-trial frame-time p50/p95/p99, drawing/VM p95 and
  paired OFF/ON geometric-mean speed ratio.
- In strict-parity mode, compare full final framebuffer hashes and
  normalized full Scratch variable-state JSON for each OFF/ON pair.

A ratio higher than 1 means ON reduced frame-processing time.
Frame-time ratios here compare the *95th percentiles* of 60 measured
frames within each run; they are not normalized by random event counts.

### Unmodified existing Scratch projects

Results:
`build-scheduler-rewrite-20261111/wait-real-frame-ab-20261112/real-projects/results.json`

| Project | OFF p95 step (ms) | ON p95 step (ms) | Pair speed ratio | Matching final images |
| --- | ---: | ---: | ---: | ---: |
| mandelbrot.sb3 | 87.41 | 94.15 | 0.943x | 2/4 |
| rubikscube.sb3 | 30.85 | 31.01 | 0.995x | 4/4 |
| 3D2.sb3 | 87.85 | 84.59 | 1.026x | 1/4 |

Static Scratch block inspection:
- mandelbrot: 1 `control_wait_until`, 1 `sensing_timer`
- rubikscube: 0 `control_wait_until`, no `sensing_timer`
- 3D2: 1 `control_wait_until`, 1 `sensing_timer`

OFF screenshot snapshots also varied across repeated OFF trials for
`mandelbrot` (2 distinct hashes) and `3D2` (4 distinct hashes).
Thus these visual differences are not attributable to the wait-ready
flag based on the evidence here. The unchanged rubikscube project was
deterministic (4/4 image matches).

### Existing wait-heavy 3D project

`【3D】ASTERIA(Ver.1.1).min.sb3` includes 76 static
`control_wait_until` blocks, 52 pen blocks, 616 random reporters
and 64 timer reporters.

Results:
`build-scheduler-rewrite-20261111/wait-real-frame-ab-20261112/asteria/results.json`

- OFF p95 step: **34.62ms**
- ON p95 step: **34.60ms**
- Paired OFF/ON p95 ratio: **0.976x** (1/4 ON wins)
- Matching final screenshots: **0/4**. But OFF itself had
  3 distinct screenshot hashes and ON had 4, so final framebuffer
  equality is **not** a valid deterministic parity oracle here.
- `XYO_WAIT_READY_STATS=1` printed
  `deferred=0 sparse_passes=0 waiter_visits=0 full_rechecks=0`
  for this 70-frame workload: static wait-until blocks existed but no
  eligible native waiting Fibers were suppressed in the observed period.

## Deterministic visual + 24-waiter integration

New generator: `tests/bench/algos/build_wait_visual_fixture.py`.

It preserves all original rubikscube drawing/scripts/assets, adds a
96x96 solid overlay sprite, 24 independent green-flag `wait until`
scripts waiting on a single Stage variable, and a Scratch broadcast
receiver that sets that variable to 1. Each completed wait increments
the Stage completion count. A successful wait also hides the overlay.

The test harness sends only `25:bc:UNLOCK`; no test-harness variable
write occurs. A JIT-compiled Scratch receiver performs the gate store.

`XYO_DUMPVARS=json` showed:
- Before broadcast: gate 0, completed 0, red overlay visible.
- After broadcast: gate 1, completed **24**, overlay hidden.
- ON and OFF final full variable JSON: **identical**.

Strict four-pair benchmark result:
`build-scheduler-rewrite-20261111/wait-real-frame-ab-20261112/visual-wait-parity/results.json`

| Metric | OFF (median of four runs) | ON (median of four runs) | Paired ratio |
| --- | ---: | ---: | ---: |
| p50 step | 30.54ms | 30.60ms | 0.988x |
| p95 step | 31.12ms | 31.18ms | 0.998x |
| p99 step | 31.48ms | 31.41ms | 1.001x |
| p95 VM | 27.18ms | 27.21ms | 0.999x |
| p95 draw | 4.33ms | 4.27ms | 1.015x |

- **All 4/4 final framebuffer images match exactly.**
- **All 4/4 normalized complete Scratch variable states match exactly.**
- Both `wait_ready_gate=1` and `wait_ready_done=24` verified
  in **every trial** (mandatory assertions in strict mode).
- With ON, `XYO_WAIT_READY_STATS=1` reported:
  `deferred=624 sparse_passes=223 waiter_visits=672 full_rechecks=26`.
  The optimization therefore *really was active*, despite nearly
  unchanged drawn-frame latency.

A separate manually controlled probe with mouse and key events
showed differing screenshots despite final completion counters
matching. Removing the mouse/key stream and retaining only the
broadcast produced exact ON/OFF screenshots and variable-state
equality. The input-heavy case remains inconclusive, not a proven
scheduler bug.

## Interpretation

The `sched_waituntil` microbenchmark improvement does **not**
automatically turn into a 9x actual frame-rate improvement. During the
drawn-frame tests the runtime still typically spends ~25ms in its VM
frame step budget, and scene drawing takes additional CPU time.
Eliminating repeated wait-slot visits can increase throughput without
meaningfully reducing the fixed-cost simulation frame.

`ASTERIA` was not actually eligible for wait-slot suppression during
the sampled gameplay segment, so no major improvement was expected.
The deterministic 24-waiter visual fixture confirms both execution
of the feature and state/image correctness with negligible p95 latency
change.

### Limitations

- No real monitor/display server, GPU/DLSS or vertical-sync delay:
  `--turbo` deliberately removes the sleep-to-target-fps.
- Only four A/B pairs per project. There are multiple comparisons,
  possible JIT compilation outliers, system CPU contention and
  timing-sensitive projects.
- These are frame-processing latencies, not input-to-photon latency
  measured with a physical display.
- Screenshot mismatches in random/timer-heavy games need replay-stable
  inputs, timer and random-seed instrumentation before they can be
  reliably attributed to scheduling.
- The feature remains **OFF by default** and retains the existing
  conservative next-frame wakeup fallback for unnotified writes.
- The SDL rendering build is a separate configuration; other agents'
  concurrent changes in the shared repository were not reverted.

## Optional per-project diagnostic

`src/shell/main.cpp` now emits the existing
`Sequencer::waitReadyStats()` at the end of a `play` run when
`XYO_WAIT_READY_STATS=1`. It prints
`[wait-ready-play] deferred=... sparse_passes=...
waiter_visits=... full_rechecks=...` to stderr.
Normal play output and behavior are unchanged if unset.

## Reproduction

```sh
python3 tests/bench/algos/build_wait_visual_fixture.py \
  --source debug/projects/rubikscube.sb3 \
  --out build-scheduler-rewrite-20261111/wait-real-frame-ab-20261112/rubik-with-waiters.sb3 \
  --waiters 24

python3 tests/bench/algos/bench_wait_ready_play.py \
  --binary build-scheduler-play-20261112/xyo \
  --project visual_wait=build-scheduler-rewrite-20261111/wait-real-frame-ab-20261112/rubik-with-waiters.sb3 \
  --out build-scheduler-rewrite-20261111/wait-real-frame-ab-20261112/visual-wait-repro \
  --runs 4 --frames 70 --warmup 10 --cpu 16 --fps 60 \
  --inject '25:bc:UNLOCK' --require-parity \
  --expect-stage-var wait_ready_done=24 \
  --expect-stage-var wait_ready_gate=1
```

All source changes remain uncommitted.
