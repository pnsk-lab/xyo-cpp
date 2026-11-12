# Branch-local exact-integer range propagation after numeric PHIs

Date: 2026-11-11 JST. Project: xyo-cpp.

## Scope

The existing JIT knows a finite, exact signed-i32 range for some immutable
LLVM numeric SSA producers after a Scratch IF/ELSE merge. A subsequent
comparison against a *constant numeric shadow* can prove a narrower
integer range in a successor. For example:

```
if i < N/2: set temp = round(i - N/2)
else:       set temp = floor(i + 1)
if temp > 0:
  change sum by item temp of values
else:
  change sum by item temp of values
```

The joined value spans nonpositive and positive integers. In the positive
true arm it is guaranteed >= 1, allowing existing LLVM `fptoui double to
i32` + i32 list bounds lowering rather than a general
`llvm.x86.sse2.cvttsd2si64` conversion with full signed handling.

Implementation: `src/jit/codegen.cpp`.

### Safety and profitability preconditions

- The operand is the *same immutable, canonical numeric boxed SSA* with a
  proven exact integer interval, not a profile prediction or mutable
  Scratch variable assertion.
- Condition is a direct `operator_gt` / `operator_lt` comparing that
  variable with a finite exact i32 integer primitive shadow. Numeric
  shadows serialized as string Values use the existing Scratch cast rules.
  Literal whitespace, unparseable strings, fractions, NaN, Infinity,
  arithmetic reporters and other opaque conditions are not inferred.
- Both arms of the same IF/ELSE must safely read the original protected
  numeric SSA, as established by the existing bounded no-yield scan.
  The protected variable may not be written (even through a slot alias).
- No following read of the protected variable after the IF/ELSE.
  This is a **profitability** filter, not an additional correctness proof.
- Measured useful conditions only: the comparator means `temp > K` with
  integral `K >= 0`, the preexisting range crosses zero, and the true
  side has a reachable strictly positive range. The syntactically
  reversed form `K < temp` is recognized as equivalent.
- The interval is temporarily attached **only while emitting the true
  successor's LLVM instructions**. It is restored before the false arm
  and after the join. There is no global assumption or altered Scratch
  arithmetic.

Scratch's flexible numeric/string comparison semantics are unchanged.
The compiler exposes already proved range information to normal LLVM O3
optimizations; it does not perform bespoke instruction substitution or
algorithm-specific rewriting.

Default **ON**. Disable using `XYO_JIT_PHI_CONDITION_RANGE=0`.
The test fixture enables the older optional
`XYO_JIT_LAZY_TYPED_SSA=1` and `XYO_JIT_LOCAL_VAR_FORWARD=1` to ensure
the original numeric PHI exists; this feature does not force them on.

## Post-O3 IR results

The fixture corpus in
`tests/differential/jit_phi_condition_range.py` has 25 generated Scratch
projects, covering ordinary and reversed comparisons and adversarial
branches. Each tests Interpreter/Eager/HotSpot, OFF/ON/default.

| Case | OFF `cvttsd2si64` | ON/default | Native `fptoui` change |
|---|---:|---:|---:|
| `temp > 0`, both arms read | 2 | **1** | 0 -> 1 |
| `0 < temp`, both arms read | 1 | 1 | **0 -> 1** |
| `temp < 1`, `1 > temp` | Unchanged | Unchanged | Unchanged |
| Single-arm read | Unchanged | Unchanged | Unchanged |
| Read again after join | Unchanged | Unchanged | Unchanged |
| Fractional limit, string limit, numeric string variable, protected write | Unchanged | Unchanged | Unchanged |

In the reversed case the static `cvttsd2si64` **count** remains one but
the eligible true arm switches to the native i32 conversion, a distinction
observable in the optimized IR dump. Do not infer identical executed
instruction counts from aggregate call counts.

## Paired performance measurements

All measurements use the same Release binary pinned to CPU8. Each generated
Scratch project prepopulates 120,000 list elements and calls a warmed warp
procedure 480 times per measured interval, with three `daysSince2000`
samples per program run. 0/1 settings alternate between pairs, and every
checksum matches. The clock resolution is roughly 1ms.

