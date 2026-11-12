// danger モード（スレッドごとの実行時間予算を外す）の意味論テスト。
// CPU ループの yield checkpoint を対象とするため、この suite 内だけ GPU
// offload を無効にする。GPU 適格ループは CPU checkpoint を通らず全反復を
// 完了し得るため、GPU の有無でこの CPU 予算テストの結果を変えない。
//
// - 通常モード: warp 手続きのループは予算（既定 500ms）を使い切ると
//   yield して他スレッドへ譲る — 数パスでは終わらない。
// - danger モード: 予算が無制限（+inf）なので同じループが 1 パスで
//   最後まで走る。
// - 非 warp のループは danger モードでも毎反復 yield する（予算ではなく
//   スクリプト種別の意味論なので外れない）。
//
// 実時間に依存しないよう、予算は 0ms に設定して「常に使い切り」にする。
// danger 側は +inf なので同じ設定でも最後まで走る。
// 観測は `stepThread`（= yield するまでの 1 パス）で行い、WORK_TIME で
// 1 ティック内に何度も再開する `step()` のループは使わない。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <limits>
#include <string>
#include <thread>

#include "core/cast.hpp"
#include "core/json.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "engine/warp_clock.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

extern "C" int32_t xyo_wait_should_yield(xyo::JitCtx*);
extern "C" int32_t xyo_warp_yield_due(xyo::JitCtx*);

namespace {
using namespace xyo;

int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL danger: %s\n", name);
    }
}

// Restore the caller's GPU setting instead of changing later GPU suites.
struct GpuEnvGuard {
    std::string previous;
    bool existed;
    GpuEnvGuard() : existed(std::getenv("XYO_GPU") != nullptr) {
        if (existed) previous = std::getenv("XYO_GPU");
        put("0");
    }
    static void put(const char* value) {
#ifdef _WIN32
        _putenv_s("XYO_GPU", value ? value : "");
#else
        if (value) setenv("XYO_GPU", value, 1);
        else unsetenv("XYO_GPU");
#endif
    }
    ~GpuEnvGuard() { put(existed ? previous.c_str() : nullptr); }
};

constexpr int kIterations = 20000;

/// hat → warp 手続き burn（引数なし）→ repeat N { change v by 1 }
std::string projectJson(bool warp) {
    const std::string mut =
        std::string("{\"tagName\":\"mutation\",\"children\":[],"
                    "\"proccode\":\"burn\",\"argumentids\":\"[]\","
                    "\"argumentnames\":\"[]\",\"argumentdefaults\":\"[]\","
                    "\"warp\":\"") +
        (warp ? "true" : "false") + "\"}";
    const std::string blocks =
        "{\"hat\":{\"opcode\":\"event_whenflagclicked\",\"next\":\"call\","
        "\"parent\":null,\"inputs\":{},\"fields\":{},\"topLevel\":true,"
        "\"shadow\":false,\"x\":0,\"y\":0},"
        "\"call\":{\"opcode\":\"procedures_call\",\"next\":null,"
        "\"parent\":\"hat\",\"inputs\":{},\"fields\":{},\"mutation\":" +
        mut +
        ",\"topLevel\":false,\"shadow\":false},"
        "\"def\":{\"opcode\":\"procedures_definition\",\"next\":\"loop\","
        "\"parent\":null,\"inputs\":{\"custom_block\":[1,\"proto\"]},"
        "\"fields\":{},\"topLevel\":true,\"shadow\":false,\"x\":0,\"y\":400},"
        "\"proto\":{\"opcode\":\"procedures_prototype\",\"next\":null,"
        "\"parent\":\"def\",\"inputs\":{},\"fields\":{},\"topLevel\":false,"
        "\"shadow\":true,\"mutation\":" +
        mut +
        "},"
        "\"loop\":{\"opcode\":\"control_repeat\",\"next\":null,"
        "\"parent\":\"def\",\"inputs\":{\"TIMES\":[1,[6,\"" +
        std::to_string(kIterations) +
        "\"]],\"SUBSTACK\":[2,\"inc\"]},\"fields\":{},\"topLevel\":false,"
        "\"shadow\":false},"
        "\"inc\":{\"opcode\":\"data_changevariableby\",\"next\":null,"
        "\"parent\":\"loop\",\"inputs\":{\"VALUE\":[1,[4,\"1\"]]},"
        "\"fields\":{\"VARIABLE\":[\"v\",\"v\"]},\"topLevel\":false,"
        "\"shadow\":false}}";
    return "{\"targets\":["
           "{\"isStage\":true,\"name\":\"Stage\",\"variables\":{},"
           "\"lists\":{},\"broadcasts\":{},\"blocks\":{},\"comments\":{},"
           "\"costumes\":[],\"sounds\":[],\"currentCostume\":0,"
           "\"volume\":100,\"layerOrder\":0},"
           "{\"isStage\":false,\"name\":\"S\",\"variables\":{\"v\":[\"v\",0]},"
           "\"lists\":{},\"broadcasts\":{},\"blocks\":" +
           blocks +
           ",\"comments\":{},\"costumes\":[],\"sounds\":[],"
           "\"currentCostume\":0,\"volume\":100,\"visible\":true,"
           "\"x\":0,\"y\":0,\"size\":100,\"direction\":90,"
           "\"draggable\":false,\"rotationStyle\":\"all around\","
           "\"layerOrder\":1}],"
           "\"monitors\":[],\"extensions\":[],\"meta\":{\"semver\":\"3.0.0\"}}";
}

