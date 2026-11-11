# LLVM ORC lazy par-for: repeat callbacks within one JIT lifetime

Branch `work/parfor-longrun-v123`; parent `d7797b4`, which passed
all 8 GitHub Actions #84 jobs.

## Motivation

Previous native callback tests repeatedly started new processes and
verified that each process completed before its JIT was destroyed.
That checked ordinary worker shutdown, but did not exercise repeated
reuse of the **same compiled callback pointers** by a single native
warp procedure.

The new `tests/differential/jit_lazy_parfor_longrun.py` creates a
self-contained Scratch project with one warp procedure called multiple
times from a single main-script repeat loop. The procedure includes
two independent par-for loops with different LLVM callback functions.
Each time it is called, it appends the ascending integer sequence
`1, 2, ..., trip` to each of two separate Scratch lists.

The checker compares **every value, order, length, and final Scratch
variable** against the Interpreter result. It also verifies that all
`2 * rounds` native parallel dispatches occurred, that the opt-in
partition path was actually chosen, and that two callback functions
were co-materialized in the same ORC partition. The full final state
is SHA256 hashed to detect changes between JIT modes and repeats.

Modes checked: Interpreter, Eager, ordinary (guarded) HotSpot, opt-in
HotSpot with `XYO_JIT_LAZY_PARFOR_PARTITION=1`, plus repeated fresh
opt-in runs. Default JIT behavior remains unchanged.

## Local results

LLVM 23 Release x86_64, existing locally compiled `xyo` from the
independent `work/parfor-parity-integration-v123` worktree.

| Test | Batches in one JIT lifetime | Total callback iterations | Status |
|---|---:|---:|---|
| 16 rounds × 4096, two callbacks | 32 | 131072 | All modes match; opt-in 3/3 matches |
| 64 rounds × 4096, two callbacks | 128 | 524288 | All modes match; opt-in 5/5 matches |
| 256 rounds × 4096, two callbacks | 512 | 2097152 | All modes match; opt-in 2/2 matches |

Each row compares ordered lists with **all** appended values, not
just a checksum. The checksum is an additional cross-mode check.
These are cumulative callback counts, not a claim about wall-clock
duration; individual test processes may finish quickly on a fast CPU.

An isolated CTest was configured and executed with the verified LLVM
23 binary copied into a fresh test build directory:

`xyo-jit-lazy-parfor-longrun`: **1/1 PASS**.

The larger stress settings are intentionally local-only to keep the
default CI job bounded; the CI version runs the 16-round case.

## LLVM 18 CI

The Ubuntu 24.04 LLVM 18/nanosvg CI job now runs the new CTest using
an `xyo-cli` built against LLVM 18. This adds runtime coverage for
reusing native callback pointers across 32 successive worker batches
under LLVM 18. Four previous CTests remain enabled, so that job runs
five selected tests.

The selective-build preflight in
`tools/ci/check_llvm18_parfor_ctests.py` now requires the long-run
test and validates its native `xyo` executable. Its unit suite
passes **6/6**, including a specific check that an absent long-run
CTest is rejected. The YAML structure and Python byte compilation
were validated locally.

These results **do not prove** correctness for destruction of a
Runtime *concurrently* from another thread while native code executes.
Ordinary callback dispatch is synchronous: `WorkerPool::run` waits
for all tasks to finish before returning; test processes terminate
only afterward. Concurrency with external runtime destruction would
need an explicit lifetime/ownership API.

## Not changed

No C++ runtime or compiler code, no ORC partition policy, no default
feature flags, no ScratchBench Parity gate and no `main` changes
are included. The experiment is still opt-in.

GitHub Actions validation for this new branch is independent from the
previous #84 success and should not be marked complete until it has
executed.
