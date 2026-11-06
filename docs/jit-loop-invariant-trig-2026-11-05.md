# Loop-invariant Scratch trigonometry (experimental, 2026-11-05)

Enable with `XYO_JIT_TRIG_HOIST=1` when running JIT.

## Scope

- Only `operator_mathop` sin/cos with a directly referenced promoted scalar variable.
- The variable must be **read-only inside the candidate loop** and have a numeric shadow.
- The loop body must be straight-line (nested `control_*` constructs disable this optimization).
- The initial rounded Scratch-compatible trig result is calculated in the loop preheader.
- On a scheduler handoff, all promoted variables are reloaded and the cached trig results are recomputed. This is essential because other scripts can modify angles while this script yields.
- Repeated uses of the same variable and operation share the same cached slot.
- `round(sin/cos(pi*x/180)*1e10)/1e10` semantics remain identical.

## Validation

`python3 tests/differential/trig_hoist.py`: seven baseline-vs-enabled cases pass (constant, numeric string, negative zero, nonnumeric string, loop write, zero iterations, concurrent writer).

For `build/trig-memo/bench-warp-large.sb3` (1,000,000 iterations, two trig evaluations per iteration), four runs each:

| Flag | Raw elapsed time (ms) | Median |
|---|---|---|
| 0 | 35.19, 32.13, 30.92, 34.84 | 33.485 |
| 1 | 25.25, 26.44, 25.09, 25.36 | 25.305 |

About 24% lower wall time on this microbenchmark, including process startup and compilation, not a general application speedup.

Optimized LLVM IR places `sin` and `cos` at loop entry and on the scheduler-resume path rather than on the normal backedge.

`build/tests/xyo-tests` currently has four unrelated E2E failures (test_e2e.cpp lines 688, 695, 706, 1067); exactly the same failures occur with the feature disabled.

## Caveats

- Feature is opt-in, **not release enabled**.
- No tests yet for every kind of Scratch numeric value, monitor/clone changes, or all nested control paths.
- Benchmark should be repeated on real 3D rendering SB3 projects; this optimization will not affect a procedure that modifies the angle within the same loop.
- Does not cache based on floating-point approximate equality or suppress any non-warp yield.
