# Static type-site guided loop promotion (2026-11-06)

The static diagnostic in `src/analysis/type_sites.cpp` is now also used by LLVM JIT loop promotion. Both use `inferInputStorageKinds` for expression producer classification.

## Optimization

Before this change, `valueIsNumeric` assumed that `data_variable`, `data_itemoflist`, and `sensing_answer` were numeric producers. Those expressions may return strings. This caused eagerly materialized `f64` shadows to require Scratch `toNumber` conversions on each assignment, even in loops that only use boxed strings. For example, string concatenation loops could trigger repeated flattening of strings for an unused numeric shadow.

The static classifier is deliberately conservative: only a proven numeric-producing expression gets an eager numeric shadow; unknown producers keep the canonical `i64` boxed value. If enabled, the pre-existing `XYO_JIT_LAZY_TYPED_SSA=1` path can create an on-demand number cache for mixed-type values instead. This preserves Scratch semantics, including numeric strings, whitespace, NaN, and yield/resume.

By default `XYO_JIT_STATIC_TYPE_SITES` is active. Set `XYO_JIT_STATIC_TYPE_SITES=0` to use the former permissive numeric producer heuristic for A/B performance comparisons. The switch is checked only during compilation; generated loops do not contain an extra runtime check.

## Scope and validation

- Applies to the existing generic promotable-loop path, not a benchmark-specific pattern.
- `xyo analyze` lists candidate write sites and source types. These are *not* proof that a variable is monomorphic at runtime, and cannot justify unconditional elimination of a value-type guard.
- Build: `cmake --build build --target xyo-cli -j4` succeeds.
- Full CTest after the JIT classifier change: 32/32 passed.
- The updated analysis unit test passed after adding type-site assertions.
- No validated performance improvement is claimed: an initial `run` test did not exercise the renderer workload, while a `play` attempt timed out. Performance still needs an appropriate controlled workload.
- The shared JIT classifier avoids trusting type guesses from live variable profiling for correctness; unknown values remain boxed or guarded.
