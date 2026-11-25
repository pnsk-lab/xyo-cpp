# CI handoff: native continuation and OSR suite (2026-11-09)

## Verified local results

- Current remote main: ce238c7; locally uncommitted suite not published
- LLVM23, SDL2, Release; CTest inventory: 153 total, 38 dedicated, 115 regular
- Regular Linux: 115/115 PASS, 302.18s with --parallel 4
- Native continuation + OSR: 38/38 PASS, 666.01s with --parallel 4
- QA-001 test oracles repaired; native-simple TIMEOUT increased to 600s
- .github/workflows/ci.yml registered 90-minute job; --parallel 2 configured
- Added CTest JSON preflight to verify all 38 required executables and Python fixtures before lengthy test execution; `board/qa001-ci-preflight.log` PASS

## Source control prerequisites

- Dedicated CTests reference 22 distinct checked-out scripts.
- Untracked scripts needed by at least one dedicated test: 7.
- Tracked but modified scripts used by dedicated tests: 1.
- Missing scripts in local tree: 0.
- All modified or untracked src/tests entries in shared tree: 146.

A remote GitHub workflow cannot validate the local results until ALL referenced
tests, their supporting native harnesses, and their runtime changes are published.
Avoid committing the entire shared dirty tree; coordinate ownership first.

### Untracked scripts required by dedicated test cases

- `tests/differential/jit_nested_warp_bounded_inner_deopt.py`
- `tests/differential/jit_nested_warp_dynamic_inner_deopt.py`
- `tests/differential/jit_nested_warp_fractional_boundary.py`
- `tests/differential/jit_nested_warp_inner_yield_deopt.py`
- `tests/differential/jit_nested_warp_quad_yield_deopt.py`
- `tests/differential/jit_nested_warp_topology_failclosed.py`
- `tests/differential/jit_nested_warp_triple_yield_deopt.py`

### Tracked but edited scripts required by dedicated tests

- `tests/differential/native_simple_resume.py`

### Native harness C++ dependencies

- `tests/platform/nested_warp_silent_edit_smoke.cpp` (tracked; target `xyo-nested-warp-silent-edit-smoke`)
- `tests/platform/nested_warp_inflight_deopt_smoke.cpp` (tracked; target `xyo-nested-warp-inflight-deopt-smoke`)
- `tests/platform/nested_warp_inflight_body_edit_smoke.cpp` (tracked; target `xyo-nested-warp-inflight-body-edit-smoke`)
- `tests/platform/nested_warp_bounded_inner_deopt_smoke.cpp` (UNTRACKED; target `xyo-nested-warp-bounded-inner-deopt-smoke`)
- `tests/platform/nested_warp_inner_yield_deopt_smoke.cpp` (UNTRACKED; target `xyo-nested-warp-inner-yield-deopt-smoke`)
- `tests/platform/nested_warp_dynamic_inner_deopt_smoke.cpp` (UNTRACKED; target `xyo-nested-warp-dynamic-inner-deopt-smoke`)
- `tests/platform/nested_warp_triple_yield_deopt_smoke.cpp` (UNTRACKED; target `xyo-nested-warp-triple-yield-deopt-smoke`)
- `tests/platform/nested_warp_quad_yield_deopt_smoke.cpp` (UNTRACKED; target `xyo-nested-warp-quad-yield-deopt-smoke`)
- `tests/platform/nested_warp_topology_edit_failclosed_smoke.cpp` (UNTRACKED; target `xyo-nested-warp-topology-failclosed-smoke`)

The OSR-specific CI build requires these 9 targets; untracked harnesses
must be included in the integration commit before Actions can compile.

### Critical integration files

- `.github/workflows/ci.yml`: native continuation job and regular exclusion;
  also contains unrelated concurrent ScratchBench updates
- `tests/CMakeLists.txt`: contains OSR + RT/IR harness changes owned by several agents
- `tests/differential/osr016_fractional_boundary_tests.cmake`: dedicated OSR-016 test definitions
- `tests/differential/jit_nested_warp_fractional_boundary.py`: OSR-016 fixture tests
- `tests/unit/test_motion_numeric.cpp`: pixel-centre hull oracle fix
- `tests/unit/test_nan_codegen.cpp`: signed-zero trig oracle fix

