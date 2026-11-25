# Incremental Ready Index (2026-11-11)

## Implementation

The opt-in `XYO_SCHED_READY_INDEX=1` no longer discards its cached ready slots at every tick boundary. It maintains:

- `schedulerMutationEpoch`: atomic generation for all scheduler-visible changes
- `schedulerStructureEpoch`: atomic generation for slot identity changes
- `schedulerStatusChanges`: mutex-protected Thread* change journal, populated
  only when the Ready Index flag is enabled
- `readySlots_`: sorted slot indices, patched from the journal with
  `lower_bound`, insertion and removal, rebuilding only on structure changes

The cache excludes `STATUS_PROMISE_WAIT` while retaining `STATUS_DONE` for
completion compaction. Thread runtimeIndex is checked against the live array;
stale hints force rebuild. Epochs are captured before draining notifications,
so a publication during reconstruction forces the ordinary live scan rather
than silently skipping a newly woken thread.

Normal, flag-OFF scheduling does not allocate journal entries or lock the
journal mutex. Existing `runSchedulerPass` ordering and parallel barriers
are unchanged.

## Verification

- LLVM JIT enabled `xyo-tests` PASS (Ready Index + deadline index enabled:
  49.18 s; both default-disabled: 47.91 s).
- New unit tests check opt-in journal identity, drain semantics, and structural
  invalidation. The tests above were executed with the matching compiled
  library.
- Isolated non-JIT scheduler-only executable: 254/255 checks, with the single
  expected failure asserting native JIT compilation; the result is identical
  with the index flag ON and OFF.
- Paired benchmark with the successfully linked JIT binary (3 A/B pairs):
  `sched_threads` 0.9926x; `sched_fanout` 0.9983x;
  `sched_waituntil` 1.0332x. Geometric means; noisy, not a demonstrated
  net performance improvement.

## Limitations

The public `Runtime::threads` vector and direct external writes to
`Thread::status` are not universally observable. Users of the opt-in index
must publish structural changes and transitions through Runtime APIs.
JIT/Interpreter Fiber-internal status changes not fully normalized. No full
ScratchBench parity campaign was executed. The index is not ready to be
enabled by default.

The current shared worktree had ongoing LAYQ codegen edits. An earlier
LLVM JIT build passed after repairing duplicated offset definitions and
replacing unsupported IRBuilder::CreateUMin uses with equivalent
ICmpULT+Select. A later recompile of independently edited codegen.cpp
failed while CLI linking; the working CLI binary was subsequently linked
directly against the last successful libxyo.a for A/B measurements.
