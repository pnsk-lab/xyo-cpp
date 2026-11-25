# LLVM-native repeat optimization experiment (2026-11-10)

## Design

`src/jit/codegen.cpp` now has an **opt-in** `XYO_JIT_LLVM_REPEAT_HINT=1`.
For the existing standard `control_repeat` CFG with a compile-time exact
integer trip count in `[2,64]`, it tags the native backedge with
`!llvm.loop` / `!llvm.loop.unroll.enable`. The LLVM O3 pipeline is responsible
for all code duplication and determining the unroll factor, using the
existing SSA/alias/cost model. No Scratch instruction is cloned by the new
path. The scheduler/yield checkpoints and the regular fixed-count loop
semantics remain represented in the original IR.

The guarded selection excludes block-count mode, OSR entry, protected
native-internal loops, and bounded native inner continuations. No fact about
Scratch variable values or list types is guessed.

This replaces an abandoned *new* source-level mode-2/8-op expansion trial;
the historical explicit `XYO_REPEAT_UNROLL=1` functionality remains intact,
off by default. The LLVM hint itself stays off by default until broader
benchmark and resource impact are confirmed.

## Targeted performance

Same compiled executable, CPU8 pinned, 30fps ScratchBench-derived `.sb3`
projects, HotSpot native release, 6 interleaved ON/OFF pairs, project
checksums equal within each pair:

| Workload | Paired geometric-mean ON/OFF | Wins |
|---|---:|---:|
| app_life | **1.105813** | **6/6** |
| app_raycast | **1.005883** | **6/6** |
| app_ast | **1.010520** | **5/6** |
| list_append | **1.023439** | **4/6** |
| algo_sieve | 1.002956 | 4/6 |
| math_nbody | 1.000591 | 5/6 |
| math_expsqrt | 1.005303 | 2/6 |
| math_collatz | 0.995256 | 2/6 |
| algo_primes | 0.995447 | 1/6 |
| math_mandel | 0.994594 | 0/6 |

Raw: `build/multi-bench-20261110/llvm-hint-ab.json` and `.log`.
The source hash and native binary are unchanged across the two conditions.

## Full-suite caution

An additional single-run 47-case comparison, sequential `off → on`, has
source hash `e8bd5be3e7345088` in both runs. The weighted overall score
was **94,993 → 108,508**, but almost every unrelated item also increased by
10–20%. This is not evidence of a 14% compiler improvement. Such a systemic
shift may reflect machine load/timing variability and requires repeated
interleaved full-suite A/B to exclude confounders. Startup was 456→467 ms,
peak memory 228→308 MB and total run time 34.5→38.2 s in these single runs.
Consequently the hint remains opt-in, not a globally proven gain.

Raw official files:
- `scratch-bench/results/llvm-hint-0-20261110/`
- `scratch-bench/results/llvm-hint-1-20261110/`

## Correctness

The focused test `tests/differential/jit_llvm_repeat_hint.py` compares 11
fixtures (zero/one/minimal/64/65 iterations, fraction, and nested repeat)
under Interpreter, Eager and HotSpot JIT with the hint both enabled and
disabled, all passing. Official GUI Parity (source `d64a28bfd605ed0c`) passed **2656/2656** in all four modes: Interpreter, HotSpot, Eager and AOT. Results: `scratch-bench/results/llvm-repeat-hint-parity-20261110/`.

No commit or push; the modified tree has many unrelated concurrent changes.
