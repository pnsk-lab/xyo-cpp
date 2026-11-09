// Deterministic source edit AFTER a native Fiber has yielded, before any
// further LLVM instruction executes. The original interpreter stack owns
// all effects and GC roots. No product test hooks or concurrent writers.
//
// The root's one-time "set sum = 0" has already committed before this test
// edits its VARIABLE field id. The change is a legitimate source mutation
// but cannot affect any remaining Interpreter behavior. With canonical
// source digest proof ON, cached native OSR must refuse the changed image.
// With the proof OFF, the same unversioned source edit is intentionally
// accepted by the older revision-only OSR, providing a positive control.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../../src/engine/runtime.hpp"
#include "../../src/engine/thread.hpp"
#include "../../src/ir/scratch_pipeline.hpp"
#include "../../src/ir/scratch_source_digest.hpp"
#include "../../src/jit/jit.hpp"
#include "../../src/load/sb3.hpp"
#include "../../src/model/target.hpp"
#include "../../src/core/sstr.hpp"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace xyo;

namespace {

void env(const char* name, const char* value) {
#if defined(_WIN32)
    assert(_putenv_s(name, value) == 0);
#else
    assert(::setenv(name, value, 1) == 0);
#endif
}

std::string canonical(Value v) {
    if (v.isBool()) return v.asBool() ? "true" : "false";
    if (v.isString()) return "s:" + SStr::toUtf8(v.asStrPtr());
    if (v.isNumber()) {
        std::ostringstream out;
        out << std::setprecision(17) << v.asNumberUnchecked();
        return "n:" + out.str();
    }
    std::ostringstream out;
    out << "raw:" << std::hex << v.bits;
    return out.str();
}

void check(bool condition, const char* reason) {
    if (condition) return;
    std::cerr << "FAIL " << reason << '\n';
    std::exit(2);
}

Target* findBench(Runtime& rt) {
    for (Target* target : rt.targets)
        if (target && target->sprite &&
            target->sprite->name == u"Bench")
            return target;
    return nullptr;
}

Block* oneTimeSumAssignment(Blocks& blocks) {
    for (auto& [id, node] : blocks.allBlocks()) {
        if (node.opcode != "data_setvariableto") continue;
        const auto field = node.fields.find("VARIABLE");
        if (field != node.fields.end() && field->second.id == "v_sum")
            return &node;
    }
    return nullptr;
}

bool injectSilentEdit(
    Thread& thread, const CompiledScript& ready, bool bumpRevision
) {
    Blocks& blocks = *thread.blockContainer;
    const Block* root = blocks.getBlock(thread.topBlock);
    if (!root || !ready.nestedOsrEntry ||
        blocks.revision() != ready.nestedOsrSourceRevision)
        return false;
    Block* previous = oneTimeSumAssignment(blocks);
    if (!previous) return false;
    auto field = previous->fields.find("VARIABLE");
    if (field == previous->fields.end() || field->second.id != "v_sum")
        return false;
    const auto prior = ir::ScratchCompilationUnit::capture(blocks, root);
    if (!prior.valid()) return false;
    const auto before = ir::scratchSourceDigest(prior);
    const uint64_t revision = blocks.revision();
    // The assignment ran before "entered" became 1. Preserve BlockCached
    // and revision to model a direct, non-resetCache editor/extension edit.
    field->second.id = "v_done";
    if (bumpRevision)
        blocks.bumpRevisionForTestingWithoutCacheReset();
    const auto after = ir::ScratchCompilationUnit::capture(blocks, root);
    check(after.valid(), "post-edit source capture invalid");
    check(ir::scratchSourceDigest(after) != before,
          "silent edit did not change canonical source digest");
    if (!bumpRevision)
        check(blocks.revision() == revision,
              "silent edit accidentally changed Blocks revision");
    else
        check(blocks.revision() != revision,
              "revision control did not increment");
    std::cerr
        << "[osr-silent-edit] injected block=" << previous->id
        << " revision-before=" << revision
        << " revision-after=" << blocks.revision()
        << " digest-changed=1"
        << " tested-proof=" << int(ready.nestedOsrSourceProofRequired)
        << '\n';
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr
            << "usage: xyo-osr-silent-edit <project.sb3> "
            << "<baseline|positive|unguarded|guarded|versioned>\n";
        return 2;
    }
    const std::string mode = argv[2];
    const bool baseline = mode == "baseline";
    const bool edited = mode == "unguarded" ||
                        mode == "guarded" ||
                        mode == "versioned";
    const bool proof = !baseline;
    const bool inFlight = mode == "positive" ||
                          mode == "guarded" || mode == "versioned";
    check(baseline || edited || mode == "positive", "unknown test mode");
    env("XYO_JIT_MODE", baseline ? "off" : "hotspot");
    env("XYO_JIT_ASYNC", "1");
    env("XYO_JIT_OSR", "1");
    env("XYO_JIT_OSR_NESTED", "1");
    env("XYO_JIT_OSR_NESTED_DEEP", "1");
    env("XYO_JIT_OSR_NESTED_PARENT_REPEAT", "1");
    env("XYO_JIT_OSR_NESTED_ARG_READS", "1");
    env("XYO_JIT_OSR_NESTED_DIRECT_ARGS", "1");
    env("XYO_JIT_OSR_NESTED_HOIST_ARGS", "1");
    env("XYO_JIT_OSR_NESTED_SOURCE_PROOF", proof ? "1" : "0");
    env("XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT", inFlight ? "1" : "0");
    env("XYO_JIT_OSR_TIMING", "1");
    env("XYO_JIT_OSR_NESTED_TRACE", "1");
    env("XYO_PAR", "0");
    env("XYO_PARFOR", "0");
    env("XYO_GPU", "0");

