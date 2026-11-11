# Experimental lazy par-for: retain LLVM contexts through ORC shutdown

Parent commit `862345e`, branch `work/parfor-context-tsan-v123`.

## Race detected

While testing native JIT callback reuse under Clang 23 ThreadSanitizer,
the guarded HotSpot and eager runs passed, but experimental
`XYO_JIT_LAZY_PARFOR_PARTITION=1` failed with exit 66.

The original TSan report showed the **main thread destroying
`LLVMContextImpl`** while an ORC compilation thread was reading
`llvm::ConstantDataSequential::isCString()`.

Ordinary `LLVMBackend::compile` builds an isolated
`ThreadSafeContext` for each lazy script. The partitioned ORC engine
has two asynchronous compile workers. A local script context can be
released before the native module's background compilation has fully
completed; merely submitting a `ThreadSafeModule` is not sufficient
to establish the desired host-side lifetime boundary for this case.

## Narrow fix

`LLVMBackend` now owns a `std::vector<ThreadSafeContext>` declared
**before `lazyJit`**. For a script that actually uses the experimental
partition and a private LLVM context, it retains a shared ownership
copy until the backend is destroyed. C++ member destruction runs in
reverse declaration order: the lazy ORC engine is destroyed before
the retained contexts, so compilation workers can finish while their
LLVM context remains alive.

No context is retained by this new vector for the default guarded
Eager path or for ordinary lazy scripts without native par-for
partitioning. The opt-in flag remains required.

## Diagnostic results

Clang 23, fully ThreadSanitizer-instrumented xyo-cpp:

- **Before** context retention, the first lazy-partition warning:
  `operator delete` in `LLVMContextImpl::~LLVMContextImpl` on the
  main thread versus `ConstantDataSequential::isCString()` on
  an ORC worker, exit 66.
- **After**, the LLVM context destruction/reading warning did **not**
  recur in the same scenario. TSan instead reported a separate race
  between JITLink memory initialization and `xyo_jitVar` reading
  memory from the mapped object.
- Under a *diagnostic-only* suppression of the
  `race:xyo_jitVar` report, TSan reported a potential lock-order
  inversion **inside LLVM ORC/JITDylib**. With only that one suppression
  and `detect_deadlocks=0`, the full native 64-round par-for execution
  completed, with both callback functions co-materialized.
- **This is not a blanket TSan-clean claim**. The JITLink report and
  LLVM lock-order report remain open, and uninstrumented JIT machine
  code limits the coverage of any host-side TSan result.

LLVM 23 Release build and focused CTest:
`xyo-warp-clock-init-smoke`,
`xyo-jit-parfor-partition-policy`,
`xyo-jit-lazy-parfor-partition`,
`xyo-jit-lazy-parfor-longrun`: **4/4 PASS**.

## Startup and memory checks

Ten alternating A/B pairs with CPU 0 pinned and otherwise identical
LLVM 23 Release builds, with the same experimental opt-in enabled:

| Workload | Metric | Before | Context retention |
|---|---|---:|---:|
| Native 64-round par-for (4096 items per loop) | median wall time | 60.30ms | 58.98ms |
| Same project | median max RSS | 90,350KiB | 90,582KiB |
| ScratchBench performance fixture, first four frames | median elapsed | 3013.73ms | 2993.05ms |
| Same performance fixture | median max RSS | 123,036KiB | 123,498KiB |

For the first four frames, the CLI exits with the expected frame-limit
status 2 after emitting the performance table header. These
measurements are full process wall time, *not* the time to first
Scratch output. The geometric mean of before/after process wall time
for the second workload was 0.9998, i.e. no demonstrated speed gain.
The memory medians increased by about 232KiB and 462KiB respectively.

These are short-run measurements with only two local workloads.
Retaining many contexts across a much longer edit/play session may
increase heap usage. This is a safety-focused experimental change,
not a memory or speed optimization.

## Remaining constraints

- Experimental par-for co-partition is still OFF by default.
- Concurrent *external* destruction of an executing Runtime is not
  supported or proven safe.
- TSan warnings concerning JITLink publication and LLVM's lock order
  remain unresolved.
- Cross-platform GitHub CI for this branch needs a separate run;
  a successful local LLVM 23 test does not prove LLVM 18 behavior.
- `main` and the official ScratchBench parity gate are unchanged.
