# Mega Scheduler β — Thread-owned return PC stack

Date: 2026-11-13
Status: opt-in, default OFF, uncommitted, shared worktree

## Implementation

The prior Mega-Yield source proof admitted a single direct argumentless, nonwarp procedure call. It now follows an acyclic, statically known *linear* call chain of up to four procedures, rejecting a repeated proccode, mismatched prototype, arguments, warp mode, reporter calls, and unsupported caller suffixes.

Each caller may have no suffix (tail call) or exactly one source-proven numeric constant variable change as its suffix. Supported innermost bodies remain limited to:
- constant-count nonwarp repeat of constant scalar variable change;
- fixed ASCII broadcast-and-wait followed by one scalar change;
- constant-count nonwarp repeat of fixed ASCII broadcast-and-wait followed by one scalar change.

`Source::returnSuffixes` records the caller continuation sequence outermost→innermost. The LLVM dispatcher `xyo_mega_resume_*` generates return case PCs after the existing `2*slot`/ `2*slot+1` PCs. On the initial entry it stores the continuation PC sequence to `Thread::megaReturnPcs[4]` and sets `Thread::megaReturnDepth`. Every iteration, broadcast join, or scheduler handoff continues with those Thread-owned values.

At terminal inner-body completion, a shared native pop routine checks depth, reads the innermost return address from the Thread stack, and branches via a native LLVM `switch` to the corresponding caller suffix. Each suffix applies its effect once and returns to the pop loop. Empty stack finishes; corrupt/overdeep/unrecognized PCs terminate conservatively without dereferencing an out-of-range slot. This unwinds the return chain within the *same* LLVM entry, with no extra scheduler tick for returns.

ABI: `YieldEntry(JitCtx*, uint32_t* pc, uint64_t* remaining, uint32_t* returnPCs, uint32_t* returnDepth)`. The added Thread fields are **append-only** at the end of the existing Thread state layout, to preserve prior member offsets. On a source edit, pre-existing source revision checks prevent replaying already committed effects; variable binding stamps still invalidate the JIT variable lookup cache.

The implementation is source-specialized, not a fully general dynamic call stack. It does not implement arbitrary recursive calls, runtime argument values, branches in callers, arbitrary `CallFrame`, or scheduling the next Thread from the same LLVM invocation.

## Targeted regression coverage

`tests/unit/test_scheduler.cpp::testMegaNonTailProcedureReturnPc` now covers:

- Two distinct non-tail repeat scripts and independent PCs/counters
- Broadcast-and-wait with a caller suffix
- Zero-repeat short circuit and caller suffix
- A two-level non-tail chain with two pending return PCs, with interleaved yields
- A nested broadcast and wait
- A nested tail-only intermediate procedure, which uses no return slot
- Four non-tail frames, exactly the stack capacity
- Five-level chain rejected from Mega; regular JIT fallback remains executable
- Unsupported two-statement caller rejected from Mega
- No native Fiber on specialized paths and same-script second green-flag compiled cache reuse

CLI:
```sh
XYO_JIT_MODE=eager XYO_JIT_MEGA_BATCH_YIELD=1 \
  XYO_JIT_MEGA_BATCH_TRACE=1 \
  ./build-scheduler-rewrite-20261111/tests/xyo-tests
```

All other uncommitted work was left untouched; nothing was committed or pushed.

## Results

Final tested binary in build-scheduler-rewrite-20261111:

- `XYO_JIT_MEGA_BATCH_YIELD=1`: xyo-tests PASS, Scheduler 480 checks / 0 failures
- `XYO_JIT_MEGA_BATCH_YIELD=0`: xyo-tests PASS, Scheduler 419 checks / 0 failures
- Real emitted batch: `[mega-yield] slots=8 definitions=1 states=29`
- Eight proven green-flag Scratch scripts use one LLVM native function
- Two ineligible scripts (unsupported suffix and a five-level chain) stay on the ordinary compiled path
- Nested broadcast-and-wait starts a real three-iteration receiver and saves two return PCs across the join
- Stage variable `s`: 12738 after the first green flag, 25476 after the second, with cached native code
- `git diff --check`: clean

The 29 states are 16 initial/restart PCs plus 13 proven caller return PCs.
There is no performance claim for ScratchBench sched_*: its existing dynamic
source is not eligible for this bounded source proof. Timing improvements must
be measured separately before changing the beta default.