- `temp > 0`, both arms: **1.0503x**, ON faster 8/8, medians ~104/99 ms.
- `0 < temp`, both arms: **1.0300x**, ON faster 7/8, medians ~104/99 ms.
- `1 > temp`, both arms, unrestricted experiment: **0.9841x**, ON faster
  0/6. **Rejected** by the final profitability gate.
- Single-arm or post-join cases, unrestricted experiment: negative/neutral
  impact. **Rejected** by the final gate.
- After the initial gate (prior to reversed operand support), the six
  unrelated ScratchBench-derived workloads had ON/OFF paired geomeans:
  `app_life` 0.9964x, `app_raycast` 1.0031x,
  `algo_primes` 1.0001x, `algo_sieve` 1.0000x,
  `list_append` 0.9988x, `math_mandel` 1.0025x.
  No clear broad regression or broad benefit.

Raw logs:
`build/multi-bench-20261111/phi-condition-range/benchmark-long.*`,
`benchmark-gated.*`, `swapped-bench.*`, `other-bench-ab.*`.
The other-workload A/B predates the final reversed comparison extension,
whose independent effect was measured in `swapped-bench.*`.

## Correctness

- 25 direct Scratch cases: Interpreter vs Eager and HotSpot with
  `XYO_JIT_PHI_CONDITION_RANGE=0`, `=1` and unset, all PASS.
- Post-O3 IR assertions distinguish native i32 lowering on eligible arms
  from unchanged IR for rejected cases.
- Official full ScratchBench GUI Parity under an earlier enabled version
  (before adding reversed-comparison support): all four modes
  **2656/2656 PASS**.
  `scratch-bench/results/llvm-phi-condition-range-parity-20261111/`.
- Official final default-ON GUI Parity before reversed-comparison support:
  all four modes **2656/2656 PASS**.
  `scratch-bench/results/llvm-phi-condition-default-parity-20261111/`.
- Final post-extension build and test results:
  `build/multi-bench-20261111/phi-condition-range/final-swapped-diff.log`
  and `scratch-bench/results/llvm-phi-condition-swapped-final-parity-20261111/`.

No commit or push. All unrelated concurrent working-tree changes preserved.

## Final post-extension verification

- CMake/Ninja normal Release build: PASS.
- Final 25-case OFF/ON/default differential with Eager and HotSpot,
  including native i32 and CVTT IR assertions: all PASS.
- Full ScratchBench GUI Parity on the final default-ON executable:
  Interpreter 2656/2656, HotSpot JIT 2656/2656,
  Eager JIT 2656/2656, AOT 2656/2656.
- Eight existing differential regression suites: all PASS.
- Final paired 480-repetition benchmark (8 pairs each):
  x > 0 with both branches reading: 1.050253x, ON faster 8/8.
  0 < x with both branches reading: 1.030025x, ON faster 7/8.
  1 > x rejected case: 0.996493x, native index IR unchanged.
  Following read rejected case: 1.003718x, native index IR unchanged.

Final performance samples:
build/multi-bench-20261111/phi-condition-range/final-benchmark.json

Final unrelated workload A/B diagnostics (6 pairs each) were noisy.
Medians of paired speed ratios: app_life 1.0014x,
app_raycast 1.0000x, algo_primes 0.9959x,
algo_sieve 1.0021x, list_append 1.0018x,
math_mandel 0.9922x. Geometric means were 0.9007x,
0.9687x, 0.9927x, 0.9808x, 0.9991x, 0.9867x.
The discrepancy reflects isolated large outliers in different
workloads, including a single app_life pair at 0.5158x and
a raycast pair at 0.8209x. These data cannot establish a
repeatable broad regression or a broad speedup. Earlier paired
samples were much closer to 1.0.

Logs:
build/multi-bench-20261111/phi-condition-range/final-other-bench-ab.log
build/multi-bench-20261111/phi-condition-range/final-swapped-diff.log
build/multi-bench-20261111/phi-condition-range/final-regression-*.log
scratch-bench/results/llvm-phi-condition-swapped-final-parity-20261111/

Uncommitted; not pushed. Other concurrent working-tree changes left intact.
