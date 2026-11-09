// Execute an actual LLJIT-compiled, resumable shared CFG from Sequencer.
// This path is opt-in and does not change the default Scratch JIT.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/cfg_lower_llvm.hpp"
#include "../../src/ir/scratch_f64_kernel.hpp"
#include "../../src/engine/numeric_thread.hpp"
#include "../../src/engine/runtime.hpp"
#include "../../src/engine/sequencer.hpp"
#include "../../src/jit/jit.hpp"
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <cassert>
#include <cstring>
#include <memory>

using namespace xyo;
using namespace xyo::ir;

static void addBlock(Blocks& blocks, const char* id, const char* opcode,
                     const char* next = "") {
    Block b;
    b.id = id;
    b.opcode = opcode;
    b.next = next;
    blocks.createBlock(std::move(b));
}

static Target* makeTarget(Runtime& rt) {
    auto* target = new Target(&rt);
    target->isStage = true;
    rt.addTarget(target);
    target->variables.emplace("x", Variable::create(
        "x", u"x", Variable::kScalarType, false));
    target->variables.emplace("y", Variable::create(
        "y", u"y", Variable::kScalarType, false));
    target->variables.at("x").value = Value::num(0);
    target->variables.at("y").value = Value::num(0);
    auto& b = *target->blocks;
    addBlock(b, "hat", "event_whenflagclicked", "set-x");
    addBlock(b, "set-x", "data_setvariableto", "repeat");
    addBlock(b, "repeat", "control_repeat", "set-y");
    addBlock(b, "inc", "data_changevariableby");
    addBlock(b, "set-y", "data_setvariableto");
    addBlock(b, "n10", "math_number");
    addBlock(b, "n3", "math_number");
    addBlock(b, "n2", "math_number");
    addBlock(b, "n7", "math_number");
    addBlock(b, "get-x", "data_variable");
    addBlock(b, "add", "operator_add");
    b.getBlock("hat")->topLevel = true;
    b.addScript("hat");
    b.getBlock("set-x")->fields["VARIABLE"].id = "x";
    b.getBlock("set-x")->inputs.set("VALUE", {"n10", ""});
    b.getBlock("repeat")->inputs.set("TIMES", {"n3", ""});
    b.getBlock("repeat")->inputs.set("SUBSTACK", {"inc", ""});
    b.getBlock("inc")->fields["VARIABLE"].id = "x";
    b.getBlock("inc")->inputs.set("VALUE", {"n2", ""});
    b.getBlock("set-y")->fields["VARIABLE"].id = "y";
    b.getBlock("set-y")->inputs.set("VALUE", {"add", ""});
    b.getBlock("add")->inputs.set("NUM1", {"get-x", ""});
    b.getBlock("add")->inputs.set("NUM2", {"n7", ""});
    b.getBlock("get-x")->fields["VARIABLE"].id = "x";
    b.getBlock("n10")->fields["NUM"].value = Value::num(10);
    b.getBlock("n3")->fields["NUM"].value = Value::num(3);
    b.getBlock("n2")->fields["NUM"].value = Value::num(2);
    b.getBlock("n7")->fields["NUM"].value = Value::num(7);
    return target;
}

// Two independent top-level repeats. Each is deoptimizable using only its
// own saved counter, without replaying the earlier loop.
static void appendSecondRepeat(Target& target) {
    auto& b = *target.blocks;
    addBlock(b, "repeat-b", "control_repeat", "set-y");
    addBlock(b, "inc-b", "data_changevariableby");
    addBlock(b, "n5", "math_number");
    addBlock(b, "n2b", "math_number");
    b.getBlock("repeat")->next = "repeat-b";
    b.getBlock("repeat-b")->inputs.set("TIMES", {"n2b", ""});
    b.getBlock("repeat-b")->inputs.set("SUBSTACK", {"inc-b", ""});
    b.getBlock("inc-b")->fields["VARIABLE"].id = "x";
    b.getBlock("inc-b")->inputs.set("VALUE", {"n5", ""});
    b.getBlock("n5")->fields["NUM"].value = Value::num(5);
    b.getBlock("n2b")->fields["NUM"].value = Value::num(2);
}

static void useDynamicRepeat(Target& target, bool arithmetic = false) {
    auto& b = *target.blocks;
    addBlock(b, "count-y", "data_variable");
    b.getBlock("count-y")->fields["VARIABLE"].id = "y";
    b.getBlock("repeat")->inputs.set("TIMES", {"count-y", ""});
    if (arithmetic) {
        addBlock(b, "count-plus", "operator_add");
        b.getBlock("count-plus")->inputs.set("NUM1", {"count-y", ""});
        b.getBlock("count-plus")->inputs.set("NUM2", {"n2", ""});
        b.getBlock("repeat")->inputs.set("TIMES", {"count-plus", ""});
    }
}

