# ORC par-for callback co-partition (experimental)

Branch: `work/parfor-lazy-partition-v123`; parent `acf4539`.
The default par-for safety guard **remains enabled**.

## Problem

`src/jit/codegen.cpp` generates a private LLVM callback
`void(JitCtx*, int64, double, void**, int64*)` and passes its function
pointer to host `xyo_par_for`. ORC's LLLazyJIT normally splits the
module into individually compiled functions. A private address-taken
function may not yet have materialized; passing that pointer to the
host worker pool can crash even when it takes the sequential path.
The existing `hasParForCallback` guard therefore forces eager JIT.

On the frozen 35,591-byte `performance.sb3`, this increases the
Cold HotSpot startup from roughly 0.3s with `XYO_PARFOR=0` to 2.4s
with the normal `XYO_PARFOR=1` setting.

## Implementation

Opt in with `XYO_JIT_LAZY_PARFOR_PARTITION=1`. It does **not**
disable the default guard for other users.

- Before choosing deferred compilation, require **every** use of
  `xyo_par_for` to be a direct 8-argument LLVM call, whose final
  argument is an internally defined LLVM function in the same module.
  Require every use of each callback body address to be that call.
  If any of these proofs fails, use the existing eager path.
- Configure LLLazyJIT's `IRPartitionLayer::PartitionFunction` to
  co-materialize each requested function and the par-for callback
  functions it directly passes to native code. Any unexpected
  call argument at this stage requests whole-module materialization
  instead of allowing an unproven pointer.
- The existing `chooseLazyCompile` size/call-density policy and
  `ensureLazyJit()` fallback remain intact. The partition function
  does not change ordinary functions that don't use `xyo_par_for`.
- `XYO_JIT_LAZY_CALLBACK_TRACE=1` adds optional
  `[jit-lazy-partition] requested=... materialized=... callbacks=...`
  diagnostics.

This is not a general guarantee for arbitrary address-taken LLVM
functions. Such functions still require their own proof or an
eager materialization policy.

## Functional validation

LLVM 23.1.4, Release, Linux x86-64, compiled from this branch.

- Existing `xyo-jit-lazy-parfor-callback`: PASS (default eager guard).
- Existing `xyo-jit-lazy-integration`: PASS.
- New `xyo-jit-lazy-parfor-partition`: PASS.
- `xyo-jit-phase-profile-parser`: PASS.
- `xyo-tests`: PASS.
- Together, **5/5 focused CTests PASS**.

The new CTest constructs two par-for loops with different callback
bodies inside a warp procedure, and compares Interpreter, Eager,
ordinary HotSpot and opted-in HotSpot:
- **1,000 iterations per loop**, sequential native callback: both
  ordered lists and final counters match exactly.
- **8,192 iterations per loop**, actual native worker pool: two
  `parfor: parallel trip=8192 workers=8 chunks=16` dispatches,
  all values, order and counters match.
- ORC trace: `requested=1 materialized=3 callbacks=2`, proving
  both callback bodies are grouped with their caller.
- Additional **8 fresh-process parallel repetitions** match the
  same final Scratch state. No crashes observed in these runs.

## Startup measurements

Frozen `performance.sb3` SHA256
`216bf3184555341c5397aa30fcc30556936937e78e970d6a43b4179f3ce64ebb`.

Binary SHA256
`6544077fb23fc7aa729407aea064231b6c2cf5a66878a94becf23b3367fadd04`.

One CPU (CPU 0), fresh process each run, 5 interleaved trials,
first output is the ScratchBench performance table header.
All variants retain `XYO_PARFOR=1`:

| Configuration | First output median | Compilation median | Process end median |
|---|---:|---:|---:|
| HotSpot default, eager safety guard | 2435.4 ms | 2308.0 ms | 2742.5 ms |
| HotSpot with opt-in co-partition | **315.0 ms** | **127.9 ms** | **626.4 ms** |
| Eager O3 | 2436.5 ms | 2309.8 ms | 2744.3 ms |

First output improvement: **7.73x** (about **87.1% less time**)
versus ordinary HotSpot on this frozen workload.

This is a *startup-only* test with the first four frames: it is
not a claim that all later lazy materializations are free, or that
the full execution rate is improved.

## Broader compatibility checks

The 47-workload performance project was also run to its `DONE`
marker in both configurations. All **47 IDs completed**. The
last-column checksums matched for **39/47** workloads; the other
**8/47 were all `sched_*`** workloads involving wall-clock-timed
activity / iteration counts. They differ between runs, so this
test cannot prove semantic equivalence for the scheduler workloads.
`tests/differential/jit_lazy_parfor_partition.py` supplies the
separate, deterministic output-equivalence proof for callbacks.

The expanded `parity.sb3` (SHA256
`63baccef6c7288cfb13778ef9a6015a6ee0e310af78d5378ce6bd3346c12f13b`)
produced **2625/2656** with or without the experimental flag under
the same SDL offscreen setup; the 11 failure groups matched.
The opt-in partition **was not selected** for that project's default
HotSpot configuration. Therefore this comparison does **not**
constitute parity proof for all possible deferred par-for scripts,
nor does it mean the expanded suite is 100% passing.
This 2656-case suite is distinct from the previously verified
2123-case pinned GitHub CI suite.

## Decision / remaining constraints

**Keep the feature opt-in.** The existing direct-function-pointer
guard is still the default. Before enabling automatically:
- test function-address escaping, mixed callbacks with indirect
  references, and live JIT destruction under simultaneous workers;
- run full cross-platform CI, and revisit the missing 31 expanded
  Parity cases separately;
- measure real full-run throughput and the background/first-call
  compilation cost, not just cold launch;
- account for the possibility that CPU-bound workloads pay for
  deferred compilation during their own critical path.

No change to JIT optimization level, par-for algorithm, or Scratch
scheduling behavior is included in this experiment.
