// START-012: native AOT v3 must retain worker dispatch for scripts whose
// BlockCached descriptors are materialized on the scheduler thread.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <vector>
#include <sstream>
#include <string>
#include "aot/aot.hpp"
#include "jit/aot_lazy_cache.hpp"
#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/blocks.hpp"

using namespace xyo;

static std::string project(int workers, int work) {
    std::ostringstream s;
    s << R"({"targets":[{"isStage":true,"name":"Stage","variables":{},"lists":{},"broadcasts":{},"blocks":{},"costumes":[],"sounds":[]})";
    for (int i=0;i<workers;++i) {
        s << ",{\"isStage\":false,\"name\":\"W" << i
          << "\",\"layerOrder\":" << i+1
          << R"(,"variables":{"v":["v",0]},"lists":{},"broadcasts":{},"costumes":[],"sounds":[],"blocks":{)"
          << R"("hat":{"opcode":"event_whenflagclicked","topLevel":true,"next":"init","inputs":{},"fields":{}},)"
          << R"("init":{"opcode":"data_setvariableto","next":"call","inputs":{"VALUE":[1,[4,"0"]]},"fields":{"VARIABLE":["v","v"]}},)"
          << R"("call":{"opcode":"procedures_call","inputs":{},"fields":{},"mutation":{"tagName":"mutation","proccode":"work","argumentids":"[]","warp":"true"}},)"
          << R"("def":{"opcode":"procedures_definition","topLevel":true,"next":"repeat","inputs":{"custom_block":[1,"proto"]},"fields":{}},)"
          << R"("proto":{"opcode":"procedures_prototype","shadow":true,"inputs":{},"fields":{},"mutation":{"tagName":"mutation","proccode":"work","argumentids":"[]","argumentnames":"[]","argumentdefaults":"[]","warp":"true"}},)"
          << R"("repeat":{"opcode":"control_repeat","inputs":{"TIMES":[1,[4,")" << work
          << R"("]],"SUBSTACK":[2,"update"]},"fields":{}},)"
          << R"("update":{"opcode":"data_setvariableto","inputs":{"VALUE":[2,"add"]},"fields":{"VARIABLE":["v","v"]}},)"
          << R"("add":{"opcode":"operator_add","inputs":{"NUM1":[2,"mul"],"NUM2":[1,[4,"1"]]},"fields":{}},)"
          << R"("mul":{"opcode":"operator_multiply","inputs":{"NUM1":[2,"read"],"NUM2":[1,[4,"1.000000001"]]},"fields":{}},)"
          << R"("read":{"opcode":"data_variable","inputs":{},"fields":{"VARIABLE":["v","v"]}}}})";
    }
    s << R"(],"monitors":[],"extensions":[],"meta":{"semver":"3.0.0"}})";
    return s.str();
}