double counter(Runtime& rt) {
    Target* sp = rt.getSpriteTargetByName(u"S");
    if (!sp) return -1;
    Variable* v = sp->lookupVariableById("v");
    return v ? cast::toNumber(v->value) : -1;
}

/// スレッドを数パスだけ進めたときのカウンタ値。
/// `stepThread` は 1 パス（= yield するまで）なので、`step()` の
/// WORK_TIME ループ（1 ティック内で何度も再開する）を介さず
/// **スレッドごとの予算**そのものを観測できる。
/// warpTimeMs = 0 → 予算は必ず使い切られる（時間に依存しない）。
/// LAYQ の quantum は負荷で変わる（既定で最大 16384）ため、プリエンプションの
/// 判定は「最初の 1 パスで止まるか」で行う — 4 パスでは 20000 反復が終わり切る
/// quantum があり得る（4*16384 > 20000）。反復回数の上限ではなく、予算切れで
/// 譲るという不変条件だけを主張する。
double onePass(bool danger, bool warp, JitMode mode, int passes = 4) {
    setJitMode(mode);
    Runtime rt;
    const Sb3LoadResult lr = loadSb3(rt, projectJson(warp), nullptr);
    check(lr.ok, "load project");
    if (!lr.ok) return -1;
    rt.warpTimeMs = 0.0;
    rt.dangerMode = danger;
    rt.greenFlag();
    rt.sequencer->timer.start();
    for (int pass = 0; pass < passes; ++pass) {
        for (Thread* t : rt.threads) {
            if (t->status != STATUS_DONE && !t->stack.empty())
                rt.sequencer->stepThread(*t);
        }
    }
    return counter(rt);
}

// A running warp loop keeps typed values in registers on timer polls, but
// must publish them before a real yield and reload another script's writes.
void testWarpYieldReload(JitMode mode) {
    setJitMode(mode);
    Runtime rt;
    if (!loadSb3(rt, projectJson(true), nullptr).ok) {
        check(false, "load warp interleaving project");
        return;
    }
    rt.warpTimeMs = 0.0;  // guaranteed checkpoint on the first poll
    rt.dangerMode = false;
    rt.greenFlag();
    check(rt.threads.size() == 1, "one warp script before interleaving");
    if (rt.threads.empty()) return;
    rt.sequencer->timer.start();
    rt.sequencer->stepThread(*rt.threads.front());

    Target* target = rt.getSpriteTargetByName(u"S");
    Variable* v = target ? target->lookupVariableById("v") : nullptr;
    if (!v) {
        check(false, "resolve warp shared variable");
        return;
    }
    const double iterationsBeforeYield = cast::toNumber(v->value);
    check(iterationsBeforeYield > 0 && iterationsBeforeYield < kIterations,
          "warp check yielded with promoted variable spilled");
    if (!(iterationsBeforeYield > 0 && iterationsBeforeYield < kIterations))
        return;

    // Simulate another script modifying the value while this warp fiber is
    // suspended. Scratch change-variable must coerce the new string "040".
    v->value = Value::string(SStr::fromUtf8("040"));
    rt.runUntilDone(10000);
    check(rt.threads.empty(), "interleaved warp loop terminates");
    check(cast::toNumber(v->value) ==
              40.0 + kIterations - iterationsBeforeYield,
          "warp resumes from externally changed string, not stale SSA");
}


