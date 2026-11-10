# OSR-010〜016: independent GitHub Actions native CI split

Base: `work/osr016-v123` at `2ef58f3`; independent branch
`work/osr-ci-v123`. No changes to JIT, interpreter or native runtime.
This task changes the workflow, a single CTest timeout, and adds a
dependency/registration audit.

## Scope

- The regular Ubuntu LLVM23/SDL2/Linux job runs 98 CTests
  (`-E '^(xyo-nested-warp-|xyo-native-simple-deopt-parity$)'`),
  then the existing smoke tests and binary artifact upload.
- A separate 90-minute `linux-nested-osr` job runs 38 native
  continuation/OSR CTests, using LLVM23, Release, SDL2 ON, RSVG OFF.
- This job builds only the CLI and the nine native OSR harnesses, then
  calls `tools/ci/check_native_osr_suite.py` to verify all **38**
  frozen CTest names and their Python/CLI/harness file dependencies.
  The guard fails if a known test is dropped or an unexpected test
  needs a manifest review; its self-tests cover missing, unexpected,
  duplicate and missing-dependency cases.
- The long `xyo-native-simple-deopt-parity` test now has 600s
  CTest timeout instead of 120s to tolerate loaded CI runners.
- Native test job runs at `--parallel 2`; a failed job uploads
  `build-osr/Testing/Temporary/LastTest.log`.
- Other jobs (macOS, Windows, Linux static/nanosvg, ScratchBench)
  are not modified.

## Local verification

The pre-existing OSR-016 independent Release / SDL2 ON run passed
**136/136 CTests** in 890.20s. This CI branch changes no runtime code.

- Python guard `--self-test`: PASS.
- YAML parsed, 7 existing/new jobs confirmed.
- `git diff --check`: PASS.
- CI-equivalent CMake configuration with LLVM 23.1.4 / SDL2 ON /
  RSVG OFF: PASS.
- Selective build (CLI and nine native harnesses): PASS.
- `ctest --show-only=json-v1` inventory: 136 total, **38** dedicated
  and **98** regular CTests; native simple timeout = 600s.
- Audit against a fresh selective build: PASS, all script and
  executable paths exist.

## Partitioned CTest replay

- Dedicated native/OSR partition, compiled from scratch in the new CI
  branch with LLVM23 / Release / SDL2 ON / RSVG OFF: **38/38 PASS**,
  0 failures, 4-way local parallelism, wall **726.66s**.
  `xyo-native-simple-deopt-parity` passed in 82.83s.
  Evidence: `osr-ci-dedicated-ctest.log`.
- Ordinary Linux partition replay using the OSR-016 parent's complete
  Release / SDL2 ON / RSVG ON build: **98/98 PASS**, 0 failures,
  4-way local parallelism, wall **225.23s**.
  Evidence: `osr-ci-regular-ctest.log`.
  The two builds have the same committed production source and differ
  only in renderer configuration. The actual Actions workflow runs
  the ordinary suite with its existing RelWithDebInfo build.
- Total 136 distinct CTests across the two partitions, no missing or
  duplicated tests. All 38 named native tests and their dependencies
  were audited before execution.
- CI-equivalent RSVG-OFF CLI SHA256:
  `e86f7ed1d8f39602883fd2cdd87de7c47847be2b621f30e227340f365458d116`.

Neither the CI configuration nor the new guard has yet been run by a
GitHub-hosted Actions runner. No remote push or release has occurred.
