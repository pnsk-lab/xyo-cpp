# Interprocedural Scalar Effect Summaries (2026-11-06)

## Scope

New `src/analysis/procedure_effects.hpp/.cpp` builds a conservative summary
for a Scratch custom procedure, following nested procedure calls and reporter
inputs. Collected fields:

- Canonical scalar read/write IDs (using the same target ID-first,
  name-fallback rule as the earlier type-flow analyzer).
- Possible list modifications, broadcasts and clone creation.
- Unknown scalar reads/writes (unresolved IDs, unsupported host/extension
  operations, or unresolvable procedures).
- Recursion, body-level yields and bounded-walk truncation.
- A separately tracked, conservatively true **call-level yield risk**.

The analyzer uses the project's normal `getProcedureDefinition(proccode)`
resolution, avoids repeated block traversal, detects recursion by active
proccode, limits work to 8,192 blocks by default, and treats over-limit and
malformed graphs as unknown.

**Important scheduler property:** Even a simple procedure with no explicitly
yielding block may yield when the containing warp exceeds its time budget.
Therefore `callMayYield` remains true. In particular, these effects are
**diagnostic** and do not justify keeping type proofs across arbitrary
procedure calls. Existing CFG transfer still conservatively invalidates type
facts at every call; no type guards have been removed.

## Inspection

```sh
build/xyo analyze some.sb3 --type-flow
```

This prints a per-procedure summary, a bounded list of canonical scalar
names/IDs, and flags for unknown writes, body yield, call yield, recursion,
and truncation. It complements the existing per-script type-set report.

## Verification

- Extended `tests/unit/test_analysis.cpp` with pure scalar writes,
  variable reporter reads, nested procedure calls, list mutation, recursive
  procedure, opaque extension, clone creation and block-budget truncation.
- `cmake --build build --target xyo-cli xyo-tests -j4` succeeds with
  the final effect analyzer and implicit for-each invalidation.
- After the build finished, the five focused CTest checks passed 5/5:
  `xyo-tests`, `xyo-jit-lazy-typed-ssa`,
  `xyo-jit-type-flow-safety`, `xyo-jit-cfg-type-hints` and
  `xyo-jit-type-flow-fuzz`.
- A concurrent all-tests run reported 29 failures out of 37; most
  contained subprocess `Permission denied` while starting build binaries.
  These were not treated as reliable codegen failures. The five affected
  focus tests passed on isolated serial rerun.
- The prior deterministic 24-program differential fuzz test still applies
  to JIT code paths; this change does not alter executable JIT behavior.

## Next work

Interprocedural type guard elimination requires stronger scheduler-aware
control-flow proofs, deoptimization and state-reload boundaries. The present
summaries are designed to support that future analysis without assuming
a syntactically simple procedure is non-yielding.
