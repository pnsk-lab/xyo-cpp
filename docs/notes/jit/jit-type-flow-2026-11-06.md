# Type-demand-aware SSA optimization (2026-11-06)

This change extends the conservative static type-site analysis already used by the JIT.

- For promoted **write-only** scalars, omit eager numeric shadows and their conversions; retain canonical boxed i64 and normal yield/loop-exit writeback.
- During loop scanning, collect numeric-use sites. Variables with mixed or unknown writes and at least two numeric-use sites can use the existing lazy numeric shadow, with the ordinary validity guards and reload invalidation.
- Forward a locally written SSA value to later reads of the same mixed-type variable in the **same LLVM basic block**, preserving known numeric/string facts. Do not cross CFG edges, yield/reload, or change-variable-by writes. Reject unresolved/aliased variable IDs.
- Numeric-only loops retain their existing eager numeric shadow. No new runtime type assumptions are made.
- `XYO_JIT_TYPE_FLOW=0` disables automatic numeric-demand-based lazy-shadow selection; `XYO_JIT_LAZY_TYPED_SSA=1` can still explicitly enable lazy typed SSA.
- `XYO_JIT_LOCAL_VAR_FORWARD=0` disables new within-block value forwarding, allowing direct A/B evaluation.
- `XYO_JIT_STATIC_TYPE_SITES=0` restores the original permissive producer classifier.

Validation: build of xyo-cli and xyo-tests succeeded. The expanded mixed-type differential test passed 16 configurations (warp/nonwarp; eager/hotspot; type flow OFF/ON), and prior targeted CTest checks passed. Quicksort 4000, mergesort 4000, and micro_read 1M retained identical checksums across forwarding OFF/ON; no general speedup is established by the noisy startup-inclusive timings. A synthetic mixed-type fixture took approximately 0.28s OFF vs 0.23s ON (3 runs each), but it was explicitly designed to exercise this optimization, so the result should not be extrapolated to real projects.

This is local basic-block value forwarding plus loop-level numeric demand analysis; it is **not** a full interprocedural CFG fixed-point type analyzer. Runtime type checks must remain for unknown types and at scheduling boundaries.

Final full CTest: 32/33 passed. The sole failure is xyo-wasm-binaryen-backend; its SB3 generator raises AssertionError for a boolean reporter in a value slot before this optimization runs. This is an unrelated test fixture failure, not proof of an LLVM codegen regression.
