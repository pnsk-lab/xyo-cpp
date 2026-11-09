// Headless/mock-renderer scheduler integration and dispatch-and-join stress.
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "engine/pool.hpp"
#include "engine/runtime.hpp"
#include "engine/event_wait_dependency.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL scheduler: %s\n", name); }
}

struct MockRenderer final : RendererAPI {
    const std::thread::id caller = std::this_thread::get_id();
    mutable std::atomic<unsigned> wrongThread{0}, visual{0}, fences{0};
    std::atomic<unsigned> clears{0};
    void observe() const {
        if (std::this_thread::get_id() != caller) ++wrongThread;
    }
    void emitVisualChange(Target*) override { observe(); ++visual; }
    void getFencedPosition(int, double&, double&, bool) const override {
        observe(); ++fences;
    }
    void draw() override { observe(); }
    int createPenSkin() override { observe(); return 1; }
    void penClear(int) override { observe(); ++clears; }
};

std::string block(const char* op, const char* next,
                  const std::string& inputs = "{}",
                  const std::string& fields = "{}") {
    return std::string("{\"opcode\":\"") + op + "\",\"next\":" +
        (next ? "\"" + std::string(next) + "\"" : "null") +
        ",\"inputs\":" + inputs + ",\"fields\":" + fields + "}";
}
std::string hat(const char* next) {
    return std::string("{\"opcode\":\"event_whenflagclicked\",\"topLevel\":true,") +
        "\"next\":\"" + next + "\",\"inputs\":{},\"fields\":{}}";
}
std::string compute(bool allocations) {
    std::string blocks = "{\"hat\":" + hat("repeat") + ",\"repeat\":" +
        block("control_repeat", nullptr,
              "{\"TIMES\":[1,[4,\"24\"]],\"SUBSTACK\":[2,\"inc\"]}") +
        ",\"inc\":" + block("data_changevariableby", allocations ? "push" : nullptr,
              "{\"VALUE\":[1,[4,\"1\"]]}", "{\"VARIABLE\":[\"v\",\"v\"]}");
    if (allocations) {
        blocks += ",\"push\":" + block("data_addtolist", nullptr,
            "{\"ITEM\":[2,\"join\"]}", "{\"LIST\":[\"items\",\"items\"]}");
        blocks += ",\"join\":" + block("operator_join", nullptr,
            "{\"STRING1\":[1,[10,\"item-\"]],\"STRING2\":[2,\"read\"]}");
        blocks += ",\"read\":" + block("data_variable", nullptr, "{}",
            "{\"VARIABLE\":[\"v\",\"v\"]}");
    }
    return blocks + "}";
}
std::string barrier(const char* kind) {
    if (std::string(kind) == "motion")
        return "{\"hat\":" + hat("move") + ",\"move\":" +
            block("motion_setx", "effect", "{\"X\":[1,[4,\"17\"]]}") +
            ",\"effect\":" + block("looks_seteffectto", nullptr,
                "{\"VALUE\":[1,[4,\"10\"]]}",
                "{\"EFFECT\":[\"ghost\",null]}") + "}";
    if (std::string(kind) == "pen")
        return "{\"hat\":" + hat("clear") + ",\"clear\":" +
            block("pen_clear", nullptr) + "}";
    return "{\"hat\":" + hat("say") + ",\"say\":" +
        block("looks_say", nullptr, "{\"MESSAGE\":[1,[10,\"barrier\"]]}") + "}";
}
std::string target(const std::string& name, const std::string& blocks,
                   bool stage = false, int layer = 0) {
    return "{\"isStage\":" + std::string(stage ? "true" : "false") +
        ",\"name\":\"" + name + "\",\"variables\":" +
        (stage ? "{\"s\":[\"s\",\"\"]}" : "{\"v\":[\"v\",0]}") +
        ",\"lists\":{\"items\":[\"items\",[]]},\"broadcasts\":{},\"blocks\":" +
        blocks + ",\"costumes\":[],\"sounds\":[],\"currentCostume\":0,\"layerOrder\":" +
        std::to_string(layer) + "}";
}
std::string project(bool mixed, bool allocations) {
    std::string json = "{\"targets\":[" + target("Stage", "{}", true);
    for (int i = 0; i < 8; ++i) {
        if (mixed && i == 4) {
            json += "," + target("Bmotion", barrier("motion"), false, 5);
            json += "," + target("Bpen", barrier("pen"), false, 6);
            json += "," + target("Bsay", barrier("say"), false, 7);
        }
        json += "," + target("C" + std::to_string(i), compute(allocations),
                              false, i + 1 + (mixed && i >= 4 ? 3 : 0));
    }
    return json + "],\"monitors\":[],\"extensions\":[\"pen\"],\"meta\":{}}";
}

bool eligibleCompute(Runtime& rt, bool mixed) {
    bool all = true;
    for (Thread* thread : rt.threads) {
        const auto name = thread->target->getName();
        const bool isBarrier = !name.empty() && name[0] == u'B';
        const bool eligible = rt.sequencer->parallelEligible(*thread);
        if (isBarrier) {
            check(!eligible, "motion/effect, pen, and say remain serial barriers");
            continue;
        }
        if (!thread->isCompiled) { all = false; continue; }
        const auto* cs = static_cast<const CompiledScript*>(thread->compiled);
        if (!cs || !cs->gpuProgs.empty()) { all = false; continue; }
        check(eligible, "compiled CPU-only compute is worker-safe with renderer");
        all &= eligible;
    }
    (void)mixed;
    return all && !rt.threads.empty();
}

void testCompute(bool mixed, bool allocations) {
    // Renderer outlives Runtime, including disposal callbacks.
    MockRenderer renderer;
    Runtime rt;
    const auto loaded = loadSb3(rt, project(mixed, allocations), nullptr);
    check(loaded.ok, "load independent compute project");
    if (!loaded.ok) return;
    rt.renderer = &renderer;
    std::atomic<unsigned> says{0}, wrongSayThread{0};
    const auto caller = std::this_thread::get_id();
    rt.onSay = [&](Target*, const char*, const std::u16string&) {
        ++says;
        if (std::this_thread::get_id() != caller) ++wrongSayThread;
    };
    rt.greenFlag();
    const bool eligible = eligibleCompute(rt, mixed);
    const bool dispatchExpected = rt.sequencer->parallelEnabled() && eligible;
    const size_t collected = Gc::get().collectCount();
    if (allocations) Gc::get().setThreshold(1);
    rt.runUntilDone(200);
    if (allocations) Gc::get().setThreshold(2 << 20);
    check(rt.threads.empty(), "repeated-yield scripts complete");
    for (Target* t : rt.targets) {
        const auto name = t->getName();
        if (name.empty() || name[0] != u'C') continue;
        Variable* v = t->lookupVariableById("v");
        check(v && cast::toNumber(v->value) == 24, "independent exact variable result");
        if (allocations) {
            Variable* list = t->lookupVariableById("items");
            bool valid = list && list->list && list->list->len == 24;
            if (valid) {
                for (unsigned i = 0; i < 24; ++i) {
                    const Value value = list->list->items[i];
                    const std::string expected = "item-" + std::to_string(i + 1);
                    if (!value.isString() ||
                        SStr::toUtf8(value.asStrPtr()->view()) != expected) valid = false;
                }
            }
            check(valid, "allocated list/string values survive deferred collection");
        }
    }
    if (dispatchExpected) {
        const auto& stats = rt.sequencer->parallelStats();
        check(stats.dispatches > 0, "renderer-connected compute actually dispatched");
        check(stats.threads >= 8 && stats.groups >= 8, "compute dispatch statistics");
    } else {
        std::printf("SKIP scheduler dispatch assertion (parallel/JIT/CPU eligibility unavailable)\n");
    }
    check(renderer.wrongThread == 0 && wrongSayThread == 0,
          "all renderer and say callbacks stay on caller thread");
    if (mixed) {
        check(renderer.fences > 0 && renderer.visual > 0,
              "mixed motion/effect callbacks observed");
        check(renderer.clears > 0 && says > 0, "mixed pen and say callbacks observed");
    }
    if (allocations)
        check(Gc::get().collectCount() > collected, "low threshold triggers collection");
    // rt.onSay captures locals declared after rt; clear before their destruction.
    rt.onSay = {};
}