// Two genuine green-flag scripts share variable "v". The warp script uses
// the transient append pattern, yields after its first checkpoint (budget=0),
// and the second script then overwrites the same variable. The suspended warp
// script MUST resume from the new value, not its cached append-only prefix.
std::string transientInterleaveProjectJson() {
    JsonValue root = jsonParse(projectJson(true));
    auto& sprite = root.objectRef().at("targets").arrayRef().at(1).objectRef();
    auto& blocks = sprite.at("blocks").objectRef();
    auto& set = blocks.at("inc").objectRef();
    set["opcode"] = JsonValue(std::string("data_setvariableto"));
    set["inputs"] = jsonParse(R"({"VALUE":[2,"join"]})");
    blocks["join"] = jsonParse(R"({"opcode":"operator_join",
        "next":null,"parent":"inc",
        "inputs":{"STRING1":[2,"read"],"STRING2":[1,[10,"ab"]]},
        "fields":{},"shadow":false})");
    blocks["read"] = jsonParse(R"({"opcode":"data_variable",
        "next":null,"parent":"join","inputs":{},
        "fields":{"VARIABLE":["v","v"]},"shadow":false})");
    blocks["hat_write"] = jsonParse(R"({"opcode":"event_whenflagclicked",
        "topLevel":true,"x":250,"y":0,"next":"overwrite",
        "inputs":{},"fields":{},"shadow":false})");
    blocks["overwrite"] = jsonParse(R"({"opcode":"data_setvariableto",
        "next":null,"parent":"hat_write",
        "inputs":{"VALUE":[1,[10,"external"]]},
        "fields":{"VARIABLE":["v","v"]},"shadow":false})");
    sprite.at("variables").objectRef()["v"] =
        jsonParse(R"(["v","seed"])");
    return jsonStringify(root);
}

void testTransientWarpInterleaving(JitMode mode) {
    setJitMode(mode);
    Runtime rt;
    const auto project = transientInterleaveProjectJson();
    if (!loadSb3(rt, project, nullptr).ok) {
        check(false, "transient interleaving project loads");
        return;
    }
    rt.warpTimeMs = 0.0;
    rt.dangerMode = false;
    rt.greenFlag();

    Thread* appender = nullptr;
    Thread* overwriter = nullptr;
    for (Thread* thread : rt.threads) {
        if (thread->topBlock == "hat") appender = thread;
        if (thread->topBlock == "hat_write") overwriter = thread;
    }
    check(appender && overwriter, "two real concurrent Scratch scripts");
    if (!appender || !overwriter) return;

    Target* target = rt.getSpriteTargetByName(u"S");
    Variable* var = target ? target->lookupVariableById("v") : nullptr;
    if (!var) {
        check(false, "resolve transient shared variable");
        return;
    }
    rt.sequencer->timer.start();
    rt.sequencer->stepThread(*appender);
    const std::u16string initialPrefix(cast::toString(var->value)->view());
    const bool yielded = initialPrefix.starts_with(u"seedab") &&
                         initialPrefix.size() < size_t(4 + 2 * kIterations) &&
                         (initialPrefix.size() - 4) % 2 == 0;
    check(yielded && appender->status != STATUS_DONE,
          "warp published its partial prefix before a real yield");
    if (mode == JitMode::Eager &&
        (!std::getenv("XYO_JIT_TRANSIENT_STRINGS") ||
         std::getenv("XYO_JIT_TRANSIENT_STRINGS")[0] != '0')) {
        check(appender->isCompiled && var->value.isString() &&
              var->value.asStrPtr() && var->value.asStrPtr()->isAppend(),
              "JIT really selected and published transient append snapshot");
    }
    if (!yielded) return;
    const size_t completed = (initialPrefix.size() - 4) / 2;

    rt.sequencer->stepThread(*overwriter);
    check(cast::toString(var->value)->view() == u"external",
          "independent Scratch script overwrote shared variable");

    // A GC pass while the first fiber is suspended must not resurrect or
    // dereference an old log pointer after the overwrite.
    Gc::get().collect();
    rt.runUntilDone(2000);
    check(rt.threads.empty(), "suspended transient fiber terminates");

    const auto finalText = cast::toString(var->value)->view();
    std::u16string expected = u"external";
    for (size_t i = completed; i < size_t(kIterations); ++i)
        expected += u"ab";
    check(finalText == expected,
          "transient resumed from second script write with exact suffix");
}

