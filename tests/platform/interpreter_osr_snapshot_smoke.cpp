// Real Thread/Blocks state: source-versioned, GC-rooted interpreter OSR
// snapshot. This test proves passive capture and fail-closed validation, NOT
// that a native LLVM runner can execute the recovered multi-frame stack.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/engine/interpreter_osr_snapshot.hpp"
#include "../../src/engine/runtime.hpp"
#include "../../src/model/target.hpp"
#include "../../src/core/sstr.hpp"
#include "../../src/core/gc.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>

using namespace xyo;

namespace {
void put(Blocks& blocks, const char* id, const char* opcode, bool top = false) {
    Block block;
    block.id = id;
    block.opcode = opcode;
    block.topLevel = top;
    blocks.createBlock(std::move(block));
}
}

int main() {
    Runtime runtime;
    Blocks blocks(&runtime);
    Target target(&runtime);
    target.blocks = &blocks;

    put(blocks, "hat", "event_whenflagclicked", true);
    put(blocks, "call", "procedures_call");
    put(blocks, "branch", "control_if_else");
    put(blocks, "loop", "control_repeat");
    put(blocks, "wait", "control_wait");
    put(blocks, "extension", "private_unknown");

    Thread thread("hat");
    thread.target = &target;
    thread.blockContainer = &blocks;
    thread.topBlockPtr = blocks.getBlock("hat");
    thread.interpreterStarted = true;
    thread.pushStack("call");
    thread.pushStack("branch");
    thread.pushStack("loop");
    assert(thread.stack.size() == thread.stackFrames.size());

    auto& call = *thread.stackFrames[0];
    call.hasParams = true;
    call.executionContext = std::make_unique<ExecContext>();
    call.executionContext->executed = true;
    // Both numerics and a heap string must survive snapshot/root collection.
    call.params[u"number"] = Value::num(53);
    call.params[u"message"] = Value::string(SStr::create(u"再開 🌙"));

    auto& branch = *thread.stackFrames[1];
    branch.warpMode = true;
    auto& loop = *thread.stackFrames[2];
    loop.warpMode = true;
    loop.isLoop = true;
    loop.executionContext = std::make_unique<ExecContext>();
    loop.executionContext->hasLoopCounter = true;
    loop.executionContext->loopCounter = 849;

    auto image = ir::InterpreterOsrSnapshot::capture(thread, 12);
    assert(image && image->depth() == 3);
    assert(image->sourceRevision() == blocks.revision());
    assert(image->schedulerEpoch() == 12);
    assert(image->rootedArguments() == 2);
    assert(image->hasCountedLoopCheckpoint());
    // No procedure definition links were installed in this synthetic
    // graph. The source continuation must fail closed even though the
    // raw Scratch control-frame snapshot is internally consistent.
    assert(!image->sourcePath());
    assert(image->validFor(thread, 12));
    assert(!image->validFor(thread, 13));
    assert(image->frames()[0].callExecuted);
    assert(image->frames()[2].loopCounter == 849);

    // Deterministic UTF-16 argument order despite unordered_map source.
    assert(image->frames()[0].arguments[0].name == u"message");
    const auto message = image->argument(0, 0);
    assert(message && message->isString());
    Gc::get().collect();
    assert(message->asStrPtr()->view() == u"再開 🌙");
    assert(!image->argument(0, 200));
    assert(!image->argument(20, 0));

    // An updated state or a changed source epoch invalidates a capture,
    // never causes a stale OSR native entry to be adopted.
    loop.executionContext->loopCounter = 848;
    assert(!image->validFor(thread, 12));
    loop.executionContext->loopCounter = 849;
    assert(image->validFor(thread, 12));
    call.params.clear();
    assert(!image->validFor(thread, 12));
    Gc::get().collect();
    assert(message->asStrPtr()->view() == u"再開 🌙");
    image.reset();
    call.params[u"number"] = Value::num(53);

    auto verifyRejected = [&](auto mutate, auto restore) {
        mutate();
        assert(!ir::InterpreterOsrSnapshot::capture(thread, 13));
        restore();
    };
    verifyRejected(
        [&] { loop.waitingReporter = true; },
        [&] { loop.waitingReporter = false; });
    verifyRejected(
        [&] { loop.executionContext->timer = std::make_unique<Timer>(); },
        [&] { loop.executionContext->timer.reset(); });
    verifyRejected(
        [&] { loop.executionContext->loopCounter = 17.2; },
        [&] { loop.executionContext->loopCounter = 849; });
    verifyRejected(
        [&] { thread.stack.back() = "extension"; },
        [&] { thread.stack.back() = "loop"; });
    verifyRejected(
        [&] { thread.stack.back().clear(); },
        [&] { thread.stack.back() = "loop"; });
    verifyRejected(
        [&] { loop.isLoop = false; loop.hasReported = true; },
        [&] { loop.isLoop = true; loop.hasReported = false; });

    auto newImage = ir::InterpreterOsrSnapshot::capture(thread, 14);
    assert(newImage && newImage->validFor(thread, 14));
    blocks.resetCache();
    assert(!newImage->validFor(thread, 14));

    std::cout << "PASS passive interpreter OSR snapshot: "
                 "loop PC, warp frames, GC-rooted args, fail-closed "
                 "reporter/timer/source-epoch\n";
    return 0;
}