void testCompatBarrier() {
    MockRenderer renderer;
    Runtime rt;
    // An argument reporter outside its procedure cannot use native arguments;
    // it must enter the interpreter reporter compatibility path.
    const std::string blocks = "{\"hat\":" + hat("compat") +
        ",\"compat\":" + block("data_setvariableto", nullptr,
            "{\"VALUE\":[2,\"arg\"]}", "{\"VARIABLE\":[\"v\",\"v\"]}") +
        ",\"arg\":" + block("argument_reporter_string_number", nullptr,
            "{}", "{\"VALUE\":[\"missing\",null]}") + "}";
    const auto json = "{\"targets\":[" + target("Stage", "{}", true) + "," +
        target("C0", blocks, false, 1) + "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    check(loadSb3(rt, json, nullptr).ok, "load legacy compat project");
    rt.renderer = &renderer;
    rt.greenFlag();
    for (Thread* thread : rt.threads) {
        check(!rt.sequencer->parallelEligible(*thread),
              "compat fallback cannot enter worker despite empty opcode effects");
        if (thread->isCompiled) {
            const auto* cs = static_cast<const CompiledScript*>(thread->compiled);
            check(cs && cs->mainThread, "compiled compat helper carries caller-thread affinity");
        }
    }
    rt.runUntilDone(100);
    check(rt.threads.empty(), "legacy compat fallback still executes");
}

void testSharedOrder() {
    MockRenderer renderer;
    Runtime rt;
    std::string json = "{\"targets\":[" + target("Stage", "{}", true);
    for (int i = 0; i < 8; ++i) {
        const std::string blocks = "{\"hat\":" + hat("append") + ",\"append\":" +
            block("data_setvariableto", nullptr, "{\"VALUE\":[2,\"join\"]}",
                  "{\"VARIABLE\":[\"s\",\"s\"]}") + ",\"join\":" +
            block("operator_join", nullptr,
                "{\"STRING1\":[2,\"read\"],\"STRING2\":[1,[10,\"" +
                std::to_string(i) + "\"]]}") + ",\"read\":" +
            block("data_variable", nullptr, "{}", "{\"VARIABLE\":[\"s\",\"s\"]}") + "}";
        json += "," + target("C" + std::to_string(i), blocks, false, i + 1);
    }
    json += "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    const auto loaded = loadSb3(rt, json, nullptr);
    check(loaded.ok, "load shared-variable ordering project");
    if (!loaded.ok) return;
    rt.renderer = &renderer;
    rt.greenFlag();
    rt.runUntilDone(100);
    Variable* shared = rt.getTargetForStage()->lookupVariableById("s");
    check(shared && shared->value.isString() &&
          shared->value.asStrPtr()->view() == u"76543210",
          "conflicting scripts retain spawn order with renderer");
    check(rt.sequencer->parallelStats().dispatches == 0,
          "single conflict component is not dispatched");
}

void testSharedYieldChain() {
    // Different repeat counts force several fibers to complete in the same
    // chain pass while the other fibers continue yielding. The stage variable
    // is shared, so the dependence check must keep the original order.
    Runtime rt;
    std::string json = "{\"targets\":[" + target("Stage", "{}", true);
    int expected = 0;
    for (int i = 0; i < 8; ++i) {
        const int repetitions = 8 + i;
        expected += repetitions;
        const std::string blocks =
            "{\"hat\":" + hat("repeat") + ",\"repeat\":" +
            block("control_repeat", nullptr,
                  "{\"TIMES\":[1,[4,\"" + std::to_string(repetitions) +
                  "\"]],\"SUBSTACK\":[2,\"inc\"]}") +
            ",\"inc\":" +
            block("data_changevariableby", nullptr,
                  "{\"VALUE\":[1,[4,\"1\"]]}",
                  "{\"VARIABLE\":[\"s\",\"s\"]}") + "}";
        json += "," + target("Y" + std::to_string(i), blocks, false, i + 1);
    }
    json += "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    const auto loaded = loadSb3(rt, json, nullptr);
    check(loaded.ok, "load mixed-lifetime shared-writer fibers");
    if (!loaded.ok) return;
    rt.greenFlag();
    const bool eligible = rt.sequencer->parallelEnabled() &&
        !rt.threads.empty() && rt.threads[0]->isCompiled;
    rt.runUntilDone(200);
    check(rt.threads.empty(), "shared-writer fibers all terminate");
    Variable* shared = rt.getTargetForStage()->lookupVariableById("s");
    check(shared && cast::toNumber(shared->value) == expected,
          "mixed-lifetime chain preserves every variable increment");
    if (eligible && std::getenv("XYO_FIBER_CHAIN") == nullptr) {
        check(rt.sequencer->parallelStats().chainedPasses > 0,
              "shared-writer direct fiber chain exercised");
    }
}

void testBroadcastYieldChain() {
    Runtime rt;
    std::string stage = target("Stage", "{}", true);
    const auto listAt = stage.find("\"items\":[\"items\",[]]");
    stage.replace(listAt, std::string("\"items\":[\"items\",[]]").size(),
                  "\"log\":[\"log\",[]]");
    std::string json = "{\"targets\":[" + stage;
    for (int i = 0; i < 4; ++i) {
        const std::string recv =
            "{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
            "\"topLevel\":true,\"next\":\"loop\",\"inputs\":{},"
            "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},"
            "\"loop\":" + block("control_repeat", nullptr,
                "{\"TIMES\":[1,[4,\"" + std::to_string(3 + i) +
                "\"]],\"SUBSTACK\":[2,\"append\"]}") +
            ",\"append\":" + block("data_addtolist", nullptr,
                "{\"ITEM\":[1,[4,\"" + std::to_string(i) + "\"]]}",
                "{\"LIST\":[\"log\",\"log\"]}") + "}";
        json += "," + target("R" + std::to_string(i), recv, false, i + 1);
    }
    json += "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    check(loadSb3(rt, json, nullptr).ok, "load broadcast shared-list chain fixture");
    if (rt.targets.size() != 5) return;
    std::map<std::string, std::u16string> match{{"BROADCAST_OPTION", u"PING"}};
    auto old = rt.startHats("event_whenbroadcastreceived", &match, nullptr);
    check(old.size() == 4, "broadcast starts four distinct receivers");
    if (old.size() != 4) return;
    std::vector<int> order;
    for (Thread* t : rt.threads) {
        order.push_back(int(t->target->getName().back() - u'0'));
        rt.sequencer->stepThread(*t);
    }
    // Force fresh receiver identities, as broadcast-and-wait does, while all
    // previous fibers are suspended. This isolates the restart/chain boundary.
    auto fresh = rt.startHats("event_whenbroadcastreceived", &match, nullptr);
    bool replaced = fresh.size() == old.size();
    for (Thread* t : old)
        replaced &= !rt.containsThread(t) && t->fiber == nullptr;
    check(replaced, "rebroadcast removes every suspended old receiver identity");
    const bool eligible = rt.sequencer->parallelEnabled() &&
        !fresh.empty() && fresh[0]->isCompiled;
    rt.runUntilDone(100);
    check(rt.threads.empty(), "mixed-lifetime broadcast fibers all terminate");
    std::vector<int> expected = order;
    for (int pass = 0; pass < 6; ++pass)
        for (int id : order)
            if (pass < 3 + id) expected.push_back(id);
    Variable* log = rt.getTargetForStage()->lookupVariableById("log");
    bool exact = log && log->list && log->list->len == expected.size();
    if (exact)
        for (size_t i = 0; i < expected.size(); ++i)
            exact &= cast::toNumber(log->list->items[i]) == expected[i];
    check(exact, "broadcast chain preserves shared-list order without stale receiver effects");
    if (eligible && std::getenv("XYO_FIBER_CHAIN") == nullptr &&
        std::getenv("XYO_PROF") == nullptr)
        check(rt.sequencer->parallelStats().chainedPasses > 0,
              "broadcast receivers exercise direct fiber chain");
}