    setJitMode(baseline ? JitMode::Off : JitMode::HotSpot);
    std::ifstream file(argv[1], std::ios::binary);
    check(bool(file), "cannot open fixture");
    std::vector<uint8_t> source{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };
    check(!source.empty(), "empty fixture");

    Runtime rt;
    const auto loaded = loadSb3File(rt, source.data(), source.size());
    check(loaded.ok, "loadSb3File failed");
    Target* target = findBench(rt);
    check(target && target->blocks, "Bench target missing");
    // The timeout is a public runtime scheduler configuration, not an
    // instrumentation hook. It makes an actual warp Fiber suspension occur
    // in a small GC/string test rather than requiring a 500ms CPU burst.
    rt.warpTimeMs = baseline ? 500.0 : 3.0;
    rt.greenFlag();

    bool changed = false;
    bool published = false;
    bool sawNativeYield = false;
    double deoptCheckpoint = -1.0;
    uint64_t iterations = 0;
    constexpr uint64_t maxSteps = 200000;
    for (; iterations < maxSteps; ++iterations) {
        if (rt.threads.empty()) break;
        // The host may edit the live source ONLY while the native Fiber
        // is suspended at xyo_yield. It is a serial scheduler safe point.
        if (!changed && !baseline) {
            for (Thread* t : rt.threads) {
                if (!t || !t->jitCtx || !t->fiber ||
                    !t->isCompiled || !t->nestedOsrActive ||
                    !t->topBlockPtr || !t->blockContainer)
                    continue;
                const auto* candidate = static_cast<const CompiledScript*>(
                    t->compiled);
                if (!candidate || !candidate->nestedOsrEntry ||
                    candidate->nestedOsrInFlightGuard != inFlight)
                    continue;
                sawNativeYield = true;
                published = true;
                if (inFlight) {
                    check(t->jitCtx->nestedOsrDeoptReady == 1,
                          "native yield missing post-body safe checkpoint");
                    deoptCheckpoint = t->jitCtx->nestedOsrDeoptRemaining;
                    check(deoptCheckpoint >= 0.0,
                          "negative source loop checkpoint");
                }
                if (edited) {
                    changed = injectSilentEdit(
                        *t, *candidate, mode == "versioned");
                    if (changed) {
                        std::cerr
                            << "[osr-inflight] edited while suspended "
                            << "remaining=" << deoptCheckpoint
                            << " guard=" << int(inFlight) << '\n';
                        break;
                    }
                }
            }
        }
        rt.step();
    }
    check(iterations < maxSteps, "runtime exceeded step budget");
    if (!baseline) {
        check(published && sawNativeYield,
              "native LLVM never yielded at an observable safe point");
    }
    if (edited) check(changed, "source edit was not injected at a native yield");
    check(rt.threads.empty(), "project left unfinished threads");
    const auto number = [&](const char* id) {
        const Variable* v = target->lookupVariableById(id);
        if (!v || !v->value.isNumber()) {
            std::cerr << "missing/non-numeric variable id=" << id
                      << " found=" << int(v != nullptr)
                      << " value=" << (v ? canonical(v->value) : "(none)")
                      << " steps=" << iterations << '\n';
            check(false, "missing numeric result");
        }
        return v->value.asNumberUnchecked();
    };
    check(number("v_sum") == 48000000.0, "incorrect sum");
    check(number("v_entered") == 3.0, "outer repeat restarted");
    check(number("v_afterInner") == 3.0,
          "caller suffix replay/missing");
    // Scratch assignments can preserve string literal Value tags.
    const auto literal = [&](const char* id) {
        const Variable* v = target->lookupVariableById(id);
        check(v != nullptr, "missing literal result");
        return canonical(v->value);
    };
    check(literal("v_done") == "s:1", "root script unfinished");
    check(literal("v_times") == "s:17",
          "dynamic repeat TIMES unexpectedly reevaluated");

    std::map<std::string, std::string> results;
    if (rt.blockCountEnabled)
        results["__block_count"] =
            "n:" + std::to_string(
                rt.executedBlocks.load(std::memory_order_relaxed));
    for (const auto& [id, variable] : target->variables)
        if (variable.type != Variable::kListType)
            results[id] = canonical(variable.value);
    std::cout << "TEST mode=" << mode << " steps=" << iterations
              << " edited=" << int(changed)
              << " native-yield=" << int(sawNativeYield)
              << " checkpoint=" << deoptCheckpoint << '\n';
    for (const auto& [id, value] : results)
        std::cout << "STATE " << id << "=" << value << '\n';
    return 0;
}
