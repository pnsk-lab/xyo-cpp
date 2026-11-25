# LLVM PHI range forwarding through a subsequent Scratch IF

Date: 2026-11-11. Project: xyo-cpp.

## Problem and implementation

The existing JIT forwards proven exact-integer intervals into a numeric
`typed.merge.number` PHI after an IF/ELSE. However, the next Scratch IF
previously cleared local SSA facts, and an inner list access had to convert
the boxed value back to a 64-bit integer.

`src/jit/codegen.cpp` now forwards the *same immutable numeric SSA producer*
into the true successor of a second IF, but only under strict proof:

- Local variable forwarding must already be alias-safe and within a promoted
  loop. The producer must belong to the immediately preceding basic block,
  be canonical numeric, not a load memo, and have a proven i32 integer
  interval.
- The second IF's condition must pass `readonlyCondition`.
- `readonlyPhiBranch` scans at most 256 Scratch blocks and must find an
  actual read of that variable, allowing only known pure reporters and
  operations that write **other** variables. It rejects the protected
  variable's writes even through different IDs resolving to the same VM slot.
- Procedures, yields, nested controls, opaque blocks, list mutations,
  malformed/cyclic graphs and unknown reporters are rejected.
- The forwarded proof is scoped only to that true BB. The ordinary control
  merge and variable-write invalidation continue unchanged.

This passes better SSA into LLVM O3; it does not rewrite Scratch operations,
manually unroll loops, or assume a list length or element type.

**Default ON**. Set `XYO_JIT_PHI_RANGE_BRANCH_FORWARD=0` for the prior
behavior. Broad `XYO_JIT_IF_PHI=1` is not needed, because the restricted
path activates only for a viable candidate. The separate option
`XYO_JIT_LAZY_TYPED_SSA=1` is still used by the synthetic test to construct
a mixed-type numeric PHI; this task does not turn on lazy typed SSA globally.

## LLVM IR proof

`tests/differential/jit_phi_branch_forward.py` generates six Scratch
projects. Each matches Interpreter in Eager and HotSpot under flag
0/1/unset.

| Subsequent branch | cvttsd2si64 OFF | ON/default |
|---|---:|---:|
| Safe read `list[temp]` | 1 | **0** |
| Write to unrelated `sum`, then read `list[temp]` | 1 | **0** |
| Write to protected `temp` | 1 | 1 |
| Nested repeat | 1 | 1 |
| Prior mixed numeric/string values | 1 | 1 |
| Generic IF-ELSE (not supported) | 1 | 1 |

Actual post-O3 IR and test logs are in
`build/multi-bench-20261110/phi-branch-forward/`.

## Performance

Same release test executable, CPU8 fixed, Eager JIT, Scratch projects with
120,000 numeric elements, 120 warp calls per timed interval, 3 clock samples
per run, 10 interleaved pairs. The Scratch timer has approximately 1ms
resolution; the figures below are paired geometric mean speed ratios.

| Case | ON/OFF | ON faster | OFF median | ON median |
|---|---:|---:|---:|---:|
| Safe second-IF list read | **1.066215×** | 10/10 | 22 ms | 21 ms |
| Other-variable write + read | **1.200124×** | 10/10 | 23 ms | 20 ms |
| Protected-variable write (ineligible) | 0.929904× | 3/10 | 21.5 ms | 22 ms |
| Mixed-type PHI (ineligible) | 1.062186× | 4/10 | 48 ms | 48 ms |

The ineligible cases retain the old conversion path; fluctuating timings
must not be attributed to changed numerical lowering.
Raw: `phi-branch-forward/default-benchmark.{json,log}`.

Six existing ScratchBench-derived diagnostic workloads (6 paired runs each)
showed ON/OFF geometric ratios:
`app_life` 1.0043×, `app_raycast` 1.0125×, `algo_primes` 1.0052×,
`algo_sieve` 0.9995×, `list_append` 1.0033×,
`math_mandel` 0.9965×. All checksums matched; no reproducible large
regression. Raw: `phi-branch-forward/other-bench-ab.{json,log}`.

## Correctness

Official ScratchBench GUI parity, source `d64a28bfd605ed0c`:
**2656/2656 PASS in each** Interpreter, HotSpot, Eager and AOT,
first with explicit flags (`scratch-bench/results/llvm-phi-branch-forward-parity-20261111/`)
and then **default ON without broad IF_PHI**
(`scratch-bench/results/llvm-phi-branch-default-parity-20261111/`).

Existing `jit_phi_integer_range.py` is pinned to
`XYO_JIT_PHI_RANGE_BRANCH_FORWARD=0` so it tests only the earlier
PHI-union operation; the new dedicated test verifies the second IF.

## Build isolation

`src/jit/codegen.cpp` compiled normally, but the complete current Ninja
build was blocked by concurrent, unrelated modifications in
`src/jit/jit.cpp` (`AotMetaArray::empty` and iterator traits).
Those source changes were **not touched**. A separate diagnostic executable
was linked from the newly compiled `codegen.cpp.o` and the existing
previously compiled `jit.cpp.o`, using Ninja's generated link commands:
`build/multi-bench-20261110/phi-branch-forward/xyo-branch-default`.
This did not overwrite the shared `libxyo.a` or `xyo` executable.
A clean standard rebuild remains required after the unrelated changes are fixed.

Files:
- `src/jit/codegen.cpp`
- `tests/differential/jit_phi_branch_forward.py`
- `tests/bench/algos/bench_jit_phi_branch_forward.py`

Uncommitted and unpushed. Other agents' changes were preserved.