// A RHS reporter may be invariant *between* warp scheduling checkpoints,
// but must be re-read after the first fiber yields. This proves batching
// does not turn an external script's changes into a permanently cached value.
std::string transientRhsProjectJson(bool numeric) {
    JsonValue root = jsonParse(transientInterleaveProjectJson());
    auto& sprite = root.objectRef().at("targets").arrayRef().at(1).objectRef();
    auto& vars = sprite.at("variables").objectRef();
    vars["rhs"] = numeric
        ? jsonParse(R"(["rhs",1])")
        : jsonParse(R"(["rhs","A"])");
    auto& blocks = sprite.at("blocks").objectRef();
    blocks["rhsRead"] = jsonParse(R"({"opcode":"data_variable",
        "next":null,"parent":"join","inputs":{},
        "fields":{"VARIABLE":["rhs","rhs"]},"shadow":false})");
    blocks.at("join").objectRef().at("inputs").objectRef()["STRING2"] =
        jsonParse(R"([2,"rhsRead"])");
    blocks.at("overwrite").objectRef().at("fields").objectRef()["VARIABLE"] =
        jsonParse(R"(["rhs","rhs"])");
    blocks.at("overwrite").objectRef().at("inputs").objectRef()["VALUE"] =
        numeric ? jsonParse(R"([1,[4,"7"]])")
                : jsonParse(R"([1,[10,"B"]])");
    return jsonStringify(root);
}

void testTransientRhsAfterYield(JitMode mode, bool numeric) {
    setJitMode(mode);
    Runtime rt;
    const auto project = transientRhsProjectJson(numeric);
    if (!loadSb3(rt, project, nullptr).ok) {
        check(false, "transient RHS-change project loads");
        return;
    }
    rt.warpTimeMs = 0.0;
    rt.dangerMode = false;
    rt.greenFlag();
    Thread* appender = nullptr;
    Thread* overwriter = nullptr;
    for (Thread* thread : rt.threads) {
        if (thread->topBlock == "hat") appender = thread;
        if (thread->topBlock == "hat_write") overwriter = thread;
    }
    check(appender && overwriter, "RHS test has two Scratch scripts");
    if (!appender || !overwriter) return;

    Target* target = rt.getSpriteTargetByName(u"S");
    Variable* s = target ? target->lookupVariableById("v") : nullptr;
    Variable* rhs = target ? target->lookupVariableById("rhs") : nullptr;
    if (!s || !rhs) {
        check(false, "resolve RHS and destination variables");
        return;
    }

    rt.sequencer->timer.start();
    rt.sequencer->stepThread(*appender);
    const std::u16string prefix(cast::toString(s->value)->view());
    const std::u16string expectedFirst = numeric ? u"seed1" : u"seedA";
    const bool yielded = prefix.starts_with(expectedFirst) &&
                         prefix.size() < size_t(4 + kIterations);
    check(yielded && appender->status != STATUS_DONE,
          "dynamic RHS batch yielded before completion");
    if (!yielded) return;
    const size_t completed = prefix.size() - 4;

    rt.sequencer->stepThread(*overwriter);
    const std::u16string newRhs = numeric ? u"7" : u"B";
    check(cast::toString(rhs->value)->view() == newRhs,
          "independent script changed RHS variable");
    Gc::get().collect();
    rt.runUntilDone(2500);
    check(rt.threads.empty(), "dynamic RHS batching completes");
    const std::u16string value(cast::toString(s->value)->view());
    std::u16string expected = u"seed";
    expected.append(completed, numeric ? u'1' : u'A');
    expected.append(kIterations - completed, numeric ? u'7' : u'B');
    check(value == expected,
          "dynamic RHS re-evaluated after real yield with identical output");
}

