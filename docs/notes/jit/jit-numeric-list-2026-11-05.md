# Numeric-list specialized JIT reads (experimental, 2026-11-05)

Enable with `XYO_JIT_NUMERIC_LIST=1 ./build/xyo run project.sb3 --jit=eager`. This remains opt-in.

## Design

- `SList::numericOnly` is monotonic: any nonnumeric write makes it false. Removal and clear do not reset it; clone copies it.
- `SList::replace0`, `push`, `insert0`, interpreter/runtime helpers and JIT fast writers maintain the flag. Parallel writers invalidate it before dispatch.
- `emitListItem` records a numeric guard for valid indexed loads. `emitToNumF64` uses direct numeric unboxing when the flag is true, preserving the normal Scratch conversion for all other values and invalid indices.
- Promoted lists have a numeric flag shadow at loop entry, refreshed after scheduler handoffs.
- A conservative compile-time hint examines the initial list and visible writing expressions and avoids adding a guard to obviously mixed or string lists. This is only a performance filter; the runtime guard remains required for correctness.

## Verification

`python3 tests/differential/numeric_list.py` passes 14 differential cases, including mixed strings/booleans, special indices, mutation, clearing, empty lists and another script writing the list. Non-concurrent cases also match the interpreter.

`build/tests/xyo-tests` with the feature enabled has the same four pre-existing E2E failures in `test_e2e.cpp` (688, 695, 706, 1067); the numeric promotion suite no longer introduces additional failures after callers were migrated to tracked list writes.

An earlier candidate generated an invalid LLVM PHI incoming block and crashed; this was fixed before the reported measurements.

## Benchmarks

20 million list reads, JIT eager, GPU/parfor disabled:

| Input | Baseline | Specialized | Result |
|---|---:|---:|---|
| Numeric-only | ~40.0 ms | ~36.0 ms | ~10% faster by internal benchmark median |
| String-varying | ~224.9 ms | ~224.9 ms | Similar process wall time after compile-time filter |

The timing types differ across rows and should not be compared directly. Original 3D project not yet measured.

## Release caveats

- Public raw `SList::items[index] = v` writes do **not** update the flag. Host code must use `replace0` or explicitly invalidate `numericOnly`. Without an enforced write API, this is a release blocker for unconditional enablement.
- The flag remains false after nonnumeric history, even if contents later all become numeric.
- No comprehensive AOT/GPU/host parity matrix yet. Feature remains disabled by default.
