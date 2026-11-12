# CI handoff: native continuation and OSR suite (2026-11-09)

## Verified local results

- Current remote main: ce238c7; locally uncommitted suite not published
- LLVM23, SDL2, Release; CTest inventory: 153 total, 38 dedicated, 115 regular
- Regular Linux: 115/115 PASS, 302.18s with --parallel 4
- Native continuation + OSR: 38/38 PASS, 666.01s with --parallel 4
- QA-001 test oracles repaired; native-simple TIMEOUT increased to 600s
- .github/workflows/ci.yml registered 90-minute job; --parallel 2 configured

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
- `board/qa001-ctest-inventory.json`

No changes were pushed.