void testSelfBroadcastDoesNotRecycleActiveFiber() {
    // A broadcast can target the script currently executing it. The active
    // fiber must never be retired or reset while its own stack is live.
    const std::string recv =
        "{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
        "\"topLevel\":true,\"next\":\"inc\",\"inputs\":{},"
        "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},"
        "\"inc\":" +
        block("data_changevariableby", "check",
              "{\"VALUE\":[1,[4,\"1\"]]}",
              "{\"VARIABLE\":[\"s\",\"s\"]}") +
        ",\"check\":" +
        block("control_if", nullptr,
              "{\"CONDITION\":[2,\"lt\"],\"SUBSTACK\":[2,\"send\"]}") +
        ",\"lt\":" +
        block("operator_lt", nullptr,
              "{\"OPERAND1\":[2,\"read\"],\"OPERAND2\":[1,[4,\"3\"]]}") +
        ",\"read\":" +
        block("data_variable", nullptr, "{}",
              "{\"VARIABLE\":[\"s\",\"s\"]}") +
        ",\"send\":" +
        block("event_broadcast", nullptr,
              "{\"BROADCAST_INPUT\":[1,[11,\"PING\",\"PING\"]]}") +
        "}";
    std::string stage = target("Stage", "{}", true);
    const std::string emptyBroadcasts = "\"broadcasts\":{}";
    const size_t at = stage.find(emptyBroadcasts);
    if (at != std::string::npos)
        stage.replace(at, emptyBroadcasts.size(),
                      "\"broadcasts\":{\"PING\":\"PING\"}");
    const std::string json =
        "{\"targets\":[" + stage + "," +
        target("Receiver", recv) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    Runtime rt;
    const auto loaded = loadSb3(rt, json, nullptr);
    check(loaded.ok, "load self-broadcast script");
    if (!loaded.ok) return;
    rt.startBroadcast(u"PING");
    rt.runUntilDone(100);
    Variable* shared = rt.getTargetForStage()->lookupVariableById("s");
    check(shared && cast::toNumber(shared->value) == 3,
          "self-broadcast correctly runs three receiver instances");
    check(rt.threads.empty(), "self-broadcast terminates without stale fiber");
}

void testBroadcastReceiverReuse() {
    // An ordinary compiled broadcast can restart a suspended receiver
    // without changing its Thread* identity. Broadcast-and-wait, however,
    // exposes receiver handles and must create a fresh Thread.
    const std::string recv =
        "{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
        "\"topLevel\":true,\"next\":\"loop\",\"inputs\":{},"
        "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},"
        "\"loop\":" + block("control_repeat", nullptr,
             "{\"TIMES\":[1,[4,\"5\"]],\"SUBSTACK\":[2,\"inc\"]}") +
        ",\"inc\":" + block("data_changevariableby", nullptr,
             "{\"VALUE\":[1,[4,\"1\"]]}",
             "{\"VARIABLE\":[\"s\",\"s\"]}") + "}";
    const std::string json =
        "{\"targets\":[" + target("Stage", "{}", true) + "," +
        target("Receiver", recv) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    Runtime rt;
    const auto loaded = loadSb3(rt, json, nullptr);
    check(loaded.ok, "load restartable broadcast receiver");
    if (!loaded.ok) return;

    rt.startBroadcast(u"PING");
    check(rt.threads.size() == 1, "ordinary broadcast starts receiver");
    if (rt.threads.empty()) return;
    Thread* first = rt.threads[0];
    if (!first->isCompiled) return; // JIT-only reuse (interpreter semantics unchanged).

    rt.sequencer->stepThread(*first);
    Variable* shared = rt.getTargetForStage()->lookupVariableById("s");
    check(shared && cast::toNumber(shared->value) == 1,
          "first receiver yields after one increment");
    check(first->fiber != nullptr, "first receiver has a suspended fiber");

    rt.startBroadcast(u"ping");
    check(rt.threads.size() == 1 && rt.threads[0] == first,
          "ordinary broadcast reuses compiled receiver");
    check(first->fiber == nullptr && first->status == STATUS_RUNNING,
          "restart discards old fiber and resets run status");

    const std::map<std::string, std::u16string> match{
        {"BROADCAST_OPTION", u"PING"}};
    auto waitReceivers =
        rt.startHats("event_whenbroadcastreceived", &match, nullptr);
    check(waitReceivers.size() == 1, "join broadcast starts receiver");
    if (waitReceivers.empty()) return;
    Thread* joined = waitReceivers[0];
    check(joined != first, "join receiver has a distinct identity");

    // A join-exposed receiver must be replaced rather than reused. Start
    // its JIT fiber first, so restarting it tests the retirement path that
    // previously kept 1 MiB fiber stack mappings alive indefinitely.
    bool suspendedJoined = false;
    if (joined->isCompiled) {
        rt.sequencer->stepThread(*joined);
        suspendedJoined = joined->fiber != nullptr;
        check(suspendedJoined, "join-exposed receiver suspended its fiber");
    }
    rt.startBroadcast(u"PING");
    check(rt.threads.size() == 1 && rt.threads[0] != joined,
          "ordinary broadcast must not reuse join-exposed receiver");
    if (suspendedJoined) {
        check(joined->fiber == nullptr && joined->jitCtx == nullptr &&
                  joined->status == STATUS_DONE,
              "replaced receiver releases suspended fiber immediately");
    }
    Thread* subsequent = rt.threads[0];
    rt.startBroadcast(u"ping");
    check(rt.threads.size() == 1 && rt.threads[0] == subsequent,
          "subsequent ordinary broadcast is reusable again");

    rt.runUntilDone(100);
    check(rt.threads.empty(), "restarted receiver completes");
    check(shared && cast::toNumber(shared->value) ==
          (suspendedJoined ? 7 : 6),
          "only executed receiver effects are visible after replacement");
}

void testGraphicEffectsStorage() {
    GraphicEffects base;
    check(base.size() == 7 && base.count("ghost") == 1 &&
          base.count("unknown") == 0 && !base.anyActive(),
          "fixed graphics effects provide seven zero-valued entries");
    base["ghost"] = 45.0;
    base["fisheye"] = -10.0;
    base["custom_fx"] = 15.0;
    check(base.size() == 8 && base.count("custom_fx") == 1 &&
          base.get("custom_fx") == 15.0 && base.anyActive(),
          "custom graphics effect keys retain legacy map insertion semantics");
    GraphicEffects copied = base;
    check(copied == base, "graphics effects copy preserves all values");
    check(copied.erase("ghost") == 1 &&
          copied.erase("ghost") == 0 &&
          copied.count("ghost") == 0 &&
          copied.size() == 7 &&
          copied.get("ghost") == 0.0,
          "erasing a standard graphics effect retains map semantics");
    copied["ghost"] = 45.0;
    check(copied == base && copied.size() == 8,
          "recreating an erased graphics effect restores its value and key");
    check(copied.erase("custom_fx") == 1 &&
          copied.count("custom_fx") == 0,
          "custom graphics effect erase remains supported");
    copied["custom_fx"] = 15.0;
    copied["ghost"] = 12.0;
    check(base.get("ghost") == 45.0 && copied != base,
          "copying graphic effects produces independent mutable storage");
    copied.clearValues();
    check(!copied.anyActive() && copied.size() == 8 &&
          copied.count("custom_fx") == 1 &&
          copied.get("custom_fx") == 0.0,
          "clearing graphic effects retains known and custom keys");
}

