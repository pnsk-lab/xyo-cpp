# Adaptive list storage experiment — 2026-11-07

## Purpose

Retain flat list's LLVM-JIT fast path until substantial in-place list editing
warrants a non-contiguous representation. The policy is generic and based on
per-list access patterns, not on names of benchmarks or programs.

Set `XYO_LIST_STORAGE=adaptive`. Default `flat` is unchanged.

## State machine

- New list: contiguous `Value*`, JIT loads and stores inlined.
- The list receives a non-edge insertion/deletion when `len >= 256`,
  `index >= 32`, and at least 32 elements are at the right.
- Observe 24 eligible edits. If >=16 of the edits were within 32 indices of
  the preceding eligible edit, migrate to Gap Buffer; otherwise migrate to
  128-item rank-indexed chunks.
- Migration copies the current data once, then releases the old flat buffer.
  `SList::len`, `numericOnly`, `monitorDirty`, and GC-owned strings
  keep their identity and semantics.
- After 64 non-edit item/replace operations on a promoted list, flatten and
  use the original JIT ABI again. A full-list scan (contains/indexOf/contents)
  flattens first, and clear immediately returns to flat.
- A clone receives a new flat buffer until its own edit pattern justifies
  promotion; no storage pointers are shared.

## JIT safety

Mode 7 emits a guard reading `SList::extra` before the native fast paths for
list item, replace and add. Promoted lists branch to the existing Scratch
runtime helpers. List length remains a direct field access.

The promoted-list preheader cache is now enabled for mode 7. An SList
storageEpoch increments on buffer relocation, migration and demotion.
LLVM caches list len, items, numericOnly, and the epoch at the loop
preheader. The loop-promotion scan rejects all list operations that
could resize or change the representation during an uninterrupted segment.
Thus the hot path can directly reuse a non-null cached items pointer.
If the cached pointer was null (non-contiguous phase), the cold path compares
storage epochs and refreshes all cached fields on a transition.
Scheduler yield reentry reloads the full header, including the epoch.
No speculative cached pointer survives a switch that can happen during
the segment. This retains correctness without imposing a per-item epoch load.

Parallel slot writes are disabled for mode 7. Other modes retain existing
code generation and parallel behavior.

## Validation

- `cmake --build build-list-adaptive -j6 --target xyo xyo-tests xyo-list-backend-smoke`
- `ctest --test-dir build-list-adaptive -R '^xyo-list-backend-' --output-on-failure`: 5/5 passed
- `XYO_LIST_STORAGE=flat XYO_JIT_MODE=eager build-list-adaptive/tests/xyo-tests`:
  all existing tests passed, `test_list_limit` 13,317 checks, `GC pool`
  10,943 checks, both 0 failures.

New tests cover switching to gap on clustered edits, switching to chunks on
scattered edits, clone equality, read-triggered demotion, re-promotion, clear,
full random-edit reference comparisons and format-independent list operations.

Existing `xyo-tests` tests that directly dereference `list->items[i]` cannot
be run with a non-contiguous mode. Use the dedicated smoke test for mode 7
and scratch-bench parity checks for end-to-end coverage.

## Performance

Run with the same executable and only vary `XYO_LIST_STORAGE`.

```sh
python3 tests/bench/list_storage_scratchbench.py \
    --binary build-list-adaptive/xyo --runs 2 \
    --modes flat,adaptive,gap \
    --out scratch-bench/results/list-adaptive-20261107
```

The `summary.json` report is written in the results directory.

Interpretation: the adaptive mode performs a guarded native access when
contiguous, and a generic helper operation when promoted. Thus results are
full-runtime measurements, not data-structure-only timings. There is no
benchmark-specific optimization, and values from two samples should be
treated as approximate rather than statistically conclusive.


### Observed results (Release, LLVM HotSpot JIT, 2 runs/mode)

Same executable `build-list-adaptive/xyo`, with only
`XYO_LIST_STORAGE` changed; 47 workloads on the identical project.
Data and raw logs are in
`scratch-bench/results/list-adaptive-20261107/`, with ratio summary in
`summary.json`.

| backend | geometric mean all 47 vs flat | geometric mean list 5 vs flat | overall score | list score | RSS median |
|---|---:|---:|---:|---:|---:|
| flat | 1.000 | 1.000 | 83,179 | 62,518 | 238.15 MB |
| adaptive | 0.987 | 1.399 | 81,896 | 87,469 | 204.35 MB |
| gap | 0.756 | 0.799 | 60,270 | 49,961 | 204.35 MB |

| backend | append | read | replace | middle insert/delete | search |
|---|---:|---:|---:|---:|---:|
| adaptive | 1.517 | 0.870 | 0.953 | 4.224 | 1.009 |
| gap | 1.272 | 0.487 | 0.351 | 3.310 | 0.453 |

Ratios are medians of `ops_per_ms` divided by the flat mode's median;
this geometric mean and the benchmark site's weighted scores are different
metrics. Each operation's own min/max across 2 runs is in the raw JSON.

The adaptive mode substantially improves repeatedly localized middle edits
without imposing the general-purpose read and replace penalty of a permanent
Gap Buffer. Native fast paths remain enabled in the flat state, and the data
structure can revert after reading.

Append has a surprisingly large measured gain despite an additional guard.
Its mechanism has **not** been isolated and may depend on resulting LLVM IR,
optimization or JIT effects; do not attribute it to the storage algorithm.
A larger replicated study and IR comparison are recommended.

For all 39 deterministic workloads, the first-run checksums matched flat.
The 8 time-driven `sched_*` workloads produced non-identical checksums,
as expected from runtime-dependent counters. Therefore this experiment does
not establish deterministic parity for those eight tasks.

### Follow-up work

- Make JIT list pointer promotion aware of storage epochs so read-heavy code
  can retain cached length and pointer when the backing store has not moved.
- Profile actual edit distribution to refine promotion/demotion thresholds.
- Add GC stress and concurrent GPU snapshots during format changes.
- Repeat with more runs, fixed CPU frequency and a broader list workload
  distribution. The current default mode should remain `flat` until
  sufficient performance and compatibility evidence accumulates.
