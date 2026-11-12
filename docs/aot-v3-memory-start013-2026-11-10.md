# START-013 — AOT v3 memory and prewarm investigation (2026-11-10)

## Background

START-012 restored worker eligibility for experimental lazy AOT v3: main-thread-only scripts dropped from 305/479 to 180/479. Startup became faster while maximum resident memory (RSS) sometimes regressed relative to AOT v2. START-013 investigates whether the caller-thread prewarm operation is responsible.

The original shared-workspace parity.sb3 changed during an early measurement, producing an AOT project-hash mismatch and a time-sensitive Scratch audio failure (2655/2656); **this invalid trial was excluded**.

For subsequent measurements, the binary, parity.sb3, and both freshly generated AOT objects were frozen under build/start013-frozen/. Hashes of the binary and project are recorded in sha256.txt. A benchmark is valid only when real native AOT load is confirmed and the Scratch 2656/2656 oracle passes.

## Implementation

- src/jit/jit.cpp: under opt-in XYO_AOT_WORKER_DIAG=1, count prewarm invocations, referenced descriptor visits, first resolutions, recursively generated BlockCached entries, failures, and elapsed time. At engine destruction, count all resolved lazy AOT descriptors.
- src/engine/runtime.cpp: under the same opt-in, count current and retired BlockCached entries before the Targets are destroyed.
- src/engine/sequencer.cpp: skip the prewarm function entirely when a parallel-eligible script has zero lazy AOT descriptors. This removes redundant empty calls, without weakening the safety check for scripts that have descriptors.
- src/core/aot_malloc_diag.hpp: when diagnostics are enabled, inspect glibc mallinfo2(), /proc/self/status and /proc/self/smaps at AOT-load completion and Runtime shutdown. These provide arena accounting, peak/current RSS and VMA categories, not a callsite-level heap trace.
- src/jit/nojit_stub.cpp: provide a no-JIT implementation of prepareAotWorker that conservatively returns false.
- src/engine/sequencer.cpp: stop including the compiler-only LLVM source reporter bridge in no-JIT builds; the previous unconditional inclusion produced an undefined LLVM ABI-breaking-check reference at link time.
- src/jit/jit.cpp: strictly opt-in, glibc-only XYO_AOT_LAZY_MALLOC_TRIM=1 executes malloc_trim(0) after v3 AOT load. It never affects the default v2 path.
- tests/CMakeLists.txt: add trim-enabled native v3 worker/parity smoke test.
- tools/bench_start013_trim_ab.py: alternating A/B measurements on the exact same AOT v3 object, varying only the trim option.

All implementation changes remain uncommitted in the shared worktree.

## Frozen native full-parity cache observations

Both scheduler configurations passed native 2656/2656 Scratch Parity.

| Metric | XYO_PAR=0 | XYO_PAR=1 (before skip-empty) |
|---|---:|---:|
| Lazy v3 descriptors at load | 3291 | 3291 |
| BlockCached entries at load | 0 | 0 |
| Resolved descriptors at shutdown | 3273 | 3273 |
| Active BlockCached entries at shutdown | 33072 | 33072 |
| Prewarm calls | 0 | 1605 |
| Slot visits/first resolutions by prewarm | 0 | 18 |
| Cache entries created by prewarming | 0 | 59 |
| Accumulated instrumented prewarm time | 0 | 53.623 us |

After the skip-empty optimization, the updated shared-build executable loaded the frozen v3 object and passed 2656/2656. Prewarm calls fell to 2, while first resolutions remained 18 and prewarm-created cache entries remained 59. The measured prewarm time was 30.216 us in this run. This shows that prewarming is far too small to explain the roughly 5–9 MiB v3 peak-RSS difference on its own.

## Frozen v2/v3 comparison: complete execution

Alternating A/B, CPU 10 fixed, each result below is the median of **three** valid full-parity runs. Kernel peak VmHWM is sampled during execution, and both versions must pass the full 2656/2656 oracle.

| Allocator/scheduler | v2 wall | v3 wall | v2 peak RSS | v3 peak RSS |
|---|---:|---:|---:|---:|
| glibc, XYO_PAR=0 | 17.681 s | 18.300 s | 373160 KiB | 378456 KiB |
| glibc, XYO_PAR=1 | 17.523 s | 17.638 s | 372972 KiB | 381340 KiB |
| jemalloc via LD_PRELOAD, XYO_PAR=0 | 17.816 s | 18.251 s | 497232 KiB | 424704 KiB |

