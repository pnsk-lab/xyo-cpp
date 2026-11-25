# PROF-001: opt-in JIT compilation telemetry

The first YJIT-inspired observation layer records compilation costs and cache/async
outcomes without changing Scratch-visible behavior or the OSR/CallFrame ABI.

## Usage

Set `XYO_JIT_COMPILE_STATS=1` on the `xyo` process. The engine prints a
short report to stderr when its JIT engine is destroyed:

```
[jit-compile] sync attempts=N success=N total_ms=X.XXX
[jit-compile] prepare attempts=N success=N total_ms=X.XXX
[jit-compile] finish attempts=N success=N total_ms=X.XXX
[jit-compile] lookup cache_hit=N failed_hit=N queue_full=N
[jit-compile] publish accepted=N stale=N failed=N
```

* `sync`: elapsed time in the synchronous backend `compile()` call;
  source-capture time is **not** included.
* `prepare`: elapsed time in `prepareAsync()`, including work on the
  main thread before the task is enqueued.
* `finish`: elapsed time inside the asynchronous task's `finish()`,
  including synchronous prefetch, **not** time spent waiting in its queue.
* `lookup cache_hit`: already compiled code found in `compile()`.
  It excludes fast-path lookups performed elsewhere in the runtime.
* `lookup failed_hit`: rejected scripts encountered again in `compile()`.
* `queue_full`: HotSpot asynchronous compilation declined at the
  32-script backpressure limit.
* `publish accepted`: completed asynchronous code accepted by the runtime.
* `publish stale`: completed code discarded because its generation changed.
* `publish failed`: asynchronous compilation finished without executable code.

All counters are atomic to support the compilation worker thread. They do not
allocate or synchronously print in compilation paths. The elapsed time uses
`steady_clock` and represents aggregate **wall-clock durations**, not CPU time.
Parallel compilations can therefore overlap; do not add the totals and claim
they equal application wall time.

When unset or set to any value other than exactly `1`, the instrumentation
does not update counters or sample clocks. A static environment check remains
on the instrumented compile/cache paths. The normal `XYO_HOT_STATS` report is
independent.

## Scope and follow-up

This patch deliberately does not modify `codegen.cpp`, `rt.cpp`,
`sequencer.cpp`, or `thread.cpp`, which have concurrent OSR and scheduling
work in progress. The next phase can add stable, shared *reason IDs* for
guard failure and deoptimization only after the relevant CallFrame/OSR owners
approve an instrumentation boundary.

Benchmark cold compilation, warm JIT execution, and startup separately; use
the same project for `XYO_JIT_COMPILE_STATS=0` and `=1`. Never use compile
timing figures as evidence of throughput changes without paired benchmarks.
