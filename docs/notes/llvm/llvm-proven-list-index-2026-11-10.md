# LLVM SSA list indexing with proven for-each integer ranges (2026-11-10)

## Motivation and implementation

The existing `control_for_each` analysis can prove that its induction index is
an exact, positive integer within a static `[1, ceil(bound)]` interval. Its
legacy `finiteIndexBody` and `strictInductionBody` rejected *all* list reads,
however, even when they could not mutate the induction variable. Consequently
`data_itemoflist(values, i)` needed a round trip through numeric boxing and
IEEE double-to-i64 conversion even when LLVM had already emitted the exact
i32 loop counter.

`src/jit/codegen.cpp` now makes two general-purpose, strictly proven changes:

1. `inductionReadOnlyList()` allows `data_itemoflist` and
   `data_lengthoflist` to retain the induction-variable proof **only when**
   the list resolves by exact ID to an existing `kListType` variable with an
   existing SList on the same target. The body must still be free of yields,
   index-variable writes (including name aliases), nested control, opaque
   reporters, other list mutations and side-effectful operations. A missing
   list, mismatched ID, JIT AOT target, or unrelated reporter rejects the
   special case. No assumption is made about the list's element types or
   runtime length.
2. `emitListBounds(d,len)` checks the compiler's *immutable SSA value*
   `intInterval(d)`. For a proven exact interval inside `[1,INT32_MAX]` it
   emits an `fptoui double → i32`, `sub nuw nsw i32,1`, `icmp ult i32,len`
   and zero-extends the zero-based offset for the item pointer. For all
   unproven values, including strings, negative/fractional numbers, large
   numbers and mutated scalar variables, the old bounds path remains.
   It is valid for both list reads and list replacements; no list-length
   constant is assumed.

The new optimization is **ON by default**. `XYO_JIT_LIST_PROVEN_I32_INDEX=0`
restores the original induction proof and bounds lowering for controlled A/B.
Scratch semantics and the original fallback are untouched. No manual unroll,
loop cloning or bespoke vectorization was added.

## LLVM O3 IR evidence

`tests/differential/jit_list_proven_i32_index.py` generates ordinary Scratch
projects with populated lists and compare modes. In the 500-element list
fixture, after O3:

- OFF: `fe_i32` → `uitofp` → `llvm.x86.sse2.cvttsd2si64` → `sub i64` →
  `icmp ult i64` → GEP; **1 actual intrinsic call** in the loop.
- ON/default: the induction variable becomes LLVM's `%indvars.iv` i64 PHI;
  LLVM lowers the range-checked index directly to `icmp samesign ult i64`
  and `getelementptr` using `%indvars.iv`; **0 intrinsic calls**.
- The optimized IR is stored in
  `build/multi-bench-20261110/list-proven-i32/optimized-{0,1,default}.ll`.

This is a result of LLVM's own O3 loop and scalar-evolution optimizations,
made possible by an accurate source-language range proof.

## Performance evidence

Dedicated Scratch benchmark `tests/bench/algos/bench_jit_list_proven_i32_index.py`:
150,000-element prepopulated numeric list, 100 invocations of the original
warp procedure in each measured interval, three intervals per program run,
8 interleaved OFF/ON comparisons, CPU 8 fixed, Eager JIT release, same
binary, timing inside the Scratch project using `daysSince2000`:

| Kernel | OFF median | ON median | Geometric mean speedup | Wins |
|---|---:|---:|---:|---:|
| `list[i]` | 22 ms | 19 ms | **1.1579×** | 8/8 |
| `list[i+1]` | 23 ms | 19 ms | **1.1951×** | 8/8 |
| list length + `list[i]` | 38 ms | 37 ms | **1.0270×** | 8/8 |

All paired program checksums match. The project clock has approximately 1 ms
resolution, which limits precision; the 8-pair geometric-mean estimates
are stronger than any individual reading. Raw results:
`build/multi-bench-20261110/list-proven-i32/benchmark.{json,log}`.

Six additional alternating A/B comparisons of existing diagnostic workloads
(`app_life`, `app_raycast`, `algo_primes`, `algo_sieve`, `list_append`,
`math_mandel`) showed differences around ±1% to a few percent, with large
intra-run timing variation and *no consistent demonstrated regression*.
These results are **not** evidence that unrelated kernels became faster.
Raw `build/multi-bench-20261110/list-proven-i32/other-bench-ab.{json,log}`.

## Correctness

- `tests/differential/jit_list_proven_i32_index.py` covers 12 standalone
  Scratch projects: positive integer, out-of-bounds, short list, fractional
  trip count, zero and negative counts, arithmetic index `i+1`, mixed numeric
  and string list elements, dynamic count, length reporter, `last` index
  reporter, and a body that writes the induction variable (must reject
  the proof). Every project matches the Interpreter under HotSpot and Eager
  JIT with the flag OFF, ON and unset. The optimized IR assertions verify
  that ON/default eliminate the extra i64 conversion call.
- Full unchanged ScratchBench GUI Parity suite (source hash
  `d64a28bfd605ed0c`) with the optimization explicitly ON:
  **2656/2656 PASS** in Interpreter, HotSpot, Eager and AOT.
  `scratch-bench/results/proven-list-i32-parity-20261110/`.
- Rebuilt after switching the default ON; the 12-case differential suite
  also confirms `flag unset` matches the explicit ON behavior and the
  explicit `=0` restores the old IR.

No commits/pushes. Other agents' unrelated changes in this shared working
tree were preserved.
