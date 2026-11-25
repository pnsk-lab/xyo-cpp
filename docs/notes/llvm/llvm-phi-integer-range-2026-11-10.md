# LLVM exact-integer interval across typed numerical branch PHIs

Experiment date: 2026-11-10 / 2026-11-11 JST. Project: xyo-cpp.

## What changed

Existing JIT local SSA forwarding joins boxed `Value` objects from two
Scratch IF predecessors as `typed.merge.bits` (`i64` PHI), and—only when
both incoming producers already have Scratch `toNumber` shadows and dynamic
numeric-comparison eligibility—as `typed.merge.number` (`f64` PHI). That
numeric PHI discarded the *stronger* exact-integer interval facts previously
proved for its incoming SSA producers. Consequently, immediately following
`list[temp]` could revert to a general float-to-i64 conversion even when
both paths assigned bounded integers.

`src/jit/codegen.cpp` now conservatively transfers the union of the two
integer ranges to the merged numerical SSA value **only if** all of these
conditions hold:

1. The existing alias-safe local scalar forwarding produced a real merge
   with *both* reachable predecessors and actual boxed/numeric PHIs. No
   cross-loop or cross-yield proof is manufactured.
2. Both boxed predecessor SSA values have canonical numeric identity
   (`knownNum`), independent `knownNumD` shadows and `knownNumOk` facts.
   A profiled/assumed numeric type is not enough.
3. `intInterval(thenNumber)` and `intInterval(elseNumber)` independently
   establish finite, exact, signed-i32 integer intervals. Their inclusive
   union is `[min(loThen,loElse),max(hiThen,hiElse)]`, always within the
   existing bounded i32 domain. The result is attached only to the new
   immutable numeric PHI, not the Scratch variable.

When all facts hold the compiler records that the merged boxed Value is
numeric and forwards the union to existing users of `intInterval`, such as
`emitListBounds`. LLVM's O3 scalar analysis remains responsible for PHI
simplification, integer conversions and final code generation. There is no
source-level loop expansion, no guessed list length or element type, and
no change to Scratch's arithmetic or string/NaN comparison semantics.

**Default: ON.** Set `XYO_JIT_PHI_INTEGER_RANGE=0` to restore the previous
path. The experiment requires the existing lazy typed SSA path to actually
produce a join; benchmark fixtures explicitly enable
`XYO_JIT_LAZY_TYPED_SSA=1` and `XYO_JIT_LOCAL_VAR_FORWARD=1`. The optional
`XYO_JIT_IF_PHI=1` extends that path to a provably read-only true-only IF.

## IR result

`tests/differential/jit_phi_integer_range.py` builds twelve Scratch projects,
including distinct and identical integer assignments, nested IF, a true-only
IF, and negative controls with negative/zero indices, fractional arithmetic,
Infinity, and string data. Checksums match the interpreter in Eager and
HotSpot, with the flag OFF, ON and unset.

For the direct IF-ELSE with two different bounded integer arithmetic
expressions, the **actual post-O3 IR** contains:

- Before: `typed.merge.number` numeric selection, followed by one
  `llvm.x86.sse2.cvttsd2si64` call, then i64 bounds correction.
- After: the same numeric selection, followed by a native
  `fptoui double to i32` and a direct i32 bounds check; **zero** calls to
  `cvttsd2si64`.
- For an identical integer on either edge, LLVM removes the conversion
  entirely in the optimized result.
- For each negative control (negative/zero interval, numeric string,
  fractional division, or Infinity), the old conversion remains.

IR captures: `build/multi-bench-20261110/phi-integer-range/*-{0,1,default}.ll`.

## Paired, in-project timings

`tests/bench/algos/bench_jit_phi_integer_range.py` measures the original
Scratch code using daysSince2000, after warming a warp procedure. The
project's clock has ~1ms granularity. Same binary, CPU8 affinity,
100,000-element prepopulated list, 100 procedure invocations per measured
interval, 3 intervals per process, 10 alternated OFF/ON pairs.

| Example | Geometric mean ON/OFF speed ratio | Faster pairs |
|---|---:|---:|
| Both branches write a *different* proven integer | 0.999997× | 0/10 |
| Both branches write an *equivalent* proven integer | **1.076924×** | **10/10** |
| A string and an integer on opposite branches | 1.000000× | 0/10 |
| A fractional and an integer on opposite branches | 0.993958× | 1/10 |
| Nonpositive index can arise on a branch | 0.999999× | 2/10 |

All checksums matched. The equal-timestamp and ~1ms timer quantization
limits precision, and machine load affects isolated samples. In particular,
**removing a conversion did not speed up the different-integer PHI case**.
Raw: `build/multi-bench-20261110/phi-integer-range/benchmark.{json,log}`.

A second, experimental native i32 PHI placed a separate `fptoui` on each
predecessor edge, allowing LLVM to eliminate *both* original float-to-integer
conversions in the different-integer case. Despite the cleaner IR, 10-pair
ON/OFF geometric ratio was **0.9951×** for that workload and it did not
consistently outperform the range-only path. This **native i32 PHI variant
was removed**, not left enabled as an unused optimizer flag. Experimental
IR and benchmark captures remain at
`phi-integer-range/*-native.ll` and `native-benchmark.{json,log}`.

Six other ScratchBench-derived diagnostic workloads had ON/OFF geometric
ratios near 1.0 with noisy outliers (`app_life` **0.9991×**, `app_raycast`
**1.0037×**, `algo_primes` **1.0227×**, `algo_sieve` **1.0015×**,
`list_append` **1.0217×**, `math_mandel` **0.9979×**). The limited
sample is not evidence of general speed improvement or a clear regression.
Raw: `phi-integer-range/unrelated-bench-ab.{json,log}`.

## Correctness

- Twelve project fixtures, Interpreter vs Eager and HotSpot, 0/1/default
  config, and post-O3 positive/negative IR assertions: PASS.
- Full ScratchBench GUI Parity, 2656 assertions per Interpreter / HotSpot /
  Eager / AOT, explicitly enabled flag with `XYO_JIT_LAZY_TYPED_SSA=1`:
  **all 2656/2656**. Results in
  `scratch-bench/results/llvm-phi-integer-range-parity-20261110/`.
- A second full Parity validation on the final source code with the feature
  **default ON** and `XYO_JIT_IF_PHI=1` will be captured in
  `scratch-bench/results/llvm-phi-range-default-final-parity-20261110/`.
  **All 2656/2656 PASS** per mode, including with
  `XYO_JIT_IF_PHI=1` enabled alongside lazy typed SSA.

A follow-up fixture reads `list[temp]` inside an additional IF *after* the
numeric join. Its comparison results match the interpreter, but the new CFG
region invalidates the local forwarded scalar fact, so it retains the old
index conversion in that inner IF. This is a **conservative limitation**, not
a claimed performance benefit. Another test compares a numeric and string
candidate after the join and confirms the same fallback.

The compiled project uses LLVM numeric SSA without asserting any new fact
about mutable VM variables, GC references or interpreter state. The default
new behavior can be disabled independently of the previous proven-index,
unary-range and ordered-IF optimizations.

No commit or push. Changes from other concurrent working-tree tasks remain
untouched.
