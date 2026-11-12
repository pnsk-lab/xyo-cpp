# Discarded numeric-reporter fact trial — 2026-11-10

## Decision

**Do not retain the production change.** Recording guaranteed numeric producer
facts was correct, but no reliable throughput improvement was established on the
selected unchanged normal workloads. The experimental switch and all three
lowering changes were removed from [codegen.cpp](../src/jit/codegen.cpp).

The adopted [arithmetic-only list projection optimization](llvm-numeric-classify-2026-11-10.md)
is independent and remains enabled by default.

## What was tested

The experiment recorded immutable returned SSA values—not mutable variables—as
Number, finite, nonNaN, integral, with absolute upper bound 4,294,967,295:

- `data_lengthoflist`: an f64 Phi of `uitofp(u32 len)` or +0 for a null list.
- `operator_length`: an f64 Phi of the u32 string length, empty singleton +0,
  or a bitcast of the existing `xyo_strlen` helper result.
- `data_itemnumoflist`: a bitcast of the retained `xyo_list_itemnum` result,
  which is +0 on miss/null or an exactly representable u32 one-based index.

The common producer path used existing `emitCanonKnownNonNaN`,
`noteFiniteUpperBound` and `noteIntegralBox` mechanisms. No memory-purity claims,
fast-math, alias assumptions, helper elision, argument reordering, profiler
changes or scheduler changes were introduced.

### Contract audit

`xyo_strlen` has a syntactic undefined fallback, but its fixed StringLength
[host dispatch](../src/ir/host_effect_abi.hpp) always computes
`Value::num(rt::strLength(left))` and returns true. The fixed operation cannot
reach that failure branch under the current implementation. String coercion may
allocate and profiling may update shared state; the helper was therefore kept.

[List/search wrappers](../src/jit/rt_blocks.cpp) retain materialization/search,
comparison semantics, profiling and statistics. Their u32 length/index contracts
were audited against [the runtime implementation](../src/rt/runtime_helpers.cpp).
Nothing was annotated `memory(none)` merely because the output was numeric.

## Paired performance

Same frozen Release binary, CPU16 pinned, normal warp budgeting, Eager,
`--fps 30`, twelve randomized/reversed blocks. The existing adopted arithmetic
projection was ON in **both** profiles. Original source-derived SB3s and all
checksums were unchanged.

| Workload | Paired geometric-mean ratio | Winning blocks |
|---|---:|---:|
| list_search | 1.0027x | 3/12 |
| str_letter | 1.0042x | 6/12 |
| str_contains | 0.9942x | 5/12 |
| app_ast | 1.0024x | 6/12 |

These small, inconsistent changes are not evidence of an effective optimization.
Search/substring/dispatch costs dominate these cases, and fewer dynamic type
checks do not necessarily shorten their critical paths. No full-suite or aggregate
speedup is claimed for this trial.

[Summary](../build/llvm-helper-effects/reporter-paired/summary.json),
[raw runs](../build/llvm-helper-effects/reporter-paired/runs.json),
[frozen binary/input/driver hashes](../build/llvm-helper-effects/reporter-paired/manifest.json).

## Correctness validation retained as evidence

The [archived trial differential driver](../tests/differential/jit_numeric_reporter_facts.py)
checks **5,016 outputs** across valid, missing-LIST-field and unresolved-list
variants. Each compares interpreter, actually compiled Eager/HotSpot and actually
loaded AOT, with facts ON and OFF. Inputs include:

- Empty/whitespace/numeric/non-numeric strings and original `0017` spelling.
- Number, Boolean, NaN and infinity inputs to string coercion/search.
- Unicode UTF-16 lengths, non-BMP characters, lone surrogates and embedded NUL.
- A long rope string.
- Arithmetic, modulo, reciprocal/sign-of-zero-sensitive operations, and
  comparisons against numeric and lexical/whitespace operands.

The driver preserves lone-surrogate CLI bytes with `surrogateescape` rather than
silently replacing invalid UTF-8 in comparisons. Full `xyo-tests` also passed with
the trial enabled.

[Expanded parity log](../build/llvm-reporter-expanded.log),
[trial-enabled unit log](../build/llvm-reporter-unit.log).

## Historical reproduction only

The trial executable is preserved under the paired-results directory. Normal
production no longer contains `XYO_JIT_NUMERIC_REPORTER_FACTS`. Both reproduction
drivers reject a requested trial when its feature literal is absent, avoiding a
false ON/OFF success after the implementation was removed.

```sh
python3 tests/differential/jit_numeric_reporter_facts.py \
  build/llvm-helper-effects/reporter-paired/runtime \
  --out build/reporter-trial-parity-recheck --aot
python3 tests/bench/algos/bench_llvm_numeric.py \
  build/llvm-helper-effects/reporter-paired/runtime \
  --out build/reporter-trial-paired-recheck --blocks 12 --cpu 16 \
  --profiles projected_base,reporter_facts --baseline projected_base \
  --names list_search,str_letter,str_contains,app_ast
```

The safety audit remains useful for a future common typed-IR contract. It does not
justify adding another default-off optimization knob without measured benefit.
