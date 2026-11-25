# Adaptive list read JIT optimization — 2026-11-07

## Scope

Continue the experimental `XYO_LIST_STORAGE=adaptive` implementation without changing the default `flat` mode. No per-benchmark shortcuts or data-dependent semantics.

`src/jit/codegen.cpp` now restores `memoizableListReads` for adaptive lists in read-only promotable loops, and moves the null-list guard out of the contiguous hot path. The loop's cached `items` pointer is validated as non-null; a null cached pointer reaches the cold path, which verifies the actual SList pointer **before** touching `storageEpoch`. A non-contiguous list is sent to the generic helper. When helper reads demote it to flat, a changed storage epoch refreshes all cached header fields before the next direct access. A scheduler yield refreshes the list header as before.

This is safe only for a structurally promotable loop: its body cannot resize or reallocate the list, and cooperative scheduling prevents another Scratch thread from running until a yield. Nothing changes default flat mode's generated fast path.

## Evidence from the actual LLVM JIT

To isolate generated code, the existing `bench_list_read.py` workload `number_varying` was converted to a 100,000-read .sb3 and executed with both storage modes under the same executable, `XYO_JIT_DUMP2=1` (optimized IR) and `XYO_JIT_DUMP_OBJECT=<prefix>` (actual ORC object). The generated IR and objects are under `build-list-read-ir/`. `llvm-objdump -d -r` generated the assembly listings. The relevant procedure is `xyo_script_0_p1`.

| JIT output | flat | old adaptive | new adaptive |
|---|---:|---:|---:|
| IR instructions (whole procedure, static) | 455 | 475 | 493 |
| LLVM conditional branches (static) | 45 | 47 | 49 |
| LLVM PHIs (static) | 43 | 55 | 64 |
| Number memo `read.key` | present | absent | restored |
| Adaptive list fallback | absent | present | present |
| Cold epoch check | absent | present | present |

These static numbers are not dynamic counts and do not imply a direct percentage of runtime cycles. The memoized numeric fast path and the cold epoch check are visible in the generated IR, not assumed from source. The dynamic CPU instruction distribution was not sampled.

## Dedicated 100-million-read benchmark (five to seven process samples)

`tests/bench/list_read_epoch_ab.py` generates the existing algorithm benchmark's four independent .sb3 fixtures, compares saved before/after xyo binaries (both `flat` and `adaptive`), rotates run order, pins the test process to CPU 8, and checks each independent numeric/string checksum. The report is `build-list-read-ir/ab-100m.json` (7 process samples per treatment, each benchmark contains 3 internal timing rounds).

| List value shape | Before adaptive | After adaptive | Flat control |
|---|---:|---:|---:|
| number, same | 206 ms | 172 ms | 162 ms |
| number, varying | 206 ms | 171 ms | 163 ms |
| numeric string, same | 224 ms | 164 ms | 152 ms |
| numeric string, varying | 224 ms | 260 ms | 262 ms |

Repeating numeric strings favor the memo, whereas a varying sequence can suffer due to memo misses. Therefore this is not an unconditional speedup. The JIT's existing `XYO_LIST_READ_MEMO=0` environment toggle can disable number memo when the workload warrants it.

A separate controlled flag A/B on the final adaptive binary, 100 million reads with six processes per mode (CPU 8):
- Same string: memo off 291.5 ms, memo on 199.5 ms.
- Varying string: memo off 286.0 ms, memo on 317.5 ms.

## Scratch Bench

The complete 47-workload benchmark was run on the final binary with the same executable and only `XYO_LIST_STORAGE` varied, two samples per mode. The result lives at `scratch-bench/results/list-read-cached-20261107/summary.json`.

| Category, adaptive / flat | Ratio |
|---|---:|
| All 47 geometric mean | 1.0263 |
| List 5 geometric mean | 1.2934 |
| Append | 1.2452 |
| Read | 0.8745 |
| Replace | 0.9103 |
| Insert/delete | 3.7827 |
| Search | 0.9652 |

The full suite shows more read overhead than the independent microbenchmark. Different code context, JIT compilation effects and environmental variability may contribute. The experiment does **not** establish the exact cost breakdown.

A further isolation experiment copied the original `scratch-bench/performance/project` and selected only the original `@suite_lists()` call, leaving list algorithms and timing code unchanged. Using 5 rotating process samples per treatment and CPU 8, the result is `build-list-read-ir/list-suite-ab.json`:

| Ratio | Before | After |
|---|---:|---:|
| Adaptive / flat for list_read | 0.8675 | 0.9485 |

There was substantial run-to-run variance even in flat, so the magnitude is not statistically conclusive.

## Correctness

The new `tests/unit/test_list_read_memo.cpp` coverage checks both:
- A compiled read loop that is suspended, then has its list promoted from flat to a gap buffer, followed by GC and JIT resumption.
- A compiled read loop that starts with a noncontiguous list and later flattens after repeated reads.

Both compare final Scratch logical values against the interpreter. Dedicated `ctest` cases `xyo-list-backend-*` and `xyo-list-epoch-jit` passed 6/6.

The full Scratch Bench preserved checksums for 39 deterministic workloads; 8 time-dependent `sched_*` checksum fields differed as expected.

## Future investigation

- The adaptive list read still has a non-null cached-pointer branch, and the full-suite read result is slower than flat.
- Consider workload-neutral *feedback-driven* memo activation only after validating the cost of the extra checks; do not specialize to benchmark names or hard-code data patterns.
- Re-run with more samples and controlled system load; 2-run full-suite measurements are noisy.
- Keep `flat` as the default until broad parity and performance evidence supports adaptive.
