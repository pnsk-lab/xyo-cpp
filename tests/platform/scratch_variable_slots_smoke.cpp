// Real Target::Variable binding and transactional numeric slot guard tests.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/scratch_variable_slots.hpp"
#include "../../src/ir/scratch_f64_kernel.hpp"
#include "../../src/ir/scratch_continuation.hpp"
#include "../../src/engine/runtime.hpp"
#include <cassert>
#include <cstddef>

int main() {
    using namespace xyo;
    Target target(nullptr);
    target.variables.emplace("x", Variable::create(
        "x", u"x", Variable::kScalarType, false));
    target.variables.emplace("y", Variable::create(
        "y", u"y", Variable::kScalarType, false));
    auto* x = target.lookupVariableById("x");
    auto* y = target.lookupVariableById("y");
    assert(x && y);
    x->value = Value::num(5.5);
    y->value = Value::num(-0.0);
    {
        ir::ScratchNumericSlots stage(target, {"x", "y"});
        assert(stage.valid() && stage.size() == 2);
        assert(stage.bits()[0] == Value::num(5.5).bits);
        assert(stage.bits()[1] == Value::num(-0.0).bits);
        stage.data()[0] = Value::num(12.25).bits;
        stage.data()[1] = val::kNan;
        // Writes are invisible until explicit successful commit.
        assert(x->value.bits == Value::num(5.5).bits);
        assert(y->value.bits == Value::num(-0.0).bits);
        assert(stage.commit());
        assert(x->value.bits == Value::num(12.25).bits);
        assert(y->value.bits == val::kNan);
        assert(!stage.commit());
    }
    assert(x->observedOnlyNumber());

    // Bailout discards every staged write.
    assert(!ir::invokeNumericKernel(target, {"x", "y"},
        [](uint64_t* slots, size_t n) {
            assert(slots && n == 2);
            slots[0] = Value::num(999).bits;
            slots[1] = Value::num(888).bits;
            return false;
        }));
    assert(x->value.bits == Value::num(12.25).bits);
    assert(y->value.bits == val::kNan);

    // Numeric kNan is treated as zero by Scratch ToNumber; this callback
    // stands in for the generated LLVM/WASM arithmetic body.
    assert(ir::invokeNumericKernel(target, {"x", "y"},
        [](uint64_t* slots, size_t) {
            auto input = Value{slots[1]};
            assert(input.isNumber() && input.isNaN());
            slots[1] = Value::num(3.25).bits;
            return true;
        }));
    assert(y->value.bits == Value::num(3.25).bits);

    // A nonnumeric staged result cannot be committed, even if another
    // earlier scalar in the same transaction has a valid value.
    {
        ir::ScratchNumericSlots stage(target, {"x", "y"});
        assert(stage.valid());
        stage.data()[0] = Value::num(44).bits;
        stage.data()[1] = Value::boolean(true).bits;
        assert(!stage.commit());
    }
    assert(x->value.bits == Value::num(12.25).bits);
    assert(y->value.bits == Value::num(3.25).bits);

    // Concurrent or callback-side state changes invalidate the snapshot
    // before any slot write is published.
    {
        ir::ScratchNumericSlots stage(target, {"x", "y"});
        assert(stage.valid());
        stage.data()[0] = Value::num(90).bits;
        x->value = Value::num(11);
        assert(!stage.commit());
    }
    assert(x->value.bits == Value::num(11).bits);
    assert(y->value.bits == Value::num(3.25).bits);

    // Noncanonical negative NaN must not leak into Scratch's tagged Value
    // namespace on successful commit.
    {
        ir::ScratchNumericSlots stage(target, {"x"});
        assert(stage.valid());
        stage.data()[0] = UINT64_C(0xfff8000000000001);
        assert(stage.commit());
        assert(x->value.bits == val::kNan);
    }

    x->value = Value::boolean(false);
    ir::ScratchNumericSlots nonnumeric(target, {"x", "y"});
    assert(!nonnumeric.valid());
    assert(!ir::invokeNumericKernel(target, {"x"},
        [](uint64_t*, size_t) { assert(false); return true; }));
    x->value = Value::num(1);
    x->isCloud = true;
    assert(!ir::ScratchNumericSlots(target, {"x"}).valid());
    x->isCloud = false;

    assert(!ir::ScratchNumericSlots(target, {"missing"}).valid());
    assert(!ir::ScratchNumericSlots(target, {"x", "x"}).valid());

    Variable list;
    list.id = "list";
    list.type = Variable::kListType;
    target.variables.emplace("list", std::move(list));
    assert(!ir::ScratchNumericSlots(target, {"list"}).valid());

    // Test the typed runtime entrypoint against an actual Scratch-script
    // capture, including the non-yielding loop rejection.
    {
        Blocks source(nullptr);
        auto add = [&](const char* id, const char* opcode) {
            Block block;
            block.id = id;
            block.opcode = opcode;
            source.createBlock(std::move(block));
        };
        add("hat", "event_whenflagclicked");
        add("change", "data_changevariableby");
        add("num", "math_number");
        source.getBlock("hat")->next = "change";
        source.getBlock("change")->fields["VARIABLE"].id = "y";
        source.getBlock("change")->inputs.set("VALUE", {"num", ""});
        source.getBlock("num")->fields["NUM"].value = Value::num(2.25);
        const auto captured = ir::ScratchScript::capture(
            source, source.getBlock("hat"));
        const auto plain = ir::lowerNumericF64Script(captured);
        assert(plain && !plain->requiresLoopScheduler);
        assert(plain->slotIds == std::vector<std::string>{"y"});
        bool invoked = false;
        assert(ir::invokeNumericKernel(target, *plain,
            [&](uint64_t* staged, size_t n) {
                invoked = true;
                assert(n == 1);
                const double old =
                    Value{staged[0]}.asNumberUnchecked();
                staged[0] = Value::num(old + 2.25).bits;
                return true;
            }));
        assert(invoked);
        assert(y->value.bits == Value::num(5.5).bits);

        add("repeat", "control_repeat");
        add("times", "math_number");
        source.getBlock("hat")->next = "repeat";
        source.getBlock("repeat")->inputs.set("TIMES", {"times", ""});
        source.getBlock("repeat")->inputs.set("SUBSTACK", {"change", ""});
        source.getBlock("times")->fields["NUM"].value = Value::num(3);
        const auto loop = ir::ScratchScript::capture(
            source, source.getBlock("hat"));
        assert(!ir::lowerNumericF64Script(loop));
        const auto unscheduled = ir::lowerNumericF64Script(
            loop, ir::NumericF64LoweringOptions{true});
        assert(unscheduled && unscheduled->requiresLoopScheduler);
        assert(unscheduled->executable.yieldBoundaries.size() == 1);
        invoked = false;
        assert(!ir::invokeNumericKernel(target, *unscheduled,
            [&](uint64_t*, size_t) {
                invoked = true;
                return true;
            }));
        assert(!invoked);
        assert(y->value.bits == Value::num(5.5).bits);

        const auto resumed = ir::lowerNumericF64Script(
            loop, ir::NumericF64LoweringOptions{false, true});
        assert(resumed && resumed->executable.resumable);
        ir::NumericContinuation state(*resumed);
        assert(state.lastTransition() == ir::NumericSliceTransition::None);
        // A changed Scratch source is rejected by the scheduler before a
        // runner can execute; that is a distinct, observable rollback.
        {
            ir::NumericContinuation changed(*resumed);
            changed.rejectUnrunSlice(ir::NumericRollbackReason::SourceChanged);
            assert(changed.lastTransition() == ir::NumericSliceTransition::RolledBack);
            assert(changed.rollbackReason() == ir::NumericRollbackReason::SourceChanged);
            assert(changed.committedSlices() == 0 && changed.rolledBackSlices() == 1);
            assert(!changed.hasCommittedSlice());
            assert(changed.frame()[ir::NumericExecutionAbi::kPcWord] == 0);
        }
        const uint64_t header =
            resumed->executable.yieldBoundaries[0].header;
        auto status = ir::resumeNumericKernel(
            target, *resumed, state,
            [&](uint64_t* slots, uint64_t* frame) -> uint64_t {
                assert(frame[0] == 0);
                assert(slots[0] == Value::num(5.5).bits);
                slots[0] = Value::num(7.75).bits;
                frame[0] = header;
                return 1;
            });
        assert(status == ir::NumericSliceResult::Yielded);
        assert(state.lastTransition() == ir::NumericSliceTransition::YieldCommitted);
        assert(state.rollbackReason() == ir::NumericRollbackReason::None);
        assert(state.committedSlices() == 1 && state.rolledBackSlices() == 0);
        assert(state.frame()[0] == header);
        assert(y->value.bits == Value::num(7.75).bits);

        // The continuation's numeric slots and SSA frame buffer are
        // allocated once. Subsequent yields must not allocate a new one.
        uint64_t* reusedSlots = nullptr;
        uint64_t* reusedFrame = nullptr;
        // An external update between two yields must be observed by the
        // next iteration, rather than the previous staged snapshot.
        y->value = Value::num(10);
        const auto savedFrame = state.frame();
        status = ir::resumeNumericKernel(
            target, *resumed, state,
            [&](uint64_t* slots, uint64_t* frame) -> uint64_t {
                assert(frame[0] == header);
                assert(slots[0] == Value::num(10).bits);
                reusedSlots = slots;
                reusedFrame = frame;
                slots[0] = Value::num(12.25).bits;
                frame[0] = UINT64_MAX;
                frame[1] = Value::num(12.25).bits;
                return 2; // backend fault: reject atomically
            });
        assert(status == ir::NumericSliceResult::Failed);
        assert(state.lastTransition() == ir::NumericSliceTransition::RolledBack);
        assert(state.rollbackReason() == ir::NumericRollbackReason::BackendFailure);
        assert(state.committedSlices() == 1 && state.rolledBackSlices() == 1);
        assert(y->value.bits == Value::num(10).bits);
        assert(state.frame() == savedFrame && !state.completed());

        // Even a legal resume PC must not commit an invalid loop counter.
        status = ir::resumeNumericKernel(
            target, *resumed, state,
            [&](uint64_t* slots, uint64_t* frame) -> uint64_t {
                slots[0] = Value::num(999).bits;
                frame[0] = header;
                frame[ir::NumericExecutionAbi::valueWord(
                    resumed->executable.yieldBoundaries[0].counterPhi)] =
                    UINT64_MAX;
                return 1;
            });
        assert(status == ir::NumericSliceResult::Failed);
        assert(state.rollbackReason() == ir::NumericRollbackReason::InvalidContinuation);
        assert(state.committedSlices() == 1 && state.rolledBackSlices() == 2);
        assert(y->value.bits == Value::num(10).bits);
        assert(state.frame() == savedFrame && !state.completed());

        // A valid-looking return is also rejected if a runtime variable
        // was replaced while the slice was in flight.
        status = ir::resumeNumericKernel(
            target, *resumed, state,
            [&](uint64_t* slots, uint64_t* frame) -> uint64_t {
                slots[0] = Value::num(40).bits;
                frame[0] = UINT64_MAX;
                frame[1] = Value::num(40).bits;
                y->value = Value::num(11);
                return 0;
            });
        assert(status == ir::NumericSliceResult::Failed);
        assert(state.rollbackReason() == ir::NumericRollbackReason::CommitConflict);
        assert(state.committedSlices() == 1 && state.rolledBackSlices() == 3);
        assert(y->value.bits == Value::num(11).bits);
        assert(state.frame() == savedFrame && !state.completed());

        status = ir::resumeNumericKernel(
            target, *resumed, state,
            [&](uint64_t* slots, uint64_t* frame) -> uint64_t {
                assert(frame[0] == header);
                assert(slots[0] == Value::num(11).bits);
                assert(slots == reusedSlots);
                // Failed slices reuse the same scratch frame buffer;
                // no new allocation is needed.
                assert(frame == reusedFrame);
                slots[0] = Value::num(13.25).bits;
                frame[1] = Value::num(13.25).bits;
                frame[0] = UINT64_MAX;
                return 0;
            });
        assert(status == ir::NumericSliceResult::Completed);
        assert(state.lastTransition() == ir::NumericSliceTransition::Completed);
        assert(state.rollbackReason() == ir::NumericRollbackReason::None);
        assert(state.committedSlices() == 2 && state.rolledBackSlices() == 3);
        assert(state.completed() &&
               state.resultBits() == Value::num(13.25).bits);
        assert(y->value.bits == Value::num(13.25).bits);
        invoked = false;
        status = ir::resumeNumericKernel(
            target, *resumed, state,
            [&](uint64_t*, uint64_t*) -> uint64_t {
                invoked = true;
                return 0;
            });
        assert(status == ir::NumericSliceResult::Failed && !invoked);
        assert(state.lastTransition() == ir::NumericSliceTransition::RolledBack);
        assert(state.rollbackReason() == ir::NumericRollbackReason::InvalidState);
        assert(state.committedSlices() == 2 && state.rolledBackSlices() == 4);

        // Commit history survives rollback of a later slice in the SAME
        // warp drive; an outer scheduler need not have observed a yield.
        {
            ir::NumericContinuation partial(*resumed);
            assert(!partial.hasCommittedSlice());
            unsigned calls = 0;
            const uint64_t original = y->value.bits;
            status = ir::driveNumericKernel(
                target, *resumed, partial, true,
                [&](uint64_t* slots, uint64_t* frame) -> uint64_t {
                    if (++calls == 2) {
                        slots[0] = Value::num(999).bits;
                        frame[0] = UINT64_MAX;
                        return 2;
                    }
                    slots[0] = Value::num(42).bits;
                    frame[0] = header;
                    return 1;
                }, [] { return false; });
            assert(status == ir::NumericSliceResult::Failed && calls == 2);
            assert(partial.hasCommittedSlice() && !partial.completed());
            assert(partial.committedSlices() == 1 && partial.rolledBackSlices() == 1);
            assert(partial.lastTransition() == ir::NumericSliceTransition::RolledBack);
            assert(partial.rollbackReason() == ir::NumericRollbackReason::BackendFailure);
            assert(partial.frame()[0] == header);
            assert(y->value.bits == Value::num(42).bits);
            y->value.bits = original;
        }

        ir::NumericContinuation warpState(*resumed);
        unsigned ticks = 0;
        unsigned probes = 0;
        auto worker = [&](uint64_t* slots, uint64_t* frame) -> uint64_t {
            if (ticks++ < 3) {
                auto number = Value{slots[0]}.asNumberUnchecked();
                slots[0] = Value::num(number + 2.25).bits;
                frame[0] = header;
                return 1;
            }
            frame[1] = slots[0];
            frame[0] = UINT64_MAX;
            return 0;
        };
        // Warp deadline not exhausted: repeat all three iterations in
        // this scheduler invocation, but still checkpoint/commit each one.
        status = ir::driveNumericKernel(
            target, *resumed, warpState, true, worker,
            [&] { ++probes; return false; });
        assert(status == ir::NumericSliceResult::Completed);
        assert(probes == 3 && ticks == 4);
        assert(y->value.bits == Value::num(20).bits);

        ir::NumericContinuation regularState(*resumed);
        ticks = 0;
        invoked = false;
        status = ir::driveNumericKernel(
            target, *resumed, regularState, false, worker,
            [&] { invoked = true; return false; });
        assert(status == ir::NumericSliceResult::Yielded);
        assert(ticks == 1 && !invoked);
        assert(y->value.bits == Value::num(22.25).bits);
    }

    // Reusable owner-bound snapshots rebind fresh Variable* on every slice.
    // No stale pointer survives a Scratch variable replacement between
    // yields, and changed values are picked up without reallocating slots.
    {
        ir::ScratchNumericSlots staged(target, {"x", "y"});
        assert(staged.valid());
        auto* stableBuffer = staged.data();
        staged.data()[1] = Value::num(20).bits;
        assert(staged.commit());
        target.variables.at("y").value = Value::num(33);
        assert(staged.refresh());
        assert(staged.data() == stableBuffer);
        assert(staged.data()[1] == Value::num(33).bits);
        staged.data()[1] = Value::num(42).bits;
        assert(staged.commit());
        assert(y->value.bits == Value::num(42).bits);

        target.variables.erase("y");
        assert(!staged.refresh());
        target.variables.emplace("y", Variable::create(
            "y", u"y", Variable::kScalarType, false));
        target.variables.at("y").value = Value::num(50);
        assert(staged.refresh());
        assert(staged.data() == stableBuffer);
        assert(staged.data()[1] == Value::num(50).bits);
    }

    // Structural generations catch an erase+reinsert even when allocator
    // reuse makes the new Variable occupy exactly the old address.
    {
        Target local(nullptr);
        const uint64_t initial = local.variables.generation();
        local.variables.emplace("a", Variable::create(
            "a", u"a", Variable::kScalarType, false));
        const uint64_t inserted = local.variables.generation();
        assert(inserted != initial);
        local.variables.emplace("a", Variable::create(
            "a", u"a", Variable::kScalarType, false));
        assert(local.variables.generation() == inserted);
        {
            ir::ScratchNumericSlots stage(local, {"a"});
            assert(stage.valid());
            stage.data()[0] = Value::num(12).bits;
            local.variables.erase("a");
            local.variables.emplace("a", Variable::create(
                "a", u"a", Variable::kScalarType, false));
            assert(local.variables.generation() != inserted);
            assert(!stage.commit());
            assert(stage.refresh());
            assert(stage.data()[0] == Value::num(0).bits);
            stage.data()[0] = Value::num(17).bits;
            assert(stage.commit());
            assert(local.variables.at("a").value.bits ==
                   Value::num(17).bits);
        }
        const auto oldStamp = local.variableBindingStamp();
        local.variables["a"] = Variable::create(
            "a", u"a", Variable::kScalarType, false);
        assert(local.variableBindingStamp() != oldStamp);
        std::unordered_map<std::string, Variable> other;
        other.emplace("a", Variable::create(
            "a", u"a", Variable::kScalarType, false));
        const auto beforeAssignment = local.variables.generation();
        local.variables = std::move(other);
        assert(local.variables.generation() != beforeAssignment);
    }

    // Stage variables are shared by sprite targets. A local variable with
    // the same ID shadows the stage, and deleting it exposes stage again.
    {
        Runtime runtime;
        auto* stage = new Target(&runtime);
        stage->isStage = true;
        runtime.addTarget(stage);
        auto* sprite = new Target(&runtime);
        runtime.addTarget(sprite);
        auto* clone = new Target(&runtime);
        runtime.addTarget(clone);
        stage->variables.emplace("global", Variable::create(
            "global", u"global", Variable::kScalarType, false));
        stage->variables.at("global").value = Value::num(41);
        sprite->variables.emplace("local", Variable::create(
            "local", u"local", Variable::kScalarType, false));
        clone->variables = sprite->variables;

        ir::ScratchNumericSlots slots(*sprite, {"global", "local"});
        assert(slots.valid());
        auto* stable = slots.data();
        assert(slots.data()[0] == Value::num(41).bits);
        slots.data()[0] = Value::num(42).bits;
        assert(slots.commit());
        assert(stage->variables.at("global").value.bits ==
               Value::num(42).bits);

        assert(slots.refresh());
        slots.data()[0] = Value::num(900).bits;
        sprite->variables.emplace("global", Variable::create(
            "global", u"global", Variable::kScalarType, false));
        assert(!slots.commit()); // a local ID started shadowing stage
        assert(slots.refresh());
        assert(slots.data() == stable);
        assert(slots.data()[0] == Value::num(0).bits);
        slots.data()[0] = Value::num(11).bits;
        assert(slots.commit());
        assert(stage->variables.at("global").value.bits ==
               Value::num(42).bits);
        assert(sprite->variables.at("global").value.bits ==
               Value::num(11).bits);

        // A clone has a separate local map and sees the stage-global
        // binding, unaffected by the sprite's newly inserted local ID.
        ir::ScratchNumericSlots cloneSlots(*clone, {"global", "local"});
        assert(cloneSlots.valid());
        assert(cloneSlots.data()[0] == Value::num(42).bits);
        assert(cloneSlots.data()[1] == Value::num(0).bits);
        const auto cloneStamp = clone->variableBindingStamp();
        sprite->variables.erase("global");
        assert(clone->variableBindingStamp() == cloneStamp);
        assert(slots.refresh());
        assert(slots.data()[0] == Value::num(42).bits);

        // Erasing a global makes every existing sprite binding invalid,
        // before it can dereference the borrowed stage Variable pointer.
        slots.data()[0] = Value::num(55).bits;
        stage->variables.erase("global");
        assert(!slots.commit());
        assert(!slots.refresh());
        stage->variables.emplace("global", Variable::create(
            "global", u"global", Variable::kScalarType, false));
        stage->variables.at("global").value = Value::num(66);
        assert(slots.refresh());
        assert(slots.data()[0] == Value::num(66).bits);
        assert(cloneSlots.refresh());
        assert(cloneSlots.data()[0] == Value::num(66).bits);
        slots.data()[0] = Value::num(77).bits;
        assert(slots.commit());
        assert(stage->variables.at("global").value.bits ==
               Value::num(77).bits);

        const auto stamp = sprite->variableBindingStamp();
        runtime.stopAll(); // does not replace variable maps
        assert(sprite->variableBindingStamp() == stamp);
    }

    // Erasing a variable during a callback invalidates the binding.
    // The commit path rejects it without touching a stale pointer.
    {
        ir::ScratchNumericSlots stage(target, {"x"});
        assert(stage.valid());
        stage.data()[0] = Value::num(123).bits;
        target.variables.erase("x");
        assert(!stage.commit());
    }
    return 0;
}
