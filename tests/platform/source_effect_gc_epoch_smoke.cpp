#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/core/gc.hpp"
#include "../../src/ir/source_effect_runtime_trace.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>

int main() {
    auto& gc = xyo::Gc::get();
    const uint64_t before = gc.collectionGeneration();
    assert(before != 0);

    xyo::ir::SourceEffectDispatchObservation delivered;
    delivered.blockId = "gc-bridge-test";
    delivered.sourcePc = 3;
    delivered.sourceRevision = 7;
    delivered.digestLo = 11;
    delivered.digestHi = 13;
    delivered.operation = xyo::ir::ObservableOp::Broadcast;
    delivered.claimed = true;
    delivered.published = 1;
    delivered.delivered = 1;
    delivered.sliceId = 1;
    delivered.journalRevision = 1;
    delivered.hostEpoch = {1, 1, 0, 0, 0, 0, true, before};
    assert(delivered.exactDelivery());

    int thread = 0;
    auto& registry = xyo::ir::NativeSourceEffectTimelineRegistry::get();
    assert(registry.observe(&thread, delivered, true));

    // Real GC, not a fabricated generation change. No Scratch runtime
    // or JIT frames are alive in this minimal test.
    gc.collect();
    const uint64_t after = gc.collectionGeneration();
    assert(after != before);
    auto observed = delivered.hostEpoch;
    observed.gcGeneration = after;
    const auto rejected = registry.checkpoint(
        &thread, {7, 11, 13, 0}, 5,
        xyo::ir::SourceResumePhase::AfterLoopBody, observed);
    assert(!rejected.captured && !rejected.valid);
    assert(!rejected.foreignVmWrite);
    registry.erase(&thread);

    // A new source event AFTER completed GC uses its new epoch and
    // should not inherit the previous record's mismatch.
    delivered.hostEpoch.gcGeneration = after;
    assert(registry.observe(&thread, delivered, true));
    auto accepted = registry.checkpoint(
        &thread, {7, 11, 13, 0}, 5,
        xyo::ir::SourceResumePhase::AfterLoopBody, delivered.hostEpoch);
    assert(accepted.captured && accepted.valid);
    registry.erase(&thread);

    // Exercise the REAL deferred explicit-collection protocol. A
    // requested collection must NOT run while a native SSA frame could
    // be unrooted, but its pending flag must deny diagnostic capture.
    const auto beforeDeferred = gc.collectionGeneration();
    {
        xyo::Gc::DeferExplicitCollect defer;
        gc.collect();
        assert(gc.explicitCollectionPending());
        assert(gc.collectionGeneration() == beforeDeferred);
        assert(registry.observe(&thread, delivered, true));
        auto pendingHost = delivered.hostEpoch;
        pendingHost.gcCollectionPending = gc.explicitCollectionPending();
        assert(!pendingHost.quiescent());
        const auto pendingCheck = registry.checkpoint(
            &thread, {7, 11, 13, 0}, 5,
            xyo::ir::SourceResumePhase::AfterLoopBody, pendingHost);
        assert(!pendingCheck.captured && !pendingCheck.valid);
        registry.erase(&thread);
    }
    gc.collectIfNeeded();
    assert(!gc.explicitCollectionPending());
    assert(gc.collectionGeneration() != beforeDeferred);
    delivered.hostEpoch.gcGeneration = gc.collectionGeneration();

    // Two actual OS threads share the registry, but only the first
    // Scratch Thread has an attributed source event. The worker's
    // potential LLVM VM write invalidates that outstanding interval.
    int workerThread = 0;
    assert(registry.observe(&thread, delivered, true));
    std::thread worker([&] {
        registry.noteNonJournalMutation(&workerThread);
    });
    worker.join();
    const auto foreignMutation = registry.checkpoint(
        &thread, {7, 11, 13, 0}, 5,
        xyo::ir::SourceResumePhase::AfterLoopBody, delivered.hostEpoch);
    assert(!foreignMutation.captured);
    assert(foreignMutation.foreignVmWrite);
    assert(foreignMutation.pendingNonJournalMutation);
    registry.erase(&thread);

    // A second actual worker OS thread mutates between the INITIAL
    // host-delivery epoch and registering the effect's completed
    // journal receipt. It must poison that receipt, not just a later
    // source checkpoint.
    auto midDelivery = delivered;
    const uint64_t beforeDelivery = registry.mutationEpochSnapshot();
    std::thread concurrentWriter([&] {
        registry.noteInterpreterMutation(&workerThread);
    });
    concurrentWriter.join();
    assert(!registry.observeDuringDelivery(
        &thread, midDelivery, true, beforeDelivery,
        midDelivery.hostEpoch.gcGeneration));
    assert(midDelivery.mismatch && !midDelivery.exactDelivery());
    registry.erase(&thread);

    // An ACTUAL collection between host-delivery entry and its
    // receipt must also invalidate that receipt, independently of
    // the VM-write epoch and the later checkpoint's collector epoch.
    const uint64_t beforeHostGc = gc.collectionGeneration();
    const uint64_t beforeHostMutation = registry.mutationEpochSnapshot();
    gc.collect();
    auto gcDuringDelivery = delivered;
    gcDuringDelivery.hostEpoch.gcGeneration = gc.collectionGeneration();
    assert(gcDuringDelivery.hostEpoch.gcGeneration != beforeHostGc);
    assert(!registry.observeDuringDelivery(
        &thread, gcDuringDelivery, true,
        beforeHostMutation, beforeHostGc));
    assert(gcDuringDelivery.mismatch);
    registry.erase(&thread);

    std::cout << "PASS IR-001 real collector generation and cross-thread fence\n";
}
