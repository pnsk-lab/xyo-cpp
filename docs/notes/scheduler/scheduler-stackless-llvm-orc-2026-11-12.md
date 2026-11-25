# xyo-cpp: LLVM ORC スタックレス・コルーチン統合

Date: 2026-11-12
Status: opt-in experimental native LLVM implementation. Defaults OFF. No commit.

## Implemented

- Env flags: XYO_JIT_FIBERLESS=1, XYO_JIT_CORO_DIRECT=1, XYO_JIT_CORO_SIMPLE_LOOP=1.
- Source-verified eligibility: static Green Flag / broadcast receiver / clone-start hat, one ordinary repeat, one scalar change-by numeric-literal body. Exact finite integral repeat count, 0 through INT32_MAX. No waits, procedures, warp, dynamic reporters, nested calls or compatibility suspensions.
- src/jit/stackless_coro_lower.hpp: converts direct native yield calls into llvm.coro.suspend; builds a heap coroutine frame, final suspension, destroy path, and ORC-exported resume/done/destroy wrappers. The correct enum, llvm::Attribute::PresplitCoroutine, is required for LLVM lowering. The previous string attribute caused unlowered llvm.coro.size and native codegen failure.
- src/jit/codegen.cpp: source-guarded optional direct repeat emission, one lexical yield, no generic warp/deadline branches. No benchmark-name recognition or changes to non-eligible scripts.
- src/jit/backend_llvm.cpp: source check, transform before optimizer, wrapper symbol lookup and lifecycle metadata. Disables lazy code materialization for this narrow native coroutine.
- src/engine/sequencer.cpp: initial JIT entry and later heap-handle resume. Checks source Blocks revision and full Target variable-binding stamp (local/stage identity and generations) before touching a cached native Variable pointer. Completion, external stop, broadcast restart and revision or binding invalidation destroy the coroutine without re-executing committed work.
- src/jit/jit.hpp: append-only coroutine context and compiled-entry metadata, including a self-contained destruction callback in the JitCtx owner.
- src/engine/thread.cpp and runtime.cpp: attach safe coroutine JIT in fiberless mode, retain interpreter fallback for unproven suspending scripts; no new Fiber allocations.
- tests/unit/test_scheduler.cpp and test_cast.cpp: isolated native runtime tests via XYO_TEST_ONLY_STACKLESS_CORO=1; other tests that explicitly require Fiber JIT remain in the usual default suite.

## Measurements

Single fixed 100,000-iteration Scratch scalar repeat fixture, identical native binary and SB3, CPU16 affinity, eager JIT, O1, XYO_YIELD_QMODE=off, 8 alternating ON/OFF paired measurements. Same compiled runtime work and final Scratch value 100000. No VM workload truncation.

| Metric | LLVM with old Fiber | LLVM native stackless |
| --- | ---: | ---: |
| Median process wall-time | 55.44 ms | 34.03 ms |
| Paired geometric speedup | — | **1.6818x** |
| ON wins | — | **8/8** |
| Verified final Scratch count | 100000 | 100000 |

Raw fixture, script, run records, and executable/project SHA256 are under:
build-scheduler-rewrite-20261111/fiberless-coro/
The final A/B JSON is bench-results.json; earlier iterations are preserved in bench-results-before.json, bench-results-post-yieldopt-O3.json, bench-results-O1-old-loop.json.

Earlier generic (non-specialized) coroutine was slower than Fiber:
80.31ms vs 59.52ms at O3; ordinary-yield branches trimmed: 71.12ms vs 57.45ms at O3; O1 generic 64.55ms vs 61.01ms. The source-certified direct single-yield loop produced the current improvement. The new coro O1/O2/O3 unpaired medians were respectively 31.76/32.83/32.83 ms in three trials each; do not generalize from the best tier.

## Tests

To run the precise native coroutine test:

    XYO_TEST_ONLY_STACKLESS_CORO=1 XYO_JIT_FIBERLESS=1     XYO_JIT_CORO_DIRECT=1 XYO_JIT_CORO_SIMPLE_LOOP=1     XYO_JIT_MODE=eager XYO_JIT_OPT_LEVEL=1 XYO_YIELD_QMODE=off     ./build-scheduler-rewrite-20261111/tests/xyo-tests

Latest isolated checks: **20/20 passed**, interpreted_yield_scripts=0, fiber_acquires=0.
Tests cover three exact successive resumes, source revision invalidation, binding erase-and-recreate without Blocks revision change, external stop, fresh reuse, receiver broadcast restart with 2+8 exact effects.
Normal default xyo-tests also PASS (46.35s in latest completed ordinary run; note that later code updates require retest).

## Limitations

The 1.68x result is a SOURCE-SPECIFIC repeat benchmark, NOT an unmodified ScratchBench sched_* result. Current sched_pingpong, sched_fanwait, sched_threads, sched_waituntil and similar complex scheduler kernels use procedures, broadcasting, async waits or dynamic reporters; their Fiber-free mode still relies on conservative interpreter fallback and can be dramatically slower. Do not make these flags default.

Next work: native heap-owned wait/deadline state, start-once broadcast join and resumed join identities; nested procedure frames and GC ownership proof for native SSA across yields. Require matching Scratch ordering, parity, source edits, and sanitizer tests before enabling a general Fiber-free runtime.

All working tree changes are intentionally uncommitted. Never reset or overwrite collaborators' changes.
