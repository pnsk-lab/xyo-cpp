// Real GC + deterministic side-effect staging / no-replay smoke.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/commit_once_effects.hpp"
#include "../../src/core/sstr.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace xyo;
using namespace xyo::ir;

int main() {
    Gc& gc = Gc::get();
    CommitOnceEffects effects(gc);
    int committedVariable = 0;
    std::vector<std::string> delivered;
    delivered.reserve(16);

    const auto send = [&](ObservableOp op, const std::string& blockId,
                          Value payload) noexcept {
        assert(committedVariable > 0); // runtime state published before effect
        gc.collect(); // payload must remain a root throughout the call
        assert(op == ObservableOp::Broadcast || op == ObservableOp::PenStamp ||
               op == ObservableOp::Say);
        if (payload.isString())
            delivered.push_back(blockId + ":" +
                                SStr::toUtf8(payload.asStrPtr()));
        else
            delivered.push_back(blockId + ":stamp");
    };

    {
        auto discarded = effects.begin(1);
        assert(!discarded.stage(static_cast<ObservableOp>(255), "bad",
                                Value::undefined()));
        assert(!discarded.stage(ObservableOp::Broadcast, "",
                                Value::undefined()));
        assert(discarded.stage(ObservableOp::Broadcast, "broadcast",
                               Value::string(SStr::fromUtf8("no-send"))));
        assert(effects.rootedPayloads() == 1);
        gc.collect();
    }
    assert(effects.rootedPayloads() == 0);
    assert(effects.pendingCount() == 0);
    assert(effects.revision() == 0);

    {
        auto rejected = effects.begin(1);
        assert(rejected.stage(ObservableOp::Broadcast, "broadcast",
                              Value::string(SStr::fromUtf8("guard-failed"))));
        assert(!rejected.commit([] { return false; }));
    }
    assert(effects.revision() == 0);
    assert(effects.rootedPayloads() == 0);
    assert(delivered.empty());

    // Two speculative runners saw revision zero. Exactly one commits; the
    // stale one can no longer produce duplicate observable actions.
    {
        auto stale = effects.begin(1);
        auto successful = effects.begin(2);
        assert(stale.stage(ObservableOp::Broadcast, "old",
                           Value::string(SStr::fromUtf8("stale"))));
        assert(successful.stage(
            ObservableOp::Broadcast, "broadcast",
            Value::string(SStr::fromUtf8("hello 🌟"))));
        assert(successful.stage(ObservableOp::PenStamp, "pen-stamp",
                                Value::undefined()));
        gc.collect();
        assert(delivered.empty());
        assert(successful.commit([&] {
            committedVariable = 42;
            return true;
        }));
        assert(!stale.commit([&] {
            committedVariable = -1;
            return true;
        }));
    }
    assert(committedVariable == 42);
    assert(effects.pendingCount() == 2);
    assert(effects.rootedPayloads() == 2);
    assert(effects.revision() == 1);
    assert(effects.lastPublishedSlice() == 2);
    gc.collect(); // committed but undelivered events are GC roots
    assert(effects.drain(send).delivered == 2);
    assert(effects.pendingCount() == 0);
    assert(effects.rootedPayloads() == 0);
    assert(delivered.size() == 2);
    assert(delivered[0] == "broadcast:hello 🌟");
    assert(delivered[1] == "pen-stamp:stamp");
    assert(effects.drain(send).delivered == 0);

    // Reusing the published slice ID is rejected even on a newer revision.
    {
        auto replay = effects.begin(2);
        assert(replay.stage(ObservableOp::Say, "say",
                            Value::string(SStr::fromUtf8("duplicate"))));
        assert(!replay.commit([&] { committedVariable = 99; return true; }));
    }
    assert(effects.rootedPayloads() == 0);
    assert(committedVariable == 42);
    assert(delivered.size() == 2);

    {
        auto fresh = effects.begin(3);
        assert(fresh.stage(ObservableOp::Say, "say",
                           Value::string(SStr::fromUtf8("new"))));
        assert(fresh.commit([&] { committedVariable = 43; return true; }));
    }
    assert(effects.drain(send).delivered == 1);
    assert(delivered[2] == "say:new");
    assert(effects.revision() == 2);
    assert(effects.lastPublishedSlice() == 3);
    assert(effects.rootedPayloads() == 0);
    // Regression: callback attempting to commit a later slice must fail
    // while outer scheduler publication is in progress. Otherwise the
    // outer transaction could move lastPublishedSlice backwards.
    {
        auto outer = effects.begin(4);
        auto nested = effects.begin(5);
        assert(outer.stage(ObservableOp::Say, "outer",
                           Value::string(SStr::fromUtf8("outer"))));
        assert(nested.stage(ObservableOp::Say, "nested",
                            Value::string(SStr::fromUtf8("nested"))));
        assert(outer.commit([&] {
            assert(!nested.commit([&] {
                committedVariable = -999;
                return true;
            }));
            assert(effects.drain(send).delivered == 0);
            committedVariable = 44;
            return true;
        }));
    }
    assert(effects.revision() == 3);
    assert(effects.lastPublishedSlice() == 4);
    assert(committedVariable == 44);
    assert(effects.drain(send).delivered == 1);
    assert(delivered.back() == "outer:outer");

    // Reentrant drain must not deliver event B during event A's callback.
    // The outer drain owns the FIFO and delivers each record once.
    {
        auto slice = effects.begin(6);
        assert(slice.stage(ObservableOp::Say, "A",
                           Value::string(SStr::fromUtf8("a"))));
        assert(slice.stage(ObservableOp::Say, "B",
                           Value::string(SStr::fromUtf8("b"))));
        assert(slice.commit([&] { committedVariable = 45; return true; }));
        std::vector<std::string> order;
        const auto guarded = [&](ObservableOp op, const std::string& name,
                                 Value payload) noexcept {
            assert(op == ObservableOp::Say);
            assert(effects.drain(send).delivered == 0);
            gc.collect();
            order.push_back(name + ":" +
                            SStr::toUtf8(payload.asStrPtr()));
        };
        assert(effects.drain(guarded).delivered == 2);
        assert(order.size() == 2);
        assert(order[0] == "A:a" && order[1] == "B:b");
        assert(effects.drain(guarded).delivered == 0);
        assert(effects.pendingCount() == 0);
        assert(effects.rootedPayloads() == 0);
    }

    std::cout << "PASS commit-once effects: GC, rollback, state publication, "
                 "stale/replay rejection and ordered delivery\n";
}
