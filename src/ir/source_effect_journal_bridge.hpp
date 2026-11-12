// IR-001: observe one real CommitOnceEffects publish/drain cycle.
// This is a diagnostic adapter, not an effect replay or deopt permission.
#pragma once

#include "commit_once_effects.hpp"
#include "source_effect_frontier.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

namespace xyo::ir {

// A *single* already-committable synchronous observable action per slice.
// The scheduler must separately prove that the state publisher is atomic
// and that the callback never yields or throws. A commit publishes a
// GC-rooted action; only successful drain delivery advances the frontier.
// The caller must not bypass the bridge to publish other actions on the
// same journal while a cycle is in flight.
class SourceEffectJournalBridge final {
public:
    SourceEffectJournalBridge(CommitOnceEffects& journal,
                              SourceEffectSequenceTracker& frontier)
        : journal_(journal), frontier_(frontier) {}

    template<class PublishState, class Dispatcher>
    bool publishAndDeliver(uint64_t sourcePc, uint64_t sliceId,
                           ObservableOp op, const std::string& blockId,
                           Value payload, PublishState&& publishState,
                           Dispatcher&& dispatcher) {
        static_assert(std::is_nothrow_invocable_v<
            Dispatcher&, ObservableOp, const std::string&, Value>,
            "Effect dispatcher must be synchronous and noexcept");

        // In particular, never coalesce an older unobserved event into
        // this transaction. One published event == one dynamic frontier
        // increment. No speculative, reentrant or backlog execution.
        if (!sourcePc || !sliceId || blockId.empty() ||
            journal_.pendingCount() != 0 || journal_.committing() ||
            journal_.draining())
            return false;

        // Keep a stable identity even when publishState mutates caller
        // storage from which blockId was originally borrowed.
        const std::string expectedBlockId(blockId);
        auto slice = journal_.begin(sliceId);
        if (!slice.stage(op, expectedBlockId, payload))
            return false;

        const auto sequence = frontier_.begin(sourcePc);
        if (!sequence) return false; // RAII slice discards rooted staging

        if (!slice.commit(std::forward<PublishState>(publishState))) {
            // Slice::commit returns false only before publishing any
            // staged observable. It also destroys its speculative roots.
            // This does not roll back changes improperly made inside a
            // broken publisher callback; that callback must be atomic.
            frontier_.cancelUnpublished(sourcePc, *sequence);
            return false;
        }

        // The dispatcher may indirectly collect garbage. CommitOnceEffects
        // retains exact Value roots until the callback finishes.
        unsigned matched = 0;
        bool foreign = false;
        const auto delivered = journal_.drain(
            [&](ObservableOp receivedOp, const std::string& receivedBlock,
                Value value) noexcept {
                if (receivedOp == op && receivedBlock == expectedBlockId)
                    ++matched;
                else
                    foreign = true;
                std::invoke(dispatcher, receivedOp, receivedBlock, value);
            });

        // Any invalid rooted handle, unexpected event or missing delivery
        // leaves the tracker pending. This intentionally disables new
        // source checkpoints rather than pretending the effect committed.
        if (delivered.delivered != 1 || delivered.invalid != 0 ||
            matched != 1 || foreign || journal_.pendingCount() != 0)
            return false;
        return frontier_.commit(sourcePc, *sequence);
    }

private:
    CommitOnceEffects& journal_;
    SourceEffectSequenceTracker& frontier_;
};

} // namespace xyo::ir