void testBroadcastIndexInvalidation() {
    const std::string receiver =
        "{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
        "\"topLevel\":true,\"next\":\"inc\",\"inputs\":{},"
        "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},"
        "\"inc\":" +
        block("data_changevariableby", nullptr,
              "{\"VALUE\":[1,[4,\"1\"]]}",
              "{\"VARIABLE\":[\"s\",\"s\"]}") + "}";
    const std::string json =
        "{\"targets\":[" + target("Stage", "{}", true) + "," +
        target("Receiver", receiver) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    Runtime rt;
    const auto loaded = loadSb3(rt, json, nullptr);
    check(loaded.ok, "load broadcast-index fixture");
    if (!loaded.ok) return;
    Target* receiverTarget = rt.getSpriteTargetByName(u"Receiver");
    Variable* shared = rt.getTargetForStage()->lookupVariableById("s");
    if (!receiverTarget || !shared) {
        check(false, "broadcast-index fixture has receivers and shared var");
        return;
    }

    rt.startBroadcast(u"ping");
    rt.runUntilDone(50);
    check(cast::toNumber(shared->value) == 1,
          "first case-insensitive broadcast is delivered");

    Block* hat = receiverTarget->blocks->getBlock("recv");
    if (!hat) {
        check(false, "receiver hat exists");
        return;
    }
    hat->fields["BROADCAST_OPTION"].value =
        Value::string(SStr::create(u"PONG"));
    receiverTarget->blocks->resetCache();
    rt.startBroadcast(u"ping");
    check(rt.threads.empty(), "stale broadcast subscription invalidated");
    rt.startBroadcast(u"pOnG");
    rt.runUntilDone(50);
    check(cast::toNumber(shared->value) == 2,
          "renamed broadcast subscription is available");

    // The clone constructor skips temporary effect-map initialization.
    // Verify that inherited effects (including nonzero values) are exactly
    // equal to those of the source target.
    check(receiverTarget->effects.size() == 7,
          "original target initializes all visual effects");
    receiverTarget->effects["ghost"] = 37.0;
    receiverTarget->effects["fisheye"] = -12.0;
    Target* clone = receiverTarget->makeClone();
    check(clone != nullptr, "create subscriber clone");
    if (!clone) return;
    check(clone->effects == receiverTarget->effects &&
          clone->effects.size() == 7,
          "clone inherits exact source effect map without extra initialization");
    rt.addTarget(clone);
    clone->goBehindOther(receiverTarget);
    rt.startBroadcast(u"pong");
    rt.runUntilDone(50);
    check(cast::toNumber(shared->value) == 4,
          "new clone receives exactly one additional broadcast");
    rt.disposeTarget(clone);
    rt.startBroadcast(u"PONG");
    rt.runUntilDone(50);
    check(cast::toNumber(shared->value) == 5,
          "disposed clone subscription is removed");

    // Last-clone pop_back and non-last erase must both preserve membership.
    Target* a = receiverTarget->makeClone();
    Target* b = receiverTarget->makeClone();
    if (!a || !b) {
        check(false, "create two further clones");
        if (a) { rt.addTarget(a); rt.disposeTarget(a); }
        if (b) { rt.addTarget(b); rt.disposeTarget(b); }
        return;
    }
    rt.addTarget(a);
    rt.addTarget(b);
    check(receiverTarget->sprite->clones.size() == 3,
          "original and two clones appear in sprite list");
    rt.disposeTarget(a);
    check(receiverTarget->sprite->clones.size() == 2 &&
          receiverTarget->sprite->clones.back() == b,
          "disposing a non-last clone preserves later clone");
    rt.disposeTarget(b);
    check(receiverTarget->sprite->clones.size() == 1 &&
          receiverTarget->sprite->clones.front() == receiverTarget,
          "disposing the last clone returns to original-only list");
}

void testThreadMembership() {
    Runtime rt;
    const auto loaded = loadSb3(rt, project(false, false), nullptr);
    check(loaded.ok, "load thread membership fixture");
    if (!loaded.ok) return;
    Target* stage = rt.getTargetForStage();
    // Avoid compilation: inspect identity without executing these dummy blocks.
    Thread* a = rt.pushThread("membership-a", stage, false, true);
    Thread* b = rt.pushThread("membership-b", stage, false, true);
    Thread* c = rt.pushThread("membership-c", stage, false, true);
    check(rt.containsThread(a) && rt.containsThread(b) && rt.containsThread(c),
          "new threads are members");
    check(rt.isActiveThread(a), "nonempty running member is active");
    a->status = STATUS_DONE;
    check(rt.containsThread(a) && !rt.isActiveThread(a),
          "DONE member remains present but is not active");
    a->status = STATUS_RUNNING;
    a->stack.clear();
    check(rt.containsThread(a) && !rt.isActiveThread(a),
          "empty-stack member remains present but is not active");
    a->pushStack("membership-a");

    rt.threads[0] = c;
    rt.threads[2] = a;
    check(rt.containsThread(a) && rt.containsThread(c) && rt.isActiveThread(a),
          "public reorder repairs stale membership hints");
    rt.threads.erase(rt.threads.begin() + 1);
    check(!rt.containsThread(b) && !rt.isActiveThread(b),
          "public erase rejects removed identity at a reused index");
    check(rt.containsThread(a) && rt.containsThread(c),
          "public erase preserves shifted members");
    rt.threads.push_back(b);
    check(rt.containsThread(b) && rt.isActiveThread(b),
          "public reinsertion finds a previously absent thread");

    // Self-restart replaces identity but the old script runs until its next yield.
    rt.sequencer->activeThread = a;
    Thread* replacement = rt.restartThread(a);
    check(replacement != a && rt.containsThread(replacement),
          "restart installs new identity in the same scheduling slot");
    check(!rt.containsThread(a) && !rt.isActiveThread(a),
          "restart rejects old identity despite its stale index hint");
    check(a->status == STATUS_RUNNING && !a->stack.empty(),
          "active old thread survives restart until sequencer boundary");
    rt.sequencer->activeThread = nullptr;

    rt.sequencer->stepping = &rt.threads;
    rt.stopAll();
    check(rt.threads.empty() && rt.sequencer->stepping != &rt.threads &&
          rt.sequencer->stepping->size() == 3,
          "stopAll detaches current pass while preserving its list");
    check(!rt.containsThread(c) && !rt.containsThread(replacement) &&
          !rt.containsThread(b),
          "detached pass identities are not current runtime members");
    Thread* fresh = rt.pushThread("membership-fresh", stage, false, true);
    check(rt.containsThread(fresh) && rt.isActiveThread(fresh),
          "new spawn after detach belongs to new runtime list");
    check(!rt.containsThread(c) && !rt.containsThread(replacement) &&
          !rt.containsThread(b),
          "new spawn does not revive detached identities with matching hints");
    check((*rt.sequencer->stepping)[0] == c,
          "new spawn leaves detached pass order unchanged");
    rt.sequencer->stepping = nullptr;
    rt.sequencer->detachedLists.clear();
}