int main() {
#ifdef _WIN32
    _putenv_s("XYO_AOT_LAZY_CACHE_SLOTS", "1");
#else
    setenv("XYO_AOT_LAZY_CACHE_SLOTS", "1", 1);
#endif
    constexpr int workers = 8;
    constexpr int count = 30000;
    const std::string text = project(workers, count);
    constexpr uint64_t hash = 0x5354415254303132ull;
    aot::BuildResult object;
    std::string error;
    {
        Runtime source;
        if (!loadSb3(source, text, nullptr).ok ||
            !aot::compileProject(source, hash, object, &error)) {
            std::fprintf(stderr, "START-012 AOT compile failed: %s\n", error.c_str());
            return 1;
        }
    }
    Runtime runtime;
    if (!loadSb3(runtime, text, nullptr).ok ||
        !runtime.jit ||
        !runtime.jit->loadAot(runtime, object.object.data(),
                              object.object.size(), hash, &error)) {
        std::fprintf(stderr, "START-012 AOT load failed: %s\n", error.c_str());
        return 2;
    }
    runtime.greenFlag();
    unsigned workerReady = 0;
    std::vector<const CompiledScript*> readyScripts;
    for (const Thread* t : runtime.threads) {
        const auto* code = t->isCompiled
            ? static_cast<const CompiledScript*>(t->compiled) : nullptr;
        if (code && !code->mainThread && !code->aotLazyWorkerSlots.empty()) {
            ++workerReady;
            readyScripts.push_back(code);
            for (const auto* slot : code->aotLazyWorkerSlots)
                if (slot->resolved.load(std::memory_order_acquire)) {
                    std::fprintf(stderr, "START-012 eager cache initialization\n");
                    return 7;
                }
        }
    }
    if (workerReady != workers) {
        std::fprintf(stderr, "START-012 expected %d eligible AOT "
            "scripts with lazy slots, got %u\n", workers, workerReady);
        return 3;
    }
    runtime.runUntilDone(10000);
    if (!runtime.threads.empty()) {
        std::fprintf(stderr, "START-012 did not finish\n");
        return 4;
    }
    double expected = 0.0;
    for (int i = 0; i < count; ++i)
        expected = expected * 1.000000001 + 1.0;
    for (Target* target : runtime.targets) {
        if (target->isStage) continue;
        const Variable* variable = target->lookupVariableById("v");
        if (!variable || std::abs(cast::toNumber(variable->value) - expected) > 1e-7) {
            std::fprintf(stderr, "START-012 result differs on %s\n", target->id.c_str());
            return 5;
        }
    }
    const auto stats = runtime.sequencer->parallelStats();
    // If a script was actually dispatched to a worker, its entire native
    // cache manifest must already have been published by the scheduler.
    if (stats.dispatches) {
        for (const auto* code : readyScripts)
            for (const auto* slot : code->aotLazyWorkerSlots)
                if (!slot->resolved.load(std::memory_order_acquire)) {
                    std::fprintf(stderr, "START-012 missing worker prewarm\n");
                    return 8;
                }
    }
    const char* setting = std::getenv("XYO_PAR");
    const bool parallel = !setting || setting[0] != '0';
    if ((stats.dispatches > 0) != parallel) {
        std::fprintf(stderr, "START-012 wrong dispatch count %llu for par=%d\n",
            (unsigned long long)stats.dispatches, int(parallel));
        return 6;
    }
    std::printf("PASS START-012 v3 scripts=%d lazy-worker=%u "
        "dispatch=%llu batches=%llu threads=%llu\n",
        object.scripts, workerReady, (unsigned long long)stats.dispatches,
        (unsigned long long)stats.batches, (unsigned long long)stats.threads);
    // START-015: the AOT code is already attached to the eight roots.
    // Edit the original source (same block IDs, changed arithmetic) before
    // their first instruction. Native stale code must be rejected and
    // all observable results must come from the interpreter's new source.
    Runtime edited;
    if (!loadSb3(edited, text, nullptr).ok || !edited.jit ||
        !edited.jit->loadAot(edited, object.object.data(),
                             object.object.size(), hash, &error))
        return 10;
    edited.greenFlag();
    if (edited.threads.size() != workers) return 11;
    AotLazyCachedSlot* staleSlot = nullptr;
    for (Thread* t : edited.threads) {
        const auto* cs = t->isCompiled
            ? static_cast<const CompiledScript*>(t->compiled) : nullptr;
        if (!cs || !cs->aotSourceRevision || !t->blockContainer ||
            cs->aotSourceBlocks != t->blockContainer ||
            cs->aotSourceRevision != t->blockContainer->revision() ||
            cs->aotLazyWorkerSlots.empty())
            return 12;
        if (!staleSlot) staleSlot = cs->aotLazyWorkerSlots.front();
        if (edited.jit->readyScript(t->topBlockPtr, t->blockContainer) != cs ||
            edited.jit->readyScript(t->topBlockPtr) != nullptr ||
            edited.jit->readyScript(t->topBlockPtr,
                                     edited.targets.front()->blocks) != nullptr)
            return 34; // only the live, exact owning Blocks authorizes v3
    }
    for (Target* target : edited.targets) {
        if (target->isStage) continue;
        Block* add = target->blocks->getBlock("add");
        if (!add || add->opcode != "operator_add") return 13;
        // START-017: raw opcode mutation is now a versioned edit even
        // without calling resetCache() at the mutation site.
        const uint64_t beforeEdit = target->blocks->revision();
        {
            Blocks::StructuralEdit transaction(*target->blocks);
            add->opcode = "operator_subtract";
            if (target->blocks->revision() != beforeEdit) return 35;
        }
        if (target->blocks->revision() == beforeEdit) return 36;
    }
    if (xyo_rt_aot_cached(staleSlot) != nullptr) return 14;
    for (const Thread* t : edited.threads)
        if (edited.jit->readyScript(t->topBlockPtr, t->blockContainer))
            return 15;
    // Restarting a receiver is a separate fast path. It must not copy the
    // retired compiled pointer or dereference an obsolete topBlock hint.
    Thread* originalThread = edited.threads.front();
    const Block* staleTopHint = originalThread->topBlockPtr;
    Thread* restarted = edited.restartThread(originalThread, staleTopHint);
    if (!restarted || restarted->isCompiled) {
        std::fprintf(stderr, "START-015 stale AOT receiver restart\n");
        return 33;
    }
    edited.runUntilDone(10000);
    if (!edited.threads.empty()) return 16;
    double changedExpected = 0.0;
    for (int i = 0; i < count; ++i)
        changedExpected = changedExpected * 1.000000001 - 1.0;
    for (Target* target : edited.targets) {
        if (target->isStage) continue;
        const Variable* variable = target->lookupVariableById("v");
        if (!variable ||
            std::abs(cast::toNumber(variable->value) - changedExpected) > 1e-7) {
            std::fprintf(stderr, "START-015 wrong edited result on %s\n",
                         target->id.c_str());
            return 17;
        }
    }
    if (edited.sequencer->parallelStats().dispatches != 0) return 18;
    std::puts("PASS START-015 source-edited AOT roots ran in interpreter");

    // A second real project yields inside a non-warp counted loop. Editing
    // after its native Fiber has already committed effects cannot legally
    // replay its root script: there is no arbitrary LLVM->Scratch resume
    // certificate. Verify that the old Fiber is retired, never resumed.
    std::string yielding = project(1, 50000000);
    const std::string warpTrue = "\"warp\":\"true\"";
    const std::string warpFalse = "\"warp\":\"false\"";
    size_t pos = 0;
    int replacements = 0;
    while ((pos = yielding.find(warpTrue, pos)) != std::string::npos) {
        yielding.replace(pos, warpTrue.size(), warpFalse);
        pos += warpFalse.size();
        ++replacements;
    }
    if (replacements != 2) return 19;
    constexpr uint64_t yieldHash = 0x5354415254303135ull;
    aot::BuildResult yieldingObject;
    {
        Runtime source;
        if (!loadSb3(source, yielding, nullptr).ok ||
            !aot::compileProject(source, yieldHash, yieldingObject, &error))
            return 20;
    }
    Runtime mid;
    if (!loadSb3(mid, yielding, nullptr).ok || !mid.jit ||
        !mid.jit->loadAot(mid, yieldingObject.object.data(),
                           yieldingObject.object.size(), yieldHash, &error))
        return 21;
    mid.greenFlag();
    mid.step();
    if (mid.threads.size() != 1) {
        std::fprintf(stderr, "START-015 native continuation ended early\n");
        return 22;
    }
    Thread* running = mid.threads.front();
    if (!running->isCompiled || !running->jitCtx || !running->fiber) {
        std::fprintf(stderr,
            "START-015 expected an actual suspended native Fiber\n");
        return 23;
    }
    Block* changed = running->blockContainer->getBlock("add");
    if (!changed) return 24;
    const uint64_t beforeSuspendedEdit = running->blockContainer->revision();
    {
        Blocks::StructuralEdit transaction(*running->blockContainer);
        changed->opcode = "operator_subtract";
    }
    if (running->blockContainer->revision() == beforeSuspendedEdit) return 37;
    mid.step();
    if (running->status != STATUS_DONE || running->jitCtx || running->fiber) {
        std::fprintf(stderr, "START-015 stale native Fiber was resumed\n");
        return 25;
    }
    std::puts("PASS START-015 edited suspended native Fiber retired safely");

    // START-015: AOT invalidation (e.g. GPU backend integration) must
    // preserve CompiledScript addresses held by existing root Threads.
    // They should fall back on next step, not dereference freed code.
    Runtime invalidated;
    if (!loadSb3(invalidated, text, nullptr).ok || !invalidated.jit ||
        !invalidated.jit->loadAot(invalidated, object.object.data(),
                                  object.object.size(), hash, &error))
        return 26;
    invalidated.greenFlag();
    if (invalidated.threads.size() != workers) return 27;
    const auto* savedCode = static_cast<const CompiledScript*>(
        invalidated.threads.front()->compiled);
    if (!savedCode || !savedCode->aotSourceBlocks ||
        !savedCode->aotSourceRevision)
        return 28;
    invalidated.jit->invalidate();
    if (savedCode->aotSourceBlocks != nullptr ||
        !savedCode->aotSourceRevision)
        return 29;
    invalidated.runUntilDone(10000);
    if (!invalidated.threads.empty()) return 30;
    for (Target* target : invalidated.targets) {
        if (target->isStage) continue;
        const Variable* variable = target->lookupVariableById("v");
        if (!variable ||
            std::abs(cast::toNumber(variable->value) - expected) > 1e-7)
            return 31;
    }
    if (invalidated.sequencer->parallelStats().dispatches != 0) return 32;
    std::puts("PASS START-015 invalidated compiled entries preserved and rejected");
    return 0;
}
