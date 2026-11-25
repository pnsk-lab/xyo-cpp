# RT-001: dynamic outer-loop + conditional warp budget handoff (2026-11-09)

## Scope and changes

Extended `tests/differential/native_simple_resume.py` with two real Scratch
fixtures, in addition to the existing certified warp and inherited-warp
budget cases:

- `ssa_budget_join_warp_nested_loop`: declared-warp outer procedure ->
  dynamic counted `repeat(z)` -> declared-nonwarp child -> native
  `repeat(24000)` with Unicode join/GC-managed PHI. On entry, `z=2`;
  the child overwrites `z` with string output during the first loop.
  The restored Interpreter MUST consume the saved parent count rather
  than re-reading the mutated `z`. Expected resumed warp frames: **4**.
- `ssa_budget_join_warp_nested_if_loop`: same source structure but with
  an additional source-verified `control_if` between parent repeat and
  child call. Expected resumed warp frames: **5**. The completed predicate
  must not be re-evaluated during CallFrame reconstruction.
- `ssa_budget_join_warp_recursive`: four recursively active declared-warp
  procedures; three committed IF arms, followed by a 24,000-iteration
  Unicode join loop at the deepest procedure. The budget fires before
  unwinding recursion, so the interpreter receives four procedure frames,
  three condition frames and one counted loop (**8 warp frames**). The
  recursive ancestor call sites must not be replayed.

Both cases compare complete final Scratch variable states with an
interpreter-only execution, not merely GC outcomes or whether a deopt
was reported. They also assert exactly one native budget adoption and
Fiber retirement before full GC.

## Measured isolated regressions

Frozen full-source binary from the prior RT-001 integration:

`/home/kicky/.rt001-gc-warp-handoff-fullsource-frozen-20261109-xyo`
SHA-256:
`d679da135b4a655d1395c8febc31bea64155dd0a305c91792b973cdde28931a2`.

| Case | Interpreter parity | Accepted | Warp frames | GC before | GC after |
| --- | --- | --- | ---: | ---: | ---: |
| nested_loop | PASS | 1 | 4 | 1,049,410 B | 32,130 B |
| nested_if_loop | PASS | 1 | 5 | 1,049,430 B | 32,366 B |
| recursive | PASS | 1 | 8 | 1,049,422 B | 32,582 B |

For each case:

- Corrupt native generated-string SSA word, recompute local fingerprint:
  transfer refused; `requested=1 adopted=0 collected=0`, collection only
  after the native Fiber finishes. Final parity PASS. After finish:
  ~3,487,2xx -> ~32K B reclaimed for nested parent loops;
  6,942,782 -> 32,654 B for recursive warp.
- Soft budget set to 8 MiB: no source handoff requested; parity PASS.
  This also holds for the recursive fixture, which performs four
  complete 24,000-iteration loops if the handoff is refused.
- Generated-SSA exact-root owner disabled with 1 MiB budget: no source
  handoff requested; parity PASS.

## Strict warp recursion depth cap

`ssa_budget_join_warp_depth_overflow` creates 34 declared-warp
recursive procedure activations before allocating >1 MiB of live native
string memory in the deepest procedure. This exceeds RT-001's hard
`kMaxRestoredCallDepth = 32` proof boundary. The native runtime runs
normally but the attempted handoff fails closed:

- Interpreter/native final-state parity: PASS.
- `requested=1 adopted=0 collected=0 reason=source-refused`.
- Full GC occurs only once the original native Fiber completes:
  1,759,376 B -> 32,256 B.

This does not raise the 32-frame transfer cap or treat unsupported
recursion as safely reconstructible.

Runtime code is unchanged; the new tests exercise the existing
`NativeSourceHandoff::simpleInterpreterResume` and
`Sequencer::adoptSimpleNativeSourceHandoff` contracts.

## Boundaries

This is still an opt-in, source-verified `AfterLoopBody` checkpoint
transfer, initiated by the soft managed-GC budget and requiring the
exact generated-SSA root experiment. It is not a general-purpose LLVM
SSA/value reconstruction facility or an RSS/hard-memory budget.
Do not enable unconditional arbitrary warp / post-call deopt based on
these results. Source edits, unknown side effects, non-certified
checkpoints, and pointer provenance mismatches must fail closed.

No commit/push. Preserve the shared uncommitted workspace.

## Full differential verification

Command:

```bash
python3 tests/differential/native_simple_resume.py \
  --native /home/kicky/.rt001-gc-warp-handoff-fullsource-frozen-20261109-xyo \
  --out build/rt001-warp-followup-verified
```

**938/938 PASS**, including the added warp tests, forged/absent-root
fail-closed tests, over-depth refusal and existing RT-001 regressions.
Actual suite log:
`build/rt001-warp-followup-verified.log`.

`python3 -m py_compile` and `git diff --check` for the modified test
harness PASS. This run uses a previously verified full-source LLVM CLI
binary; this follow-up did not rebuild the entire currently changing
shared workspace, nor change production runtime code.