void testThreadReclamation() {
    Runtime rt;
    const auto own = [&](bool done = true) {
        auto thread = std::make_unique<Thread>("reclaim");
        Thread* t = thread.get();
        t->pushStack("reclaim");
        if (done) {
            t->status = STATUS_DONE;
            t->stack.clear();
        }
        rt.ownedThreads.push_back(std::move(thread));
        return t;
    };
    Thread* first = own(false);
    for (int i = 0; i < 2048; ++i) own();
    Thread* joined = own();
    Thread* compat = own();
    Thread* nested = own();
    Thread* lastDone = own();
    Thread* waiting = own();
    Thread* detached = own();
    Thread* active = own();
    Thread* mapped = own();
    Thread* jitJoined = own();
    Thread* trashJoined = own();
    Thread* last = own(false);
    rt.threads = {first, last};
    first->stackFrames.back()->executionContext = std::make_unique<ExecContext>();
    first->stackFrames.back()->executionContext->startedThreads = {joined};
    first->compatibilityStackFrame.startedThreads = {compat};
    joined->compatibilityStackFrame.startedThreads = {nested};
    first->trashFrames.push_back(std::make_unique<StackFrame>());
    first->trashFrames.back()->executionContext = std::make_unique<ExecContext>();
    first->trashFrames.back()->executionContext->startedThreads = {trashJoined};
    std::vector<Thread*> jitHandles{jitJoined};
    first->jitCtx = new JitCtx{};
    first->jitCtx->joinRefs = &jitHandles;
    rt.lastStepDoneThreads = {lastDone};
    Runtime::WaitEntry entry;
    entry.thread = waiting;
    rt.waitQueue.push_back(entry);
    auto oldPass = std::make_unique<std::vector<Thread*>>();
    oldPass->push_back(detached);
    rt.sequencer->stepping = oldPass.get();
    rt.sequencer->detachedLists.push_back(std::move(oldPass));
    rt.sequencer->activeThread = active;
    rt.threadMap["retained"] = mapped;
    rt.ownedThreads.push_back(nullptr);
    first->justReported = Value::string(SStr::create(u"retained GC root"));
    Gc::get().collect();
    check(first->justReported.isString() &&
          first->justReported.asStrPtr()->view() == u"retained GC root",
          "GC safely visits owned roots even with an empty ownership slot");

    rt.reclaimThreads();
    check(rt.ownedThreads.size() == 12,
          "reclamation compacts dead historical entries and null slots");
    check(rt.ownedThreads.front().get() == first && rt.ownedThreads.back().get() == last,
          "ownership compaction preserves endpoint addresses and order");
    bool retained = true;
    for (Thread* expected : {joined, compat, nested, lastDone, waiting, detached,
                             active, mapped, jitJoined, trashJoined}) {
        bool found = false;
        for (const auto& owned : rt.ownedThreads) found |= owned.get() == expected;
        retained &= found;
    }
    check(retained, "join, compat, trash, queue, detached and active handles survive reclamation");
    check(rt.reclaimAt == 12 + 12 / 4 + 1024,
          "next reclamation threshold depends on survivors rather than spawn history");
    Gc::get().collect();
    check(first->justReported.asStrPtr()->view() == u"retained GC root",
          "GC roots survive ownership compaction");

    first->stackFrames.back()->executionContext.reset();
    first->compatibilityStackFrame.startedThreads.clear();
    first->trashFrames.clear();
    first->jitCtx->joinRefs = nullptr;
    rt.lastStepDoneThreads.clear();
    rt.waitQueue.clear();
    rt.threadMap.clear();
    rt.sequencer->stepping = nullptr;
    rt.sequencer->detachedLists.clear();
    rt.sequencer->activeThread = nullptr;
    rt.reclaimThreads();
    check(rt.ownedThreads.size() == 2 && rt.ownedThreads[0].get() == first &&
          rt.ownedThreads[1].get() == last,
          "released handles and their transitive join references are reclaimed");
    delete first->jitCtx;
    first->jitCtx = nullptr;
    first->status = last->status = STATUS_DONE;
    first->stack.clear();
    last->stack.clear();
    rt.threads.clear();
    rt.reclaimThreads();
    check(rt.ownedThreads.empty() && rt.reclaimAt == 1024,
          "fully completed workload leaves no historical ownership storage");
    Gc::get().collect();
}

void testRecycledThreadStorage() {
    if (const char* setting = std::getenv("XYO_THREAD_REUSE");
        setting && setting[0] == '0') return;
    Runtime rt;
    const auto loaded = loadSb3(rt, project(false, false), nullptr);
    check(loaded.ok, "load recycle fixture");
    if (!loaded.ok) return;
    Target* stage = rt.getTargetForStage();
    Thread* held = rt.pushThread("held", stage, false, true);
    Thread* recycled = rt.pushThread("recycled", stage, false, true);
    held->status = recycled->status = STATUS_DONE;
    held->stack.clear();
    recycled->stack.clear();
    rt.threads.clear();
    // This pointer could be a pending UI handle or join result; it must
    // never be recycled while the runtime advertises that ownership.
    rt.lastStepDoneThreads.push_back(held);
    recycled->justReported = Value::string(SStr::create(u"old value"));
    rt.reclaimThreads();
    check(rt.ownedThreads.size() == 1 &&
              rt.ownedThreads.front().get() == held,
          "live done-thread handle survives recycling");
    check(rt.reusableThreads.size() == 1 &&
              rt.reusableThreads.back().get() == recycled,
          "unreferenced done-thread storage enters pool");
    Gc::get().collect();
    Thread* next = rt.pushThread("new-identity", stage, false, true);
    check(next == recycled && next != held,
          "new script reuses only unreferenced thread storage");
    check(next->topBlock == "new-identity" &&
              next->status == STATUS_RUNNING && next->target == stage &&
              next->justReported.isUndefined() &&
              next->compatibilityStackFrame.startedThreads.empty() &&
              next->idCache_.empty(),
          "recycled thread has fresh logical identity and state");
    check(rt.containsThread(next), "recycled thread has current index");
    check(held->topBlock == "held" && held->status == STATUS_DONE,
          "held thread remains an independent completed identity");
}

void testFrameAddressStability() {
    Thread thread("frames");
    std::vector<StackFrame*> frames;
    std::vector<ExecContext*> contexts;
    for (int i = 0; i < 512; ++i) {
        thread.pushStack("depth-" + std::to_string(i));
        StackFrame* frame = thread.peekStackFrame();
        frame->executionContext = std::make_unique<ExecContext>();
        frame->executionContext->returnValue = Value::num(i);
        frames.push_back(frame);
        contexts.push_back(frame->executionContext.get());
    }
    bool stable = true;
    for (size_t i = 0; i < frames.size(); ++i)
        stable &= thread.stackFrames[i].get() == frames[i] &&
                  frames[i]->executionContext.get() == contexts[i] &&
                  cast::toNumber(contexts[i]->returnValue) == double(i);
    check(stable, "512 frame growth preserves StackFrame and ExecContext addresses");
    for (size_t i = 0; i < frames.size(); ++i) thread.popStack();
    stable = thread.stackFrames.empty() && thread.trashFrames.size() == frames.size();
    for (size_t i = 0; i < frames.size(); ++i) {
        const size_t original = frames.size() - 1 - i;
        stable &= thread.trashFrames[i].get() == frames[original] &&
                  frames[original]->executionContext.get() == contexts[original] &&
                  cast::toNumber(contexts[original]->returnValue) == double(original);
    }
    check(stable, "stack-to-trash movement preserves frame/context references until recycle");
}

