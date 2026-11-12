// OSR-010: test a bounded nested counted repeat (2 inner iterations)
// while OSR-008 emits a scheduler yield only at the outer body boundary.
// Edit the nested changevariableby target after native Fiber suspension.
// Edit a *currently executing* Scratch counted-repeat body AFTER a real
// native Fiber yield, before any more compiled instructions run. The old
// body writes sum; the edited source writes branchCount. resetCache()
// rebuilds Interpreter BlockCached fields just as the real editor does.
// Without the in-flight guard, stale LLVM keeps writing sum. With the guard,
// the original Interpreter frame must execute the new source from its exact
// saved loop counter, without replaying any committed effects.
// Tests perform the edit after either the first or fourth native yield.
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

Block* runningSumIncrement(Blocks& blocks) {
    // Read-only scan: Blocks::allBlocks() is const by design. Re-resolve
    // via the legacy mutable getBlock() (plain find, no revision or
    // cache effects), matching the direct non-resetCache edit below.
    for (const auto& [id, node] : blocks.allBlocks()) {
        if (node.opcode != "data_changevariableby") continue;
        const auto field = node.fields.find("VARIABLE");
        if (field != node.fields.end() && field->second.id == "v_sum")
            return blocks.getBlock(id);
    }
    return nullptr;
}

bool replaceActiveBody(Thread& thread, const CompiledScript& ready) {
    Blocks& blocks = *thread.blockContainer;
    const Block* root = blocks.getBlock(thread.topBlock);
    check(root && ready.nestedOsrEntry, "missing source root/LLVM");
    Block* running = runningSumIncrement(blocks);
    check(running, "missing running sum increment in procedure body");
    auto field = running->fields.find("VARIABLE");
    check(field != running->fields.end(), "missing VARIABLE field");
    const auto initial = ir::ScratchCompilationUnit::capture(blocks, root);
    check(initial.valid(), "invalid pre-edit Scratch source");
    const auto before = ir::scratchSourceDigest(initial);
    const uint64_t revision = blocks.revision();
    // Real source edit: the interpreter must create fresh BlockCached
    // ArgValues; a silent field edit without resetCache would leave the
    // interpreter itself executing the old target variable.
    field->second.id = "v_branchCount";
    blocks.resetCache();
    const auto updated = ir::ScratchCompilationUnit::capture(blocks, root);
    check(updated.valid(), "invalid updated Scratch source");
    check(blocks.revision() != revision,
          "real editor edit must advance source revision");
    check(ir::scratchSourceDigest(updated) != before,
          "active-body edit did not change source digest");
    std::cerr << "[osr-body-edit] changed id=" << running->id
              << " v_sum->v_branchCount revision=" << revision
              << "->" << blocks.revision() << '\n';
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: body-edit <project.sb3> "
                     "<baseline|positive|unguarded|guarded|unguarded-late|guarded-late>\n";
        return 2;
    }
    const std::string mode = argv[2];
    const bool baseline = mode == "baseline";
    const bool rejected = mode == "rejected";
    const bool disabled = mode == "disabled";
    const bool late = mode == "unguarded-late" ||
                      mode == "guarded-late";
    const bool edited = mode == "unguarded" || mode == "guarded" ||
                        late;
    const bool proof = !baseline;
    const bool inFlight = rejected || disabled ||
        mode == "positive" || mode == "guarded" ||
        mode == "guarded-late";
    const uint32_t nativeYieldsBeforeEdit = late ? 4 : 1;
    check(baseline || rejected || disabled || edited ||
          mode == "positive",
          "unknown test mode");
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
    env("XYO_JIT_OSR_NESTED_INNER_BOUNDED",
        inFlight && !disabled ? "1" : "0");
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
    // Use normal test warp timing. On very fast native executions the
    // fourth observed yield may occur in a later parent invocation.
    // The verifier accounts for each completed native caller suffix.
    rt.warpTimeMs = baseline ? 500.0 : 3.0;
    rt.greenFlag();

    bool changed = false;
    bool published = false;
    bool sawNativeYield = false;
    double deoptCheckpoint = -1.0;
    double sumAtEdit = -1.0;
    uint64_t countAtEdit = 0;
    uint64_t observedNativeYields = 0;
    double lastObservedSum = -1;
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
                    const Variable* sum =
                        target->lookupVariableById("v_sum");
                    check(sum && sum->value.isNumber(),
                          "cannot snapshot sum at native yield");
                    const double currentSum =
                        sum->value.asNumberUnchecked();
                    if (currentSum <= lastObservedSum) continue;
                    lastObservedSum = currentSum;
                    ++observedNativeYields;
                    if (observedNativeYields < nativeYieldsBeforeEdit)
                        continue;
                    sumAtEdit = currentSum;
                    countAtEdit =
                        rt.executedBlocks.load(std::memory_order_relaxed);
                    changed = replaceActiveBody(*t, *candidate);
                    rt.warpTimeMs = 500.0;
                    std::cerr
                        << "[osr-body-edit] suspended remaining="
                        << deoptCheckpoint << " guard=" << int(inFlight)
                        << " committed-sum="
                        << static_cast<uint64_t>(sumAtEdit)
                        << " blocks=" << countAtEdit
                        << " yields=" << observedNativeYields << '\n';
                    break;
                }
            }
        }
        rt.step();
    }
    check(iterations < maxSteps, "runtime exceeded step budget");
    if (!baseline && !rejected && !disabled) {
        check(published && sawNativeYield,
              "native LLVM never yielded at an observable safe point");
    }
    if (rejected || disabled) check(!sawNativeYield,
                       "unapproved nested loop was promoted");
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
    if (inFlight && edited) {
        check(sumAtEdit > 0 && sumAtEdit < 48000000.0,
              "edit did not occur after partial native execution");
        check(number("v_sum") == sumAtEdit,
              "post-deopt interpreter used stale native variable field");
        check(number("v_branchCount") == 48000000.0 - sumAtEdit,
              "remaining loop work did not use edited branchCount");
    } else if (edited) {
        check(number("v_sum") > sumAtEdit,
              "without guard, native loop unexpectedly respected edit");
    } else {
        check(number("v_sum") == 48000000.0, "incorrect original sum");
        const Variable* branch = target->lookupVariableById("v_branchCount");
        check(branch && canonical(branch->value) == "s:0",
              "unexpected branchCount");
    }
    if (edited)
        check(number("v_sum") + number("v_branchCount") == 48000000.0,
              "missing/duplicated increments across native/interpreter split");
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
    std::cout << "SPLIT sum-at-edit=" << sumAtEdit
              << " count-at-edit=" << countAtEdit << '\n';
    std::cout << "TEST mode=" << mode << " steps=" << iterations
              << " edited=" << int(changed)
              << " native-yield=" << int(sawNativeYield)
              << " checkpoint=" << deoptCheckpoint << '\n';
    for (const auto& [id, value] : results)
        std::cout << "STATE " << id << "=" << value << '\n';
    return 0;
}
