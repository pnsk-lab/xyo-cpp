# ScratchBench algo_sieve: monotonic numeric-only list flag (2026-11-09)

## Goal and workload

Continue PERF-001 (beat the best of MistWarp, TurboWarp, NitroBolt, Scratch Everywhere! and Scratch on all 47 original ScratchBench cases). Original `algo_sieve` initializes a numeric `l_flags` list of 5,000 slots, repeatedly tests `l_flags[k_i] != k_g`, then writes `l_flags[k_j] = k_g` while incrementing `k_j` by `k_i`. All official measurements use the *unchanged* source-derived `sieve.sb3` / unchanged 47-case source hash `e8bd5be3e7345088`.

Isolated probe was added as `sieve` to `tests/bench/algos/scratchbench_probe.py`, without rewriting any benchmark operations.

## Causal diagnosis

Source-level list comparisons and numeric-list speculative hints were not the main bottleneck.

A CPU8-pinned 11-flag x6 rotation on the identical frozen binary (`build/perf-sieve-baseline-frozen-xyo`) found:

| Probe setting | Median ops/ms |
|---|---:|
| Default | 197,734 |
| `XYO_LIST_READ_NULL_CHECK=1` | 204,420 |
| `XYO_JIT_REPLACE_NO_NULL=0` | 191,263 |
| `XYO_JIT_NUMERIC_LIST=1` | 197,641 |
| `XYO_LIST_READ_MEMO=0` | 197,740 |
| `XYO_LIST_STORAGE=adaptive` | 197,572 |
| `XYO_JIT_EQ_STRING_CACHE=0` | 197,792 |
| `XYO_JIT_INDUCTION_RANGES=0` | 198,303 |

All 66 observations returned checksum **669**. However **null-check benefit did not reproduce** in strict 16-pair A/B: OFF 204,519.47, ON 204,341.91 ops/ms, ON only 7/16 wins. Retained the previous null-check default.

A temporary extension to `numericListSiteHint` to include observed-number-only scalar writers plus `XYO_JIT_NUMERIC_LIST=1` gave OFF 197,785.61 / ON 198,077.94 ops/ms (12 pairs, 7/12 wins). Extending numeric-list hints to `emitCmpNumF64` was **slower**: OFF 198,400.99 / ON 196,754.99 ops/ms, ON only 1/16 paired wins. Both speculative read-path source changes were **rolled back**.

## General native JIT optimization

Original `Codegen::invalidateNumericList(list, item)` always computes:

```cpp
list->numericOnly = list->numericOnly && item.isNumber();
```

`SList::numericOnly` is monotonic on `push`, `insert0`, `replace0`: **once false it stays false** until the list is cleared/recreated. For a numeric value, no read or write of the flag is needed. For a nonnumeric value, storing false directly has the same effect, regardless of the previous flag.

With `XYO_JIT_LIST_MONOTONE_FLAG=1`, the native JIT checks `emitIsNum(item)`. If the item is numeric, the metadata flag is untouched; otherwise it stores false. Statically `knownNum` items skip the check entirely. The list item itself is still stored and its monitorDirty flag still updated using the existing code. Mixed lists remain mixed, and list contents/indices are unaffected. `XYO_JIT_LIST_MONOTONE_FLAG=0` retains the original unconditional read/AND/store.

Only `src/jit/codegen.cpp`'s metadata invalidation helper was changed; other agents' math, scheduler and ABI edits were preserved.

## Pinned alternating A/B: same binary, CPU8, 30fps

Frozen native Release executable `build/perf-sieve-flag-frozen-xyo`, SHA256 `88a355ed6018a924358d4a1de7cad5686b3fe51f5e948ddfd8faf2def4f45ba4`. Twelve alternating ON/OFF pairs per workload, HotSpot JIT:

| Unmodified workload | OFF median ops/ms | ON median ops/ms | Relative | ON wins |
|---|---:|---:|---:|---:|
| `algo_sieve` | 197,953.71 | **207,336.40** | **+4.74%** | **12/12** |
| `app_life` (probe counts differently from full suite) | 26.8725 | 26.8580 | −0.05% | 5/12 |
| `list_append` | 639,461.98 | 643,637.25 | +0.65% | 6/12 |

For `algo_sieve` every checksum is **669**, for `app_life` **53**, for `list_append` **11000**, all processes exited successfully. Raw `build/perf-sieve-monotone-ab.{json,log}`. The isolated `app_life` rates reflect a diagnostic fixture's unit of work, and must not be compared numerically to the standard 47-case suite's rates.

## Refined final experiment: replacement-only

