// IR-001: real GC-rooted CommitOnceEffects publication -> delivery ->
// diagnostic source frontier. Not a proof of general deoptimization.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/source_effect_journal_bridge.hpp"
#include "../../src/ir/source_deopt_audit.hpp"
#include "../../src/engine/thread.hpp"
#include "../../src/core/sstr.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace xyo;
using namespace xyo::ir;

int main() {
    Gc& gc = Gc::get();
    CommitOnceEffects journal(gc);
    SourceEffectSequenceTracker tracker({12, 13, 14, 3});
    SourceEffectJournalBridge bridge(journal, tracker);
    std::vector<std::string> delivered;
    int publishedVariable = 0;
    unsigned callbacks = 0;

    const auto dispatch = [&](ObservableOp op, const std::string& block,
                              Value value) noexcept {
        assert(op == ObservableOp::Broadcast);
        assert(block == "repeat.broadcast");
        assert(publishedVariable != 0);
        assert(tracker.hasPendingEffect());
        assert(!tracker.checkpoint(4, 9,
                                   SourceResumePhase::AfterLoopBody));
        gc.collect(); // retained string survives collection during delivery
        assert(value.isString());
        delivered.push_back(SStr::toUtf8(value.asStrPtr()));
        ++callbacks;
    };

    // A rejected transaction never publishes, delivers or increments the
    // source counter, and must not poison future checkpoint attempts.
    assert(!bridge.publishAndDeliver(
        9, 1, ObservableOp::Broadcast, "repeat.broadcast",
        Value::string(SStr::fromUtf8("should-not-run")),
        [&] { return false; }, dispatch));
    assert(tracker.committedCount() == 0);
    assert(!tracker.hasPendingEffect());
    assert(journal.pendingCount() == 0);
    assert(journal.rootedPayloads() == 0);
    assert(delivered.empty());

    // A previously published journal slice ID is not allowed to replay.
    assert(bridge.publishAndDeliver(
        9, 2, ObservableOp::Broadcast, "repeat.broadcast",
        Value::string(SStr::fromUtf8("first")),
        [&] { publishedVariable = 42; return true; }, dispatch));
    assert(journal.lastPublishedSlice() == 2);
    assert(journal.revision() == 1);
    assert(tracker.committedCount() == 1);
    assert(!tracker.hasPendingEffect());
    assert(callbacks == 1 && delivered[0] == "first");
    assert(journal.rootedPayloads() == 0);

    assert(!bridge.publishAndDeliver(
        9, 2, ObservableOp::Broadcast, "repeat.broadcast",
        Value::string(SStr::fromUtf8("duplicate-slice")),
        [&] { publishedVariable = -1; return true; }, dispatch));
    assert(publishedVariable == 42);
    assert(callbacks == 1 && tracker.committedCount() == 1);
    assert(!tracker.hasPendingEffect());

    // Same statement PC in a later loop iteration is a new effect.
    assert(bridge.publishAndDeliver(
        9, 3, ObservableOp::Broadcast, "repeat.broadcast",
        Value::string(SStr::fromUtf8("second")),
        [&] { publishedVariable = 43; return true; }, dispatch));
    assert(callbacks == 2 && delivered[1] == "second");
    assert(tracker.committedCount() == 2 && journal.revision() == 2);

    const auto after = tracker.checkpoint(
        4, 9, SourceResumePhase::AfterLoopBody);
    assert(after && after->committedCount == 2);
    assert(after->matches(*after, 2));
    assert(!after->matches(*after, 1));

    // An unrelated pending event cannot be hidden in a supposedly
    // one-to-one source frontier transaction.
    {
        auto unrelated = journal.begin(4);
        assert(unrelated.stage(ObservableOp::Say, "foreign",
                               Value::undefined()));
        assert(unrelated.commit([] { return true; }));
    }
    assert(journal.pendingCount() == 1);
    assert(!bridge.publishAndDeliver(
        9, 5, ObservableOp::Broadcast, "repeat.broadcast",
        Value::string(SStr::fromUtf8("must-not-leapfrog")),
        [&] { publishedVariable = -100; return true; }, dispatch));
    assert(publishedVariable == 43);
    assert(tracker.committedCount() == 2);
    assert(journal.drain(
        [&](ObservableOp op, const std::string& block, Value) noexcept {
            assert(op == ObservableOp::Say && block == "foreign");
        }).delivered == 1);
    assert(journal.pendingCount() == 0);

    // Reentrancy is refused during delivery; no second slice is staged.
    const auto reentrant = [&](ObservableOp op, const std::string& block,
                               Value value) noexcept {
        assert(op == ObservableOp::Broadcast &&
               block == "repeat.broadcast");
        gc.collect();
        assert(value.isString());
        ++callbacks;
        assert(!bridge.publishAndDeliver(
            9, 7, ObservableOp::Broadcast, "repeat.broadcast",
            Value::undefined(), [] { return true; }, dispatch));
    };
    assert(bridge.publishAndDeliver(
        9, 6, ObservableOp::Broadcast, "repeat.broadcast",
        Value::string(SStr::fromUtf8("reentrant")),
        [&] { publishedVariable = 44; return true; }, reentrant));
    assert(callbacks == 3 && tracker.committedCount() == 3);
    assert(journal.revision() == 4);
    assert(journal.rootedPayloads() == 0);

    // The publishing callback may mutate its caller-owned source string.
    // Observe the stable staged identity, not the now-mutated reference.
    std::string mutableId = "repeat.broadcast";
    assert(bridge.publishAndDeliver(
        9, 7, ObservableOp::Broadcast, mutableId,
        Value::string(SStr::fromUtf8("stable-identity")),
        [&] {
            mutableId = "forged-after-stage";
            publishedVariable = 45;
            return true;
        }, dispatch));
    assert(callbacks == 4 && delivered.back() == "stable-identity");
    assert(tracker.committedCount() == 4);
    assert(journal.rootedPayloads() == 0);

    // Integrate all three read-only layers: real journal deliveries,
    // RT-owned detached diagnostics, and the IR proof-gap audit.
    const auto completedFrontier = tracker.checkpoint(
        4, 9, SourceResumePhase::AfterLoopBody);
    assert(completedFrontier && completedFrontier->committedCount == callbacks);
    SourceTypedSsaManifest compilerManifest{
        12, 13, 14, 3, 9, 4, {{uint64_t(64) << 32}}};
    xyo::NativeRtSsaCapture detached;
    detached.sourceRevision = compilerManifest.revision;
    detached.digestLo = compilerManifest.digestLo;
    detached.digestHi = compilerManifest.digestHi;
    detached.procedureId = compilerManifest.procedureId;
    detached.sourcePc = compilerManifest.resumePc;
    detached.siteId = compilerManifest.siteId;
    detached.descriptors = compilerManifest.descriptors;
    detached.words = {0x12345678ULL};
    detached.numericComplete = true;
    detached.fingerprint = detached.computeFingerprint();

    auto audit = auditSourceDeoptWithEffectFrontier(
        SourceResumePhase::AfterLoopBody, &detached, compilerManifest,
        &*completedFrontier, *completedFrontier, callbacks);
    assert(!audit.missing(SourceDeoptGap::InvalidSnapshot));
    assert(!audit.missing(SourceDeoptGap::EffectFrontierMismatch));
    assert(audit.missing(SourceDeoptGap::EffectCommit));
    assert(audit.missing(SourceDeoptGap::LiveSsa));
    assert(!audit.executable());
    audit = auditSourceDeoptWithEffectFrontier(
        SourceResumePhase::AfterLoopBody, &detached, compilerManifest,
        &*completedFrontier, *completedFrontier, callbacks - 1);
    assert(audit.missing(SourceDeoptGap::EffectFrontierMismatch));

    // Unsupported ops fail at stage(), without leaving a pending token.
    assert(!bridge.publishAndDeliver(
        9, 8, static_cast<ObservableOp>(255), "invalid-op",
        Value::undefined(), [] { return true; }, dispatch));
    assert(!tracker.hasPendingEffect());
    assert(tracker.committedCount() == 4);
    std::cout << "PASS source frontier + real commit-once GC journal\n";
}
