# Mega Scheduler β — Broadcast receiver construction audit (2026-11-15)

## Scope and status

This iteration measured the actual `Runtime::startBroadcastAndWait`
indexed receiver path and `Runtime::pushThread` in Release. It does
**not** enable another native optimization by default.

Two opt-in, nonsemantic profiling controls were added:

```sh
XYO_BCAST_WAIT_PROFILE=1
XYO_PUSH_THREAD_PHASE_PROFILE=1
```

The first emits one `[bcast-wait-prof]` record per runtime lifetime,
tracking receiver index lookup, indexed dispatch, push/restart,
immediate hat evaluation, and JIT receiver attachment.
The second emits a `[push-phase-prof]` record measuring the four
stages of creating compiled broadcast receiver Threads only:

- allocation/storage and Runtime thread-vector ownership,
- `threadMap` update,
- JIT lookup/compile decision (`Thread::tryCompile`),
- root-frame initialization.

The second profiler explicitly excludes the initial green-flag
launch, which includes one-time JIT compilation and would otherwise
distort repeated receiver averages. When both profiling flags are
disabled, no clock is read by the new instrumentation.

## Grounded measurements

Configuration: original ScratchBench `sched_pingpong` receiver,
the source-validated fixed-work transformation
`sc_hits < b_budget`, 20,000 joined broadcasts per round for
five rounds (100,000 broadcasts), CPU 16, Eager JIT, Mega fast-pair
continuation enabled.

Instrumented example output:

```
[bcast-wait-prof] calls=100000 receivers=100000 pushes=100000 restarts=0 lookup_ns=4057017 dispatch_ns=94641876 hat_ns=2145966 push_ns=84144533 restart_ns=0 attach_ns=2785227
[push-phase-prof] count=100000 alloc_ns=22280894 map_ns=9062482 jit_ns=38055617 root_ns=9689868
```

These are aggregate *instrumented* values, not uninstrumented
scheduler throughput timings.

The outcome is specific: this workload calls `pushThread` 100,000
times and `restartThread` zero times. Roughly 84ms of the indexed
broadcast preparation path is spent in pushThread, with about 38ms
in receiver JIT code resolution, 22ms in object/storage work, and
10ms in root initialization. The sum of timed phases is slightly
smaller than `push_ns` due to instrumentation and other work.

## Experiments, including rejected optimizations

- The existing `XYO_JIT_MEGA_SINGLE_RECEIVER=1` and
  `XYO_RESTART_THREAD_REUSE=1` controls were each exercised.
  They did not demonstrate a reliable improvement for this workload;
  the latter does not target the observed 100,000 pushes at all.
- An experimental source-version/JIT-generation validated one-entry
  compiled receiver cache was implemented and A/B tested. CPU 16,
  20,000 broadcasts per round, 12 alternating paired Mega-vs-Mega
  whole-process samples: paired geometric speed ratio
  **0.99596x**, with medians baseline 303.93ms and trial 304.42ms.
  This did **not** demonstrate a speedup. The proposed cache, its
  additional runtime/JIT interface, and the test-only switch were
  **removed**; the original `Thread::tryCompile` semantics remain.
- Raw experimental timing JSON:
  `build-scheduler-rewrite-20261111/mega-ready-short-direct-20000-12pairs.json`.
  Phase timings:
  `build-scheduler-rewrite-20261111/mega-bcast-phase-receiveronly-20000.json`.

## Follow-up work

The current hot spots worth investigating are repeated
`Thread::tryCompile` ready-script lookup, storage construction,
and root-frame setup. Any new fast path must preserve
source revision invalidation, hot/restart behavior, broadcast-and-wait
Thread identity, and GC lifecycle; reusing the just-completed Thread
immediately would not be safe because joining senders may still
retain it.

The existing Mega fixed-work path is still slower than Fiber in
end-to-end subprocess tests. Its optimized join path remains opt-in.
This iteration introduces no new default-enabled scheduler behavior.

All changes are uncommitted. Unrelated multiagent changes were not
reset, staged, or committed.