// A pair of independent transient builders must publish both snapshots on
// a real yield, then independently rebase only the overwritten variable.
std::string transientMultiYieldProjectJson() {
    JsonValue root = jsonParse(transientInterleaveProjectJson());
    auto& sprite = root.objectRef().at("targets").arrayRef().at(1).objectRef();
    sprite.at("variables").objectRef()["aux"] =
        jsonParse(R"(["aux","auxstart"])");
    auto& blocks = sprite.at("blocks").objectRef();
    blocks.at("inc").objectRef()["next"] = JsonValue(std::string("aux_append"));
    blocks["aux_append"] = jsonParse(R"({"opcode":"data_setvariableto",
        "next":null,"parent":"loop",
        "inputs":{"VALUE":[2,"aux_join"]},
        "fields":{"VARIABLE":["aux","aux"]},"shadow":false})");
    blocks["aux_join"] = jsonParse(R"({"opcode":"operator_join",
        "next":null,"parent":"aux_append",
        "inputs":{"STRING1":[2,"aux_read"],"STRING2":[1,[10,"cd"]]},
        "fields":{},"shadow":false})");
    blocks["aux_read"] = jsonParse(R"({"opcode":"data_variable",
        "next":null,"parent":"aux_join",
        "inputs":{},"fields":{"VARIABLE":["aux","aux"]},
        "shadow":false})");
    blocks.at("overwrite").objectRef().at("inputs").objectRef()["VALUE"] =
        jsonParse(R"([1,[10,"other"]])");
    blocks.at("overwrite").objectRef().at("fields").objectRef()["VARIABLE"] =
        jsonParse(R"(["aux","aux"])");
    return jsonStringify(root);
}
void testTransientMultiYield(JitMode mode) {
    setJitMode(mode);
    Runtime rt;
    const auto source = transientMultiYieldProjectJson();
    if (!loadSb3(rt, source, nullptr).ok) {
        check(false, "load multi-variable yield project");
        return;
    }
    rt.warpTimeMs = 0.0;
    rt.dangerMode = false;
    rt.greenFlag();
    Thread* producer = nullptr;
    Thread* writer = nullptr;
    for (Thread* t : rt.threads) {
        if (t->topBlock == "hat") producer = t;
        if (t->topBlock == "hat_write") writer = t;
    }
    if (!producer || !writer) {
        check(false, "two multi-variable scripts");
        return;
    }
    Target* target = rt.getSpriteTargetByName(u"S");
    Variable* v = target ? target->lookupVariableById("v") : nullptr;
    Variable* aux = target ? target->lookupVariableById("aux") : nullptr;
    if (!v || !aux) {
        check(false, "multi-variable yield slots");
        return;
    }
    rt.sequencer->timer.start();
    rt.sequencer->stepThread(*producer);
    const std::u16string before(cast::toString(v->value)->view());
    const std::u16string beforeAux(cast::toString(aux->value)->view());
    bool yielded = before.starts_with(u"seedab") &&
                   beforeAux.starts_with(u"auxstartcd") &&
                   before.size() < size_t(4 + 2*kIterations);
    check(yielded && producer->status != STATUS_DONE,
          "both multi-variable snapshots published before yield");
    if (!yielded) return;
    size_t iterations = (before.size() - 4) / 2;
    check(beforeAux.size() == 8 + 2*iterations,
          "multi-variable snapshots from identical loop count");

    rt.sequencer->stepThread(*writer);
    check(cast::toString(aux->value)->view() == u"other",
          "second shared variable overwritten independently");
    Gc::get().collect();
    rt.runUntilDone(2000);
    check(rt.threads.empty(), "multi-variable resume completes");

    std::u16string expectedV = u"seed";
    std::u16string expectedAux = u"other";
    for (size_t i = 0; i < size_t(kIterations); ++i)
        expectedV += u"ab";
    for (size_t i = iterations; i < size_t(kIterations); ++i)
        expectedAux += u"cd";
    check(cast::toString(v->value)->view() == expectedV,
          "unmodified transient continues from its own snapshot");
    check(cast::toString(aux->value)->view() == expectedAux,
          "overwritten transient rebases independently");
}

// The optimization must preserve Scratch repeat's >=0.5 IEEE boundary
// even when the last batch is partial or a loop starts at a nonzero poll phase.
void testTransientRepeatEdges() {
    setJitMode(JitMode::Eager);
    struct Case { double count; size_t expected; };
    const Case cases[] = {
        {0, 0}, {0.49, 0}, {0.5, 1}, {1.5, 2},
        {63.49, 63}, {63.5, 64}, {64.49, 64}, {64.5, 65},
        {65.5, 66}, {127.49, 127}, {127.5, 128}, {128.5, 129}
    };
    for (const auto test : cases) {
        JsonValue root = jsonParse(transientInterleaveProjectJson());
        auto& blocks = root.objectRef().at("targets").arrayRef().at(1)
                           .objectRef().at("blocks").objectRef();
        blocks.erase("hat_write");
        blocks.erase("overwrite");
        blocks.at("loop").objectRef().at("inputs").objectRef()["TIMES"] =
            jsonParse("[1,[4,\"" + std::to_string(test.count) + "\"]]");
        Runtime rt;
        if (!loadSb3(rt, jsonStringify(root), nullptr).ok) {
            check(false, "transient repeat edge project loads");
            continue;
        }
        rt.warpTimeMs = 0.0;
        rt.greenFlag();
        rt.runUntilDone(1000);
        Target* target = rt.getSpriteTargetByName(u"S");
        Variable* v = target ? target->lookupVariableById("v") : nullptr;
        std::u16string expected = u"seed";
        for (size_t i = 0; i < test.expected; ++i)
            expected += u"ab";
        const bool ok = rt.threads.empty() && v && v->value.isString() &&
                        v->value.asStrPtr() &&
                        v->value.asStrPtr()->view() == expected;
        if (!ok) std::printf("transient repeat failed at %.6g\n", test.count);
        check(ok, "batched repeat preserves fractional iteration count");
    }
}

const char* modeName(JitMode m) {
    return m == JitMode::Off ? "interp" : "jit";
}