// A second, independent Scratch hat executed by the interpreter. This
// mutates the same live x slot between two compiled thread checkpoints.
static void appendInterleavedWriter(Target& target, bool writeText) {
    auto& b = *target.blocks;
    addBlock(b, "writer-hat", "event_whenflagclicked",
             writeText ? "writer-set" : "writer-repeat");
    b.getBlock("writer-hat")->topLevel = true;
    b.addScript("writer-hat");
    if (writeText) {
        addBlock(b, "writer-set", "data_setvariableto");
        addBlock(b, "writer-text", "text");
        b.getBlock("writer-set")->fields["VARIABLE"].id = "x";
        b.getBlock("writer-set")->inputs.set("VALUE", {"writer-text", ""});
        b.getBlock("writer-text")->fields["TEXT"].value = Value::emptyStr();
        return;
    }
    addBlock(b, "writer-repeat", "control_repeat");
    addBlock(b, "writer-change", "data_changevariableby");
    addBlock(b, "writer-count", "math_number");
    addBlock(b, "writer-step", "math_number");
    b.getBlock("writer-repeat")->inputs.set("TIMES", {"writer-count", ""});
    b.getBlock("writer-repeat")->inputs.set("SUBSTACK", {"writer-change", ""});
    b.getBlock("writer-change")->fields["VARIABLE"].id = "x";
    b.getBlock("writer-change")->inputs.set("VALUE", {"writer-step", ""});
    b.getBlock("writer-count")->fields["NUM"].value = Value::num(3);
    b.getBlock("writer-step")->fields["NUM"].value = Value::num(100);
}

using NativeRunner = uint64_t (*)(uint64_t*, uint64_t*);
struct NativeKernel {
    std::shared_ptr<const ScratchNumericKernel> kernel;
    std::shared_ptr<llvm::orc::LLJIT> jit;
    NativeRunner run = nullptr;
};

static NativeKernel compileNative(const Blocks& blocks) {
    auto source = ScratchScript::capture(blocks, blocks.getBlock("hat"));
    auto lowered = lowerNumericF64Script(
        source, NumericF64LoweringOptions{false, true});
    assert(lowered && lowered->slotIds.size() == 2);
    auto kernel = std::make_shared<const ScratchNumericKernel>(
        std::move(*lowered));
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("numeric-sequencer", *context);
    LlvmCfgBackend backend(*context, *module, "numeric_loop_entry");
    assert(lowerExecutable(kernel->executable, backend));
    assert(!llvm::verifyModule(*module));
    auto expected = llvm::orc::LLJITBuilder().create();
    assert(expected);
    auto jit = std::shared_ptr<llvm::orc::LLJIT>(
        std::move(*expected));
    if (auto error = jit->addIRModule(llvm::orc::ThreadSafeModule(
            std::move(module), std::move(context)))) {
        llvm::consumeError(std::move(error));
        assert(false && "LLJIT addIRModule");
    }
    auto symbol = jit->lookup("numeric_loop_entry");
    assert(symbol);
    NativeRunner runner = reinterpret_cast<NativeRunner>(symbol->getValue());
    assert(runner);
    return {std::move(kernel), std::move(jit), runner};
}

static Thread* attach(Runtime& rt, Target& target,
                      const NativeKernel& native, bool warp = false) {
    Thread* thread = rt.pushThread("hat", &target, false, false);
    assert(thread && !thread->isCompiled);
    // Keep the executable memory alive as long as the thread's runner.
    assert(attachNumericThread(*thread, native.kernel,
        [jit = native.jit, fn = native.run](
            uint64_t* slots, uint64_t* frame) -> uint64_t {
            return fn(slots, frame);
        }, warp));
    return thread;
}

static double num(const Target& target, const char* id) {
    return target.variables.at(id).value.asNumberUnchecked();
}

