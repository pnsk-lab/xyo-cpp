# Mega Scheduler β — source-certified native bench_end (2026-11-15)

## Summary

An opt-in source-certified implementation of **the original ScratchBench**
`bench_end("sched_pingpong", 1)` is now integrated into Mega Scheduler's
LLVM outer-repeat completion path. The existing
`XYO_JIT_MEGA_UNTIL_BENCH_BEGIN`, `BENCH_ROUND`, and `OUTER_POST_SET`
flags remain prerequisites. This is **NOT** a generic argument-bearing
Scratch procedure lowering, and it remains **OFF by default**.

Enable only for testing:

```sh
XYO_JIT_MEGA_UNTIL_BENCH_END=1
```

The implementation emits one LLVM call to a synchronous native helper,
`xyo_mega_bench_end(JitCtx*)`. The native helper reads live Scratch
variables, applies the original sequence of Scratch number coercions,
multiplication, division, and JavaScript-style `round`, joins
the output fragments in the source order, and appends the result to
the existing `output` list exactly once.

All intermediate strings are held in C++ `Value` locals while
`Gc::DeferCollect` prevents synchronous allocation-triggered GC.
The guard is released only once the finished string has been anchored
to the live list. This is **not** full LLVM-IR string expression lowering:
the LLVM control-flow graph invokes the C++ helper.

The original project has no definition for the unusual external
`​​log​​ %s` procedure call. The source proof explicitly checks that
no replacement definition exists. Consequently the original call has
no observable effect and the native helper reproduces the list append.
Any definition of this call **disqualifies** the optimized path.

The specialized source proof additionally checks:
- call arguments and prototype argument IDs;
- all nested reporter structure, literal values, variable names and IDs;
- procedure and external logging call sequence, warp flags and side effects;
- the destination `output` list's exact ID and append statement;
- installation-time presence and list type, plus dispatch-time revalidation;
- bounded reporter recursion and no unexpected trailing statements.

Mutated/unsupported code runs through the original Fiber path.
The native flag never silently discards additional Scratch statements.

## Tests

`tests/bench/algos/test_mega_bench_end_proof.py`:

- Original source-certified composite: native Mega installed; live
  `output` list and main variables match Fiber.
- Outer repeats 1 and 3: live output list and round count match.
- Nonzero numeric aggregation values, including negative fractions and
  division by zero: output list matches Fiber.
- Live `b_n` samples: 0, 1, 7, 42, `"hello"`, `"0042"`, -13.5:
  the appended string matches.
- Invalid source mutations with native opt-in ON: operator, separator,
  output list name/ID, argument reporter, variable ID alias,
  added external-log procedure definition, and extra logging side effect
  all reject Mega and fall back safely.
- Missing list target prevents native installation.

`tests/bench/algos/test_mega_real_pingpong_full.py`:
- Uses the **unchanged original `sched_pingpong` procedure body** from
  the complete `sched-8.sb3`, with the original receiver scripts.
- Only replaces its green-flag entrypoint with a direct call and changes
  the test workload's `b_budget` value.
- With `--budget -1`, native Mega installs and the output list and
  main scalar variables match the Fiber run.
- With `--budget 0.5`, native Mega installs and completes all 5 rounds,
  including actual broadcast-and-wait resumes. Both runs produce a
  single log entry; exact wall-clock-derived numerical parity is **not**
  asserted because the runs execute at different times.

Release `xyo` and `xyo-tests` build. The eager-mode unit test suite
reported E2E 99/99 and Scheduler 466/466 before the final
`Gc::DeferCollect` guard; rerun below for final status.

## Remaining

The full `suite_sched` call hierarchy is still unsupported.
Throughput must be compared under fixed-work conditions before
enabling the feature by default. Generic user-defined `bench_end`
procedures are not optimized. The one-off synchronous native helper is
a safe initial specialization, not general-purpose code generation.

All work is uncommitted. No unrelated source files were staged, reset,
overwritten or committed.
