# Mega Scheduler β — original ScratchBench bench_end boundary audit

2026-11-15. No new opt-in flag, no performance claim. The prior Mega
`bench_round` and outer-final setter experiments remain OFF by default.

## Original SB3 inspection

Source: `sched-8.sb3` from ScratchBench. The actual
`sched_pingpong` procedure consists of:

1. `bench_begin` (non-warp procedure call)
2. outer `repeat(5)` with a clock-bounded broadcast/join and the original
   `bench_round` body
3. `b_chk = b_n` after the outer repeat
4. `bench_end("sched_pingpong", 1)`

`bench_end %s %s` takes the actual Scratch argument IDs `id` and
`opc`. Its sole statement calls `log text %s` with a nested
`operator_join` expression. That expression includes repeated
round/multiply/divide operations over `b_total`, `b_chunks`,
`b_bestrate`, `opc`, and the live `b_chk`. The `log text %s`
procedure calls `​​log​​ %s` and appends the message to the `output`
Scratch list. This is not a pure final scalar suffix.

The original `b_chk` variable reporter is encoded as an SB3
primitive (`[12, "b_n", "b_n"]`), which `src/load/sb3.cpp`
deserializes into a `data_variable` block. The already-implemented
`innerAfterSetter` source proof can recognize that form. It does
*not* authorize skipping the following `bench_end` statement.

## Executable original-source test added

`tests/bench/algos/test_mega_real_bench_end.py` extracts the actual
`bench_end` and transitive `log text` procedure definitions and
their reachable AST nodes from the supplied original SB3. It retains the
original call arguments and Scratch operations, without replacing the
message expression by a synthetic formatter.

Run:

```sh
python3 tests/bench/algos/test_mega_real_bench_end.py \
  --binary build-scheduler-rewrite-20261111/xyo \
  --source build-scheduler-rewrite-20261111/mega-multi-pc/real-sched-multiburst-20261114/sched-8.sb3
```

Results: **4/4 PASS** for initial `b_chk` values 0, 1, 10, 100,
under paired final-set flag toggles. The output list is explicitly
checked against the original expression's observable result, for
example `sched_pingpong | 10 | 5 | 0.5 | 2 | 100`.
Both modes correctly continue to use native Fiber: this is a
**fail-closed regression test**, not a test proving `bench_end`
has been lowered to LLVM. `git diff --check` passes.

## Next implementation boundary

To lower the unchanged real `bench_end` call, first source-certify the
whole parameterized call chain (including the argument binding and
`log text` procedure). The native continuation must evaluate each
reporter in Scratch order, execute the logging primitive, and append
to the live `output` list, with correct GC roots and source revision
checks. Reject every source revision that introduces unsupported
effects. The current Mega return-PC stack only captures constant
numeric suffixes, so it cannot yet represent this call.

Even after this, `bench_begin` and the `suite_sched` call hierarchy
remain separate prerequisites for unchanged full-suite Mega eligibility.
Do not enable the new path by default until real-source A/B validates
fixed-work output and throughput.

This audit and test are uncommitted. Existing unrelated agent changes
were left untouched.