int main(int argc, char** argv) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    if (argc > 1 && std::strcmp(argv[1], "--auto") == 0) {
        setJitMode(JitMode::Eager);
        {
            Runtime rt;
            Target* target = makeTarget(rt);
            // Automatic code selection: Runtime::pushThread calls
            // Thread::tryCompile before the root StackFrame is installed.
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession && !thread->isCompiled);
            for (double x : {12.0, 14.0, 16.0}) {
                rt.sequencer->stepThread(*thread);
                assert(num(*target, "x") == x);
                assert(thread->numericSession);
            }
            rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "y") == 23);
            rt.sequencer->retireThread(*thread);
            thread->releaseFinished();
            // The second spawn reuses cached machine code and starts with
            // a fresh continuation, not the old thread's completed frame.
            target->variables.at("x").value = Value::num(0);
            target->variables.at("y").value = Value::num(0);
            Thread* second = rt.pushThread("hat", target, false, false);
            assert(second && second->numericSession && !second->isCompiled);
            for (int i = 0; i < 4; ++i)
                rt.sequencer->stepThread(*second);
            assert(second->status == STATUS_DONE);
            assert(num(*target, "x") == 16 && num(*target, "y") == 23);
        }
        {
            // Source mutation must invalidate the numeric runner even if
            // the cache still holds a compiled kernel; after invalidation
            // the changed source is lowered and compiled again.
            Runtime rt;
            Target* target = makeTarget(rt);
            Thread* first = rt.pushThread("hat", target, false, false);
            assert(first && first->numericSession);
            for (int i = 0; i < 4; ++i)
                rt.sequencer->stepThread(*first);
            assert(first->status == STATUS_DONE);
            rt.step(); // retire all completed script references
            target->blocks->getBlock("n2")->fields["NUM"].value =
                Value::num(3);
            Thread* stale = rt.pushThread("hat", target, false, false);
            assert(stale && !stale->numericSession);
            rt.stopThread(stale);
            rt.step();
            rt.jit->invalidate();
            target->variables.at("x").value = Value::num(0);
            target->variables.at("y").value = Value::num(0);
            Thread* updated = rt.pushThread("hat", target, false, false);
            assert(updated && updated->numericSession);
            for (int i = 0; i < 4; ++i)
                rt.sequencer->stepThread(*updated);
            assert(updated->status == STATUS_DONE);
            assert(num(*target, "x") == 19 &&
                   num(*target, "y") == 26);
        }
        {
            // Another Scratch thread may change a numeric variable after
            // a committed yield. The next compiled slice must refresh it,
            // rather than replaying the previous per-slice snapshot.
            Runtime rt;
            Target* target = makeTarget(rt);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 12);
            target->variables.at("x").value = Value::num(100);
            rt.sequencer->stepThread(*thread);
            assert(thread->numericSession && num(*target, "x") == 102);
            rt.sequencer->stepThread(*thread);
            assert(thread->numericSession && num(*target, "x") == 104);
            rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 104 && num(*target, "y") == 111);
        }
        {
            // Invalidation after a committed loop iteration must not replay
            // the script prefix. The interpreter resumes at the saved PC.
            Runtime rt;
            Target* target = makeTarget(rt);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 12);
            target->blocks->resetCache();
            rt.sequencer->stepThread(*thread);
            assert(!thread->numericSession && thread->interpreterStarted);
            for (int tick = 0; tick < 10 && thread->status != STATUS_DONE; ++tick)
                rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 16 && num(*target, "y") == 23);
        }
        {
            // Dynamic reporter is sampled ONCE before entering the loop.
            // Changing y between yields does not change the trip count.
            Runtime rt;
            Target* target = makeTarget(rt);
            useDynamicRepeat(*target);
            target->variables.at("y").value = Value::num(2.5);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            assert(thread->numericSession->kernel
                       ->executable.yieldBoundaries[0].limitValue != kNoValue);
            rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 12);
            target->variables.at("y").value = Value::num(100);
            rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 14);
            rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 16);
            rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 16 && num(*target, "y") == 23);
        }
        {
            // Dynamic arithmetic count, including Math.round tie behavior.
            Runtime rt;
            Target* target = makeTarget(rt);
            useDynamicRepeat(*target, true);
            target->variables.at("y").value = Value::num(0.5);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            for (int i = 0; i < 3; ++i)
                rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 16);
            rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "y") == 23);
        }
        {
            // A guard failure after a dynamic repeat has partially run must
            // restore the ROUNDED initial count, not re-read a changed y.
            Runtime rt;
            Target* target = makeTarget(rt);
            useDynamicRepeat(*target);
            target->variables.at("y").value = Value::num(2.5);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 12);
            target->variables.at("y").value = Value::boolean(false);
            rt.sequencer->stepThread(*thread);
            assert(thread->interpreterStarted && !thread->numericSession);
            for (int i = 0; i < 10 && thread->status != STATUS_DONE; ++i)
                rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 16 && num(*target, "y") == 23);
        }
        {
            // An out-of-range dynamic repeat bails out before committing
            // any prefix write, returning to the original interpreter.
            Runtime rt;
            Target* target = makeTarget(rt);
            useDynamicRepeat(*target);
            target->variables.at("y").value = Value::num(double(kMaxScheduledNumericRepeat) + 0.5);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            rt.sequencer->stepThread(*thread);
            assert(!thread->numericSession && thread->interpreterStarted);
            rt.stopThread(thread);
        }
        {
            // Multiple top-level repeats reuse one compiled CFG and keep
            // independent counters for each checkpoint.
            Runtime rt;
            Target* target = makeTarget(rt);
            appendSecondRepeat(*target);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            assert(thread->numericSession->kernel
                       ->executable.yieldBoundaries.size() == 2);
            for (double x : {12., 14., 16., 21., 26.}) {
                rt.sequencer->stepThread(*thread);
                assert(thread->numericSession && num(*target, "x") == x);
            }
            rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 26 && num(*target, "y") == 33);
        }
        {
            // A failed numeric guard inside the SECOND repeat must resume
            // from that repeat, not replay the first repeat.
            Runtime rt;
            Target* target = makeTarget(rt);
            appendSecondRepeat(*target);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            for (int i = 0; i < 4; ++i)
                rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 21);
            target->variables.at("x").value = Value::boolean(false);
            rt.sequencer->stepThread(*thread);
            assert(!thread->numericSession && thread->interpreterStarted);
            for (int i = 0; i < 10 && thread->status != STATUS_DONE; ++i)
                rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 5 && num(*target, "y") == 12);
        }
        {
            // The second loop's dynamic count must be evaluated only after
            // the first loop finishes, then kept despite host modifications.
            Runtime rt;
            Target* target = makeTarget(rt);
            appendSecondRepeat(*target);
            addBlock(*target->blocks, "count-y-b", "data_variable");
            target->blocks->getBlock("count-y-b")->fields["VARIABLE"].id = "y";
            target->blocks->getBlock("repeat-b")->inputs.set(
                "TIMES", {"count-y-b", ""});
            target->variables.at("y").value = Value::num(2.5);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            for (double x : {12., 14., 16., 21.}) {
                rt.sequencer->stepThread(*thread);
                assert(num(*target, "x") == x);
            }
            target->variables.at("y").value = Value::num(0);
            for (double x : {26., 31.}) {
                rt.sequencer->stepThread(*thread);
                assert(num(*target, "x") == x);
            }
            rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 31 && num(*target, "y") == 38);
        }
        {
            // A zero-trip first loop has no checkpoint. Subsequent loops
            // still yield and resume using their own counter/CFG header.
            Runtime rt;
            Target* target = makeTarget(rt);
            appendSecondRepeat(*target);
            target->blocks->getBlock("n3")->fields["NUM"].value = Value::num(0);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 15);
            assert(thread->numericSession);
            rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 20);
            rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 20 && num(*target, "y") == 27);
        }
        {
            // When the first loop already completed but yielded before
            // entering the second, a later type-change deopts from the
            // first loop's final checkpoint and starts the second once.
            Runtime rt;
            Target* target = makeTarget(rt);
            appendSecondRepeat(*target);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            for (int i = 0; i < 3; ++i)
                rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == 16);
            target->variables.at("x").value = Value::boolean(false);
            rt.sequencer->stepThread(*thread);
            assert(!thread->numericSession && thread->interpreterStarted);
            for (int i = 0; i < 12 && thread->status != STATUS_DONE; ++i)
                rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 10 && num(*target, "y") == 17);
        }
        {
            // Loop-only nesting is now accepted by the automatic tier.
            Runtime rt;
            Target* target = makeTarget(rt);
            appendSecondRepeat(*target);
            // Move the second loop into the first loop's body.
            target->blocks->getBlock("repeat")->next = "set-y";
            target->blocks->getBlock("inc")->next = "repeat-b";
            target->blocks->getBlock("repeat-b")->next.clear();
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && thread->numericSession);
            for (int i = 0; i < 20 && thread->status != STATUS_DONE; ++i)
                rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_DONE);
            assert(num(*target, "x") == 46 && num(*target, "y") == 53);
        }
        {
            // Unsupported operators are delegated to the pre-existing JIT.
            Runtime rt;
            Target* target = makeTarget(rt);
            target->blocks->getBlock("inc")->opcode = "looks_say";
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && !thread->numericSession);
            rt.stopThread(thread);
        }
        {
            // Inconsistent scalar type at compile/spawn time should not
            // poison the shared CFG code cache or make compiled code run.
            Runtime rt;
            Target* target = makeTarget(rt);
            target->variables.at("x").value = Value::boolean(false);
            Thread* thread = rt.pushThread("hat", target, false, false);
            assert(thread && !thread->numericSession);
            rt.stopThread(thread);
            target->variables.at("x").value = Value::num(0);
            Thread* next = rt.pushThread("hat", target, false, false);
            assert(next && next->numericSession);
            for (int i = 0; i < 4; ++i)
                rt.sequencer->stepThread(*next);
            assert(next->status == STATUS_DONE);
            assert(num(*target, "x") == 16 && num(*target, "y") == 23);
        }
        return 0;
    }
    setJitMode(JitMode::Off);
    {
        Runtime rt;
        Target* target = makeTarget(rt);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        for (double value : {12.0, 14.0, 16.0}) {
            rt.sequencer->stepThread(*thread);
            assert(thread->status == STATUS_RUNNING && thread->numericSession);
            assert(num(*target, "x") == value);
            assert(num(*target, "y") == 0);
        }
        rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE && !thread->numericSession);
        assert(num(*target, "x") == 16 && num(*target, "y") == 23);
        // A finished thread must not retain the compiled module or frame.
        thread->releaseFinished();
        assert(!thread->numericSession);
    }
    {
        // Two real Scratch Thread instances share one variable, and are
        // explicitly interleaved by the actual Sequencer. The compiled
        // runner must refresh external slots on every scheduler slice.
        Runtime rt;
        Target* target = makeTarget(rt);
        appendInterleavedWriter(*target, false);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* numeric = attach(rt, *target, native);
        Thread* writer = rt.pushThread("writer-hat", target, false, false);
        assert(writer && !writer->isCompiled && !writer->numericSession);
        for (double expected : {112.0, 214.0, 316.0}) {
            rt.sequencer->stepThread(*numeric);
            rt.sequencer->stepThread(*writer);
            assert(num(*target, "x") == expected);
        }
        // The interpreter checks the repeat termination on its next
        // scheduler step; the last completed iteration already yielded.
        rt.sequencer->stepThread(*numeric);
        rt.sequencer->stepThread(*writer);
        assert(numeric->status == STATUS_DONE &&
               writer->status == STATUS_DONE);
        assert(num(*target, "x") == 316 && num(*target, "y") == 323);
    }
    {
        // A real second Scratch thread changes the type between compiled
        // checkpoints. The first thread deopts without replaying its
        // committed prefix, and finishes in the normal interpreter.
        Runtime rt;
        Target* target = makeTarget(rt);
        appendInterleavedWriter(*target, true);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* numeric = attach(rt, *target, native);
        Thread* writer = rt.pushThread("writer-hat", target, false, false);
        assert(writer && !writer->isCompiled && !writer->numericSession);
        rt.sequencer->stepThread(*numeric);
        assert(num(*target, "x") == 12);
        rt.sequencer->stepThread(*writer);
        assert(writer->status == STATUS_DONE &&
               target->variables.at("x").value.isEmptyString());
        rt.sequencer->stepThread(*numeric);
        assert(numeric->interpreterStarted && !numeric->numericSession);
        for (int step = 0; step < 10 && numeric->status != STATUS_DONE; ++step)
            rt.sequencer->stepThread(*numeric);
        assert(numeric->status == STATUS_DONE);
        assert(num(*target, "x") == 4 && num(*target, "y") == 11);
    }
    {
        // Exercise the full Runtime -> Sequencer pass, not just a direct
        // stepThread call. The scheduler may resume in subsequent passes.
        Runtime rt;
        Target* target = makeTarget(rt);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        for (int tick = 0; tick < 12 && thread->status != STATUS_DONE; ++tick)
            rt.step();
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 16 && num(*target, "y") == 23);
    }
    {
        // Compiled code is never attached to a live script that has been
        // edited since it was lowered, even when its slot IDs still match.
        Runtime rt;
        Target* target = makeTarget(rt);
        const NativeKernel native = compileNative(*target->blocks);
        target->blocks->getBlock("n2")->fields["NUM"].value = Value::num(3);
        Thread* thread = rt.pushThread("hat", target, false, false);
        assert(!attachNumericThread(*thread, native.kernel,
            [fn = native.run](uint64_t* slots, uint64_t* frame) {
                return fn(slots, frame);
            }));
        rt.stopThread(thread);
        assert(thread->status == STATUS_DONE && !thread->numericSession);
    }
    {
        // A mid-repeat type switch fails the compiled numeric guard. The
        // interpreter must resume from the correct remaining loop count,
        // without executing the prefix or first iteration twice.
        Runtime rt;
        Target* target = makeTarget(rt);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(num(*target, "x") == 12);
        target->variables.at("x").value = Value::boolean(false);
        rt.sequencer->stepThread(*thread);
        assert(!thread->numericSession && thread->interpreterStarted);
        assert(thread->status == STATUS_RUNNING);
        assert(num(*target, "x") == 2);
        for (int i = 0; i < 10 && thread->status != STATUS_DONE; ++i)
            rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 4);
        assert(num(*target, "y") == 11);
    }
    {
        // Explicit attachment also supports multiple sequential repeats.
        Runtime rt;
        Target* target = makeTarget(rt);
        appendSecondRepeat(*target);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        for (double x : {12., 14., 16., 21., 26.}) {
            rt.sequencer->stepThread(*thread);
            assert(num(*target, "x") == x);
        }
        rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 26 && num(*target, "y") == 33);
    }
    {
        // Warp checks the actual Sequencer warp deadline; dangerMode
        // permits all short numeric iterations in one call.
        Runtime rt;
        rt.dangerMode = true;
        Target* target = makeTarget(rt);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native, true);
        rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 16 && num(*target, "y") == 23);
    }
    {
        Runtime rt;
        Target* target = makeTarget(rt);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(thread->numericSession);
        rt.stopThread(thread);
        assert(thread->status == STATUS_DONE && !thread->numericSession);
    }
    {
        // Warp can commit a slice then fail before returning to Sequencer.
        Runtime rt;
        rt.dangerMode = true;
        Target* target = makeTarget(rt);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = rt.pushThread("hat", target, false, false);
        unsigned calls = 0;
        assert(attachNumericThread(*thread, native.kernel,
            [fn = native.run, &calls](uint64_t* slots, uint64_t* frame) {
                if (++calls == 2) return uint64_t(2);
                return fn(slots, frame);
            }, true));
        rt.sequencer->stepThread(*thread);
        assert(calls == 2 && !thread->numericSession);
        assert(num(*target, "x") == 14); // prefix/first iteration not replayed
        for (int i = 0; i < 10 && thread->status != STATUS_DONE; ++i)
            rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 16 && num(*target, "y") == 23);
    }
    {
        Runtime rt;
        Target* target = makeTarget(rt);
        target->blocks->getBlock("hat")->opcode = "event_whengreaterthan";
        const auto source = ScratchCompilationUnit::capture(
            *target->blocks, target->blocks->getBlock("hat"));
        assert(!lowerNumericF64Script(
            source, NumericF64LoweringOptions{false, true}));
    }
    for (const char* hat : {"event_whenbroadcastreceived",
            "event_whenkeypressed", "event_whenthisspriteclicked",
            "event_whenstageclicked", "event_whenbackdropswitchesto",
            "control_start_as_clone"}) {
        Runtime rt;
        Target* target = makeTarget(rt);
        target->blocks->getBlock("hat")->opcode = hat;
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(num(*target, "x") == 12);
        target->variables.at("x").value = Value::boolean(false);
        rt.sequencer->stepThread(*thread);
        assert(!thread->numericSession && num(*target, "x") == 2);
        for (int i = 0; i < 10 && thread->status != STATUS_DONE; ++i)
            rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 4 && num(*target, "y") == 11);
    }
    {
        // A long dynamic repeat still yields every iteration, and deopt
        // restores its large remaining count without replaying the prefix.
        Runtime rt;
        Target* target = makeTarget(rt);
        useDynamicRepeat(*target);
        target->variables.at("y").value = Value::num(4000.49);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(thread->numericSession && num(*target, "x") == 12);
        target->variables.at("x").value = Value::boolean(false);
        rt.sequencer->stepThread(*thread);
        assert(!thread->numericSession && num(*target, "x") == 2);
        for (int i = 0; i < 4010 && thread->status != STATUS_DONE; ++i)
            rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 7998 && num(*target, "y") == 8005);
        {
            auto invalid = native.kernel->executable;
            bool found = false;
            for (auto& chain : invalid.instructions)
                for (auto& inst : chain)
                    if (inst.op == ExecOp::RepeatCount) {
                        assert(inst.literal == int64_t(kMaxScheduledNumericRepeat));
                        inst.literal = 0;
                        found = true;
                    }
            assert(found && !verifyExecutable(invalid));
            auto bounded = lowerNumericF64Script(
                ScratchCompilationUnit::capture(*target->blocks,
                    target->blocks->getBlock("hat")),
                NumericF64LoweringOptions{true, false});
            assert(bounded);
            found = false;
            for (auto& chain : bounded->executable.instructions)
                for (auto& inst : chain)
                    if (inst.op == ExecOp::RepeatCount) {
                        assert(inst.literal == 1024);
                        inst.literal = 1025;
                        found = true;
                    }
            assert(found && !verifyExecutable(bounded->executable));
        }
        target->blocks->getBlock("count-y")->opcode = "math_number";
        target->blocks->getBlock("count-y")->fields["NUM"].value =
            Value::num(double(kMaxScheduledNumericRepeat) + 1);
        assert(!lowerNumericF64Script(
            ScratchCompilationUnit::capture(*target->blocks,
                target->blocks->getBlock("hat")),
            NumericF64LoweringOptions{false, true}));
    }
    for (const char* opcode : {"control_while", "control_repeat_until"}) {
        Runtime rt;
        Target* target = makeTarget(rt);
        auto& blocks = *target->blocks;
        blocks.getBlock("repeat")->opcode = opcode;
        addBlock(blocks, "condition", opcode == std::string("control_while")
            ? "operator_lt" : "operator_gt");
        blocks.getBlock("condition")->inputs.set("OPERAND1", {"get-x", ""});
        blocks.getBlock("condition")->inputs.set("OPERAND2", {"n3", ""});
        blocks.getBlock("n3")->fields["NUM"].value = Value::num(
            opcode == std::string("control_while") ? 16 : 14);
        blocks.getBlock("repeat")->inputs.set("CONDITION", {"condition", ""});
        const NativeKernel native = compileNative(blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(thread->numericSession && num(*target, "x") == 12);
        target->variables.at("x").value = Value::boolean(false);
        rt.sequencer->stepThread(*thread);
        assert(!thread->numericSession && num(*target, "x") == 2);
        for (int i = 0; i < 20 && thread->status != STATUS_DONE; ++i)
            rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 16 && num(*target, "y") == 23);
    }
    {
        Runtime rt;
        Target* target = makeTarget(rt);
        target->blocks->getBlock("repeat")->opcode = "control_forever";
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(thread->numericSession && num(*target, "x") == 12);
        rt.sequencer->stepThread(*thread);
        assert(thread->numericSession && num(*target, "x") == 14);
        target->variables.at("x").value = Value::boolean(false);
        rt.sequencer->stepThread(*thread);
        assert(!thread->numericSession && num(*target, "x") == 2);
        for (int i = 0; i < 3; ++i) rt.sequencer->stepThread(*thread);
        assert(num(*target, "x") == 8 && num(*target, "y") == 0);
        rt.stopThread(thread);
        assert(thread->status == STATUS_DONE);
    }
    {
        Runtime rt;
        Target* target = makeTarget(rt);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(num(*target, "x") == 12);
        target->blocks->getBlock("n2")->fields["NUM"].value = Value::num(3);
        target->blocks->resetCache();
        rt.sequencer->stepThread(*thread);
        assert(!thread->numericSession && num(*target, "x") == 15);
        for (int i = 0; i < 10 && thread->status != STATUS_DONE; ++i)
            rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 18 && num(*target, "y") == 25);
    }
    {
        Runtime rt;
        Target* target = makeTarget(rt);
        const NativeKernel native = compileNative(*target->blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(num(*target, "x") == 12);
        target->blocks->deleteBlock("repeat");
        rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE && !thread->numericSession);
        assert(num(*target, "x") == 12 && num(*target, "y") == 0);
    }
    {
        Runtime rt;
        Target* target = makeTarget(rt);
        auto& blocks = *target->blocks;
        blocks.getBlock("repeat")->opcode = "control_while";
        addBlock(blocks, "nan-condition", "operator_lt");
        blocks.getBlock("nan-condition")->inputs.set("OPERAND1", {"get-x", ""});
        blocks.getBlock("nan-condition")->inputs.set("OPERAND2", {"n3", ""});
        blocks.getBlock("n3")->fields["NUM"].value = Value::num(16);
        blocks.getBlock("repeat")->inputs.set("CONDITION", {"nan-condition", ""});
        const NativeKernel native = compileNative(blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(num(*target, "x") == 12);
        target->variables.at("x").value = Value::numCanonical(
            std::numeric_limits<double>::quiet_NaN());
        rt.sequencer->stepThread(*thread);
        assert(!thread->numericSession);
        for (int i = 0; i < 5 && thread->status != STATUS_DONE; ++i)
            rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(std::isnan(num(*target, "x")) && num(*target, "y") == 7);
    }
    {
        Runtime rt;
        Target* target = makeTarget(rt);
        auto& blocks = *target->blocks;
        addBlock(blocks, "inner", "control_repeat");
        blocks.getBlock("repeat")->inputs.set("SUBSTACK", {"inner", ""});
        blocks.getBlock("inner")->inputs.set("TIMES", {"n2", ""});
        blocks.getBlock("inner")->inputs.set("SUBSTACK", {"inc", ""});
        const NativeKernel native = compileNative(blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(thread->numericSession && num(*target, "x") == 12);
        target->variables.at("x").value = Value::boolean(false);
        rt.sequencer->stepThread(*thread);
        assert(!thread->numericSession && num(*target, "x") == 2);
        for (int i = 0; i < 30 && thread->status != STATUS_DONE; ++i)
            rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 10 && num(*target, "y") == 17);
    }
    for (bool elseBranch : {false, true}) {
        Runtime rt;
        Target* target = makeTarget(rt);
        auto& blocks = *target->blocks;
        addBlock(blocks, "branch", elseBranch ? "control_if_else" : "control_if");
        addBlock(blocks, "branch-test", elseBranch ? "operator_lt" : "operator_gt");
        addBlock(blocks, "n5-branch", "math_number");
        addBlock(blocks, "branch-inner", "control_repeat");
        blocks.getBlock("n5-branch")->fields["NUM"].value = Value::num(5);
        blocks.getBlock("branch-test")->inputs.set("OPERAND1", {"get-x", ""});
        blocks.getBlock("branch-test")->inputs.set("OPERAND2", {"n5-branch", ""});
        blocks.getBlock("branch")->inputs.set("CONDITION", {"branch-test", ""});
        blocks.getBlock("branch")->inputs.set(elseBranch ? "SUBSTACK2" : "SUBSTACK", {"branch-inner", ""});
        blocks.getBlock("branch-inner")->inputs.set("TIMES", {"n2", ""});
        blocks.getBlock("branch-inner")->inputs.set("SUBSTACK", {"inc", ""});
        blocks.getBlock("repeat")->inputs.set("SUBSTACK", {"branch", ""});
        const NativeKernel native = compileNative(blocks);
        Thread* thread = attach(rt, *target, native);
        rt.sequencer->stepThread(*thread);
        assert(thread->numericSession && num(*target, "x") == 12);
        target->variables.at("x").value = Value::boolean(false);
        rt.sequencer->stepThread(*thread);
        assert(!thread->numericSession && num(*target, "x") == 2);
        for (int i = 0; i < 30 && thread->status != STATUS_DONE; ++i)
            rt.sequencer->stepThread(*thread);
        assert(thread->status == STATUS_DONE);
        assert(num(*target, "x") == 2 && num(*target, "y") == 9);
    }
    for (unsigned failAt = 1; failAt <= 8; ++failAt) {
        Runtime rt;
        Target* target = makeTarget(rt);
        auto& b = *target->blocks;
        addBlock(b, "prefix", "data_changevariableby", "select");
        addBlock(b, "select", "control_if", "suffix");
        addBlock(b, "suffix", "data_changevariableby");
        addBlock(b, "nested", "control_repeat");
        addBlock(b, "less40", "operator_lt");
        addBlock(b, "n1", "math_number");
        addBlock(b, "n40", "math_number");
        b.getBlock("n1")->fields["NUM"].value = Value::num(1);
        b.getBlock("n40")->fields["NUM"].value = Value::num(40);
        b.getBlock("prefix")->fields["VARIABLE"].id = "x";
        b.getBlock("prefix")->inputs.set("VALUE", {"n1", ""});
        b.getBlock("suffix")->fields["VARIABLE"].id = "x";
        b.getBlock("suffix")->inputs.set("VALUE", {"n10", ""});
        b.getBlock("less40")->inputs.set("OPERAND1", {"get-x", ""});
        b.getBlock("less40")->inputs.set("OPERAND2", {"n40", ""});
        b.getBlock("select")->inputs.set("CONDITION", {"less40", ""});
        b.getBlock("select")->inputs.set("SUBSTACK", {"nested", ""});
        b.getBlock("nested")->inputs.set("TIMES", {"n2", ""});
        b.getBlock("nested")->inputs.set("SUBSTACK", {"inc", ""});
        b.getBlock("repeat")->inputs.set("SUBSTACK", {"prefix", ""});
        const NativeKernel native = compileNative(b);
        Thread* thread = rt.pushThread("hat", target, false, false);
        unsigned calls = 0;
        assert(attachNumericThread(*thread, native.kernel,
            [fn = native.run, &calls, failAt](uint64_t* slots, uint64_t* frame) {
                if (++calls == failAt) return uint64_t(2);
                return fn(slots, frame);
            }));
        for (int step = 0; step < 100 && thread->status != STATUS_DONE; ++step)
            rt.sequencer->stepThread(*thread);
        assert(calls == failAt);
        assert(thread->status == STATUS_DONE && !thread->numericSession);
        // 10 + 3 prefixes + 2*(2*2) inner work + 3*10 suffixes.
        assert(num(*target, "x") == 51 && num(*target, "y") == 58);
    }
    return 0;
}