void testWaitUntilChain() {
    Runtime rt;
    std::string stage = target("Stage", "{}", true);
    const auto listAt = stage.find("\"items\":[\"items\",[]]");
    stage.replace(listAt, std::string("\"items\":[\"items\",[]]").size(),
                  "\"log\":[\"log\",[]]");
    std::string json = "{\"targets\":[" + stage;
    for (int i = 0; i < 4; ++i) {
        const std::string recv =
            "{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
            "\"topLevel\":true,\"next\":\"wait\",\"inputs\":{},"
            "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},"
            "\"wait\":" + block("control_wait_until", "append",
                "{\"CONDITION\":[2,\"eq\"]}") +
            ",\"eq\":" + block("operator_equals", nullptr,
                "{\"OPERAND1\":[2,\"gate\"],\"OPERAND2\":[1,[4,\"1\"]]}") +
            ",\"gate\":" + block("data_variable", nullptr, "{}",
                "{\"VARIABLE\":[\"s\",\"s\"]}") +
            ",\"append\":" + block("data_addtolist", nullptr,
                "{\"ITEM\":[1,[4,\"" + std::to_string(i) + "\"]]}",
                "{\"LIST\":[\"log\",\"log\"]}") + "}";
        json += "," + target("R" + std::to_string(i), recv, false, i + 1);
    }
    // Writer is first in executable order (reverse layer order). It yields
    // several times before opening the shared gate, then appends a sentinel.
    const std::string writer =
        "{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
        "\"topLevel\":true,\"next\":\"loop\",\"inputs\":{},"
        "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},"
        "\"loop\":" + block("control_repeat", "open",
            "{\"TIMES\":[1,[4,\"4\"]],\"SUBSTACK\":[2,\"inc\"]}") +
        ",\"inc\":" + block("data_changevariableby", nullptr,
            "{\"VALUE\":[1,[4,\"1\"]]}", "{\"VARIABLE\":[\"v\",\"v\"]}") +
        ",\"open\":" + block("data_setvariableto", "sentinel",
            "{\"VALUE\":[1,[4,\"1\"]]}", "{\"VARIABLE\":[\"s\",\"s\"]}") +
        ",\"sentinel\":" + block("data_addtolist", nullptr,
            "{\"ITEM\":[1,[4,\"-1\"]]}", "{\"LIST\":[\"log\",\"log\"]}") + "}";
    json += "," + target("Writer", writer, false, 5);
    json += "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    const auto loaded = loadSb3(rt, json, nullptr);
    check(loaded.ok, "load wait-until receiver chain fixture");
    if (!loaded.ok) return;
    rt.getTargetForStage()->lookupVariableById("s")->value = Value::num(0);
    std::map<std::string, std::u16string> match{{"BROADCAST_OPTION", u"PING"}};
    const auto receivers = rt.startHats("event_whenbroadcastreceived", &match, nullptr);
    bool eligible = rt.sequencer->parallelEnabled() && receivers.size() == 5;
    for (Thread* t : receivers) eligible &= rt.sequencer->parallelEligible(*t);
    rt.runUntilDone(100);
    check(rt.threads.empty(), "wait-until receivers terminate after writer opens gate");
    Variable* log = rt.getTargetForStage()->lookupVariableById("log");
    const int expected[] = {-1, 3, 2, 1, 0};
    bool exact = log && log->list && log->list->len == 5;
    if (exact)
        for (size_t i = 0; i < 5; ++i)
            exact &= cast::toNumber(log->list->items[i]) == expected[i];
    check(exact, "wait-until chain observes writer before appending in receiver order");
    // Dependency-watched pure waits, even without the global event
    // scheduler, resume serially so a waiter cannot overtake its writer.
    // Exact output and receiver order are tested above for both paths.
    // The low-level fiber-chain counter is meaningful only with regular
    // polling, not when the watcher intentionally suppresses retries.
    if (eligible && !rt.sequencer->eventSchedulerEnabled() &&
        !isolatedEventWaitDependencyEnabled() &&
        std::getenv("XYO_FIBER_CHAIN") == nullptr &&
        std::getenv("XYO_PROF") == nullptr)
        check(rt.sequencer->parallelStats().chainedPasses > 0,
              "pure wait-until receivers exercise direct fiber chain");
}


void testCloneMutationSchedulerAffinity() {
    // A native clone-mutation script is never safe for worker threads.
    // Codegen can mark this once and avoid re-analyzing the same source
    // separately for thousands of ephemeral clone Target identities.
    if (!jitEnabled()) return;
    const std::string blocks =
        "{\"cloneHat\":{\"opcode\":\"control_start_as_clone\","
        "\"topLevel\":true,\"next\":\"del\",\"inputs\":{},\"fields\":{}},"
        "\"del\":{\"opcode\":\"control_delete_this_clone\","
        "\"next\":null,\"inputs\":{},\"fields\":{}}}";
    const std::string json =
        "{\"targets\":[" + target("Stage", "{}", true) + "," +
        target("CloneHost", blocks, false, 1) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    Runtime rt;
    const auto loaded = loadSb3(rt, json, nullptr);
    check(loaded.ok, "load clone mutation compiler fixture");
    if (!loaded.ok) return;
    Target* sprite = rt.getSpriteTargetByName(u"CloneHost");
    check(sprite != nullptr, "clone mutation sprite exists");
    if (!sprite) return;

    Thread thread("cloneHat");
    thread.target = sprite;
    thread.blockContainer = sprite->blocks;
    thread.pushStack("cloneHat");
    const Block* top = sprite->blocks->getBlock("cloneHat");
    const CompiledScript* compiled =
        rt.jit->compile(thread, rt, *rt.sequencer, top);
    check(compiled != nullptr,
          "clone mutation script compiles to native JIT");
    if (!compiled) return;
    const char* flag = std::getenv("XYO_CLONE_SERIAL_CODEGEN");
    const bool enabled = !flag || flag[0] != '0';
    check(compiled->mainThread == enabled,
          "clone mutation host affinity matches explicit setting");

}

void testShortBroadcastReadyCache() {
    // A compiled, non-yielding receiver can reattach directly to a cached
    // native script. Each fire-and-forget broadcast still creates a fresh
    // logical execution and commits its variable write once.
    const JitMode previousMode = jitMode();
    setJitMode(JitMode::HotSpot);
    const std::string receiver =
        "{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
        "\"topLevel\":true,\"next\":\"inc\",\"inputs\":{},"
        "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},"
        "\"inc\":" + block("data_changevariableby", nullptr,
        "{\"VALUE\":[1,[4,\"1\"]]}",
        "{\"VARIABLE\":[\"s\",\"s\"]}") + "}";
    const std::string json =
        "{\"targets\":[" + target("Stage", "{}", true) + "," +
        target("Receiver", receiver) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    Runtime rt;
    const auto loaded = loadSb3(rt, json, nullptr);
    check(loaded.ok, "load short broadcast cached-native fixture");
    if (loaded.ok) {
        Target* stage = rt.getTargetForStage();
        Variable* shared = stage->lookupVariableById("s");
        bool usedShortSync = false;
        for (int i = 0; i < 12; ++i) {
            rt.startBroadcast(i & 1 ? u"ping" : u"PING");
            check(rt.threads.size() == 1,
                  "one short broadcast starts exactly one script");
            if (!rt.threads.empty()) {
                Thread* started = rt.threads.front();
                if (started->isCompiled) {
                    const auto* compiled =
                        static_cast<const CompiledScript*>(started->compiled);
                    usedShortSync |= compiled && compiled->shortSync &&
                                     !compiled->mayYield;
                }
            }
            rt.runUntilDone(30);
            check(rt.threads.empty(),
                  "short receiver terminates before next broadcast");
            check(shared && cast::toNumber(shared->value) == i + 1,
                  "short broadcast increments Stage exactly once");
        }
        if (rt.jit && rt.jit->ok())
            check(usedShortSync,
                  "HotSpot warms a non-yielding cached broadcast listener");
    }
    setJitMode(previousMode);
}

