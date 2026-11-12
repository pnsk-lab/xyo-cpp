// OSR-015: source topology disconnection AFTER a real suspended phase4
// LLVM Fiber yield must fail CLOSED, not replay native or Interpreter
// effects. One of the three frozen lexical ancestor-to-child edges is
// disconnected by a real editor-style Blocks::resetCache() source edit.
// No product-code test hooks and no other task's files modified.
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

bool disconnectActiveTopology(Thread& thread,
                              const CompiledScript& ready,
                              int edge) {
    Blocks& blocks = *thread.blockContainer;
    const Block* root = blocks.getBlock(thread.topBlock);
    check(root && ready.nestedOsrEntry, "missing source root/LLVM");
    const auto beforeUnit =
        ir::ScratchCompilationUnit::capture(blocks, root);
    check(beforeUnit.valid(), "invalid pre-edit source snapshot");
    const auto before = ir::scratchSourceDigest(beforeUnit);
    const uint64_t revision = blocks.revision();

    const std::string* childId =
        edge == 0 ? &ready.nestedOsrInnerLoopId :
        edge == 1 ? &ready.nestedOsrDeepInnerLoopId :
                    &ready.nestedOsrFourthLoopId;
    check(!childId->empty(), "missing proven nested source child");
    Block* child = blocks.getBlock(*childId);
    check(child && child->opcode == "control_repeat",
          "missing connected nested control");
    // The project's immediate lexical parent is an ordinary times=17
    // setter. Clearing its NEXT removes the child repeat from that
    // body's live source chain without destroying the remaining graph
    // or mutating any completed variable effect.
    Block* prefix = blocks.getBlock(child->parent);
    check(prefix && prefix->opcode == "data_setvariableto" &&
          prefix->next == *childId,
          "fixture lacks immediate pre-repeat source setter");
    prefix->next.clear();
    blocks.resetCache();
    const auto afterUnit =
        ir::ScratchCompilationUnit::capture(blocks, root);
    check(afterUnit.valid(), "edited source capture became invalid");
    check(blocks.revision() != revision, "editor revision unchanged");
    check(ir::scratchSourceDigest(afterUnit) != before,
          "disconnected source edge did not change digest");
    std::cerr << "[osr-topology] severed edge=" << edge
              << " prefix=" << prefix->id << " child=" << *childId
              << " revision=" << revision << "->"
              << blocks.revision() << '\n';
    return true;
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
    const char* expectedText = std::getenv("XYO_OSR_EXPECTED_SUM");
    const double expectedSum = expectedText ?
        std::strtod(expectedText, nullptr) : 48000000.0;
    check(std::isfinite(expectedSum) && expectedSum > 0,
          "invalid expected native/interpreter sum");
    const bool baseline = mode == "baseline";
    const bool rejected = mode == "rejected";
    const bool disabled = mode == "disabled";
    const bool quadDisabled = mode == "quad-disabled";
    const bool topologyEdit =
        mode == "disconnect-outer" ||
        mode == "disconnect-middle" ||
        mode == "disconnect-deep" ||
        mode == "disconnect-outer-late" ||
        mode == "disconnect-middle-late" ||
        mode == "disconnect-deep-late";
    const bool late = mode == "unguarded-late" ||
                      mode == "guarded-late" ||
                      (topologyEdit && mode.ends_with("-late"));
    const bool edited = mode == "unguarded" || mode == "guarded" ||
                        late || topologyEdit;
    const bool proof = !baseline;
    const bool inFlight = rejected || disabled || quadDisabled ||
        mode == "positive" || mode == "guarded" ||
        mode == "guarded-late" || topologyEdit;
    const uint32_t nativeYieldsBeforeEdit = late ? 4 : 1;
    check(baseline || rejected || disabled || quadDisabled || edited ||
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
    env("XYO_JIT_OSR_NESTED_INNER_BOUNDED", "0");
    env("XYO_JIT_OSR_NESTED_INNER_YIELD_DEOPT",
        inFlight && !disabled ? "1" : "0");
    env("XYO_JIT_OSR_NESTED_INNER_DYNAMIC",
        !baseline && !disabled ? "1" : "0");
    env("XYO_JIT_OSR_NESTED_TRIPLE_YIELD",
        !baseline && !disabled ? "1" : "0");
    env("XYO_JIT_OSR_NESTED_QUAD_YIELD",
        !baseline && !disabled && !quadDisabled ? "1" : "0");
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
    rt.warpTimeMs = baseline ? 500.0 : 0.0;
    rt.greenFlag();

    bool changed = false;
    bool published = false;
    bool sawNativeYield = false;
    bool sawNativeInnerYield = false;
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
                if (inFlight && t->jitCtx->nestedOsrDeoptPhase == 4)
                    sawNativeInnerYield = true;
                if (inFlight) {
                    // Only a REAL inner-body-completed Fiber suspension
                    // is the verified source edit injection site.
                    if (t->jitCtx->nestedOsrDeoptPhase != 4)
                        continue;
                    check(t->jitCtx->nestedOsrDeoptReady == 1,
                          "native inner yield missing safe checkpoint");
                    check(t->jitCtx->nestedOsrDeoptInnerRemaining >= 0.0,
                          "negative nested source loop checkpoint");
                    check(t->jitCtx->nestedOsrDeoptDeepRemaining >= 0.0,
                          "negative deepest source loop checkpoint");
                    check(t->jitCtx->nestedOsrDeoptFourthRemaining >= 0.0,
                          "negative fourth source loop checkpoint");
                    deoptCheckpoint =
                        t->jitCtx->nestedOsrDeoptFourthRemaining;
                }
                if (edited) {
                    const Variable* sum =
                        target->lookupVariableById("v_sum");
                    check(sum && sum->value.isNumber(),
                          "cannot snapshot sum at native yield");
                    // A reporter that determines inner TIMES was
                    // overwritten by a completed INNER body, before the
                    // active native Fiber suspension. The current loop
                    // must continue with its ORIGINAL private counter.
                    const Variable* times =
                        target->lookupVariableById("v_times");
                    check(times && canonical(times->value) == "s:7",
                          "TIMES variable did not change inside inner body");
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
                    if (topologyEdit) {
                        const int edge = mode.find("disconnect-outer") == 0
                            ? 0 : mode.find("disconnect-middle") == 0
                                ? 1 : 2;
                        changed = disconnectActiveTopology(
                            *t, *candidate, edge);
                    } else {
                        changed = replaceActiveBody(*t, *candidate);
                    }
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
    if (!baseline && !rejected && !disabled && !quadDisabled) {
        check(published && sawNativeYield,
              "native LLVM never yielded at an observable safe point");
        if (inFlight)
            check(sawNativeInnerYield,
                  "LLVM never yielded inside the native inner repeat");
    }
    if (rejected || disabled || quadDisabled) check(!sawNativeYield,
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
    if (topologyEdit) {
        check(sumAtEdit > 0 && sumAtEdit < expectedSum,
              "topology edit not in active native counted loop");
        check(number("v_sum") == sumAtEdit,
              "stale LLVM/native or Interpreter effects after disconnect");
        const Variable* branch =
            target->lookupVariableById("v_branchCount");
        check(branch && canonical(branch->value) == "s:0",
              "unexpected redirected variable write after fail-closed");
        const Variable* done = target->lookupVariableById("v_done");
        check(done && canonical(done->value) != "s:1",
              "root incorrectly ran to completion after unsafe edit");
        std::cerr << "[osr-topology] fail-closed sum="
                  << static_cast<uint64_t>(sumAtEdit)
                  << " no post-edit effects; suffix not executed\n";
    } else if (inFlight && edited) {
        check(sumAtEdit > 0 && sumAtEdit < expectedSum,
              "edit did not occur after partial native execution");
        check(number("v_sum") == sumAtEdit,
              "post-deopt interpreter used stale native variable field");
        check(number("v_branchCount") == expectedSum - sumAtEdit,
              "remaining loop work did not use edited branchCount");
    } else if (edited) {
        check(number("v_sum") > sumAtEdit,
              "without guard, native loop unexpectedly respected edit");
    } else {
        check(number("v_sum") == expectedSum, "incorrect original sum");
        const Variable* branch = target->lookupVariableById("v_branchCount");
        check(branch && canonical(branch->value) == "s:0",
              "unexpected branchCount");
    }
    if (edited && !topologyEdit)
        check(number("v_sum") + number("v_branchCount") == expectedSum,
              "missing/duplicated increments across native/interpreter split");
    if (!topologyEdit) {
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
    const char* expectedTimes = std::getenv("XYO_OSR_EXPECTED_TIMES");
    check(literal("v_times") == (expectedTimes
              ? std::string(expectedTimes) : std::string("s:17")),
          "dynamic repeat TIMES final value mismatch");
    } // topology mutation must retire the script, not finish it

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
    std::cout << "TEST mode=" << mode
              << " fail-closed=" << int(topologyEdit)
              << " steps=" << iterations
              << " nested-yield=" << int(sawNativeInnerYield)
              << " edited=" << int(changed)
              << " native-yield=" << int(sawNativeYield)
              << " checkpoint=" << deoptCheckpoint << '\n';
    for (const auto& [id, value] : results)
        std::cout << "STATE " << id << "=" << value << '\n';
    return 0;
}