## Evidence

- `board/qa001-ci-regular-final.log`
- `board/qa001-ci-dedicated-final.log`
- `board/qa001-native-simple-ctest600.log`
- `board/qa001-ci-partition-guard.log`
- `board/qa001-ci-preflight.log`
- `board/qa001-ctest-inventory.json`

No changes were pushed.

## Git checkout guard and isolated patch set

To avoid a green local suite hiding uncommitted test dependencies, the
native continuation job now runs an **additional pre-configure gate**:

`python3 tools/ci/native_continuation_checkout.py`

This reads `tests/CMakeLists.txt` and the OSR-016 included CMake definitions.
It enumerates 38 CTests, 9 native harness sources, and 22 Python scripts,
then checks that every dependency exists **and is committed in HEAD**.
The gate itself must also be tracked.

Local validation:

- `python3 tools/ci/native_continuation_checkout.py --allow-uncommitted`: PASS
  (development-only mode), all dependencies physically present
- Strict default: correctly FAIL with **16 untracked files**:
  7 Python scripts, 6 C++ harnesses, 1 OSR-016 CMake include,
  and the checkout audit and unit-test scripts
- Source CTest inventory: 38 dedicated tests, all command/executable dependencies
  checked by the existing post-build `ctest --show-only=json-v1` guard
- Logs: `board/qa001-source-audit-local.log`, `board/qa001-source-audit-strict.log`

Isolated integration patches are prepared without mutating the Git index:

- `board/qa001-ci-isolated.patch` (workflow change + checkout guard and its test suite),
  excludes unrelated ScratchBench workflow edits already present in the
  concurrent worktree. Its CI YAML parses successfully.
- `board/qa001-unit-oracles.patch` (the two unit test reference fixes only).

Both patches were verified using `git apply --cached --check` against
the current Git index (**PASS**); neither was actually applied or committed.
The CI-only patch is not independently deployable: the dependent OSR test
scripts/harnesses and underlying runtime modifications must be integrated
together before GitHub Actions can pass.


### Isolated regression suite for the pre-configure gate

- CI runs tools/ci/test_native_continuation_checkout.py before the audit.
- **13/13 unit tests PASS** in independent temporary Git repositories;
  all temporary Git commits are isolated, not made in the shared repo.
- The guard now uses git ls-tree HEAD to reject staging-only files,
  and git diff HEAD to detect modified committed dependencies.
- A modified tests/CMakeLists.txt is now detected as well as modified
  Python fixtures, removed C++ harnesses, and missing CTest includes.
- Current dirty working tree: **16 untracked required files** and
  **2 modified committed dependencies**, correctly rejected by strict mode.
- Development diagnostics with --allow-uncommitted still verify existence.
- Execution evidence: board/qa001-checkout-unit-tests.log,
  board/qa001-checkout-ci-step.log, board/qa001-source-audit-strict.log.
- The regenerated 3-file CI patch includes the workflow, audit,
  and its 13-test self-check; git apply --cached --check PASS.


## v1.2.3 integration recheck (2026-11-10)

Published `main` moved from `ce238c7` to `d567ae8` (v1.2.3).
A temporary alternate Git index was initialized from `d567ae8`,
then both isolated patches were applied in order without changing the
shared index or worktree:

1. `board/qa001-v123-signed-zero-and-oracles.patch` — five-file
   Scratch `sin(-0)` runtime and QA oracle compatibility correction.
2. `board/qa001-ci-isolated.patch` — CI split, missing-file guard
   and guard regression tests.

Both `git apply --cached --check` and application, followed by
`git diff --cached --check`, passed without conflicts.

The QA patch is necessary in addition to the QA test-only patch: the
published v1.2.3 runtime still returns negative zero from `sin(-0)`,
while the Scratch-compatible test expects positive zero. See the
independent worktree `/home/kicky/dev/cpp/xyo-cpp-qa-v123` and its
`docs/qa-v123-signed-zero-2026-11-10.md` for the minimal three
production code changes plus the two tests.

The existing OSR tests remain uncommitted and must be integrated with
their own source/harness dependencies. Publishing the CI-only workflow
patch before those dependencies would create a failing CI run.
