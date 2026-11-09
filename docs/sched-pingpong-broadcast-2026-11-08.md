# Broadcast-and-wait indexed name dispatch: ScratchBench follow-up (2026-11-08)

## Target

Continue improving the remaining 22-25 underperforming ScratchBench workloads. The initial target was `sched_pingpong`: a tight repeat calling `broadcast and wait sc_ping` and a single receiver incrementing `sc_hits`.

The user requested that all 47 workloads exceed the highest measured value of MistWarp, TurboWarp, NitroBolt, Scratch Everywhere! and Scratch. The 47/47 target has **not** been achieved.

## Root cause and implementation

The LLVM JIT host helper `xyo_broadcast_wait_v()` had repeatedly:
1. copied the Scratch message into a GC-safe owned UTF-16 string;
2. searched the Stage broadcast variables by name, lowercasing many entries;
3. dispatched the event through an already indexed, uppercased receiver table.

For ordinary ASCII Stage namespaces step 2 can be eliminated without changing the selected receiver set. **Do not remove the GC-safe string copy**, because spawn/compilation can trigger GC. Unicode names such as U+212A KELVIN SIGN can lowercase to ASCII `k` without their uppercase form matching the direct ASCII key; they must retain the legacy canonical Stage lookup.

- `src/jit/rt.cpp`: direct indexed input for ASCII broadcast messages when `Runtime::stageBroadcastNamesAsciiOnly()` returns true. Disable with `XYO_BCAST_FAST_ASCII=0`. Keep the historical canonical path for Unicode Stage namespaces or input names.
- `src/engine/runtime.hpp` and `runtime.cpp`: Stage namespace ASCII-only classification cached per `stepSerial` and invalidated by `invalidateBroadcastIndex`. Both ordinary broadcast variables and colliding broadcast identifiers are checked. No per-message Stage map iteration on the fast path.
- The same files also maintain a one-entry last-message `broadcastReceivers` lookup cache, clearing it when the receiver index is invalidated and preserving `shared_ptr` snapshots across nested broadcast dispatch. Disable with `XYO_BCAST_RECEIVER_CACHE=0`.
- `tests/unit/test_scheduler.cpp`: ordinary ASCII Stage broadcasts, Kelvin Unicode namespace guard, per-tick refresh, and colliding-broadcast guard.

## Repeated paired benchmarks

All rates are `ops/ms` of the unchanged isolated `sched_pingpong` fixture; CPU i7-12700, HotSpot, 30fps, same Release executable for each A/B, ten alternating pairs. The checksum changes between timed rounds by design.

| Change | Old median | New median | Ratio | Raw data |
|---|---:|---:|---:|---|
| ASCII canonical lookup eliminated | 709.24 | 1,396.54 | **1.969×** | `build/perf-ping-ascii-ab.json` |
| last receiver lookup cached (ASCII fast path enabled) | 1,403.18 | 1,477.11 | **1.053×** | `build/perf-ping-receiver-ab.json` |

The improvements are separate toggles on freshly rebuilt versions; multiplying their medians is not an exact one-binary combined A/B result.

## Whole-suite / accuracy

- `scratch-bench/results/perf-ping-cache-clean-20261108/performance-xyo-cpp.json` (isolated from CPU-intensive CTest): weighted overall **91,656**, `sched_pingpong` **1,429.58 ops/ms**.
- Five-rival results saved at `scratch-bench/results/perf-ping-cache-rivals-20261108.{json,md}`: **25/47 ahead, 22/47 behind**. Source hash `e8bd5be3e7345088`.
- Competitor `sched_pingpong` best stored performance: Scratch Everywhere! **1,825.85 ops/ms**, so current full-suite xyo result is about **0.783×**, **not a win**.
- Other agents changed math, variable and compiler paths concurrently. Whole-suite aggregate score differences are not isolated evidence of the broadcast change.
- Unit tests: `ctest --test-dir build-perf-all --output-on-failure -R '^xyo-tests$'` **PASS 1/1**; log `build/perf-ping-receiver-ctest.log`.
- ScratchBench parity with all changes: **2121/2123** (99.9%). Remaining pre-existing failures are `Looks: Size bounds per costume` 8,9. See `build/perf-ping-cache-parity.log`. Previous fast-on/off parity results also had identical failures.

## Diagnostics

The first concurrent CTest + performance attempt in `perf-ping-cache-final-20261108` was CPU-contended and measured 43,275 overall score; it is **invalid for comparative performance**. The un-contended rerun is `perf-ping-cache-clean-20261108`.

`PERF-001` remains in progress. Shared dirty worktree, unrelated concurrent edits preserved; no commit/push.
