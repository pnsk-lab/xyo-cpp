# Scheduler terminal transition audit (2026-11-11)

## Updated code

The common Sequencer::retireThread now publishes STATUS_DONE via
Runtime::transitionThread rather than silently writing status. Native
stop-all, stop-script and clone-delete terminal paths in src/jit/rt.cpp and
src/jit/rt_blocks.cpp also publish through transitionThread.

Runtime::schedulerMutationEpoch is now std::atomic<uint64_t>.
invalidateSchedulerReadySet() performs fetch_add(1, memory_order_relaxed),
and Sequencer sparse-ready checks load with memory_order_relaxed. This avoids
data races on the generation counter when parallel JIT worker code terminates
scripts. It does NOT make unrelated Runtime / Thread state or public vector
mutations safe for concurrent writes.

The hot intra-Fiber RUNNING/YIELD exchanges remain direct. They should not
invalidate the cross-pass ready index on every cooperative yield. Threads can
also set status in Thread::stopThisScript and WASM continuation paths that
lack an owning Runtime reference; these paths are not yet completely
normalized.

## Coverage

- New test: Sequencer::retireThread publishes the terminal transition, and
  repeated retirement does not increment the epoch again.
- New test: four workers concurrently publish 2,000 mutations each, and the
  epoch reaches 8,000 without lost updates.
- Existing tests exercise Promise wait/resolution, broadcast restart and
  conditional sparse-ready behavior.

The ready index and wait deadline index remain opt-in. All-thread scanning
has NOT been completely replaced. ScratchBench performance and full Scratch
parity have not been certified by this change.
