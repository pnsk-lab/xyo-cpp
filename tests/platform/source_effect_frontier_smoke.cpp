#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/source_effect_frontier.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

using namespace xyo::ir;

int main() {
    SourceEffectIdentity source{42, 10, 20, 3};
    SourceEffectSequenceTracker tracker(source);
    auto first = tracker.checkpoint(7, 11, SourceResumePhase::AfterLoopBody);
    assert(first && first->committedCount == 0);
    assert(first->valid());

    auto seq1 = tracker.begin(5);
    assert(seq1 && *seq1 == 1);
    assert(tracker.hasPendingEffect());
    assert(!tracker.begin(5)); // cannot begin an overlapping observable effect
    assert(!tracker.checkpoint(7, 11, SourceResumePhase::AfterLoopBody));
    assert(!tracker.commit(5, 2)); // skipped sequence
    assert(!tracker.commit(6, 1)); // wrong source statement
    assert(tracker.committedCount() == 0);
    assert(tracker.commit(5, 1));
    assert(!tracker.commit(5, 1)); // duplicate commit
    auto afterFirst = tracker.checkpoint(
        7, 11, SourceResumePhase::AfterLoopBody);
    assert(afterFirst && afterFirst->committedCount == 1);
    assert(afterFirst->matches(*afterFirst, 1));
    assert(!afterFirst->matches(*afterFirst, 0)); // host journal disagrees
    assert(!first->matches(*afterFirst, 1)); // stale execution generation

    // Revisit identical statement PC in a dynamic loop iteration.
    auto seq2 = tracker.begin(5);
    assert(seq2 && *seq2 == 2 && tracker.commit(5, *seq2));
    auto afterSecond = tracker.checkpoint(
        7, 11, SourceResumePhase::AfterLoopBody);
    assert(afterSecond && afterSecond->committedCount == 2);
    assert(!afterFirst->matches(*afterSecond, 2));

    // A failed/unpublished journal transaction may release its pending
    // sequence; it must not consume a dynamic effect number.
    auto unpublished = tracker.begin(55);
    assert(unpublished && *unpublished == 3);
    assert(!tracker.cancelUnpublished(55, 4));
    assert(!tracker.cancelUnpublished(56, 3));
    assert(!tracker.checkpoint(7, 11, SourceResumePhase::AfterLoopBody));
    assert(tracker.cancelUnpublished(55, 3));
    assert(!tracker.hasPendingEffect());
    assert(tracker.committedCount() == 2);
    auto retried = tracker.begin(55);
    assert(retried && *retried == 3);
    assert(tracker.commit(55, *retried));

    auto wrong = *afterSecond;
    wrong.siteId = 8;
    assert(!afterSecond->matches(wrong, 2));
    wrong = *afterSecond;
    wrong.resumePc = 12;
    assert(!afterSecond->matches(wrong, 2));
    wrong = *afterSecond;
    wrong.phase = SourceResumePhase::UnknownBody;
    assert(!afterSecond->matches(wrong, 2));
    wrong = *afterSecond;
    wrong.source.procedureId++;
    assert(!afterSecond->matches(wrong, 2));
    wrong = *afterSecond;
    wrong.source.digestHi ^= 1;
    assert(!afterSecond->matches(wrong, 2));

    SourceEffectSequenceTracker invalid({0, 0, 0, 0});
    assert(!invalid.begin(5));
    assert(!invalid.checkpoint(7, 11, SourceResumePhase::AfterLoopBody));
    assert(!tracker.begin(0));
    assert(!tracker.checkpoint(0, 11, SourceResumePhase::AfterLoopBody));
    assert(!tracker.checkpoint(7, 0, SourceResumePhase::AfterLoopBody));
    assert(!tracker.checkpoint(7, 11, static_cast<SourceResumePhase>(100)));
    SourceEffectSequenceTracker zeroHalf({43, 0, 20, 1});
    assert(zeroHalf.begin(9) == 1);
    std::cout << "PASS source effect frontier diagnostics\n";
}
