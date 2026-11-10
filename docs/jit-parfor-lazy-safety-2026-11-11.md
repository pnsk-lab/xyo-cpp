# Par-for lazy JIT: prove each LLVM function-pointer Use

Parent: `88e3186` (`work/parfor-lazy-partition-v123`).
Branch: `work/parfor-lazy-safety-v123`.
Default safety guard unchanged; the optimization still requires
`XYO_JIT_LAZY_PARFOR_PARTITION=1`.

## Bug reproduced and fixed

The original opt-in proof inspected `Function::users()` and checked
only the `User*` owning a function-pointer reference. That is not
sufficient. LLVM's opaque pointers allow a single CallBase to contain
multiple distinct `Use` references to the same `Function`.

A real, verified synthetic LLVM module demonstrated the false
acceptance: `xyo_par_for` had its normal callback body in argument 7
but the same callback function was also used as argument 0. The
original `supportsLazyParForPartition()` returned true, even though
the private callback pointer had an additional consumer.

The same issue affected `xyo_par_for` itself when used as its own
argument 0 as well as the callee operand.

The proof now examines **individual Use edges**:

- For `xyo_par_for`, every LLVM `Use` must be exactly a direct
  `CallBase::getCalledOperandUse()`, with 8 arguments.
- For the private callback function, `hasOneUse()` must hold, and
  that sole `Use` must be the native call's argument 7.
- Any indirect function, external callback, second caller, alternate
  argument, global initializer, or additional address consumer fails
  closed and retains the existing eager path.
- The ORC co-partition function still returns `nullopt` for a
  callback that cannot be identified directly; this is a conservative
  whole-module materialization request in the partition layer.

The two policy functions have declarations in
`src/jit/parfor_lazy_partition.hpp` so that a native test can
construct and verify real LLVM IR without injecting mutated IR into
production execution.

## LLVM IR unit regression

New separate executable `xyo-jit-parfor-partition-policy`,
built only for native LLVM JIT. Located in `tests/platform`, rather
than `tests/unit`, since `xyo-tests` automatically glob all
`tests/unit/*.cpp` and already defines `main()`.

Covered cases:

| Scenario | Expected |
|---|---|
| Two private callbacks, direct call, same caller | Accept; partition contains caller + two callback bodies |
| Unrelated lazy function | Partition remains independent |
| Callback pointer stored in global variable | Reject |
| Callback pointer passed to another native function | Reject |
| Same callback referenced by two separate callers | Reject |
| Callback reused as argument 0 in the **same** native call | Reject |
| Dispatcher pointer reused as argument 0 in the **same** native call | Reject |
| Runtime-selected callback via LLVM `select` | Reject; no partial partition |
| External-linkage callback | Reject |
| Dispatcher address escaped through global initializer | Reject |
| Unused par-for dispatcher | Reject opt-in proof |

**Red/green verification:** the additional-argument case was
observed **failing against the old proof** (exit 1), then passing
after the Use-level change (exit 0). The complete native test passed.

## Local test results

LLVM 23.1.4 / Release / SDL2 ON / librsvg OFF:

- Native policy IR suite: **PASS**.
- `xyo-tests`: PASS.
- `xyo-jit-lazy-integration`: PASS.
- `xyo-jit-lazy-parfor-callback` (default eager guard): PASS.
- `xyo-jit-lazy-parfor-partition` (two callbacks): PASS.
- `xyo-jit-phase-profile-parser`: PASS.
- Focused CTest: **6/6 PASS**.
- New `xyo-jit-lazy-parfor-partition-stress`: **64 additional
  fresh-process runs PASS**, each requiring two real worker-pool
  dispatches of 8192 iterations, exact list ordering and final state.
  With the original partition test: **7/7 CTests**.
- The code in `src/engine/pool.hpp` increments the batch completion
  counter after `Task.fn` returns; the dispatching caller waits for
  all task completions before returning. The stress test exercises
  JIT/runtime teardown *after* this synchronous join. It does **not**
  prove that destroying an active Runtime from another thread is safe.

Expanded Scratch Parity:
`parity.sb3` SHA256
`63baccef6c7288cfb13778ef9a6015a6ee0e310af78d5378ce6bd3346c12f13b`.

With SDL offscreen, `--jit hotspot`,
`XYO_JIT_LAZY_MODE=on`, and `XYO_PARFOR=1`:

| Configuration | Result |
|---|---|
| Existing callback guard | 2623/2656 |
| Partition flag enabled | 2623/2656 |

The **12 failure groups** match exactly. No
`parfor-partition-opt-in` trace was observed for this expanded
project, so the result does not itself exercise/prove the new
co-partition path. The dedicated two-callback CTest covers the latter.
This extended suite has pre-existing failures and is distinct from
the pinned 2123-case ScratchBench CI suite.

The earlier five-trial performance result on `performance.sb3`
(`XYO_PARFOR=1`) remains 2435.4ms -> 315.0ms first output
for the experimental path. This safety patch is not itself a
new performance comparison; no claim of additional speedup.

## Scope and remaining work

The default Eager safety path is unchanged. The fix is deliberately
conservative, accepting only precisely proven direct native callbacks.

Before default enablement: cross-platform CI, dedicated test for
active-runtime destruction synchronization, more extensive Parity
comparison where the co-partition path is actually selected, and
full-duration performance measurements. No code has been merged to
`main`, and no new GitHub Actions success is claimed here.