void testNonRestartBroadcastFanout() {
    // A non-restarting broadcast must skip a receiver that is already
    // running without suppressing any of the other matching receivers.
    // Twenty pre-existing receivers force the lazy membership index to
    // activate; the remaining twelve must still be started in order.
    for (int route = 0; route < 3; ++route) {
        Runtime rt;
        const std::string receiver =
            "{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
            "\"topLevel\":true,\"next\":\"inc\",\"inputs\":{},"
            "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},"
            "\"inc\":" +
            block("data_changevariableby", nullptr,
                  "{\"VALUE\":[1,[4,\"1\"]]}",
                  "{\"VARIABLE\":[\"v\",\"v\"]}") + "}";
        std::string json = "{\"targets\":[" + target("Stage", "{}", true);
        for (int i = 0; i < 32; ++i) {
            json += "," + target("Receiver" + std::to_string(i),
                                  receiver, false, i + 1);
        }
        json += "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
        const auto loaded = loadSb3(rt, json, nullptr);
        check(loaded.ok, "load non-restarting broadcast fanout fixture");
        if (!loaded.ok) continue;

        // Override the registered hat metadata to exercise the non-restart
        // path (normal Scratch broadcast hats restart existing scripts).
        rt.registerHat("event_whenbroadcastreceived", HatMeta{false, false});
        const auto receivers = rt.broadcastReceivers(u"PING");
        check(receivers && receivers->size() == 32,
              "all 32 broadcast receiver subscriptions are indexed");
        if (!receivers || receivers->size() != 32) continue;

        for (size_t i = 0; i < 20; ++i) {
            const auto& receiverInfo = (*receivers)[i];
            rt.pushThread(receiverInfo.blockId, receiverInfo.target,
                          false, false, receiverInfo.top);
        }
        check(rt.threads.size() == 20, "twenty receivers are initially running");

        if (route == 1) {
            rt.startBroadcast(u"PING");
        } else if (route == 2) {
            const auto started = rt.startBroadcastAndWait(u"PING");
            check(started.size() == 12,
                  "indexed broadcast-and-wait returns only missing receiver handles");
        } else {
            std::map<std::string, std::u16string> fields{
                {"BROADCAST_OPTION", u"PING"}};
            const auto started =
                rt.startHats("event_whenbroadcastreceived", &fields, nullptr);
            check(started.size() == 12,
                  "generic startHats starts only missing receivers");
        }
        check(rt.threads.size() == 32,
              "non-restart fanout starts every missing receiver");
        // Repeating the broadcast while all are still active must not
        // produce additional copies of any of the 32 receiver threads.
        if (route == 1) rt.startBroadcast(u"PING");
        else if (route == 2) {
            const auto started = rt.startBroadcastAndWait(u"PING");
            check(started.empty(),
                  "indexed broadcast-and-wait preserves active receiver identity");
        } else {
            std::map<std::string, std::u16string> fields{
                {"BROADCAST_OPTION", u"PING"}};
            rt.startHats("event_whenbroadcastreceived", &fields, nullptr);
        }
        check(rt.threads.size() == 32,
              "non-restart fanout never duplicates active receivers");

        rt.runUntilDone(200);
        bool exact = rt.threads.empty();
        for (Target* t : rt.targets) {
            if (t->isStage) continue;
            Variable* v = t->lookupVariableById("v");
            exact &= v && cast::toNumber(v->value) == 1.0;
        }
        check(exact, "each non-restarting receiver runs exactly once");
    }
}

void testBroadcastAsciiNamespaceGuard() {
    Runtime rt;
    const auto loaded = loadSb3(rt, project(false, false), nullptr);
    check(loaded.ok, "load broadcast ASCII namespace fixture");
    if (!loaded.ok) return;
    Target* stage = rt.getTargetForStage();
    check(stage != nullptr, "stage exists for namespace guard");
    if (!stage) return;

    check(rt.stageBroadcastNamesAsciiOnly(), "ordinary stage has ASCII namespace");
    stage->variables.emplace("bcast-k",
        Variable::create("bcast-k", u"k", Variable::kBroadcastType, false));
    rt.invalidateBroadcastIndex();
    check(rt.stageBroadcastNamesAsciiOnly(),
          "ASCII broadcast name remains directly indexable");

    // JS lowercasing treats the Kelvin sign as 'k', while uppercasing
    // does not necessarily produce the same identifier. Preserve the
    // legacy Stage-variable canonicalization whenever Unicode is present.
    stage->variables.emplace("bcast-kelvin",
        Variable::create("bcast-kelvin", u"\u212a",
                         Variable::kBroadcastType, false));
    rt.invalidateBroadcastIndex();
    check(!rt.stageBroadcastNamesAsciiOnly(),
          "Unicode Stage name disables direct ASCII dispatch");
    check(!rt.stageBroadcastNamesAsciiOnly(),
          "Unicode result stays cached for this scheduler tick");

    stage->variables.erase("bcast-kelvin");
    ++rt.stepSerial;
    check(rt.stageBroadcastNamesAsciiOnly(),
          "next scheduler tick invalidates Unicode namespace cache");

    stage->collidingBroadcasts.emplace("collision",
        Variable::create("collision", u"\u212a",
                         Variable::kBroadcastType, false));
    rt.invalidateBroadcastIndex();
    check(!rt.stageBroadcastNamesAsciiOnly(),
          "colliding Unicode broadcast also disables direct dispatch");
}

void testSingleVariableWaitDependency() {
    // Explicitly exercise the opt-in snapshot even when this test suite
    // does not enable the event-driven scheduler for other tests.
    Runtime rt;
    Sprite sprite(&rt);
    Target target(&rt, &sprite);
    target.variables.emplace("gate", Variable::create(
        "gate", u"gate", Variable::kScalarType, false));

    Block wait;
    wait.id = "wait";
    wait.opcode = "control_wait_until";
    wait.inputs.set("CONDITION", BlockInput{"eq", ""});
    sprite.blocks->createBlock(std::move(wait));
    Block eq;
    eq.id = "eq";
    eq.opcode = "operator_equals";
    eq.inputs.set("OPERAND1", BlockInput{"read", ""});
    eq.inputs.set("OPERAND2", BlockInput{"literal", ""});
    sprite.blocks->createBlock(std::move(eq));
    Block read;
    read.id = "read";
    read.opcode = "data_variable";
    read.fields["VARIABLE"] = BlockField{
        Value::string(SStr::create(u"gate")), "gate"};
    sprite.blocks->createBlock(std::move(read));
    Block literal;
    literal.id = "literal";
    literal.opcode = "math_number";
    literal.fields["NUM"] = BlockField{Value::num(1), ""};
    sprite.blocks->createBlock(std::move(literal));

    Thread thread("wait");
    thread.target = &target;
    thread.blockContainer = sprite.blocks.get();
    const uintptr_t site = reinterpret_cast<uintptr_t>(
        sprite.blocks->getBlock("wait"));
    snapshotEventWaitDependency(thread, site);
    if (!eventWaitDependencyEnabled()) {
        check(!thread.eventWatchValid,
              "dependency watchers opt in only when explicitly enabled");
        return;
    }

    check(thread.eventWatchValid,
          "one-variable pure wait predicate is dependency-watchable");
    check(canSkipEventWaitByDependency(thread),
          "unchanged scalar value may suppress pure wait resumption");

    Variable* gate = target.lookupVariableById("gate");
    gate->value = Value::num(1);
    check(!canSkipEventWaitByDependency(thread),
          "changed scalar value wakes the suspended condition");
    gate->value = Value::num(0);
    snapshotEventWaitDependency(thread, site);
    check(canSkipEventWaitByDependency(thread),
          "rearming records the new scalar snapshot");

    target.variables.erase("gate");
    check(!canSkipEventWaitByDependency(thread),
          "deleted variable invalidates pointers before dereference");
    target.variables.emplace("gate", Variable::create(
        "gate", u"gate", Variable::kScalarType, false));
    snapshotEventWaitDependency(thread, site);
    check(canSkipEventWaitByDependency(thread),
          "variable rebound after mutation yields a new safe snapshot");

    sprite.blocks->resetCache();
    check(!canSkipEventWaitByDependency(thread),
          "edited source revision forces condition reevaluation");
    snapshotEventWaitDependency(thread, site);
    check(canSkipEventWaitByDependency(thread),
          "fresh source revision permits safe dependency watching");

    snapshotEventWaitDependency(thread, uintptr_t(0x1234));
    check(!thread.eventWatchValid,
          "unknown AOT block tokens never become watchable pointers");

    // Time/random/other nondeterministic reporters must never be cached.
    auto* expr = sprite.blocks->getBlock("eq");
    expr->opcode = "sensing_timer";
    sprite.blocks->resetCache();
    snapshotEventWaitDependency(thread, site);
    check(!thread.eventWatchValid,
          "time-dependent predicates cannot suppress wait reevaluation");
}