glibc v3 peak RSS remains higher when parallel prewarming is fully disabled. Jemalloc reverses the v2/v3 RSS ranking, but its absolute memory usage here is higher than glibc. Allocation timing and allocator behavior therefore matter; these results do not prove that fragmentation is the only mechanism. Three pairs are insufficient to claim a small completion-time difference robustly.

Logs: build/start013-frozen/serial-ab.log, parallel-ab.log, jemalloc-serial-ab.log.

## Page-residency breakdown from Linux smaps

Additional exact full 2656/2656 runs, glibc and parallel OFF:

| At shutdown before Target disposal | v2 | v3 | Delta v3-v2 |
|---|---:|---:|---:|
| [heap] RSS | 196372 KiB | 203000 KiB | +6628 KiB |
| Unnamed anonymous mapping RSS | 32820 KiB | 33000 KiB | +180 KiB |
| File mapping RSS | 86572 KiB | 86084 KiB | -488 KiB |
| Total current RSS | 315868 KiB | 322192 KiB | +6324 KiB |
| glibc used heap bytes (mallinfo2) | 92191088 | 92598448 | +407360 B |

The increased RSS is overwhelmingly in the ordinary glibc heap, whereas live glibc allocations differ by only 407360 B. This supports an allocation-order/free-page-residency explanation but cannot identify the precise callsite or free list. v3 has about 31.7 MB of free glibc arena memory immediately after AOT load, compared with only about 19 KB for v2.

## Strictly opt-in glibc malloc_trim experiment

For v3 only, XYO_AOT_LAZY_MALLOC_TRIM=1 requests glibc to return reclaimable pages immediately after native AOT loading. It is not enabled automatically by diagnostics and does not modify v2.

One instrumented native Scratch Parity run (2656/2656 PASS):
- malloc_trim(0) returned 1 and took 629 us.
- RSS fell from 161672 KiB to 130832 KiB: -30840 KiB, or -30.12 MiB.
- [heap] RSS fell from 97504 KiB to 66664 KiB, the same difference.
- glibc used allocation size remained 68829072 B.

Matched A/B with the **same** v3 object, runtime binary and frozen project, CPU 10:

| Metric | Normal v3 | Trim v3 |
|---|---:|---:|
| Startup median (20 pairs) | 264.30 ms | 274.88 ms |
| Trim wins (startup) | — | 5/20 |
| Full run median (3 pairs) | 17469.64 ms | 17651.99 ms |
| Trim wins (full completion) | — | 0/3 |
| Full run peak RSS median | 377652 KiB | 368632 KiB |

The full-run median peak-RSS saving is 9020 KiB (8.81 MiB). In exchange, startup and full-run completion get slower on this workload. Reclaiming pages can make later allocations fault pages back in. **Keep trim disabled by default**, and only consider it explicitly for memory-constrained applications after testing on other projects.

Trim benchmark script: tools/bench_start013_trim_ab.py.
Logs: build/start013-frozen/trim-startup-ab.log and trim-full-ab.log.

## Regression tests and follow-up

- Native LLVM JIT CLI rebuilt; 4/4 AOT lazy-related CTests passed, including trim-enabled native parallel smoke.
- Native no-JIT CLI linked successfully after correcting the LLVM-only header inclusion.
- Full Scratch Parity 2656/2656 passed with trim both disabled and enabled.
- Default AOT remains v2. AOT v3 is experimental, and its source-edit safety remains unresolved. These measurements do not authorize using stale lazy pointers after edits.

Reproduce from /home/kicky/dev/cpp/xyo-cpp:

1. Build native xyo-cli and xyo-aot-lazy-parallel-smoke using CMake.
2. Run CTest with regex xyo-aot-lazy-(parallel|cache).
3. Run tools/bench_start013_trim_ab.py with --bin build/xyo, --project build/start013-frozen/parity.sb3, --aot build/start013-frozen/v3.o, --cpu 10, --pairs 20.
4. Repeat with --pairs 3 --full for the complete Scratch oracle and VmHWM peak RSS.

Do not use an AOT object compiled from a different Scratch project. Do not overwrite or reset unrelated working-tree changes.
