# MATH-010 — LLVM JIT pow10 table deduplication

Date: 2026-11-08. Owner: agent-math-gpt6.

## Cause and implementation

MATH-007 introduced a read-only table of pow(10, k/20) for integers
k = 0..50. The original emitter recreated the table for EVERY Scratch
10 ^ reporter in a compiled script. LLVM's global optimizer subsequently
merged equivalent unnamed_addr constants, but code generation still
performed repeated host pow calculations, constant construction,
and generated extra intermediate globals.

Modified src/jit/numeric_repeat_float_ssa.inc:

- Lookup math007.pow10.table using mod.getNamedGlobal first.
- Build one 51-element ConstantDataArray on the first occurrence in the
  LLVM module; reuse the same GlobalVariable for later reporter nodes.
- Preserve the exact input guards, 10^ fallback, JIT-only condition
  and all other arithmetic semantics.
- Add diagnostic XYO_JIT_POW10_DUPLICATE_TABLE=1 to reproduce the
  pre-fix table-per-reporter behavior in the SAME executable.
  Deduplication remains default.

No benchmark-name or project-ID special cases were introduced.

## Benchmark

Created tools/bench_math_table_dedup.py. This synthesizes a genuine
Scratch project with 12 distinct 10 ^ reporters inside a pure warp
repeat, then measures process CPU time and peak RSS via os.wait4.
Each of eight groups alternates old/new/new/old or the reverse.
The result digest must be identical for all 32 runs.

An early 16-reporter fixture was INVALID because the compiler's
numeric-repeat parser limits the number of distinct variables to 16,
and the added loop counter exceeded that bound. Its old measurements
are discarded. The corrected 12-reporter fixture is verified to
actually select the optimized JIT path using XYO_JIT_DUMP2=1:
there were 48 occurrences of rep.pow10.table in the IR diagnostics.
The module emitted a single final pow10 global in both configurations,
but with duplication the LLVM optimizer had to coalesce dozens of
generated globals, producing a suffixed name (.24).

Eight-group paired results on the corrected active fixture:

- Median CPU time ratio (new / old): 0.94246, approximately 5.75% less.
- Seven of eight groups had lower CPU time after deduplication.
- Median peak RSS difference: -0.052 MiB, a very small change.
- All 32 observed output digests agreed.
- Individual process CPU time was roughly 40-65 milliseconds; this
  is a small absolute gain and sensitive to host scheduling noise.

Data: build/math010-dedup-active-cost.json,
build/math010-dedup-active-cost.log,
build/math010-active-0.err, build/math010-active-1.err.

The first invalid (non-JIT-specialized) measurement is archived as
build/math010-dedup-cost.json; do not use it as evidence.

## Correctness

The standalone pow10 corpus exercised 241 outputs, all 51 table hits,
ULP-neighbor misses, infinities, NaN, strings, signed zero and
out-of-range values in warp/non-warp Eager/HotSpot execution. All
matched the interpreter:
build/math010-final-pow10-parity.log.

Independent log and exp corpus tests also passed:
build/math010-final-log-parity.log (295 outputs per mode),
build/math010-final-exp-parity.log (241 outputs per mode).

ScratchBench parity, four execution configurations:
- HotSpot JIT 2123/2123
- Eager JIT 2123/2123
- Interpreter 2123/2123
- AOT 2123/2123

Log: build/math010-parity.log.

The unmodified 47-kernel ScratchBench performance project completed
with diagnostic table duplication both enabled and disabled.
All non-scheduler checksums matched, including the three Math table
tests. Sequential rate changes on unrelated kernels showed notable
host load/clock drift, so they should not be interpreted as a
per-feature runtime speed comparison.
Logs: build/math010-full-duplicate.log,
build/math010-full-dedup.log.

## Multi-agent scope

Only the math-owned native SSA include, new benchmark harness and
documentation were edited for this task. No runtime, scheduler,
renderer or other agents' codegen control-flow was changed.
The shared worktree is left uncommitted and unpushed.

## Final unit-test verification

The updated shared worktree was rebuilt with xyo-tests and the full
CTest xyo-tests target passed (1/1 in 49.63 seconds). All earlier
scheduler failures were absent in this test run.
Log: build/math010-ctest.log.

One caveat: external IR optimization already merges the old
per-reporter globals before final machine-code emission; this task
therefore mainly reduces *compiler IR construction and optimization
work*, not the speed of executing a compiled Scratch loop.
