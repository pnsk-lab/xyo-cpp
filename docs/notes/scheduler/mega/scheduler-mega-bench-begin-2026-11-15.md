# Mega Scheduler β — original ScratchBench bench_begin LLVM prelude

2026-11-15. Source-certified opt-in implementation, default OFF.

## Implemented

Added `XYO_JIT_MEGA_UNTIL_BENCH_BEGIN=1` to recognize the actual
argumentless `bench_begin` as the first statement of a source-certified
sender (including a proven surrounding argumentless call). It accepts
exactly five in-order `data_setvariableto` statements, each with an
unchanged numeric literal 0:

1. `b_round = 0`
2. `b_total = 0`
3. `b_chunks = 0`
4. `b_bestrate = 0`
5. `b_chk = 0`

Both call and prototype mutation, argument lists, source statement count,
field identity, literal values and statement order must match. The proof
stores the actual variable IDs and names. At installation time and when
forming the Mega batch, all destination variables must still exist.

LLVM emits the five `xyo_var_set` calls in source order at the **start
PC only**, before the first outer repeat iteration, even when repeat(0).
The helpers preserve Scratch tagged-value write semantics and wait
notifications. Joined-broadcast resumes bypass the prelude. No change to
the JitCtx ABI, return PC, or Thread lifetime management.

## Tests

`tests/bench/algos/test_mega_bench_begin.py` grafts both the actual
`bench_begin` and `bench_round` procedure ASTs from the original
ScratchBench `sched-8.sb3`. With an initial value of 27 in all five
prelude variables, it checks:

- 0, 1, 3 outer repeat iterations, each in Fiber and Mega.
- Positive/zero iteration initialization order and observable state.
- Stable variable-output equivalence; a zero-time duration can produce
  NaN in `b_bestrate` in *both* executors.
- Mutated numeric initialization constant: Mega must reject, Fiber runs it.
- Mutated variable destination/order: Mega must reject.
- Extra trailing initializer side effect: Mega must reject and Fiber
  must still perform the additional write.

**6 cases PASS** (including 3 negative source-proof cases).
Release `xyo` target compiles successfully.

Additional run results:
- Previous `test_mega_outer_final_set.py` PASS.
- Previous exact real `test_mega_real_bench_end.py` PASS
  (unsupported `bench_end` continues to fall back).
- Previous exact real `test_mega_bench_round.py` PASS.
- `git diff --check` PASS.

**Scheduler unit regression note:** `xyo-tests scheduler` was
run after rebuilding. It reported 417 scheduler checks and one
failure: `all fanout writer and waiter scripts compile to LLVM`.
Explicitly repeating this test with
`XYO_JIT_MEGA_UNTIL_BENCH_BEGIN=0` reproduced the same scheduler
failure, so the failure does not require the new lowering to be enabled.
The same explicit-OFF test also reported four E2E failures among 99
checks. These failures have not yet been investigated against a clean
baseline; do not claim all regression tests pass.

## Remaining

The full unmodified `sched_pingpong` is still not Mega-eligible:
the final `bench_end(id, opc)` reaches `log text`, custom logging
and an `output` list append, all of which must be executed in their
original order. The `suite_sched` caller chain remains unsupported.
Do not turn this feature on by default or claim any full-benchmark
speedup.

Uncommitted; unrelated agent modifications were neither reset nor staged.
