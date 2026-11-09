# Lazy type-guarded SSA for Scratch values (2026-11-06)

## Purpose

Replace block-shape-specific rewrites with an extension to the existing generic loop-variable promotion. The loop body is lowered normally. No names, benchmark IDs, unroll patterns, known trip counts, or integer-only special cases trigger this feature.

## Implementation

- src/jit/codegen.cpp: ordinary scalar loop promotion already stores boxed Scratch values in local LLVM allocas, which LLVM promotes to SSA/PHI across if/else/while/repeat paths. The existing eager numeric shadow continues to serve variables whose writes are known to be numeric.
- For a promotable variable whose assignments may be strings or other types, XYO_JIT_LAZY_TYPED_SSA=1 allocates a lazy f64 shadow, numeric-comparison eligibility bit and validity bit. The boxed i64 slot remains authoritative. Nothing is converted when a boxed-only consumer reads the value.
- At the first numeric coercion of a particular loaded SSA value, the compiler emits a cache-valid guard. The valid path reads the f64 shadow; the invalid path uses the full Scratch conversion and records the f64 shadow and comparison eligibility. A PHI joins the paths.
- On set-variable-to, a known numeric SSA producer forwards its already-validated f64 value and comparison eligibility into the shadow; an unknown producer invalidates the cache. On change-variable-by, the newly canonicalized numeric result can be forwarded directly.
- On every actual yield, the existing promoted-variable spill/reload protocol publishes boxed values, reads changes from other scripts and invalidates the lazy numeric cache. The normal loop boundary also retains the ordinary write-back. A non-warp loop checks for handoffs every iteration.
- Caches do not assume a monomorphic Scratch variable. Numeric strings, whitespace strings, booleans, NaN, infinities, and unexpected type changes continue to use the generic Scratch conversion and comparison semantics. No floating-point fast-math flags or speculative arithmetic reordering are introduced.

## Configuration

XYO_JIT_LAZY_TYPED_SSA=1 enables the new path; default is OFF until independently selected real SB3s establish a reproducible benefit without memory, compilation, and frame-time regressions. The earlier experimental conditional-SSA and nested empty-loop changes remain independent and default OFF.

## Verification

- tests/differential/jit_lazy_typed_ssa.py builds an independent Scratch project with mixed string/number assignment, repeated numeric consumption, nested if/else, numeric producer forwarding, changes across iterations, and loop lengths 0/1/2/63/64/65/127/129/257/600.
- Interpreter baseline matches eager JIT with the feature OFF and ON, HotSpot ON, O2 ON and type-profile OFF, for both warp and non-warp: 10 configurations passed.
- AOT compilation and execution of both generated projects: 14 compared variables for warp, 14 for non-warp, all equal to the interpreter.
- LLVM O3 optimized IR shows typed.ssa.number PHIs and typed.ssa cached/conversion control flow. That confirms generated use, not a claimed speedup.
- The current working tree includes concurrent GUI changes; a GUI compilation error unrelated to this optimization prevents a clean CLI rebuild. The latest libxyo.a has been linked to already-built GUI objects as build/xyo-typed-ssa for functional verification. Neither GUI source nor unrelated files were changed as part of this work.

## Limits

This extends on-demand type shadows for already-promotable loops; it does not make side-effecting or opaque Scratch blocks promotable, and does not invent a new whole-function deoptimization engine. If a loop has an opaque reporter, calls an unknown procedure, or has unpredictable external side effects, existing analysis declines promotion. To expand further, introduce a first-class side-effect and alias analysis and explicit safepoints; do not attach increasingly narrow benchmark pattern recognizers.

Evaluation should use a frozen held-out corpus of real projects with correctness, frame-time distributions, JIT compile costs, code size and memory. ScratchBench can detect regressions but is not the optimization target.