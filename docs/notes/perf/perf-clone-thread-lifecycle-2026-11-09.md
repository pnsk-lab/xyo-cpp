# PERF-001 — Clone thread lifecycle and stop-index experiments (2026-11-09)

## Objective and methodology

Continue optimizing the unmodified 47-workload ScratchBench suite (source hash `e8bd5be3e7345088`). The remaining target is 47/47 wins against the best of MistWarp, TurboWarp, NitroBolt, Scratch Everywhere!, and Scratch. `sched_clonechurn` creates 100 clones repeatedly, and every clone updates a shared Stage scalar and deletes itself.

For causal results, use the original source-derived isolated `clonechurn.sb3`, a single frozen native Release executable and CPU14 affinity on the i7-12700, alternating on/off processes and comparing medians. Clone-churn checksum varies with actual elapsed time; equal checksums are not the correctness oracle. Independent ScratchBench parity and a test for multiple clone hats are used for correctness.

## Existing controls: eight measurements per setting

CPU14 and frozen prior executable `build/perf-clone-order-frozen-xyo`:

| Mode | Median ops/ms | Change from default |
|---|---:|---:|
| Baseline | **1048.263** | — |
| Disable Thread reuse (`XYO_THREAD_REUSE=0`) | 1019.795 | -2.7% |
| Disable lazy root StackFrame (`XYO_LAZY_ROOT_FRAME=0`) | 1039.428 | -0.8% |
| Disable caller-affine clone JIT code (`XYO_CLONE_SERIAL_CODEGEN=0`) | 1013.759 | -3.3% |
| Disable parallelism | 1047.462 | -0.1% |
| Eager instead of HotSpot JIT | 1046.957 | -0.1% |

Existing Thread reuse and caller affinity help; disabling them is not a path to win.

## Rejected Target* hash index — fully reverted

A guarded `XYO_CLONE_STOP_INDEX=1` experiment used
`std::unordered_map<Target*, CloneStopBucket>`, appended and indexed
Thread* on `pushThread/restartThread`, removed handles before reclamation,
and erased the Target key before destruction. It reduced the scope of
`Runtime::stopForTarget` from all threads to the relevant clone.

24 alternating CPU14 pairs showed **legacy OFF 1040.1275** vs
**hash index ON 1001.241 ops/ms**, **-3.74%**, ON only **3/24**
paired wins. Registration, hash table, and allocation overhead outweighed
the savings. This optimization and its test were fully removed from the
source tree. Raw data: `build/perf-clone-stop-index-ab.{json,log}`.

An incremental shared-directory build initially segfaulted **after DONE**
inside `JitEngine::~JitEngine` (shared_ptr destructor) even with this
flag set to OFF. A full independent Release rebuild of the same sources
ran both flags successfully, and ON unit CTest passed 53.47 seconds
with GUI parity **2123/2123 in both modes**. This is evidence of a
potential shared incremental build/ABI consistency issue, not evidence
that enabling the hash index caused the destructor failure. The
diagnostic core and GDB trace remain in
`build/perf-clone-stop-index-crash.core`,
`build/perf-clone-stop-index-core-gdb.log`.

## Current candidate: allocation-free per-clone intrusive list

`XYO_CLONE_STOP_LINKED=1` (experimental, **OFF by default**) maintains
a doubly-linked Thread* list directly on each cloned Target. No
per-clone hash-node or vector allocation is needed:

- `Runtime::pushThread`: append to its clone's list in startup order.
- `Runtime::restartThread`: insert replacement before its old identity,
  preserving execution order and guarding later stale identities.
- `Runtime::stopForTarget`: on clones only, traverse the linked list and
  validate `runtimeIndex` against `threads[index]` before stopping.
  Original targets and legacy mode keep scanning the full threads array.
- `Runtime::reclaimThreads`: unlink a now-unreachable Thread in O(1)
  before its storage can be recycled, preventing stale or ABA links.
- `Runtime::disposeTarget/stopAll`: detach all links while the Target
  is still alive, before freeing it. Old Thread handles may remain alive
  for broadcast-and-wait continuations.
- `ctlDeleteClone` interpreter and `xyo_ctl_deleteclone` JIT:
  `Target::dispose` already stops all threads; with linked mode enabled,
  skip the redundant `stopForTarget` call that would otherwise access a
  now-freed Target. With linked mode OFF, retain the original call.
- `loadHotStart` restores Thread identities without `pushThread`;
  therefore `Runtime::rebuildLinkedCloneStopIndex` rebuilds the list
  from the restored runnable vector in scheduler order, after the
  checkpoint has populated all Thread* and before execution resumes.

All additions are guarded by the mode flag. Fields are appended after
existing JIT ABI-sensitive Thread members.

### Controlled A/B

Frozen independent-build executable from before adding the hot-start
rebuild hook: `build/perf-clone-stop-linked-frozen-xyo`,
SHA256 `7a58a15d889189c5dcd658db092b9dca7f6c184e4a1abf20986b9c467a1cb097`.

24 alternating CPU14 pairs on the original source-derived clonechurn project:

| Metric | Linked OFF | Linked ON |
|---|---:|---:|
| Median ops/ms | 1040.0745 | **1055.941** |
| Mean ops/ms | 1041.0688 | **1052.7588** |
| Standard deviation | 18.755 | 14.587 |
| ON paired wins | — | **16/24** |
| Geometric mean of paired ratios | — | **1.01129x** |

Median improvement **+1.53%**, small and below the gap to the
stored Scratch Everywhere! score of 1907.96. Raw:
`build/perf-clone-stop-linked-ab.{json,log}`.

### Correctness

`tests/unit/test_scheduler.cpp::testCloneStopLinked` uses a synthetic
target with two clone-start hats. It checks list start order, stop
exception, replacement Thread position, exact stop semantics,
detachment from all old live handles, and a post-disposal scheduler step.
The test additionally invokes the hot-start index rebuild to verify
that stale handles are omitted.

- Initial independent build: successful.
- Initial unit CTest ON and OFF: **1/1 PASS** each (43.22 s / 41.48 s).
- Initial GUI ScratchBench parity ON and OFF: **2123/2123 (100%)**.
- After including hot-start restoration, final independent Release build
  successful; frozen `build/perf-clone-stop-linked-final-frozen-xyo`
  SHA256 `139edc8295cb341b42b46b85a9a6ab208545600b5a2d743ad080c690af5df6b6`.
- Final GUI ScratchBench parity ON and OFF:
  **2123/2123 (100%)** (both), logs
  `build/perf-clone-stop-linked-final-parity-{on,off}.log`.
- Final unit CTest ON/OFF and whole 47-case suite: inspect pending
  `build/perf-clone-stop-linked-final-ctest-{on,off}.log` and
  subsequent performance outputs.
- `git diff --check`: clean at time of patch.
- No commit/push; other agents' shared-tree edits remain intact.

**No 47/47 claim.** Leave `XYO_CLONE_STOP_LINKED` disabled by default
unless original all-47 results support a measured benefit and the final
regression passes.
