// Integration proof for the existing scheduler-owned ScratchCallTransaction
// and the new commit-once, GC-rooted external effect journal.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/commit_once_effects.hpp"
#include "../../src/ir/scratch_call_frame.hpp"
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
    ScratchCallStack frames({1, 2, 6});
    assert(frames.push(0, CallFrameAbi::kNoPc, 1, {}, 0));
    CommitOnceEffects journal(gc);
    std::vector<std::string> broadcasts;

    // Speculatively advance the same scheduler CallFrame whose revision
    // the host uses to establish a replay-safe continuation boundary.
    {
        ScratchCallTransaction tx(frames);
        auto observable = journal.begin(1);
        const uint64_t initialRevision = frames.revision();
        assert(tx.staged().setResumePc(10));
        assert(tx.staged().push(1, 20, 30, {}, 2));
        assert(observable.stage(
            ObservableOp::Broadcast, "event_broadcast",
            Value::string(SStr::fromUtf8("GC rooted 🌟"))));
        gc.collect();
        assert(frames.depth() == 1 && frames.top()->resumePc == 1);
        assert(broadcasts.empty());
        assert(journal.pendingCount() == 0);
        assert(observable.commit([&] {
            assert(frames.revision() == initialRevision);
            return tx.commit();
        }));
        assert(frames.depth() == 2 && frames.top()->resumePc == 30);
        assert(journal.pendingCount() == 1);
        gc.collect();
    }
    const auto handler = [&](ObservableOp op, const std::string& source,
                             Value value) noexcept {
        assert(op == ObservableOp::Broadcast);
        assert(source == "event_broadcast");
        assert(frames.depth() == 2 && frames.top()->resumePc == 30);
        gc.collect();
        broadcasts.push_back(SStr::toUtf8(value.asStrPtr()));
    };
    assert(journal.drain(handler).delivered == 1);
    assert((broadcasts == std::vector<std::string>{"GC rooted 🌟"}));
    assert(journal.drain(handler).delivered == 0);
    assert(journal.rootedPayloads() == 0);

    // Competing scheduler step invalidates a speculative continuation:
    // neither a partial PC update nor any external effect can publish.
    const auto currentPc = frames.top()->resumePc;
    {
        ScratchCallTransaction stale(frames);
        auto observable = journal.begin(2);
        assert(stale.staged().setResumePc(40));
        assert(observable.stage(
            ObservableOp::Broadcast, "event_broadcast",
            Value::string(SStr::fromUtf8("MUST NOT RUN"))));
        assert(frames.setResumePc(41)); // another scheduler action
        assert(!observable.commit([&] { return stale.commit(); }));
        assert(frames.top()->resumePc == 41);
        assert(journal.pendingCount() == 0);
        assert(journal.rootedPayloads() == 0);
    }
    assert(currentPc == 30);
    assert(broadcasts.size() == 1);

    // A successful fresh slice reuses neither the earlier slice ID nor the
    // stale PC; the effect is emitted after the new source PC is published.
    {
        ScratchCallTransaction fresh(frames);
        auto observable = journal.begin(3);
        assert(fresh.staged().setResumePc(50));
        assert(observable.stage(
            ObservableOp::Broadcast, "event_broadcast",
            Value::string(SStr::fromUtf8("fresh"))));
        assert(observable.commit([&] { return fresh.commit(); }));
    }
    assert(frames.top()->resumePc == 50);
    assert(journal.drain([&](ObservableOp op, const std::string&,
                             Value v) noexcept {
        assert(op == ObservableOp::Broadcast);
        assert(frames.top()->resumePc == 50);
        broadcasts.push_back(SStr::toUtf8(v.asStrPtr()));
    }).delivered == 1);
    assert((broadcasts == std::vector<std::string>{
        "GC rooted 🌟", "fresh"}));
    assert(journal.rootedPayloads() == 0);
    std::cout << "PASS ScratchCallTransaction + CommitOnceEffects: "
                 "atomic continuation publication, GC and no replay\n";
}
