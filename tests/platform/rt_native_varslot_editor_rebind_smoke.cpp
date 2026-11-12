// RT-001: mutate an actual Scratch variable binding while a native
// LLVM Fiber is suspended, then resume the compiled script. The guarded
// variant must resolve freshly by Scratch ID/name instead of using the
// stale Variable* in Thread::jitVarSlots or hoisted LLVM IR.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/engine/runtime.hpp"
#include "../../src/engine/sequencer.hpp"
#include "../../src/engine/thread.hpp"
#include "../../src/engine/fiber.hpp"
#include "../../src/jit/jit.hpp"
#include "../../src/load/sb3.hpp"
#include "../../src/model/target.hpp"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace xyo;

namespace {
void env(const char* name, const char* value) {
#ifdef _WIN32
    assert(_putenv_s(name, value) == 0);
#else
    assert(::setenv(name, value, 1) == 0);
#endif
}
Target* findBench(Runtime& rt) {
    for (Target* t : rt.targets)
        if (t && t->sprite && t->sprite->name == u"Bench")
            return t;
    return nullptr;
}
double number(Target& target, const char* id) {
    Variable* v = target.lookupVariableById(id);
    assert(v && v->value.isNumber());
    return v->value.asNumberUnchecked();
}
} // namespace

int main(int argc, char** argv) {
    assert(argc == 2 && "usage: smoke project.sb3");
    env("XYO_JIT_MODE", "eager");
    env("XYO_RT_GUARD_NATIVE_VAR_SLOTS", "1");
    env("XYO_JIT_ASYNC", "0");
    // Check that the experimental flag forces serial ownership even
    // if an application explicitly requests parallel JIT workers.
    env("XYO_PAR", "1");
    env("XYO_PARFOR", "0");
    env("XYO_GPU", "0");
    setJitMode(JitMode::Eager);

    std::ifstream in(argv[1], std::ios::binary);
    assert(in && "missing sb3");
    const std::vector<uint8_t> source{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
    assert(!source.empty());

    Runtime rt;
    assert(rt.sequencer && !rt.sequencer->parallelEnabled());
    const auto loaded = loadSb3File(rt, source.data(), source.size());
    assert(loaded.ok && "invalid sb3");
    Target* target = findBench(rt);
    assert(target);
    // Force at least one scheduler return from a REAL native Fiber
    // rather than completing a 2,400-iteration loop in one 33-ms tick.
    constexpr double loopCount = 2400;
    const bool stageShadow = [] {
        const char* v = std::getenv("XYO_RT001_VARSLOT_EDITOR_STAGE_SHADOW");
        return v && v[0] == '1';
    }();
    const bool stageGlobal = [] {
        const char* v = std::getenv("XYO_RT001_VARSLOT_EDITOR_STAGE_GLOBAL");
        return v && v[0] == '1';
    }();
    assert(!stageShadow || !stageGlobal);
    if (stageShadow) {
        // Before compiling the SAME Scratch script, bind its source
        // variable to the Stage. An editor local shadow is then added
        // while the native Fiber is suspended, with no second yield
        // required from this bounded test script.
        Target* stage = rt.getTargetForStage();
        assert(stage);
        Variable* global = stage->lookupOrCreateVariable("v_x", u"x");
        assert(global);
        target->deleteVariable("v_x");
        assert(target->lookupVariableById("v_x") == global);
    }
    rt.currentStepTime = 0.25;
    rt.greenFlag();

    double atEdit = -1;
    bool nativeWasSuspended = false;
    Variable* oldVariable = nullptr;
    uint64_t stampBeforeEdit = 0;

    for (unsigned step = 0; step < 10000 && !rt.threads.empty(); ++step) {
        rt.step();
        if (atEdit >= 0) continue;
        const double y = number(*target, "v_y");
        if (y < 3 || y > loopCount - 3) continue;
        for (Thread* t : rt.threads) {
            if (!t || t->target != target || !t->isCompiled ||
                !t->jitCtx || !t->fiber || t->fiber->finished())
                continue;
            nativeWasSuspended = true;
            // In the stamped-fast variant, the real LLVM-generated
            // code must have exercised actual guarded cache hits and
            // misses before the editor invalidates the binding.
            if (const char* fast = std::getenv(
                    "XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST");
                fast && fast[0] == '1') {
                assert(t->nativeVarCacheValid);
                const bool inlineTrace = [] {
                    const char* v = std::getenv(
                        "XYO_JIT_VAR_GUARD_INLINE_TRACE");
                    return v && v[0] == '1';
                }();
                if (inlineTrace)
                    assert(t->nativeVarInlineTraceHits > 0);
                else
                    assert(t->nativeVarCacheHits > 0);
                assert(t->nativeVarCacheMisses > 0);
            }
            oldVariable = target->lookupVariableById("v_x");
            assert(oldVariable && number(*target, "v_x") == y);
            const auto beforeStamp = target->variableBindingStamp();
            stampBeforeEdit = beforeStamp.localGeneration;
            Target* oldOwner = stageShadow
                ? rt.getTargetForStage() : target;
            assert(oldOwner);
            oldOwner->deleteVariable("v_x");
            const auto erasedStamp = target->variableBindingStamp();
            assert(erasedStamp != beforeStamp);
            assert(target->lookupVariableById("v_x") == nullptr);
            // Native Fiber has NOT been resumed. No dereference or
            // identity-dependent comparison of the old pointer.
            Target* newOwner = stageGlobal
                ? rt.getTargetForStage() : target;
            assert(newOwner);
            Variable* replacement = newOwner->lookupOrCreateVariable(
                "v_x", u"x");
            assert(replacement);
            replacement->value = Value::num(
                stageShadow ? 300 : 200);
            assert(target->variableBindingStamp() != erasedStamp);
            atEdit = y;
            std::cerr << "[rt001-varslot-editor] native_suspended=1"
                      << " y_at_edit=" << atEdit
                      << " old_generation=" << stampBeforeEdit
                      << " new_generation="
                      << target->variableBindingStamp().localGeneration
                      << '\n';
            break;
        }
    }

    assert(nativeWasSuspended && atEdit >= 3);
    assert(rt.threads.empty() && "native script did not complete");
    const double y = number(*target, "v_y");
    const double x = number(*target, "v_x");
    const double expected =
        (stageShadow ? 300 : 200) + (loopCount - atEdit);
    if (y != loopCount || x != expected) {
        std::cerr << "FAIL guarded native varslot x=" << x
                  << " expected=" << expected << " y=" << y
                  << " edit_at=" << atEdit << '\n';
        return 2;
    }
    std::cout << "PASS RT-001 native LLVM Fiber editor mutation: "
              << "stale Variable* rejected; x=" << x
              << " y=" << y << " edited_after=" << atEdit
              << " iterations; stage_shadow=" << int(stageShadow)
              << " stage_global=" << int(stageGlobal) << "\n";
}
