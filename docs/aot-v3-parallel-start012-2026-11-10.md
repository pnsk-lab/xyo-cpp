# START-012 — AOT v3 lazy cache worker dispatch (2026-11-10)

## Purpose

START-011 introduced AOT metadata v3 and deferred `BlockCached` creation. Native code calls `xyo_rt_aot_cached` when referencing a cache slot. The generic `xyo_rt_*` caller-thread affinity rule added `mainThread=true` to scripts that had no other caller-thread effect, reducing CPU worker eligibility.

The goal is to preserve deferred object load without allowing `Blocks::getCachedExecute` to mutate shared caches from a native worker.

## Implementation

- `src/jit/aot_emit.hpp`: while emitting each script, capture every referenced `BlockCached` slot index, including reused indices and callees, and whether the script references a GPU Program. Sort/deduplicate at the script boundary.
- `src/aot/aot.cpp`: persist per-script `lazySlots` and `gpuSlots` only in the experimental v3 metadata. v1 and default v2 remain unchanged.
- `src/jit/codegen.cpp`: the v3 `xyo_rt_aot_cached` indirection itself no longer confers unconditional main-thread affinity; actual compat helpers and other side effects remain pinned.
- `src/jit/jit.cpp`: validate each manifest index/type, associate descriptors with their owning compiled script and preserve a serial-only fallback when loading an *older* v3 object lacking the new metadata. GPU affinity is script-specific for new v3 objects instead of using a whole-object GPU-program check.
- `src/jit/jit.hpp` / `src/engine/sequencer.cpp`: before a statically worker-safe script enters a CPU parallel batch, the scheduler thread materializes all referenced lazy caches. No cache-mutating first resolution is allowed on a native worker. Results are cached in the existing thread-level eligibility record.
- `tests/platform/aot_lazy_cache_smoke.cpp`: repeated and shared slot reference recording checks.
- `tests/platform/aot_lazy_parallel_smoke.cpp`: real compiled AOT v3 with 8 independent warp calculation scripts using lazy `BlockCached` references; assert no cache resolution at load, all cache slots resolved before worker execution, numerical parity, dispatch with XYO_PAR=1 and no dispatch with XYO_PAR=0. Two CTests.
- `tools/bench_start012_full_ab.py`: paired real full-parity runs for v2/v3, strict native-load/oracle check and Linux process VmHWM sampling.

## Verified so far

- Rebuilt `xyo` native CLI and affected tests.
- CTest `xyo-aot-lazy-cache-smoke`, `xyo-aot-lazy-parallel-smoke`, `xyo-aot-lazy-parallel-serial-smoke`: 3/3 PASS.
- Native independent 8-warp test: 16 AOT scripts, 8 lazy worker-eligible scripts; XYO_PAR=1 had 1 actual worker dispatch (8 submitted threads); XYO_PAR=0 had 0 dispatches. Both produced the same per-sprite numeric results.
- Regenerated native v3 ScratchBench parity AOT object: 479 scripts, 8423 slots, 3291 BlockCached descriptors. After this patch, only **180 of 479 scripts** are pinned, compared with 305/479 in the previous START-011 v3. 199 scripts carry one or more cached-slot references; 8 reference GPU-program slots.
- **Real native ScratchBench parity: 2656/2656 PASS**, with `XYO_AOT_LAZY_DIAGNOSTIC=1` and `XYO_AOT_LAZY_EXPECT=1`, rejecting JIT fallback.
- The older 2123-check Scratch fixture also passed native v3 (2123/2123).

## Same-binary v2/v3 follow-up measurement

Both objects were regenerated from the updated shared Release compiler and
the same 2656-test ScratchBench parity SB3. CPU 10 pinned, ordered A/B pairs
alternating direction, native AOT load and full Scratch oracle verified.

**Startup to first output**, 20 paired runs each:

| Metric | v2 default | v3 START-012 |
|---|---:|---:|
| Median startup | 319.8 ms | 293.6 ms |
| v3 wins | — | 20/20 |
| Geometric speedup (v2/v3) | — | 1.0959x |

These absolute durations are specific to the current machine load, build and
measurement harness; do not combine them with START-011's earlier absolute
times. Startup still improved despite v3 adding the script manifest.

**Full 2656/2656 completion**, only 3 paired runs per version:

| Metric | v2 | v3 |
|---|---:|---:|
| Median wall time | 17562.0 ms | 17798.1 ms |
| Median sampled process VmHWM | 371540 KiB | 381316 KiB |
| v3 faster pairs | — | 1/3 |

The wall-time medians indicate a small regression, but one unusually slow
v2 trial (19805.3 ms) makes the geometric average favor v3. This is
insufficient evidence to claim a robust completion-time improvement.
The sampled RSS increased by roughly 9.5 MiB in this run, reversing the
memory trend seen in START-011. CPU worker prewarming may contribute;
further controlled profiling is needed to establish the cause.

Reproduction logs are `build/start012-startup-ab.log` and
`build/start012-full-ab.log`. Measurements:

```sh
python3 tools/bench_start012_startup_ab.py \
  --bin build/xyo --project scratch-bench/.cache/build/parity.sb3 \
  --v2 build/start012-full-v2.o --v3 build/start012-full-v3.o \
  --cpu 10 --pairs 20
python3 tools/bench_start012_full_ab.py \
  --bin build/xyo --project scratch-bench/.cache/build/parity.sb3 \
  --v2 build/start012-full-v2.o --v3 build/start012-full-v3.o \
  --cpu 10 --pairs 3
```

## Limits and non-goals

- The original v3 editor/source mutation problem is still unresolved: a descriptor not previously resolved might fail to find a block after an edit, leaving generated native code with a null pointer. START-012 does not prove source-edit safety. Keep v2 as the default.
- This does not enable a new dependency-scheduling model, remove real renderer/compat/GPU caller-thread affinity, or make unknown side effects worker-safe.
- The `prepareAotWorker` path assumes the scheduler's normal ordering and batch join semantics. Do not invoke it from worker threads.
- Do not reuse AOT objects compiled from a different project or compiler configuration for performance measurements.

## Reproduce

```sh
cmake -S . -B build
cmake --build build -j4 --target xyo-cli xyo-aot-lazy-cache-smoke xyo-aot-lazy-parallel-smoke
ctest --test-dir build -R 'xyo-aot-lazy-(parallel|cache)' --output-on-failure
XYO_AOT_LAZY_CACHE_SLOTS=1 build/xyo aot scratch-bench/.cache/build/parity.sb3 --obj build/start012-full-v3.o
XYO_AOT_LAZY_DIAGNOSTIC=1 XYO_AOT_LAZY_EXPECT=1 XYO_PAR=1 \
 python3 tests/differential/gui_scratch_parity_regression.py \
  --native build/xyo --project scratch-bench/.cache/build/parity.sb3 \
  --aot-object build/start012-full-v3.o --minimum 2656 --modes aot
```

All implementation changes are left uncommitted; existing shared worktree modifications are preserved.