void testMultipleVariableWaitDependency() {
    Runtime rt;
    Sprite sprite(&rt);
    Target target(&rt, &sprite);
    for (const char* id : {"first", "second"})
        target.variables.emplace(id, Variable::create(
            id, utf8ToU16(id), Variable::kScalarType, false));

    Block wait;
    wait.id = "wait";
    wait.opcode = "control_wait_until";
    wait.inputs.set("CONDITION", BlockInput{"eq", ""});
    sprite.blocks->createBlock(std::move(wait));
    Block eq;
    eq.id = "eq";
    eq.opcode = "operator_equals";
    eq.inputs.set("OPERAND1", BlockInput{"readFirst", ""});
    eq.inputs.set("OPERAND2", BlockInput{"readSecond", ""});
    sprite.blocks->createBlock(std::move(eq));
    for (const auto& [blockId, varId] : {
             std::pair<const char*,const char*>{"readFirst", "first"},
             {"readSecond", "second"}}) {
        Block block;
        block.id = blockId;
        block.opcode = "data_variable";
        block.fields["VARIABLE"] = BlockField{
            Value::string(SStr::fromUtf8(varId)), varId};
        sprite.blocks->createBlock(std::move(block));
    }
    Thread thread("wait");
    thread.target = &target;
    thread.blockContainer = sprite.blocks.get();
    const uintptr_t site = reinterpret_cast<uintptr_t>(
        sprite.blocks->getBlock("wait"));
    snapshotEventWaitDependency(thread, site);
    if (!eventWaitDependencyEnabled() || !multiVariableEventWaitEnabled()) {
        check(!thread.eventWatchValid,
              "two-variable wait keeps conservative fallback when opt-out");
        return;
    }
    check(thread.eventWatchValid && thread.eventWatchAdditional &&
          thread.eventWatchAdditional->count == 1,
          "two distinct scalar variables are captured without aliasing");
    check(canSkipEventWaitByDependency(thread),
          "unchanged two-variable wait safely skips reevaluation");
    Variable* a = target.lookupVariableById("first");
    Variable* b = target.lookupVariableById("second");
    a->value = Value::num(17);
    check(!canSkipEventWaitByDependency(thread),
          "first variable mutation wakes two-variable condition");
    snapshotEventWaitDependency(thread, site);
    check(canSkipEventWaitByDependency(thread),
          "rearming remembers first variable mutation");
    b->value = Value::string(SStr::create(u"17"));
    check(!canSkipEventWaitByDependency(thread),
          "second variable string-type mutation wakes condition");
    snapshotEventWaitDependency(thread, site);
    check(canSkipEventWaitByDependency(thread),
          "watch snapshots all scalar Value bits, including strings");
    target.variables.erase("second");
    check(!canSkipEventWaitByDependency(thread),
          "binding deletion invalidates watched pointers before access");
    target.variables.emplace("second", Variable::create(
        "second", u"second", Variable::kScalarType, false));
    snapshotEventWaitDependency(thread, site);
    check(canSkipEventWaitByDependency(thread),
          "rebound second variable permits a fresh safe snapshot");
    sprite.blocks->resetCache();
    check(!canSkipEventWaitByDependency(thread),
          "source revision mismatch invalidates multi-scalar dependency");
    snapshotEventWaitDependency(thread, uintptr_t(0x1234));
    check(!thread.eventWatchValid,
          "unknown native/AOT site never validates multi-scalar pointers");
    thread.releaseFinished();
    check(!thread.eventWatchAdditional,
          "finished thread releases optional multi-scalar snapshot storage");
}

void testFractionalLayerMovement() {
    Runtime rt;
    const auto loaded = loadSb3(rt, project(false, false), nullptr);
    check(loaded.ok, "load layer movement fixture");
    if (!loaded.ok) return;
    // The executable list is ordered back-to-front, with stage at index 0.
    Target* upper = rt.executableTargets.back();
    const size_t original = rt.executableTargets.size() - 1;
    rt.moveExecutable(upper, -1.5);
    check(rt.executableTargets[original - 2] == upper,
          "backward 1.5 truncates the final index, moving two layers");
    rt.moveExecutable(upper, 0.5);
    check(rt.executableTargets[original - 2] == upper,
          "forward 0.5 does not move an integral-index target");
    rt.moveExecutable(upper, 1.5);
    check(rt.executableTargets[original - 1] == upper,
          "forward 1.5 moves one layer");
    rt.moveExecutable(upper, std::numeric_limits<double>::infinity());
    check(rt.executableTargets.back() == upper,
          "positive infinity moves to front safely");
    rt.moveExecutable(upper, -std::numeric_limits<double>::infinity());
    check(rt.executableTargets[1] == upper,
          "negative infinity keeps sprite behind others but after stage");
    // setExecutablePosition on a last element is the common clone-insertion
    // case. Both optimized pop_back and legacy find/erase must preserve
    // stage-first ordering, vector size, and general non-last moves.
    Target* stage = rt.executableTargets.front();
    const size_t targetCount = rt.executableTargets.size();
    rt.setExecutablePosition(upper, INT_MAX);
    check(rt.executableTargets.back() == upper,
          "last-element positioning moves chosen sprite to the end");
    rt.setExecutablePosition(upper, 0);
    check(rt.executableTargets.front() == stage &&
          rt.executableTargets[1] == upper,
          "last-element insertion clamps behind stage at index 1");
    Target* nonLast = rt.executableTargets[2];
    rt.setExecutablePosition(nonLast, INT_MAX);
    check(rt.executableTargets.back() == nonLast,
          "non-last target still reorders through the standard path");
    check(rt.executableTargets.size() == targetCount &&
          std::count(rt.executableTargets.begin(),
                     rt.executableTargets.end(), upper) == 1 &&
          std::count(rt.executableTargets.begin(),
                     rt.executableTargets.end(), nonLast) == 1,
          "reordering does not duplicate or lose targets");
}

void testPool() {
    WorkerPool pool;
    struct Context {
        std::atomic<unsigned> count{0};
        unsigned result = 0;
        unsigned expected = 0;
    };
    Context contexts[8];
    WorkerPool::Task tasks[8];
    for (size_t i = 0; i < 8; ++i) {
        tasks[i] = {[](void* raw) {
            auto& c = *static_cast<Context*>(raw);
            ++c.count;
            c.result = c.expected;
        }, &contexts[i]};
    }
    bool exact = true, visible = true;
    const size_t sizes[] = {0, 1, 2, 8};
    for (unsigned round = 0; round < 1000; ++round) {
        const size_t n = sizes[round % 4];
        for (size_t i = 0; i < 8; ++i) {
            contexts[i].count = 0;
            contexts[i].result = 0;
            contexts[i].expected = round * 8 + unsigned(i) + 1;
        }
        pool.run(tasks, n);
        for (size_t i = 0; i < 8; ++i) {
            exact &= contexts[i].count == (i < n ? 1u : 0u);
            visible &= contexts[i].result == (i < n ? contexts[i].expected : 0u);
        }
    }
    check(exact, "1000 varying pool batches execute every task exactly once");
    check(visible, "pool completion synchronizes non-atomic task results");
}
} // namespace

int test_scheduler_main() {
    testPool();
    testThreadMembership();
    testThreadReclamation();
    testRecycledThreadStorage();
    testFrameAddressStability();
    testWaitUntilChain();
    testFractionalLayerMovement();
    testCompute(false, false);
    testCompute(true, false);
    testCompute(false, true);
    testSharedOrder();
    testSharedYieldChain();
    testBroadcastYieldChain();
    testBroadcastReceiverReuse();
    testShortBroadcastReadyCache();
    testNonRestartBroadcastFanout();
    testCloneMutationSchedulerAffinity();
    testBroadcastAsciiNamespaceGuard();
    testSingleVariableWaitDependency();
    testMultipleVariableWaitDependency();
    testGraphicEffectsStorage();
    testBroadcastIndexInvalidation();
    testSelfBroadcastDoesNotRecycleActiveFiber();
    testCompatBarrier();
    std::printf("scheduler: %d checks, %d failures\n", checks, failures);
    return failures;
}
