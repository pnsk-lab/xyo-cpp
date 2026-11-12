#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/source_effect_runtime_trace.hpp"
#include "../../src/ir/source_interpreter_mutations.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using namespace xyo::ir;

static SourceEffectDispatchObservation event(
    const char* name, uint64_t pc, uint64_t slice) {
    SourceEffectDispatchObservation result;
    result.blockId = name;
    result.sourcePc = pc;
    result.sourceRevision = 7;
    result.digestLo = 11;
    result.digestHi = 13;
    result.operation = ObservableOp::Broadcast;
    {
        ScopedSourceEffectDispatch source(result);
        assert(sourceEffectStageLabel(ObservableOp::Broadcast,
                                      "runtime.broadcast") == name);
        noteSourceEffectPublished(ObservableOp::Broadcast, name,
                                  slice, slice);
        noteSourceEffectDelivered(ObservableOp::Broadcast, name);
    }
    assert(activeSourceEffect == nullptr);
    result.hostEpoch = {slice, slice, 0, 0, 0, 0, true, 1};
    assert(result.exactDelivery());
    return result;
}

int main() {
    static_assert(isInterpreterDataMutation("data_setvariableto"));
    static_assert(isInterpreterDataMutation("data_changevariableby"));
    static_assert(isInterpreterDataMutation("data_addtolist"));
    static_assert(isInterpreterDataMutation("data_deleteoflist"));
    static_assert(isInterpreterDataMutation("data_deletealloflist"));
    static_assert(isInterpreterDataMutation("data_insertatlist"));
    static_assert(isInterpreterDataMutation("data_replaceitemoflist"));
    static_assert(!isInterpreterDataMutation("data_variable"));
    static_assert(!isInterpreterDataMutation("data_itemoflist"));
    static_assert(!isInterpreterDataMutation("event_broadcast"));
    static_assert(!isInterpreterDataMutation("looks_say"));
    // Known stateful built-ins beyond variable and list writes.
    static_assert(isInterpreterStateMutation("motion_setx"));
    static_assert(isInterpreterStateMutation("motion_ifonedgebounce"));
    static_assert(isInterpreterStateMutation("looks_hide"));
    static_assert(isInterpreterStateMutation("looks_switchbackdropto"));
    static_assert(isInterpreterStateMutation("looks_say"));
    static_assert(isInterpreterStateMutation("pen_penDown"));
    static_assert(isInterpreterStateMutation("pen_stamp"));
    static_assert(isInterpreterStateMutation("sound_setvolumeto"));
    static_assert(isInterpreterStateMutation("sensing_resettimer"));
    static_assert(isInterpreterStateMutation("control_create_clone_of"));
    static_assert(isInterpreterStateMutation("control_wait"));
    static_assert(isInterpreterStateMutation("event_broadcastandwait"));
    static_assert(!isInterpreterStateMutation("motion_xposition"));
    static_assert(!isInterpreterStateMutation("motion_direction"));
    static_assert(!isInterpreterStateMutation("looks_size"));
    static_assert(!isInterpreterStateMutation("data_itemoflist"));
    static_assert(!isInterpreterStateMutation("operator_add"));
    static_assert(!isInterpreterStateMutation("control_repeat"));
    static_assert(!isInterpreterStateMutation("argument_reporter_string_number"));
    auto first = event("broadcast-A", 3, 1);
    NativeSourceEffectTimeline timeline;
    assert(timeline.observe(first, true));
    SourceEffectHostEpoch currentHost = first.hostEpoch;
    assert(timeline.count() == 1);
    assert(timeline.lastDelivery()->sourcePc == 3);
    assert(timeline.lastDelivery()->blockId == "broadcast-A");
    assert(!timeline.executable());
    SourceEffectIdentity identity{7, 11, 13, 0};
    assert(timeline.checkpoint(identity, 5,
                               SourceResumePhase::AfterLoopBody, currentHost));
    assert(timeline.lastCheckpoint()->committedCount == 1);
    assert(timeline.lastCheckpoint()->resumePc == 5);

    auto second = event("broadcast-A", 3, 2);
    assert(timeline.observe(second, true));
    currentHost = second.hostEpoch;
    assert(timeline.count() == 2);
    assert(!timeline.lastCheckpoint()); // invalidate older checkpoint
    assert(timeline.checkpoint(identity, 5,
                               SourceResumePhase::AfterLoopBody, currentHost));
    assert(timeline.lastCheckpoint()->committedCount == 2);
    assert(!timeline.checkpoint(identity, 5,
                                SourceResumePhase::InsideLoopBody, currentHost));
    assert(!timeline.lastCheckpoint());
    assert(!timeline.checkpoint({7, 11, 14, 0}, 5,
                                SourceResumePhase::AfterLoopBody, currentHost));
    assert(!timeline.lastCheckpoint());
    assert(!timeline.checkpoint(identity, 0,
                                SourceResumePhase::AfterLoopBody, currentHost));

    // Nested compiled events are independently attributed with a
    // stack of borrowed scopes; their actual block labels never alias.
    SourceEffectDispatchObservation outer;
    outer.blockId = "outer";
    outer.sourcePc = 4; outer.sourceRevision = 7;
    outer.digestLo = 11; outer.digestHi = 13;
    SourceEffectDispatchObservation inner = outer;
    inner.blockId = "inner";
    {
        ScopedSourceEffectDispatch scopedOuter(outer);
        assert(sourceEffectStageLabel(ObservableOp::Broadcast,
                                      "generic") == "outer");
        {
            ScopedSourceEffectDispatch scopedInner(inner);
            assert(sourceEffectStageLabel(ObservableOp::Broadcast,
                                          "generic") == "inner");
            noteSourceEffectPublished(ObservableOp::Broadcast,
                                      "inner", 3, 3);
            noteSourceEffectDelivered(ObservableOp::Broadcast, "inner");
        }
        assert(activeSourceEffect == &outer);
        noteSourceEffectPublished(ObservableOp::Broadcast,
                                  "outer", 4, 4);
        noteSourceEffectDelivered(ObservableOp::Broadcast, "outer");
    }
    inner.hostEpoch = {3, 3, 0, 0, 0, 0, true, 1};
    outer.hostEpoch = {4, 4, 0, 0, 0, 0, true, 1};
    assert(inner.exactDelivery() && !outer.exactDelivery());
    assert(activeSourceEffect == nullptr);

    // A wrong event label can never prove delivery.
    auto wrong = event("good", 6, 5);
    wrong.delivered = 0;
    {
        ScopedSourceEffectDispatch scope(wrong);
        noteSourceEffectDelivered(ObservableOp::Broadcast, "foreign");
    }
    assert(!wrong.exactDelivery());
    auto unstaged = event("unstaged", 6, 6);
    noteSourceEffectImmediate(ObservableOp::Broadcast); // outside scope
    assert(unstaged.exactDelivery());
    {
        ScopedSourceEffectDispatch scope(unstaged);
        noteSourceEffectImmediate(ObservableOp::Broadcast);
    }
    assert(!unstaged.exactDelivery());

    // Fail closed on stale source/duplicate slice/missing delivery.
    auto duplicate = event("same", 3, 2);
    assert(!timeline.observe(duplicate, true));
    assert(!timeline.valid());
    assert(!timeline.lastCheckpoint());
    assert(!timeline.observe(event("new", 8, 7), true));
    NativeSourceEffectTimeline invalid;
    assert(!invalid.observe(event("missing-source", 3, 8), false));
    assert(!invalid.checkpoint(identity, 5,
                               SourceResumePhase::AfterLoopBody, first.hostEpoch));

    // Host events between TWO observed effects cannot be hidden by
    // overwriting the timeline's most recent epoch.
    NativeSourceEffectTimeline hiddenPublished;
    assert(hiddenPublished.observe(first, true));
    assert(!hiddenPublished.observe(event("after-gap", 3, 3), true));
    assert(!hiddenPublished.valid());

    NativeSourceEffectTimeline hiddenImmediate;
    assert(hiddenImmediate.observe(first, true));
    auto secondAfterImmediate = second;
    ++secondAfterImmediate.hostEpoch.immediateBroadcasts;
    assert(secondAfterImmediate.exactDelivery());
    assert(!hiddenImmediate.observe(secondAfterImmediate, true));
    assert(!hiddenImmediate.valid());

    // An unobserved host event between delivery and checkpoint must
    // invalidate the timeline even when the source site still matches.
    NativeSourceEffectTimeline contaminated;
    assert(contaminated.observe(first, true));
    auto foreignEpoch = first.hostEpoch;
    ++foreignEpoch.journalSlice;
    ++foreignEpoch.journalRevision;
    assert(!contaminated.checkpoint(identity, 5,
                SourceResumePhase::AfterLoopBody, foreignEpoch));
    assert(!contaminated.valid());
    assert(!contaminated.observe(second, true));
    NativeSourceEffectTimeline immediateDrift;
    assert(immediateDrift.observe(first, true));
    auto immediateEpoch = first.hostEpoch;
    ++immediateEpoch.immediateEffects;
    assert(!immediateDrift.checkpoint(identity, 5,
                SourceResumePhase::AfterLoopBody, immediateEpoch));

    // A collection AFTER a real journal delivery but before the
    // checkpoint is invisible to the journal counters; the collector
    // generation must independently reject the stale source record.
    NativeSourceEffectTimeline collectedBetween;
    assert(collectedBetween.observe(first, true));
    auto afterCollection = first.hostEpoch;
    ++afterCollection.gcGeneration;
    assert(afterCollection.quiescent());
    assert(!collectedBetween.checkpoint(
        identity, 5, SourceResumePhase::AfterLoopBody, afterCollection));
    assert(!collectedBetween.valid());

    // A requested but deferred explicit collection also means the
    // native source checkpoint cannot be certified as quiescent.
    NativeSourceEffectTimeline pendingCollection;
    assert(pendingCollection.observe(first, true));
    auto deferredGc = first.hostEpoch;
    deferredGc.gcCollectionPending = true;
    assert(!deferredGc.quiescent());
    assert(!pendingCollection.checkpoint(
        identity, 5, SourceResumePhase::AfterLoopBody, deferredGc));

    NativeSourceEffectTimeline noCollectorToken;
    auto missingGc = first;
    missingGc.hostEpoch.gcGeneration = 0;
    assert(!missingGc.exactDelivery());
    assert(!noCollectorToken.observe(missingGc, true));

    // Multiple same-scope effects must never be certified as one event.
    auto extra = event("twice", 4, 9);
    {
        ScopedSourceEffectDispatch scope(extra);
        assert(sourceEffectStageLabel(ObservableOp::PenStamp,
                                      "runtime.observable") ==
               "runtime.observable");
    }
    assert(!extra.exactDelivery());
    auto sameKindExtra = event("twice-kind", 4, 10);
    {
        ScopedSourceEffectDispatch scope(sameKindExtra);
        assert(sourceEffectStageLabel(ObservableOp::Broadcast,
                                      "runtime.broadcast") ==
               "runtime.broadcast");
    }
    assert(!sameKindExtra.exactDelivery());

    // Interpreter or nested JIT mutations DURING the synchronous host
    // delivery must invalidate the active attribution even if the
    // journal publishes/delivers its one expected event successfully.
    auto nestedInterpreted = event("nested-interpreter", 4, 11);
    {
        ScopedSourceEffectDispatch scope(nestedInterpreted);
        noteSourceEffectUnjournaledMutation();
    }
    assert(!nestedInterpreted.exactDelivery());
    NativeSourceEffectTimeline nestedRejected;
    assert(!nestedRejected.observe(nestedInterpreted, true));
    auto noNestedMutation = event("no-nested-interpreter", 4, 12);
    noteSourceEffectUnjournaledMutation(); // outside a scope has no effect
    assert(noNestedMutation.exactDelivery());

    // VM writes are not in CommitOnceEffects, but a recorded write
    // after delivery must expire the diagnostic source checkpoint.
    NativeSourceEffectTimeline writtenAfterDelivery;
    assert(writtenAfterDelivery.observe(first, true));
    writtenAfterDelivery.noteNonJournalMutation();
    assert(!writtenAfterDelivery.checkpoint(
        identity, 5, SourceResumePhase::AfterLoopBody, first.hostEpoch));
    assert(!writtenAfterDelivery.lastCheckpoint());
    assert(!writtenAfterDelivery.observe(second, true));

    // A write AFTER a valid checkpoint is outside that observed
    // interval. The next delivered event can start another interval.
    NativeSourceEffectTimeline nextInterval;
    assert(nextInterval.observe(first, true));
    assert(nextInterval.checkpoint(
        identity, 5, SourceResumePhase::AfterLoopBody, first.hostEpoch));
    nextInterval.noteNonJournalMutation();
    assert(!nextInterval.lastCheckpoint());
    assert(nextInterval.observe(second, true));
    assert(nextInterval.checkpoint(
        identity, 5, SourceResumePhase::AfterLoopBody, second.hostEpoch));
    assert(nextInterval.count() == 2);

    // Scheduler registry owns a snapshot for the Thread lifetime and
    // drops it before a recycled pointer could impersonate the old one.
    int simulatedThread = 0;
    auto& registry = NativeSourceEffectTimelineRegistry::get();
    assert(registry.observe(&simulatedThread, first, true));
    auto observed = registry.checkpoint(
        &simulatedThread, identity, 5, SourceResumePhase::AfterLoopBody,
        first.hostEpoch);
    assert(observed.captured && observed.count == 1);
    assert(observed.latest && observed.latest->sourcePc == 3);
    assert(observed.checkpoint && observed.checkpoint->committedCount == 1);
    registry.erase(&simulatedThread);
    observed = registry.checkpoint(
        &simulatedThread, identity, 5, SourceResumePhase::AfterLoopBody,
        first.hostEpoch);
    assert(!observed.captured && !observed.latest);
    assert(registry.observe(&simulatedThread, second, true));
    observed = registry.checkpoint(
        &simulatedThread, identity, 5, SourceResumePhase::AfterLoopBody,
        second.hostEpoch);
    assert(observed.captured && observed.count == 1);
    registry.erase(&simulatedThread);

    // An unjournaled write DURING the host's synchronous delivery
    // must not be hidden when the new timeline starts AFTER delivery.
    // The sampled epoch and the timeline registration share the same
    // lock on the consumer side, closing the TOCTOU gap.
    int deliveryThread = 0;
    int writerDuringDelivery = 0;
    auto deliveryExact = event("delivery-no-write", 3, 1);
    const uint64_t cleanStart = registry.mutationEpochSnapshot();
    assert(registry.observeDuringDelivery(
        &deliveryThread, deliveryExact, true, cleanStart, 1));
    assert(deliveryExact.exactDelivery());
    registry.erase(&deliveryThread);

    auto deliveryInterleaved = event("delivery-with-write", 3, 1);
    const uint64_t interleavedStart = registry.mutationEpochSnapshot();
    registry.noteInterpreterMutation(&writerDuringDelivery);
    assert(!registry.observeDuringDelivery(
        &deliveryThread, deliveryInterleaved, true,
        interleavedStart, 1));
    assert(deliveryInterleaved.mismatch);
    assert(!deliveryInterleaved.exactDelivery());
    assert(!registry.checkpoint(
        &deliveryThread, identity, 5,
        SourceResumePhase::AfterLoopBody,
        deliveryInterleaved.hostEpoch).captured);
    registry.erase(&deliveryThread);

    auto jitInterleaved = event("delivery-with-jit-write", 3, 1);
    const uint64_t nativeStart = registry.mutationEpochSnapshot();
    registry.noteNonJournalMutation(&writerDuringDelivery);
    assert(!registry.observeDuringDelivery(
        &deliveryThread, jitInterleaved, true, nativeStart, 1));
    assert(jitInterleaved.mismatch);
    registry.erase(&deliveryThread);

    // The native source delivery can also run a FULL GC before it
    // registers the completed event; only sampling after the
    // synchronous callback would miss this collection.
    auto gcInterleaved = event("delivery-with-gc", 3, 1);
    const uint64_t beforeGcStart = registry.mutationEpochSnapshot();
    gcInterleaved.hostEpoch.gcGeneration = 2;
    assert(!registry.observeDuringDelivery(
        &deliveryThread, gcInterleaved, true, beforeGcStart, 1));
    assert(gcInterleaved.mismatch && !gcInterleaved.exactDelivery());
    registry.erase(&deliveryThread);

    int otherThread = 0;
    assert(registry.observe(&otherThread, first, true));
    registry.noteNonJournalMutation(&otherThread);
    auto writeBlocked = registry.checkpoint(
        &otherThread, identity, 5, SourceResumePhase::AfterLoopBody,
        first.hostEpoch);
    assert(!writeBlocked.captured && !writeBlocked.checkpoint);
    assert(writeBlocked.pendingNonJournalMutation &&
           writeBlocked.foreignVmWrite);
    registry.erase(&otherThread);

    // Two different Scratch Thread identities, only one of which has
    // an attributed source event. The OTHER Thread's optimized native
    // store still advances the global mutation epoch, even though it
    // never owned a timeline or appeared in the observable journal.
    int threadA = 0, threadB = 0;
    assert(registry.observe(&threadA, first, true));
    registry.noteNonJournalMutation(&threadB);
    const auto crossWrite = registry.checkpoint(
        &threadA, identity, 5, SourceResumePhase::AfterLoopBody,
        first.hostEpoch);
    assert(!crossWrite.captured && !crossWrite.checkpoint);
    assert(crossWrite.pendingNonJournalMutation);
    assert(crossWrite.foreignVmWrite);
    registry.erase(&threadA);
    registry.erase(&threadB);

    // A foreign write after the first checkpoint does not contaminate
    // the NEXT independently attributed source event. ABA of the
    // Thread* registry slot is already covered by erase/reobserve.
    assert(registry.observe(&threadA, first, true));
    assert(registry.checkpoint(
        &threadA, identity, 5,
        SourceResumePhase::AfterLoopBody, first.hostEpoch).captured);
    registry.noteNonJournalMutation(&threadB);
    assert(registry.observe(&threadA, second, true));
    const auto nextEpoch = registry.checkpoint(
        &threadA, identity, 5,
        SourceResumePhase::AfterLoopBody, second.hostEpoch);
    assert(nextEpoch.captured && !nextEpoch.foreignVmWrite);
    registry.erase(&threadA);

    // If another Thread mutates state BETWEEN two attributed events
    // with no intervening checkpoint, the new delivery cannot hide
    // the interrupted first interval.
    assert(registry.observe(&threadA, first, true));
    registry.noteNonJournalMutation(&threadB);
    assert(!registry.observe(&threadA, second, true));
    const auto interrupted = registry.checkpoint(
        &threadA, identity, 5,
        SourceResumePhase::AfterLoopBody, second.hostEpoch);
    assert(!interrupted.captured);
    registry.erase(&threadA);

    // A foreign interpreter write must invalidate a live native
    // checkpoint and identify the interpreter as the source, even if
    // that interpreter Thread was never registered with the journal.
    assert(registry.observe(&threadA, first, true));
    registry.noteInterpreterMutation(&threadB);
    const auto interpreted = registry.checkpoint(
        &threadA, identity, 5, SourceResumePhase::AfterLoopBody,
        first.hostEpoch);
    assert(!interpreted.captured);
    assert(interpreted.foreignVmWrite && interpreted.interpreterVmWrite);
    assert(interpreted.pendingNonJournalMutation);
    registry.erase(&threadA);

    // Interpreted mutation BEFORE any observed event does not
    // contaminate the newly established and independently attributed
    // source interval; a later LLVM write has a distinct cause.
    registry.noteInterpreterMutation(&threadB);
    assert(registry.observe(&threadA, first, true));
    const auto afterPriorInterpret = registry.checkpoint(
        &threadA, identity, 5, SourceResumePhase::AfterLoopBody,
        first.hostEpoch);
    assert(afterPriorInterpret.captured);
    assert(!afterPriorInterpret.foreignVmWrite);
    assert(!afterPriorInterpret.interpreterVmWrite);
    registry.erase(&threadA);

    assert(registry.observe(&threadA, first, true));
    registry.noteNonJournalMutation(&threadB);
    const auto compiledWrite = registry.checkpoint(
        &threadA, identity, 5, SourceResumePhase::AfterLoopBody,
        first.hostEpoch);
    assert(!compiledWrite.captured && compiledWrite.foreignVmWrite);
    assert(!compiledWrite.interpreterVmWrite);
    registry.erase(&threadA);

    // Interpreted mutations between two journal deliveries cannot be
    // masked by a newer delivery from the same source identity.
    assert(registry.observe(&threadA, first, true));
    registry.noteInterpreterMutation(&threadB);
    assert(!registry.observe(&threadA, second, true));
    registry.erase(&threadA);

    std::cout << "PASS source event attribution + owned checkpoint\n";
}
