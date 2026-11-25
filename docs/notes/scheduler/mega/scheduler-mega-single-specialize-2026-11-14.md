# Mega Scheduler β — LLVM single-item special entry (2026-11-14)

Status: opt-in, default OFF. All existing multi-agent dirty-worktree files remain uncommitted; no reset or clean.

## Goal and implementation

Existing `XYO_JIT_MEGA_SINGLE_DISPATCH=1` lets exactly one surviving source-certified Mega Thread use the generic `multiPcBatchEntry(items, 1)` or `scalarBatchEntry(values, 1)` rather than constructing a `JitCtx`. It still pays for the generic batched loop.

New opt-in `XYO_JIT_MEGA_SINGLE_SPECIALIZE=1` (requires `XYO_JIT_MEGA_SINGLE_DISPATCH=1`):

- `src/jit/jit.hpp`: typed `MultiPcSingleEntry(MultiPcItem*)` and `ScalarSingleEntry(Variable*)` function pointers, initially null.
- `src/jit/backend_llvm.cpp`: when a source-certified multi-PC batch is created, also creates an ORC-exported one-item wrapper around the *same* `multiPcBatchEntry` with constant `count=1`. It explicitly inlines the generic function using `llvm::InlineFunction` **before O1**, allowing constant propagation, removal of index arithmetic and loop branch. Scalar one-shot batches use a direct one-pointer `load / fadd / store` instead.
- `src/engine/sequencer.cpp`: after all existing source revision, binding generation, finite numeric type, PC/remaining, wait registry, GC, renderer, instrumentation, JitCtx, Fiber and OSR guards pass, a one-item batch uses its specialized pointer when non-null. Multi-item batches are unchanged. Missing specialization simply uses the previous batch function.
- `tests/unit/test_scheduler.cpp`: verifies opt-in symbol presence and independently compares each multi-PC transition (including terminal PC and remaining counter) and numeric scalar update against generic one-item LLVM execution.
- `tests/bench/algos/bench_mega_single_dispatch.py --specialize`: pairs `XYO_JIT_MEGA_SINGLE_SPECIALIZE=0/1` while holding `XYO_JIT_MEGA_SINGLE_DISPATCH=1` and every other flag fixed. The original no-argument benchmark behavior is unchanged.

The specialization does **not** reduce the number of Scratch scheduler passes or yields, nor does it allow an existing native `JitCtx` or `Fiber` to bypass its continuation.

## Opt-in command

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_DIRECT_SCALAR=1 \
XYO_JIT_MEGA_SCALAR_BATCH=1 \
XYO_JIT_MEGA_MULTI_PC_BATCH=1 \
XYO_JIT_MEGA_SINGLE_DISPATCH=1 \
XYO_JIT_MEGA_SINGLE_SPECIALIZE=1 \
./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 0
```

For eligible broadcast receivers add `XYO_JIT_MEGA_BCAST_RECEIVERS=1`.

## Results: per-pair specialization A/B

Frozen Release binary, frozen SB3 (two green flag scripts, 120,000 and 1 repeats), CPU16, Eager O1, process wall-clock including compile and startup. 16 alternating OFF/ON pairs, **only** the specialized-single flag changes.

| Metric | Generic single batch | Specialized |
|---|---:|---:|
| Median wall | 124.337 ms | 122.951 ms |
| Paired geomean speed ratio | 1x | **1.00691x** |
| Specialized wins | — | 11/16 |
| LLVM batch calls | 120001 | 120001 |
| LLVM items processed | 120003 | 120003 |

Raw results:
`build-scheduler-rewrite-20261111/mega-multi-pc/bench-single-special-120k/results.json`

Improvement is ~0.7% and is near run-to-run noise; **not sufficient to enable by default**. The compiled generic and specialized code each perform the same number of actual Scratch body operations.

## Machine code validation

`XYO_JIT_DUMP_OBJECT` captured the ORC-emitted ELF object at
`build-scheduler-rewrite-20261111/mega-multi-pc/single-special-object/edge-2.o`.
`llvm-nm -S` shows:

- `xyo_mega_resume_2_multi_pc_batch`: 0x9e = 158 machine-code bytes.
- `xyo_mega_resume_2_multi_pc_single`: 0x74 = 116 bytes.

`llvm-objdump -d --disassemble-symbols=...` shows the specialized function has direct PC-based dispatch, `vaddsd` and PC/remaining updates followed by `ret`, without the generic count decrement and batch loop. Numeric order is unchanged.

## Tests

- Release `xyo-tests` and `xyo-cli` build: PASS.
- Focused `XYO_TEST_ONLY_STACKLESS_CORO=1 XYO_TEST_ONLY_MEGA_SINGLE=1`: specialized ON **21/21 PASS**; specialized OFF **20/20 PASS**.
- `python3 -m py_compile tests/bench/algos/bench_mega_single_dispatch.py`: PASS.
- `git diff --check`: PASS.
- Full Eager xyo-tests, all relevant Mega flags and SINGLE_DISPATCH=1:
  SPECIALIZE=1: Scheduler 582/582 PASS, exit=0.
  SPECIALIZE=0: Scheduler 580/580 PASS, exit=0.
  The differing check counts are from opt-in-only assertions;
  all other test groups have zero failures.
- Native Fiber, GC deferral and multi-Fiber GC barrier smoke: 3/3 PASS.
- XYO_WAIT_WAKE_REGISTRY=1: numerical results match (v0=120000, v1=2).
  The ORC object symbols do not include the specialized entries.
- Full logs: single-special-full-on.out, single-special-full-off.out
  in build-scheduler-rewrite-20261111/mega-multi-pc.
- Python syntax check and git diff --check: PASS.

Keep default OFF until stable wins on diverse projects are shown.
