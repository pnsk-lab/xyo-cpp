# Mega Scheduler β — outer-final variable setter (2026-11-15)

## Scope

After the previous source-certified `bench_round` lowering, the Mega
outer-repeat continuation can now execute a **single final setter that copies
one live Scratch variable into another**, such as `b_chk = b_n`. This
statement executes exactly once after the outer repeat, including zero trips,
and never inside the inner repeat or after a joined broadcast.

The opt-in feature is `XYO_JIT_MEGA_UNTIL_OUTER_POST_SET=1`. It remains OFF
by default. The existing benchmark flags are still required.

The source validator requires exactly one `data_setvariableto` final
statement, one `data_variable` input reporter, a resolved source variable,
and no subsequent statement. Any unexpected opcode, input, or trailing effect
rejects the entire native continuation. The LLVM backend uses
`xyo_var_get` followed by `xyo_var_set` to keep Scratch tagged-value
semantics and wait-variable notifications. Both variables are checked in the
same install-time and dispatch-time gates as the existing setters. The older
numeric outer suffix is unchanged when the feature is disabled.

## Verification

- Release `xyo` target builds successfully.
- `tests/bench/algos/test_mega_outer_final_set.py`: 0, 1, and 3 outer
  repetitions have matching Fiber/Mega output; zero trip still performs the
  final copy once.
- With `--source <original sched-8.sb3>`, the original ScratchBench
  `bench_round` AST is grafted into the test and runs together with the
  final outer setter. The two executors agree on `b_chk`, `b_n`,
  `b_round`, and `sum`.
- Adding an unsupported trailing statement or replacing the value reporter
  correctly rejects Mega and falls back to native Fiber.
- Existing original-`bench_round` parity suite still passes.
- `xyo-tests scheduler` returned exit 0.
- `git diff --check` passed.

## Limitations

This does **not** make unchanged real `sched_pingpong` Mega-eligible:
the actual final setter is followed by `bench_end(id, opc)`. That call has
observable effects and arguments and must be source-certified and executed,
not skipped. The caller hierarchy reaching `suite_sched` remains unsupported.
No end-to-end performance improvement is claimed.

All modifications were left uncommitted and the unrelated working-tree
changes were not reset, staged or committed.