The first experimental branch affected both `data_replaceitemoflist` and
`data_addtolist`. Its full 47-case one-run comparison was inconclusive:
ON overall **106,507** vs OFF **106,912** (-0.38%); isolated
`list_append` did not regress, but the full-suite `list_append` run
dropped **671,523 vs 725,456 ops/ms**. The suite also showed ON
`algo_sieve` **200,935** vs OFF **197,293** (+1.85%) and
`list_replace` **522,666** vs **496,805** (+5.21%). Because single
full-suite runs are noisy and append need not be specialized, the final
implementation limits the new metadata branch to **list replacements
only**. List append retains its original code generation regardless
of the flag.

Final restricted frozen executable:
`build/perf-sieve-replaceonly-frozen-xyo`,
SHA256 `d4575ee4fce827f30ea7a10cf9bb5881cce28177ddd67550828966725f1c99ad`.

CPU8 pinned 12 alternating ON/OFF pairs, same binary and unchanged
source-derived projects:

| Workload | OFF median | ON median | Change | Paired ON wins |
|---|---:|---:|---:|---:|
| `algo_sieve` | 198,119.24 | **207,164.15** | **+4.57%** | **12/12** |
| `app_life` diagnostic unit | 27.4390 | 27.3865 | -0.19% | 3/12 |
| `list_append` | 723,471.59 | 724,221.92 | +0.10% | 5/12 |

All checksums preserved (669, 53 and 11000) and exit status 0.
Raw results: `build/perf-sieve-replaceonly-ab.{json,log}`.

Corrected new-target tests with the restriction:
`XYO_JIT_LIST_MONOTONE_FLAG=1` CTest **1/1 PASS 45.51s**,
`=0` CTest **1/1 PASS 41.01s**. With the restricted frozen
executable GUI ScratchBench parity ON **2123/2123** and OFF
**2123/2123**, both **100%**; logs
`build/perf-sieve-replaceonly-parity-{on,off}.log`.

## Correctness

New `testMonotoneNumericFlag` in `tests/unit/test_list_codegen_bounds.cpp`: two lists begin numericOnly. One has numeric, string, numeric replacements in succession and must remain permanently nonnumeric; the other receives only numeric replacement and must retain numericOnly. Check both exact values and metadata under interpreter and Eager JIT. The existing full suite also checks list indexes, aliases, saturation bounds, mutations, cyclic tests and JIT recompilation.

- Build `cmake --build build-perf-fan-only-20261108 --target xyo xyo-tests -j4`: PASS.
- Pre-added-test CTest: flag ON 1/1 PASS 43.03s, OFF 1/1 PASS 41.56s.
- Final corrected numeric-Value fixture CTest: ON **1/1 PASS (44.80s)** and OFF **1/1 PASS (41.82s)**, see `build/perf-sieve-monotone-final-ctest-{on,off}.log`. The first test fixture mistakenly treated Scratch numeric-shadow strings as numeric Values, causing two failures in both interpreter and JIT; arithmetic reporter Values fixed the fixture without weakening the flag assertions.
- Original ScratchBench GUI parity: **2123/2123 (100%)** for BOTH opt-in ON and OFF, see `build/perf-sieve-monotone-parity-{on,off}.log`. Full 47-case suite ON/OFF is also being measured.
- `git diff --check` clean.
- No shared-worktree resets, commits or pushes.

**47/47 is still the goal, not yet achieved.**

## Final default-enabled native checkpoint

The replacement-only JIT optimization is now enabled by default. Set XYO_JIT_LIST_MONOTONE_FLAG=0 to restore unconditional numeric-only flag read/AND/store. List append stays on the existing path. Final frozen native Release executable: build/perf-sieve-default-frozen-xyo (SHA256 2b774787aecbff87d2397081d42ecc6b71a6876d51f9b1e4c659758b51e54fe5).

Final unit CTest with default ON: 1/1 PASS (41.55s), including new interpreter/Eager LLVM numericOnly regression. Log: build/perf-sieve-default-ctest.log.

Original ScratchBench GUI parity with default ON: 2123/2123 (100%). Log: build/perf-sieve-default-parity.log.

Unmodified 47-case performance (one run): overall 108,133; algo_sieve 202,665.58 ops/ms; list_replace 512,731.84; list_append 725,281.53; peak RSS 244.3 MB. Report: scratch-bench/results/perf-sieve-default-final-20261109/performance-xyo-cpp.json.

Saved five-rival comparison: 32/47 wins and 15 losses, scratch-bench/results/perf-sieve-default-rivals-20261109.md and .json. algo_sieve remains at 0.649x the saved MistWarp 312,462.64 ops/ms.

The robust effect is the 12/12 paired CPU8 wins for the isolated unchanged sieve workload (+4.57%). The weighted overall score between experimental ON/OFF differed by only -0.03%, within plausible CPU-load noise. Rank counts compare with historical rival reports and are not simultaneous competitor A/B. 47/47 remains unmet. No commits or pushes.
