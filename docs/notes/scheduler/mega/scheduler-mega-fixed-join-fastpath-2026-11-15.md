# Mega Scheduler β — fixed-work join fast path (2026-11-15)

## Summary

**Experimental, default OFF**: the source-certified dynamic
`repeat_until(not(left < right))` form can skip the redundant LLVM
reentry when the post-broadcast live comparison is still true.

Flag:
```
XYO_JIT_MEGA_FAST_VAR_PAIR_JOIN=1
```

This **requires** the existing
`XYO_JIT_MEGA_UNTIL_VAR_PAIR=1` source-eligibility flag and all
relevant sender/bench flags. It is not a general Scratch condition
optimizer, and does not change the original time-bound benchmark.

### Safety contract

The Mega compiler emits optional `FixedJoinPair` metadata in its
`MegaDispatchCode` only after the complete source proof has verified:

- dynamic `BroadcastUntil` inside a bounded outer repeat;
- `operator_not(operator_lt(left, right))` exactly;
- both reporters are direct variable reads; and
- the left-hand variable is exactly the variable reset at the start
  of each outer iteration.

The metadata stores **variable IDs, not pointers**. After all joined
broadcast receivers are complete, the fast scheduler lane resolves
both variables on the live `Target` and evaluates
`xyo_lt(lhs.bits, rhs.bits)` — the exact comparison helper that
the LLVM path would call.

If the expression is **still true** (`left < right`), the LLVM
condition path would execute no Scratch effect and return to the odd
launch PC; the new lane returns to the scheduler directly, preserving
one logical broadcast per turn. Otherwise the unmodified LLVM
condition/outer-suffix/return path executes. Missing or non-scalar
variables also fall back.

Source revision and Thread lifetime checks still run at Mega entry.
No Scratch Value, GC pointer or borrowed Variable pointer is saved
between scheduler passes. The existing JitCtx/return-PC ABI remains
unchanged.

The optimization is opt-in and has no effect on unspecialized sources.

## Instrumentation

`XYO_JIT_MEGA_JOIN_PROFILE=1` enables diagnostic timing in
`src/engine/sequencer.cpp`. Otherwise, no clock reads occur.
The run-end summary includes:
`steps`, `launches`, `polls`, `llvm_calls`,
`step_ns`, `launch_ns`, `poll_ns`, `llvm_ns`.

With 20,000 broadcasts per round, five rounds (100,000 broadcasts,
200,000 scheduler entries), sample results:

| Metric | Without fast pair | With fast pair |
|---|---:|---:|
| LLVM entry calls | 100,001 | 6 |
| Mega step elapsed (instrumented) | ~142 ms | ~124 ms |
| Broadcast launch elapsed | ~81 ms | ~82 ms |
| Join recipient poll elapsed | ~4.7 ms | ~4.5 ms |
| LLVM elapsed | ~12 ms | ~0.12 ms |

Profiling itself has overhead; those timings must not be compared
to uninstrumented elapsed times.

## Performance

Paired whole-process fixed-work runs on CPU 16,
20,000 broadcasts/round, 5 rounds:

| Comparison | Trials | Baseline median | Experimental median | Paired geometric mean speedup |
|---|---:|---:|---:|---:|
| Mega normal vs Mega fast pair (direct AB/BA) | 12 | 286.69 ms | 273.78 ms | **1.0503x** |
| Fiber vs Mega fast pair (separate paired run) | 8 | 265.64 ms | 276.23 ms | 0.9641x |

The direct Mega-vs-Mega comparison alternates the two versions
and verifies correct output on **every run**. All 12 pairs favor fast
pair. Absolute times are whole-process times including JIT
compilation. No claim of faster-than-Fiber execution is warranted.

JSON data:
`build-scheduler-rewrite-20261111/mega-fixed-pingpong-fastpair-direct-20000-12pairs.json`,
`build-scheduler-rewrite-20261111/mega-fixed-pingpong-fastpair-20000-8pairs.json`.

## Tests

- `test_mega_real_pingpong_full.py` with
  `XYO_TEST_MEGA_FAST_PAIR=1`: repeats 0,1,3,7,50,200,2000.
  Correct fixed-work counters, no missing broadcasts, no omitted
  trailing log.
- `test_mega_fixed_pair_safety.py`: swapping comparison operands,
  changing `<` to `>`, introducing a shadow variable, or
  changing the outer reset target all disqualify Mega and preserve
  Fiber results.
- `test_mega_live_pair.py`: numeric-string budgets, fractional
  budgets, and a receiver changing the RHS budget after its counter
  increment. Results match Fiber; no stale RHS is used.
- `test_mega_bench_end_proof.py`: retained source certification and
  negative mutation test suite.
- Release `xyo` builds successfully.

## Remaining

The largest remaining costs are receiver creation / broadcast
launch and general scheduler infrastructure, not LLVM condition
reentry. Full heterogeneous `suite_sched` continuation is not yet
supported. Keep the fast join lane disabled by default until wider
parity/benchmark coverage justifies enabling it.

Changes remain uncommitted and unrelated agent changes are untouched.
