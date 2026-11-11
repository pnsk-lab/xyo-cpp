# CI: LLVM 18 executes real lazy par-for callbacks

Base: `372bc05`, from `work/parfor-parity-integration-v123`.
Branch: `work/parfor-llvm18-runtime-v123`.

## Previously verified

GitHub Actions #83, commit `372bc05`, **8/8 jobs PASS**.
In particular, the LLVM 18.1.3/nanosvg job compiled the ORC
`CompileOnDemandLayer` compatibility path and passed its native
LLVM IR policy test; Linux fully static also succeeded.

The full ScratchBench job used LLVM 23 and passed:
- Interpreter 2123/2123
- Eager 2123/2123
- HotSpot 2123/2123
- AOT 2123/2123
- Opted-in full-Parity par-for integration 2123/2123, both
  native 8192-iteration worker-pool dispatches, callback co-materialization

**Gap:** the LLVM 18 job did not execute real generated callback machine
code. A build-only/passive LLVM IR policy test cannot prove that the
LLVM 18 `CompileOnDemandLayer` correctly materializes native function
pointers in the same way as LLVM 23 `IRPartitionLayer`.

## New runtime coverage

Extend the existing LLVM 18/nanosvg CI job (Ubuntu 24.04,
`llvm-18-dev`) to selectively build `xyo-cli` alongside the
renderer and ORC policy test.

Run the following four actual CTests:

- `xyo-render-tests` — retains renderer regression coverage
- `xyo-jit-parfor-partition-policy` — fail-closed policy on synthetic LLVM IR
- `xyo-jit-lazy-parfor-callback` — original eager guard with native calls
- `xyo-jit-lazy-parfor-partition` — interpreter/eager/hotspot baseline
  against opt-in lazy, two native callbacks, sequential and 8-worker
  parallel paths, exact Scratch list ordering and counters, plus 8
  independent fresh-process parallel repeats

The last two cases **run on the LLVM 18-compiled xyo executable**, not
on the LLVM 23 artifacts built by another job.

`tools/ci/check_llvm18_parfor_ctests.py` makes the selective CMake
build fail fast if any of the required CTests is absent, duplicated or
missing a built executable. It also checks the `--native` argument
points to an actual `xyo` binary. Five independent self-tests check
its own registration and missing-binary detection.

The test is not a performance benchmark; it validates semantics and
callback/worker lifetime within a normal joined par-for dispatch. It
does not prove that deliberately deleting an active Runtime from a
second thread is safe. The experimental co-partition path remains
**off by default**.

## Verification and expected CI check

Local YAML parse/selection assertions: PASS.
Preflight five Python unit tests: PASS.
`python3 -m py_compile`: PASS.
`git diff --check`: PASS.

The actual LLVM 18 callback runtime test must be checked in the new
GitHub Actions run; successful LLVM 23 CI alone is insufficient.