/// warp 手続き内で `broadcast and wait` → `wait until v > 0` → done=1。
/// 受信側は非 warp で `wait 0.05` してから v=1 を書く（他スレッドが
/// 進まないと待機が解けない構図）。
std::string waitProjectJson() {
    const std::string mut =
        "{\"tagName\":\"mutation\",\"children\":[],\"proccode\":\"go\","
        "\"argumentids\":\"[]\",\"argumentnames\":\"[]\","
        "\"argumentdefaults\":\"[]\",\"warp\":\"true\"}";
    const std::string blocks =
        "{\"hat\":{\"opcode\":\"event_whenflagclicked\",\"next\":\"call\","
        "\"parent\":null,\"inputs\":{},\"fields\":{},\"topLevel\":true,"
        "\"shadow\":false,\"x\":0,\"y\":0},"
        "\"call\":{\"opcode\":\"procedures_call\",\"next\":null,"
        "\"parent\":\"hat\",\"inputs\":{},\"fields\":{},\"topLevel\":false,"
        "\"shadow\":false,\"mutation\":" + mut + "},"
        "\"def\":{\"opcode\":\"procedures_definition\",\"next\":\"bw\","
        "\"parent\":null,\"inputs\":{\"custom_block\":[1,\"proto\"]},"
        "\"fields\":{},\"topLevel\":true,\"shadow\":false,\"x\":0,\"y\":400},"
        "\"proto\":{\"opcode\":\"procedures_prototype\",\"next\":null,"
        "\"parent\":\"def\",\"inputs\":{},\"fields\":{},\"topLevel\":false,"
        "\"shadow\":true,\"mutation\":" + mut + "},"
        "\"bw\":{\"opcode\":\"event_broadcastandwait\",\"next\":\"wu\","
        "\"parent\":\"def\",\"inputs\":{\"BROADCAST_INPUT\":[1,[11,\"msg\",\"msg-id\"]]},"
        "\"fields\":{},\"topLevel\":false,\"shadow\":false},"
        "\"wu\":{\"opcode\":\"control_wait_until\",\"next\":\"done\","
        "\"parent\":\"bw\",\"inputs\":{\"CONDITION\":[2,\"gt\"]},\"fields\":{},"
        "\"topLevel\":false,\"shadow\":false},"
        "\"gt\":{\"opcode\":\"operator_gt\",\"next\":null,\"parent\":\"wu\","
        "\"inputs\":{\"OPERAND1\":[3,\"varv\",[4,\"0\"]],\"OPERAND2\":[1,[4,\"0\"]]},"
        "\"fields\":{},\"topLevel\":false,\"shadow\":false},"
        "\"varv\":{\"opcode\":\"data_variable\",\"next\":null,\"parent\":\"gt\","
        "\"inputs\":{},\"fields\":{\"VARIABLE\":[\"v\",\"v\"]},\"topLevel\":false,"
        "\"shadow\":false},"
        "\"done\":{\"opcode\":\"data_setvariableto\",\"next\":null,"
        "\"parent\":\"wu\",\"inputs\":{\"VALUE\":[1,[4,\"1\"]]},"
        "\"fields\":{\"VARIABLE\":[\"done\",\"done\"]},\"topLevel\":false,"
        "\"shadow\":false},"
        "\"rhat\":{\"opcode\":\"event_whenbroadcastreceived\",\"next\":\"rwait\","
        "\"parent\":null,\"inputs\":{},\"fields\":{\"BROADCAST_OPTION\":[\"msg\",\"msg-id\"]},"
        "\"topLevel\":true,\"shadow\":false,\"x\":300,\"y\":0},"
        "\"rwait\":{\"opcode\":\"control_wait\",\"next\":\"rset\","
        "\"parent\":\"rhat\",\"inputs\":{\"DURATION\":[1,[4,\"0.05\"]]},\"fields\":{},"
        "\"topLevel\":false,\"shadow\":false},"
        "\"rset\":{\"opcode\":\"data_setvariableto\",\"next\":null,"
        "\"parent\":\"rwait\",\"inputs\":{\"VALUE\":[1,[4,\"1\"]]},"
        "\"fields\":{\"VARIABLE\":[\"v\",\"v\"]},\"topLevel\":false,\"shadow\":false}}";
    return "{\"targets\":["
           "{\"isStage\":true,\"name\":\"Stage\",\"variables\":{},\"lists\":{},"
           "\"broadcasts\":{\"msg-id\":\"msg\"},\"blocks\":{},\"comments\":{},"
           "\"costumes\":[],\"sounds\":[],\"currentCostume\":0,\"volume\":100,"
           "\"layerOrder\":0},"
           "{\"isStage\":false,\"name\":\"S\",\"variables\":{\"done\":[\"done\",0],"
           "\"v\":[\"v\",0]},\"lists\":{},\"broadcasts\":{},\"blocks\":" + blocks +
           ",\"comments\":{},\"costumes\":[],\"sounds\":[],\"currentCostume\":0,"
           "\"volume\":100,\"visible\":true,\"x\":0,\"y\":0,\"size\":100,"
           "\"direction\":90,\"draggable\":false,\"rotationStyle\":\"all around\","
           "\"layerOrder\":1}],"
           "\"monitors\":[],\"extensions\":[],\"meta\":{\"semver\":\"3.0.0\"}}";
}

/// fn を別スレッドで実行し、ms 以内に終わらなければ false（ハング検出）。
/// タイムアウト時のフラグと fn のコピーはヒープに置く（detach したスレッドが
/// 呼出し側のスタックを触らないようにする）。
bool runWithin(std::function<void()> fn, int ms) {
    auto* done = new std::atomic<bool>(false);
    std::thread th([fn, done]() mutable {
        fn();
        done->store(true);
    });
    for (int waited = 0; waited <= ms; waited += 10) {
        if (done->load()) {
            th.join();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    th.detach();
    return false;
}

/// danger でも warp 内の待機系が完了すること（ハングしないこと）。
void testWarpWaits(JitMode mode) {
    setJitMode(mode);
    std::shared_ptr<Runtime> rt(new Runtime);
    if (!loadSb3(*rt, waitProjectJson(), nullptr).ok) {
        check(false, "load wait project");
        return;
    }
    rt->dangerMode = true;
    rt->greenFlag();
    const bool finished = runWithin([rt]() { rt->runUntilDone(10000); }, 5000);
    if (!finished)
        std::printf("  %s: warp waits did not finish (deadlock?)\n",
                    modeName(mode));
    check(finished, "danger keeps warp waits live (no deadlock)");
    if (!finished) return;   // 結果は判定しない（ランタイムはリーク）
    Target* sp = rt->getSpriteTargetByName(u"S");
    Variable* done = sp ? sp->lookupVariableById("done") : nullptr;
    Variable* v = sp ? sp->lookupVariableById("v") : nullptr;
    check(done && cast::toNumber(done->value) == 1,
          "warp wait-until proceeded after the broadcast");
    check(v && cast::toNumber(v->value) == 1,
          "broadcast receiver ran while the warp sender waited");
}

} // namespace

/// テストランナ（tests/unit/test_cast.cpp の main）から呼ばれる。
void testSampledWarpClock() {
    WarpClockWatchdog clock(true);
    clock.arm(Timer::stdNow() + 1000.0, false);
    const int64_t first = clock.sampledUs.load(std::memory_order_relaxed);
    int64_t next = first;
    for (int attempt = 0; attempt < 100 && next <= first; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        next = clock.sampledUs.load(std::memory_order_relaxed);
    }
    check(next > first, "sampled warp clock advances asynchronously");
    const auto unixNow = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
    const int64_t sampledWall = clock.sampledUnixMs.load(std::memory_order_relaxed);
    check(std::llabs(sampledWall - unixNow()) < 200,
          "sampled wall clock agrees with system clock");

    check(next < clock.deadlineUs.load(std::memory_order_relaxed),
          "sampled warp clock not yet past deadline");

    clock.arm(Timer::stdNow() + 3.0, false);
    bool expired = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (clock.sampledUs.load(std::memory_order_relaxed) >
            clock.deadlineUs.load(std::memory_order_relaxed)) {
            expired = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(expired, "shared clock crosses expired warp deadline");
    // Deadline expiry must not freeze a Scratch-visible wall-clock reporter.
    const int64_t afterDeadline = clock.sampledUnixMs.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(clock.sampledUnixMs.load(std::memory_order_relaxed) > afterDeadline,
          "sampled wall clock advances after warp deadline");
    clock.arm(Timer::stdNow() + 1000.0, true);
    const int64_t inDanger = clock.sampledUnixMs.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(clock.sampledUnixMs.load(std::memory_order_relaxed) > inDanger,
          "sampled wall clock advances in danger mode");


    // A previous tick's deadline must not continue to preempt the next.
    clock.arm(Timer::stdNow() + 1000.0, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    check(clock.sampledUs.load(std::memory_order_relaxed) <
          clock.deadlineUs.load(std::memory_order_relaxed),
          "rearming invalidates previous warp deadline");

    clock.arm(Timer::stdNow() - 100.0, true);
    check(clock.deadlineUs.load(std::memory_order_relaxed) ==
              std::numeric_limits<int64_t>::max(),
          "danger mode never reaches the sampled warp deadline");
    clock.disarm();
    check(!clock.armed(), "watchdog disarmed after tick");
    // disarm synchronizes with the sampler; both atomics must represent
    // the exact same millisecond sample after the watchdog stops.
    const int64_t settledMs =
        clock.sampledUnixMs.load(std::memory_order_relaxed);
    const uint64_t settledDays =
        clock.sampledUnixDaysBits.load(std::memory_order_relaxed);
    const uint64_t expectedDays =
        Value::num((double(settledMs) - 946684800000.0) / 86400000.0).bits;
    check(settledDays == expectedDays,
          "preboxed Scratch days match last sampled wall milliseconds");
}

int test_danger_main() {
    GpuEnvGuard gpuDisabled;
    testSampledWarpClock();
    testTransientRepeatEdges();
    const JitMode modes[] = {JitMode::Off, JitMode::Eager};
    for (JitMode mode : modes) {
        testTransientWarpInterleaving(mode);
        testTransientMultiYield(mode);
        testTransientRhsAfterYield(mode, false);
        testTransientRhsAfterYield(mode, true);
        testWarpYieldReload(mode);
        // 予算あり: 最初のパスで終わらない（予算切れで yield して譲る）。
        // LAYQ quantum に依存しないよう 1 パスだけ進めて判定する。
        const double limited = onePass(false, true, mode, 1);
        if (!(limited > 0 && limited < kIterations))
            std::printf("  %s: limited counter after 4 passes = %g\n",
                        modeName(mode), limited);
        check(limited > 0 && limited < kIterations,
              "warp loop is preempted by the time budget");

        // danger: 同じパス数で最後まで走る（予算で譲らない）。
        const double danger = onePass(true, true, mode);
        if (danger != kIterations)
            std::printf("  %s: danger counter after 4 passes = %g\n",
                        modeName(mode), danger);
        check(danger == kIterations,
              "danger mode runs the warp loop to completion without yielding");

        // 非 warp ループは danger でも毎反復 yield する（パスごとに 1 反復）。
        const double plain = onePass(true, false, mode);
        if (plain != 4)
            std::printf("  %s: plain counter after 4 passes = %g\n",
                        modeName(mode), plain);
        check(plain == 4, "non-warp loop still yields in danger mode");

        // 意味論は不変 — 最後まで回せば同じ結果になる。
        {
            setJitMode(mode);
            Runtime rt;
            if (loadSb3(rt, projectJson(true), nullptr).ok) {
                rt.warpTimeMs = 0.0;
                rt.dangerMode = true;
                rt.greenFlag();
                rt.runUntilDone(10000);
                check(counter(rt) == kIterations,
                      "danger mode preserves the final result");
            } else {
                check(false, "load project (run to completion)");
            }
        }
    }

    // 待機系は danger でも譲る（ハングしない）— interp / JIT 両方。
    for (JitMode mode : modes) testWarpWaits(mode);

    // 待機 yield の判定（テスト用フック）: 通常 warp は stuck 時のみ、
    // danger の warp は常に、非 warp は常に譲る。
    {
        Runtime rt;
        rt.sequencer->timer.start();
        JitCtx c{};
        c.runtime = &rt;
        c.sequencer = rt.sequencer.get();
        c.isWarp = 1;
        rt.dangerMode = false;
        rt.warpTimeMs = 1.0e9;
        check(xyo_warp_yield_due(&c) == 0,
              "warp poll never yields before the time budget expires");
        rt.warpTimeMs = -1.0;
        check(xyo_warp_yield_due(&c) == 1,
              "warp poll reports when a scheduler handoff is required");
        rt.dangerMode = true;
        check(xyo_warp_yield_due(&c) == 0,
              "danger warp poll never forces a handoff");
        rt.warpTimeMs = 500.0;
        rt.dangerMode = false;
        check(xyo_wait_should_yield(&c) == 0,
              "normal warp waits yield only when stuck");
        rt.dangerMode = true;
        check(xyo_wait_should_yield(&c) == 1,
              "danger warp waits always yield");
        c.isWarp = 0;
        check(xyo_wait_should_yield(&c) == 1,
              "non-warp waits always yield");
    }

    // 環境変数の既定値（XYO_DANGER）— POSIX のみ検証する。
#ifndef _WIN32
    {
        setenv("XYO_DANGER", "1", 1);
        Runtime rt;
        check(rt.dangerMode, "XYO_DANGER=1 enables danger mode by default");
        setenv("XYO_DANGER", "0", 1);
        Runtime rt2;
        check(!rt2.dangerMode, "XYO_DANGER=0 keeps the budget");
        unsetenv("XYO_DANGER");
    }
#endif

    setJitMode(JitMode::Eager);
    std::printf("test_danger: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
