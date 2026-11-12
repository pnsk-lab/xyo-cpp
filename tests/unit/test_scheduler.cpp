// Headless/mock-renderer scheduler integration and dispatch-and-join stress.
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

#if defined(XYO_JIT)
extern "C" void xyo_wait_notify_var_store(xyo::JitCtx*, xyo::Variable*);
#endif

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

void testSchedulerStatusTransitions() {
    Runtime rt;
    Thread waiting("test-status-transition");
    const uint64_t initial = rt.schedulerMutationEpoch;
    rt.transitionThread(waiting, STATUS_PROMISE_WAIT);
    check(waiting.status == STATUS_PROMISE_WAIT &&
          rt.schedulerMutationEpoch == initial + 1,
          "promise suspension publishes ready-set invalidation");
    rt.transitionThread(waiting, STATUS_PROMISE_WAIT);
    check(rt.schedulerMutationEpoch == initial + 1,
          "unchanged scheduler status does not invalidate the index");
    rt.transitionThread(waiting, STATUS_RUNNING);
    check(waiting.status == STATUS_RUNNING &&
          rt.schedulerMutationEpoch == initial + 2,
          "promise resolution republishes runnable membership");
    rt.transitionThread(waiting, STATUS_DONE);
    check(waiting.status == STATUS_DONE &&
          rt.schedulerMutationEpoch == initial + 3,
          "thread retirement is a visible scheduler transition");
}

void testConcurrentReadyInvalidation() {
    Runtime rt;
    constexpr int workers = 4;
    constexpr int changesPerWorker = 2000;
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&rt] {
            for (int n = 0; n < changesPerWorker; ++n)
                rt.invalidateSchedulerReadySet();
        });
    }
    for (auto& worker : threads) worker.join();
    check(rt.schedulerMutationEpoch.load(std::memory_order_relaxed) ==
          uint64_t(workers * changesPerWorker),
          "concurrent worker invalidations retain every mutation");
}

void testReadyChangePublication() {
    Runtime rt;
    Thread thread("test-ready-change-publication");
    rt.transitionThread(thread, STATUS_PROMISE_WAIT);
    rt.transitionThread(thread, STATUS_RUNNING);
    auto changes = rt.takeSchedulerStatusChanges();
    const char* index = std::getenv("XYO_SCHED_READY_INDEX");
    const bool indexed = index && index[0] == '1';
    check(indexed ? changes.size() == 2 &&
                    changes[0] == &thread && changes[1] == &thread
                  : changes.empty(),
          "status-change journal is opt-in and retains thread identity");
    check(rt.takeSchedulerStatusChanges().empty(),
          "drained status-change journal contains no stale entries");
    const auto before = rt.schedulerStructureEpoch.load(
        std::memory_order_relaxed);
    rt.invalidateSchedulerReadySet();
    check(rt.schedulerStructureEpoch.load(std::memory_order_relaxed) ==
          before + 1, "structural invalidation forces full index rebuild");
}

// An actual native Scratch script, rather than fabricated Thread counters:
// a warp procedure repeats a wait-until at one source site twice. The first
// wakeup changes v from 0 to -1 (still false); the next changes v to 1,
// allowing the procedure to complete the wait and rearm at the same site.
// Build one reproducible Scratch project for both manual and autonomous
// JIT writer integration checks; the original manual fixture stays intact.
std::string nativeWaitResumeProject(bool autonomousWriter) {
    const std::string mut =
        R"({"tagName":"mutation","children":[],"proccode":"pulse",)"
        R"("argumentids":"[]","argumentnames":"[]",)"
        R"("argumentdefaults":"[]","warp":"true"})";
    std::string blocks =
        "{\"hat\":" + hat("call") +
        ",\"call\":{\"opcode\":\"procedures_call\",\"next\":null,"
            "\"inputs\":{},\"fields\":{},\"mutation\":" + mut + "}" +
        ",\"def\":{\"opcode\":\"procedures_definition\","
            "\"topLevel\":true,\"next\":\"loop\","
            "\"inputs\":{\"custom_block\":[1,\"proto\"]},\"fields\":{}}" +
        ",\"proto\":{\"opcode\":\"procedures_prototype\","
            "\"next\":null,\"inputs\":{},\"fields\":{},\"mutation\":" +
            mut + "}" +
        ",\"loop\":" + block("control_repeat", nullptr,
            R"({"TIMES":[1,[4,"2"]],"SUBSTACK":[2,"wait"]})") +
        ",\"wait\":" + block("control_wait_until", "dec",
            R"({"CONDITION":[2,"gt"]})") +
        ",\"gt\":" + block("operator_gt", nullptr,
            R"({"OPERAND1":[2,"vref"],"OPERAND2":[1,[4,"0"]]})") +
        ",\"vref\":" + block("data_variable", nullptr, "{}",
            R"({"VARIABLE":["v","v"]})") +
        ",\"dec\":" + block("data_changevariableby", "doneinc",
            R"({"VALUE":[1,[4,"-1"]]})", R"({"VARIABLE":["v","v"]})") +
        ",\"doneinc\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})",
            R"({"VARIABLE":["done","done"]})") +
        // Independent broadcast receiver scripts are the *only* writers
        // for the watched variable after the initial green flag.
        ",\"negativeHat\":{\"opcode\":\"event_whenbroadcastreceived\","
          "\"topLevel\":true,\"next\":\"negativeSet\","
          "\"inputs\":{},\"fields\":{\"BROADCAST_OPTION\":[\"NEG\",null]}}" +
        ",\"negativeSet\":" + block("data_setvariableto", nullptr,
            R"({"VALUE":[1,[4,"-1"]]})", R"({"VARIABLE":["v","v"]})") +
        ",\"positiveHat\":{\"opcode\":\"event_whenbroadcastreceived\","
          "\"topLevel\":true,\"next\":\"positiveSet\","
          "\"inputs\":{},\"fields\":{\"BROADCAST_OPTION\":[\"POS\",null]}}" +
        ",\"positiveSet\":" + block("data_setvariableto", nullptr,
            R"({"VALUE":[1,[4,"1"]]})", R"({"VARIABLE":["v","v"]})") +
        "}";
    if (autonomousWriter) {
        // Both flag hats are dispatched by Runtime::greenFlag().
        // Timed Scratch waits give the main warp watcher an opportunity to
        // arm before the independent writer changes its variable.
        blocks.pop_back(); // reopen the JSON blocks object
        blocks +=
            ",\"autoWriterHat\":" + hat("autoPause1") +
            ",\"autoPause1\":" +
                block("control_wait", "autoNegative",
                      R"({"DURATION":[1,[4,"0.02"]]})") +
            ",\"autoNegative\":" +
                block("data_setvariableto", "autoPause2",
                      R"({"VALUE":[1,[4,"-1"]]})",
                      R"({"VARIABLE":["v","v"]})") +
            ",\"autoPause2\":" +
                block("control_wait", "autoPositive1",
                      R"({"DURATION":[1,[4,"0.02"]]})") +
            ",\"autoPositive1\":" +
                block("data_setvariableto", "autoPause3",
                      R"({"VALUE":[1,[4,"1"]]})",
                      R"({"VARIABLE":["v","v"]})") +
            ",\"autoPause3\":" +
                block("control_wait", "autoPositive2",
                      R"({"DURATION":[1,[4,"0.02"]]})") +
            ",\"autoPositive2\":" +
                block("data_setvariableto", nullptr,
                      R"({"VALUE":[1,[4,"1"]]})",
                      R"({"VARIABLE":["v","v"]})") + "}";
    }
    std::string sprite = target("WaitPulse", blocks, false, 1);
    const std::string original = R"("variables":{"v":["v",0]})";
    const auto pos = sprite.find(original);
    check(pos != std::string::npos,
          "real wait-resume fixture has expected variable layout");
    if (pos == std::string::npos) return {};
    sprite.replace(pos, original.size(),
                   R"("variables":{"v":["v",0],"done":["done",0]})");
    const std::string json = "{\"targets\":[" +
        target("Stage", "{}", true) + "," + sprite +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";

    return json;
}

void testNativeWaitResumeOutcomes() {
    const char* audit = std::getenv("XYO_WAIT_NOTIFY_AUDIT");
    const char* registry = std::getenv("XYO_WAIT_WAKE_REGISTRY");
    if (!jitEnabled() || !audit || audit[0] != '1' ||
        !registry || registry[0] != '1' ||
        !isolatedEventWaitDependencyEnabled()) return;

    const std::string json = nativeWaitResumeProject(false);

    Runtime rt;
    check(loadSb3(rt, json, nullptr).ok,
          "load real native repeat/wait-until Scratch project");
    Target* runner = rt.getSpriteTargetByName(u"WaitPulse");
    Variable* gate = runner ? runner->lookupVariableById("v") : nullptr;
    Variable* done = runner ? runner->lookupVariableById("done") : nullptr;
    if (!gate || !done) {
        check(false, "resolve real native wait variables");
        return;
    }
    rt.greenFlag();
    for (int i = 0; i < 3 && !rt.threads.empty() &&
                    !rt.threads.front()->eventWaitArmed; ++i)
        rt.step();
    check(!rt.threads.empty() && rt.threads.front()->eventWaitArmed,
          "warp procedure reaches a native wait with a false condition");
    if (rt.threads.empty() || !rt.threads.front()->eventWaitArmed) return;
    check(rt.threads.front()->isCompiled,
          "warp wait fixture actually executes the native compiled path");
    const uintptr_t waitSite = rt.threads.front()->eventWaitSite;
    const uint64_t exitBefore = rt.threads.front()->eventWaitExitCount;

    // Trigger an actual separate Scratch event script. Execute it through
    // the real native Sequencer::stepThread path so the notification comes
    // from generated LLVM code, not the test harness.
    const auto runWriter = [&](const std::u16string& message, double expected) {
        const std::map<std::string, std::u16string> fields{
            {"BROADCAST_OPTION", message}};
        const auto writers = rt.startHats(
            "event_whenbroadcastreceived", &fields, runner);
        check(writers.size() == 1,
              "broadcast starts exactly one independent Scratch writer");
        if (writers.size() != 1) return false;
        Thread* writer = writers.front();
        check(writer != rt.threads.front(),
              "writer is a separate Scratch script from the waiting fiber");
        rt.sequencer->stepThread(*writer);
        check(writer->isCompiled && writer->compiled,
              "Scratch writer executed as a native JIT script");
        check(cast::toNumber(gate->value) == expected,
              "Scratch writer published the expected scalar value");
        check(rt.waitNotificationPending(*rt.threads.front()),
              "native LLVM write notified the registered wait dependency");
        return writer->isCompiled && writer->compiled &&
            cast::toNumber(gate->value) == expected &&
            rt.waitNotificationPending(*rt.threads.front());
    };
    if (!runWriter(u"NEG", -1)) return;
    rt.step();
    const auto afterFalse = rt.waitResumeAuditSnapshot();
    const size_t same = static_cast<size_t>(WaitResumeOutcome::RewaitSame);
    check(afterFalse.notified[same] > 0,
          "real native wait remains false after a notified variable write");
    check(!rt.threads.empty() && rt.threads.front()->eventWaitArmed &&
          rt.threads.front()->eventWaitSite == waitSite &&
          rt.threads.front()->eventWaitExitCount == exitBefore,
          "false reevaluation preserves wait identity and completion counter");

    if (!runWriter(u"POS", 1)) return;
    rt.step();
    const auto afterTrue = rt.waitResumeAuditSnapshot();
    const size_t rearmed =
        static_cast<size_t>(WaitResumeOutcome::ExitedRearmed);
    check(afterTrue.notified[rearmed] > 0,
          "real native wait completes and rearms at the same source site");
    check(!rt.threads.empty() && rt.threads.front()->eventWaitArmed &&
          rt.threads.front()->eventWaitSite == waitSite &&
          rt.threads.front()->eventWaitExitCount > exitBefore,
          "same-source reentry is distinguished by native exit counter");

    if (!runWriter(u"POS", 1)) return;
    rt.runUntilDone(100);
    check(rt.threads.empty() && cast::toNumber(done->value) == 2,
          "real native wait loop completes twice with exact Scratch state");
}

// Fully autonomous two-script Scratch scenario: the green flag starts both
// scripts and Runtime::step() dispatches *all* execution. No host writes,
// manually selected steps, or manually triggered broadcasts.
void testAutonomousNativeWaitWriter() {
    const char* audit = std::getenv("XYO_WAIT_NOTIFY_AUDIT");
    const char* registry = std::getenv("XYO_WAIT_WAKE_REGISTRY");
    if (!jitEnabled() || !audit || audit[0] != '1' ||
        !registry || registry[0] != '1' ||
        !isolatedEventWaitDependencyEnabled()) return;

    Runtime rt;
    const std::string projectJson = nativeWaitResumeProject(true);
    const auto loaded = loadSb3(rt, projectJson, nullptr);
    check(loaded.ok,
          "autonomous two-green-flag Scratch project loads");
    if (!loaded.ok) return;
    Target* runner = rt.getSpriteTargetByName(u"WaitPulse");
    Variable* gate = runner ? runner->lookupVariableById("v") : nullptr;
    Variable* done = runner ? runner->lookupVariableById("done") : nullptr;
    check(gate && done, "autonomous native wait variables exist");
    if (!gate || !done) return;

    rt.greenFlag();
    const Thread* reader = nullptr;
    const Thread* writer = nullptr;
    for (const Thread* t : rt.threads) {
        if (t->topBlock == "hat") reader = t;
        if (t->topBlock == "autoWriterHat") writer = t;
    }
    check(rt.threads.size() == 2 && reader && writer && reader != writer,
          "green flag starts two independent native Scratch scripts");
    if (!reader || !writer) return;
    check(reader->isCompiled && reader->compiled &&
          writer->isCompiled && writer->compiled,
          "both Scratch programs compiled to native JIT before dispatch");

    // The writer uses Scratch wait blocks to create two interleavings:
    // v=-1 -> false again; v=1 -> complete/rearm; v=1 -> complete.
    // No direct Sequencer::stepThread calls or host variable mutations.
    const int steps = rt.runUntilDone(300);
    const auto outcomes = rt.waitResumeAuditSnapshot();
    const auto writes = rt.waitNotifyAuditSnapshot();
    const auto index = [](WaitResumeOutcome outcome) {
        return static_cast<size_t>(outcome);
    };
    check(steps > 0 && steps < 300 && rt.threads.empty(),
          "ordinary Runtime::step scheduler drains autonomous scripts");
    check(cast::toNumber(done->value) == 2 &&
          cast::toNumber(gate->value) == 0,
          "autonomous Scratch writer and waiter produce exact final state");
    check(outcomes.notified[index(WaitResumeOutcome::RewaitSame)] > 0,
          "autonomous native writer produces notified false reevaluation");
    check(outcomes.notified[index(WaitResumeOutcome::ExitedRearmed)] > 0,
          "autonomous native writer wakes a loop which rearms at same site");
    check(outcomes.notified[index(WaitResumeOutcome::Exited)] > 0,
          "autonomous native writer wakes the final wait completion");
    check(writes.coveredChanges >= 3 && writes.missedChanges == 0,
          "autonomous JIT variable stores match all observed changed waits");
}

// Fan-out stress of independent native waiters and writers. Each writer
// publishes to a separate Stage scalar, and multiple Scratch sprites wait
// on that same binding. No host variable mutation or manual thread stepping.
void testNativeWaitFanoutStress() {
    const char* audit = std::getenv("XYO_WAIT_NOTIFY_AUDIT");
    const char* registry = std::getenv("XYO_WAIT_WAKE_REGISTRY");
    if (!jitEnabled()) return;
    const bool auditEnabled = audit && audit[0] == '1' &&
        registry && registry[0] == '1' &&
        isolatedEventWaitDependencyEnabled();

    constexpr int kGroups = 4;
    constexpr int kWaitersPerGroup = 6;
    constexpr int kTotalWaiters = kGroups * kWaitersPerGroup;

    const auto gateId = [](int group) {
        return group == 0 ? std::string("s")
                          : "g" + std::to_string(group);
    };
    std::string stageVars = R"({"s":["s",0])";
    std::string stageBlocks = "{";
    for (int group = 0; group < kGroups; ++group) {
        const std::string gate = gateId(group);
        if (group) {
            stageVars += ",\"" + gate + "\":[\"" + gate + "\",0]";
            stageBlocks += ",";
        }
        const std::string id = std::to_string(group);
        const std::string pause1 = "pauseA" + id;
        const std::string minus = "minus" + id;
        const std::string pause2 = "pauseB" + id;
        const std::string plus = "plus" + id;
        const std::string hatId = "writer" + id;
        stageBlocks += "\"" + hatId + "\":" + hat(pause1.c_str()) +
            ",\"" + pause1 + "\":" +
                block("control_wait", minus.c_str(),
                      R"({"DURATION":[1,[4,"0.02"]]})") +
            ",\"" + minus + "\":" +
                block("data_setvariableto", pause2.c_str(),
                      R"({"VALUE":[1,[4,"-1"]]})",
                      "{\"VARIABLE\":[\"" + gate + "\",\"" + gate + "\"]}") +
            ",\"" + pause2 + "\":" +
                block("control_wait", plus.c_str(),
                      R"({"DURATION":[1,[4,"0.02"]]})") +
            ",\"" + plus + "\":" +
                block("data_setvariableto", nullptr,
                      R"({"VALUE":[1,[4,"1"]]})",
                      "{\"VARIABLE\":[\"" + gate + "\",\"" + gate + "\"]}");
    }
    stageVars += "}";
    stageBlocks += "}";
    std::string stage = target("Stage", stageBlocks, true);
    const std::string oldVars = R"("variables":{"s":["s",""]})";
    const auto varsPos = stage.find(oldVars);
    check(varsPos != std::string::npos,
          "fanout stress Stage exposes its scalar variable layout");
    if (varsPos == std::string::npos) return;
    stage.replace(varsPos, oldVars.size(), "\"variables\":" + stageVars);

    std::string projectJson = "{\"targets\":[" + stage;
    for (int group = 0; group < kGroups; ++group) {
        const std::string gate = gateId(group);
        const std::string fields =
            "{\"VARIABLE\":[\"" + gate + "\",\"" + gate + "\"]}";
        const std::string blocks =
            "{\"hat\":" + hat("wait") +
            ",\"wait\":" +
                block("control_wait_until", "finish",
                      R"({"CONDITION":[2,"positive"]})") +
            ",\"positive\":" +
                block("operator_gt", nullptr,
                      R"({"OPERAND1":[2,"stageValue"],"OPERAND2":[1,[4,"0"]]})") +
            ",\"stageValue\":" +
                block("data_variable", nullptr, "{}", fields) +
            ",\"finish\":" +
                block("data_setvariableto", nullptr,
                      R"({"VALUE":[1,[4,"1"]]})",
                      R"({"VARIABLE":["v","v"]})") + "}";
        for (int index = 0; index < kWaitersPerGroup; ++index) {
            projectJson += "," + target(
                "Waiter" + std::to_string(group) + "_" +
                    std::to_string(index), blocks, false,
                group * kWaitersPerGroup + index + 1);
        }
    }
    projectJson += R"(],"monitors":[],"extensions":[],"meta":{}})";

    Runtime rt;
    const auto loaded = loadSb3(rt, projectJson, nullptr);
    check(loaded.ok,
          "native many-waiter many-writer Scratch project loads");
    if (!loaded.ok) return;
    rt.greenFlag();
    check(rt.threads.size() == kTotalWaiters + kGroups,
          "green flag starts all independent writers and waiters");
    bool allCompiled = true;
    for (const Thread* t : rt.threads)
        allCompiled &= t && t->isCompiled && t->compiled;
    check(allCompiled, "all fanout writer and waiter scripts compile to LLVM");

    // Both sides are run only by the ordinary scheduler.
    const int steps = rt.runUntilDone(500);
    check(steps > 0 && steps < 500 && rt.threads.empty(),
          "native wait fanout drains without starvation or leaked threads");

    bool exactState = true;
    for (int group = 0; group < kGroups; ++group) {
        const Variable* gate = rt.getTargetForStage()->lookupVariableById(
            gateId(group));
        exactState &= gate && cast::toNumber(gate->value) == 1;
        for (int index = 0; index < kWaitersPerGroup; ++index) {
            const std::u16string name = u"Waiter" +
                std::u16string(1, char16_t('0' + group)) + u"_" +
                std::u16string(1, char16_t('0' + index));
            Target* waiter = rt.getSpriteTargetByName(name);
            const Variable* done = waiter
                ? waiter->lookupVariableById("v") : nullptr;
            exactState &= done && cast::toNumber(done->value) == 1;
        }
    }
    check(exactState,
          "every Stage scalar and every waiter has the expected final value");
    const auto changes = rt.waitNotifyAuditSnapshot();
    const auto outcomes = rt.waitResumeAuditSnapshot();
    const size_t exited = static_cast<size_t>(WaitResumeOutcome::Exited);
    if (auditEnabled) {
        check(changes.missedChanges == 0 && changes.changedUntracked == 0,
              "stress recorded no missing or untracked changed dependencies");
        check(changes.unchangedNotified == 0,
              "unrelated Stage writes do not notify unchanged wait dependencies");
        check(changes.coveredChanges >= kTotalWaiters,
              "native writers notify at least one changed snapshot per waiter");
        check(outcomes.notified[exited] == kTotalWaiters,
              "each native waiting script has exactly one notified completion");
        check(outcomes.quiet[exited] == 0,
              "no native waiter completes without a corresponding notification");
    }
    const char* sleepFlag = std::getenv("XYO_WAIT_READY_SLEEP");
    const char* readyFlag = std::getenv("XYO_SCHED_READY_INDEX");
    if (sleepFlag && sleepFlag[0] == '1' &&
        readyFlag && readyFlag[0] == '1' &&
        registry && registry[0] == '1') {
        const auto& sparse = rt.sequencer->waitReadyStats();
        check(sparse.deferredSlots >= kTotalWaiters,
              "wait-ready index defers all unchanged native waiters");
        check(sparse.sparsePasses > 0 && sparse.frameRechecks > 0,
              "wait-ready index dispatches sparsely and periodically rechecks");
        if (const char* stats = std::getenv("XYO_WAIT_READY_STATS");
            stats && stats[0] == '1')
            std::fprintf(stderr,
                "[wait-ready-stress] deferred=%llu sparse_passes=%llu "
                "waiter_visits=%llu full_rechecks=%llu\n",
                static_cast<unsigned long long>(sparse.deferredSlots),
                static_cast<unsigned long long>(sparse.sparsePasses),
                static_cast<unsigned long long>(sparse.scannedWaiters),
                static_cast<unsigned long long>(sparse.frameRechecks));
    }
}

// The opt-in ready index must NEVER rely solely on notifications.
// An embedder may change Variable::value directly, without publishing a
// registry wakeup. Revisit all suppressed native waits on the next frame.
void testWaitReadyUnnotifiedFallback() {
    const char* sleep = std::getenv("XYO_WAIT_READY_SLEEP");
    const char* registry = std::getenv("XYO_WAIT_WAKE_REGISTRY");
    const char* index = std::getenv("XYO_SCHED_READY_INDEX");
    const char* audit = std::getenv("XYO_WAIT_NOTIFY_AUDIT");
    if (!jitEnabled() || !isolatedEventWaitDependencyEnabled() ||
        !sleep || sleep[0] != '1' ||
        !registry || registry[0] != '1' ||
        !index || index[0] != '1' ||
        !audit || audit[0] != '1') return;

    constexpr int kWaiters = 16;
    const std::string blocks =
        "{\"hat\":" + hat("wait") +
        ",\"wait\":" + block("control_wait_until", "finish",
            R"({"CONDITION":[2,"positive"]})") +
        ",\"positive\":" + block("operator_gt", nullptr,
            R"({"OPERAND1":[2,"value"],"OPERAND2":[1,[4,"0"]]})") +
        ",\"value\":" + block("data_variable", nullptr, "{}",
            R"({"VARIABLE":["s","s"]})") +
        ",\"finish\":" + block("data_setvariableto", nullptr,
            R"({"VALUE":[1,[4,"1"]]})",
            R"({"VARIABLE":["v","v"]})") + "}";
    std::string projectJson = "{\"targets\":[" +
        target("Stage", "{}", true);
    for (int i = 0; i < kWaiters; ++i)
        projectJson += "," + target(
            "Fallback" + std::to_string(i), blocks, false, i + 1);
    projectJson += R"(],"monitors":[],"extensions":[],"meta":{}})";

    Runtime rt;
    const auto loaded = loadSb3(rt, projectJson, nullptr);
    check(loaded.ok, "unnotified write fallback Scratch project loads");
    if (!loaded.ok) return;
    Variable* gate = rt.getTargetForStage()->lookupVariableById("s");
    check(gate != nullptr, "fallback uses an existing Stage variable");
    if (!gate) return;
    gate->value = Value::num(0); // before any wait registration
    rt.greenFlag();
    check(rt.threads.size() == kWaiters,
          "fallback starts sixteen independent native waiters");
    if (rt.threads.size() != kWaiters) return;
    bool native = true;
    for (const Thread* thread : rt.threads)
        native &= thread->isCompiled && thread->compiled;
    check(native, "all fallback native waiters have compiled LLVM bodies");

    // The second pass of the first frame proves the dependencies unchanged
    // and excludes their slots. No writer exists in this project.
    rt.step();
    const auto before = rt.sequencer->waitReadyStats();
    check(before.deferredSlots >= kWaiters,
          "all quiet native waits were excluded from the ready index");

    // Explicitly bypass the JIT write callback, modeling a host/plugin
    // directly updating a public Scratch variable.
    gate->value = Value::num(1);
    check(rt.takeWaitWakeups().empty(),
          "unnotified host value update does not invent notifications");
    const int steps = rt.runUntilDone(12);
    check(steps > 0 && steps < 12 && rt.threads.empty(),
          "frame recheck resumes all native waiters after a missed notification");
    bool complete = true;
    for (int i = 0; i < kWaiters; ++i) {
        const std::string asciiName = "Fallback" + std::to_string(i);
        const std::u16string name(asciiName.begin(), asciiName.end());
        Target* target = rt.getSpriteTargetByName(name);
        const Variable* result =
            target ? target->lookupVariableById("v") : nullptr;
        complete &= result && cast::toNumber(result->value) == 1;
    }
    check(complete, "unnotified wake fallback preserves all final Scratch values");
    const auto outcomes = rt.waitResumeAuditSnapshot();
    const auto exited = static_cast<size_t>(WaitResumeOutcome::Exited);
    check(outcomes.quiet[exited] == kWaiters,
          "all unnotified wait completions were observed as quiet");
    const auto after = rt.sequencer->waitReadyStats();
    check(after.frameRechecks > before.frameRechecks &&
          after.scannedWaiters >= before.scannedWaiters + kWaiters,
          "next frame rechecks every omitted waiter after a host write");
}

void testWaitResumeOutcomeAudit() {
    const WaitResumeState before{true, 0x1234, 7};
    check(classifyWaitResume(before, {true, 0x1234, 7}) ==
              WaitResumeOutcome::RewaitSame,
          "same site and exit count means condition stayed false");
    check(classifyWaitResume(before, {false, 0x1234, 8}) ==
              WaitResumeOutcome::Exited,
          "wait completion counter records an exited condition");
    check(classifyWaitResume(before, {true, 0x1234, 8}) ==
              WaitResumeOutcome::ExitedRearmed,
          "exit then reenter at same site is not a false condition");
    check(classifyWaitResume(before, {true, 0x5678, 7}) ==
              WaitResumeOutcome::ChangedSite,
          "new wait site without exit has a distinct outcome");
    check(classifyWaitResume(before, {false, 0x1234, 7}) ==
              WaitResumeOutcome::Incomplete,
          "disarmed wait without completion is not a true predicate");
    check(classifyWaitResume(before, {true, 0x1234, 6}) ==
              WaitResumeOutcome::Incomplete,
          "reset exit counter does not claim a completed wait");

    Runtime rt;
    const WaitResumeOutcome outcomes[] = {
        WaitResumeOutcome::RewaitSame,
        WaitResumeOutcome::Exited,
        WaitResumeOutcome::ExitedRearmed,
        WaitResumeOutcome::ChangedSite,
        WaitResumeOutcome::Incomplete,
    };
    for (const auto outcome : outcomes) {
        rt.auditWaitResumeOutcome(outcome, true);
        rt.auditWaitResumeOutcome(outcome, false);
    }
    const auto result = rt.waitResumeAuditSnapshot();
    const char* env = std::getenv("XYO_WAIT_NOTIFY_AUDIT");
    const uint64_t expected = env && env[0] == '1' ? 1 : 0;
    bool countersMatch = true;
    for (size_t i = 0; i < Runtime::WaitResumeAudit::kOutcomes; ++i)
        countersMatch &= result.notified[i] == expected &&
                         result.quiet[i] == expected;
    check(countersMatch,
          "wait-resume audit tracks each outcome by notification presence");
}

void testWaitNotificationDifferentialAudit() {
    Runtime rt;
    Sprite sprite(&rt);
    Target target(&rt, &sprite);
    Variable* gate = target.createVariable(
        "audit-gate", u"audit-gate", Variable::kScalarType, false);
    gate->value = Value::num(0);

    Thread thread("audit-wait");
    thread.target = &target;
    thread.blockContainer = sprite.blocks.get();
    thread.eventWaitArmed = true;
    thread.eventWatchValid = true;
    thread.eventWatchVariable = gate;
    thread.eventWatchBits = gate->value.bits;
    thread.eventWatchSourceRevision = sprite.blocks->revision();
    const auto stamp = target.variableBindingStamp();
    thread.eventWatchStage = stamp.stage;
    thread.eventWatchLocalGeneration = stamp.localGeneration;
    thread.eventWatchStageGeneration = stamp.stageGeneration;
    rt.trackWaitDependencies(thread);

    const char* audit = std::getenv("XYO_WAIT_NOTIFY_AUDIT");
    const char* registry = std::getenv("XYO_WAIT_WAKE_REGISTRY");
    const bool enabled = audit && audit[0] == '1' &&
                         registry && registry[0] == '1';
    rt.auditWaitDependencyDecision(thread);
    if (!enabled) {
        const auto result = rt.waitNotifyAuditSnapshot();
        check(result.coveredChanges == 0 && result.missedChanges == 0 &&
              result.unchangedQuiet == 0,
              "wait notification comparison is opt-in");
        return;
    }

    rt.notifyWaitVariableWrite(*gate);
    rt.auditWaitDependencyDecision(thread); // write with no net value change
    gate->value = Value::num(1);
    rt.auditWaitDependencyDecision(thread); // covered changed snapshot

    // Simulate a rearmed wait snapshot after condition reevaluation.
    rt.trackWaitDependencies(thread);
    thread.eventWatchBits = gate->value.bits;
    rt.auditWaitDependencyDecision(thread); // quiet unchanged snapshot
    gate->value = Value::num(2);             // deliberately bypass callbacks
    rt.auditWaitDependencyDecision(thread); // missed notification

    rt.untrackWaitDependencies(thread);
    gate->value = Value::num(3);
    rt.auditWaitDependencyDecision(thread); // changed but not registered

    thread.eventWatchBits = gate->value.bits;
    rt.trackWaitDependencies(thread);
    sprite.blocks->resetCache();
    rt.auditWaitDependencyDecision(thread); // invalid source binding

    const auto result = rt.waitNotifyAuditSnapshot();
    check(result.coveredChanges == 1 &&
          result.missedChanges == 1 &&
          result.unchangedNotified == 1 &&
          result.unchangedQuiet == 2 &&
          result.changedUntracked == 1 &&
          result.invalidBindings == 1,
          "wait notification audit separates covered, missed and stale cases");
    rt.untrackWaitDependencies(thread);
}

void testSchedulerRetirementNotification() {
    Runtime rt;
    Thread thread("test-retirement-notification");
    thread.pushStack("test-retirement-notification");
    const uint64_t before = rt.schedulerMutationEpoch;
    rt.sequencer->retireThread(thread);
    check(thread.stack.empty() && thread.status == STATUS_DONE &&
          rt.schedulerMutationEpoch == before + 1,
          "retireThread publishes terminal status to scheduler");
    rt.sequencer->retireThread(thread);
    check(rt.schedulerMutationEpoch == before + 1,
          "repeated retirement does not publish an extra mutation");
}

void testWaitDeadlineIndex() {
    Runtime rt;
    rt.currentMSecs = 100;
    for (int i = 0; i < 40; ++i) {
        Runtime::WaitEntry w;
        w.deadlineMs = 1000 + i;
        rt.enqueueWait(w);
    }
    check(rt.indexedWaitCount == 40 &&
          rt.earliestWaitDeadlineMs == 1000,
          "wait deadline index tracks earliest scheduled timer");
    rt.processWaitQueue();
    check(rt.waitQueue.size() == 40,
          "not-yet-due wait entries remain queued");
    Runtime::WaitEntry immediate;
    immediate.deadlineMs = 99;
    rt.enqueueWait(immediate);
    rt.processWaitQueue();
    check(rt.waitQueue.size() == 40 &&
          rt.earliestWaitDeadlineMs == 1000,
          "earlier injected deadline resolves without reordering remaining timers");
    rt.currentMSecs = 1000;
    rt.processWaitQueue();
    check(rt.waitQueue.size() == 39 &&
          rt.earliestWaitDeadlineMs == 1001,
          "deadline index rebuilds after expiration");
    rt.clearWaitQueue();
    check(rt.waitQueue.empty() && rt.indexedWaitCount == 0 &&
          std::isinf(rt.earliestWaitDeadlineMs),
          "cleared wait registry has no stale deadline");
    // Legacy direct public mutation still forces a conservative rebuild.
    Runtime::WaitEntry legacy;
    legacy.deadlineMs = 77;
    rt.waitQueue.push_back(legacy);
    rt.processWaitQueue();
    check(rt.waitQueue.empty(),
          "legacy direct deadline enqueue is still processed");
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

    const uint64_t beforeRestartEpoch = rt.schedulerMutationEpoch;
    rt.startBroadcast(u"ping");
    check(rt.schedulerMutationEpoch > beforeRestartEpoch,
          "broadcast receiver reuse invalidates cached ready membership");
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

    // A mark epoch is a non-owning reachability hint, not part of GC roots.
    // Force rollover in the stamped-index mode: all live threads must be
    // re-marked from their *current* roots, never from stale historical IDs.
    if (const char* e = std::getenv("XYO_THREAD_RECLAIM_STAMP");
        e && e[0] == '1') {
        rt.reclaimRetentionEpoch = UINT64_MAX - 1;
        rt.reclaimThreads();
        check(rt.reclaimRetentionEpoch == UINT64_MAX &&
              rt.ownedThreads.size() == 12 &&
              rt.ownedThreads.front().get() == first &&
              rt.ownedThreads.back().get() == last,
              "stamped reclaim retains transitive references before epoch wrap");
        rt.reclaimThreads();
        check(rt.reclaimRetentionEpoch == 1 &&
              rt.ownedThreads.size() == 12 &&
              rt.ownedThreads.front().get() == first &&
              rt.ownedThreads.back().get() == last,
              "stamped reclaim re-marks current references after epoch wrap");
    }

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

void testReclaimLargeJoinClosure() {
    // >4KB of unordered_set node/bucket storage forces the PMR arena to
    // allocate its overflow blocks. The references and ownership semantics
    // must be identical to the standard allocator.
    Runtime rt;
    constexpr size_t kHeld = 320;
    constexpr size_t kHistorical = 128;
    std::vector<Thread*> expected;
    expected.reserve(kHeld);
    for (size_t i = 0; i < kHeld + kHistorical; ++i) {
        auto owned = std::make_unique<Thread>("large-closure");
        Thread* raw = owned.get();
        raw->status = STATUS_DONE;
        rt.ownedThreads.push_back(std::move(owned));
        if (i < kHeld) {
            expected.push_back(raw);
            rt.lastStepDoneThreads.push_back(raw);
        }
    }
    rt.reclaimThreads();
    bool preserved = rt.ownedThreads.size() == expected.size();
    if (preserved) {
        for (size_t i = 0; i < expected.size(); ++i)
            preserved &= rt.ownedThreads[i].get() == expected[i];
    }
    check(preserved,
          "large retained join closure survives hash arena overflow");
    rt.lastStepDoneThreads.clear();
    rt.reclaimThreads();
    check(rt.ownedThreads.empty(),
          "large closure becomes reclaimable after all handles are removed");
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

    Thread* spare = rt.pushThread("restart-spare", stage, false, true);
    spare->status = STATUS_DONE;
    spare->stack.clear();
    rt.threads.pop_back();
    rt.reclaimThreads();
    check(rt.reusableThreads.size() == 1 &&
              rt.reusableThreads.back().get() == spare,
          "restart storage is reclaimed only after losing all references");
    // Retain the replaced identity as a waiting sender would do. The new
    // execution may consume spare storage but must never reset this handle.
    rt.lastStepDoneThreads.push_back(next);
    const size_t index = next->runtimeIndex;
    Thread* replacement = rt.restartThread(next);
    const char* restartEnv = std::getenv("XYO_RESTART_THREAD_REUSE");
    const bool reuseRestart = restartEnv && restartEnv[0] == '1';
    check(replacement != next && replacement != held &&
              (replacement == spare) == reuseRestart,
          "restart consumes only proven-dead storage with distinct identity");
    check(replacement->runtimeIndex == index && rt.threads[index] == replacement &&
              replacement->topBlock == "new-identity" &&
              replacement->status == STATUS_RUNNING &&
              replacement->justReported.isUndefined() &&
              replacement->jitVarSlots.empty() &&
              replacement->compatibilityStackFrame.startedThreads.empty(),
          "restarted pooled thread is fresh and keeps original schedule position");
    rt.reclaimThreads();
    Gc::get().collect();
    check(next->status == STATUS_DONE && next->stack.empty() &&
              held->status == STATUS_DONE,
          "join-exposed old identities survive restart reclamation and GC");
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


void testFreshCloneHatLaunchOrder() {
    // Two independent start-as-clone hats must both execute exactly once.
    // A host creation callback forces the generic fallback; the no-callback
    // case can use the new opt-in direct hat startup.
    const std::string blocks =
        "{\"cloneA\":{\"opcode\":\"control_start_as_clone\","
            "\"topLevel\":true,\"next\":\"incA\",\"inputs\":{},\"fields\":{}}" +
        std::string(",\"incA\":") +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"1"]]})",
              R"({"VARIABLE":["s","s"]})") +
        ",\"cloneB\":{\"opcode\":\"control_start_as_clone\","
            "\"topLevel\":true,\"next\":\"incB\",\"inputs\":{},\"fields\":{}}" +
        std::string(",\"incB\":") +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"1"]]})",
              R"({"VARIABLE":["s","s"]})") + "}";
    Runtime rt;
    const std::string json = "{\"targets\":[" +
        target("Stage", "{}", true) + "," +
        target("FreshClone", blocks, false, 1) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    const auto loaded = loadSb3(rt, json, nullptr);
    check(loaded.ok, "load clone start hat order fixture");
    if (!loaded.ok) return;
    Target* original = rt.getSpriteTargetByName(u"FreshClone");
    Variable* stageCounter =
        rt.getTargetForStage()->lookupVariableById("s");
    if (!original || !stageCounter) {
        check(false, "clone hat fixture contains original and stage value");
        return;
    }
    stageCounter->value = Value::num(0);
    const auto spawnClone = [&]() {
        Target* clone = original->makeClone();
        check(clone != nullptr, "fresh clone creation succeeds");
        if (!clone) return;
        rt.addTarget(clone);
        check(rt.threads.size() == 2,
              "fresh clone starts both independent clone hats");
        rt.runUntilDone(40);
        check(rt.threads.empty(), "all fresh clone handlers finish");
        rt.disposeTarget(clone);
    };
    spawnClone();
    check(cast::toNumber(stageCounter->value) == 2,
          "fast candidate executes both clone hat effects exactly once");
    int callbackCount = 0;
    rt.onTargetCreated = [&](Target*) { ++callbackCount; };
    spawnClone();
    check(callbackCount == 1 && cast::toNumber(stageCounter->value) == 4,
          "creation callback fallback preserves two clone hat effects");
    rt.onTargetCreated = {};
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

    // A compiled clone-start script is shared by all Targets of the same
    // Sprite. The optional direct readyScript reattachment must preserve
    // main-thread affinity and the exact CompiledScript identity.
    Target* clone = sprite->sprite->createClone();
    clone->isOriginal = false;
    rt.addTarget(clone);
    Thread sameScript("cloneHat");
    sameScript.target = clone;
    sameScript.blockContainer = clone->blocks;
    sameScript.pushStack("cloneHat");
    sameScript.tryCompile(top);
    check(sameScript.isCompiled && sameScript.compiled == compiled,
          "native clone JIT cache reattaches exact published compiled script");
    rt.disposeTarget(clone);
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

void testShortBroadcastLazyRestart() {
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
    check(loaded.ok, "load lightweight broadcast restart fixture");
    if (loaded.ok) {
        Variable* shared = rt.getTargetForStage()->lookupVariableById("s");
        for (int i = 0; i < 12; ++i) {
            rt.startBroadcast(u"PING");
            rt.runUntilDone(30);
        }
        shared->value = Value::num(0);
        rt.startBroadcast(u"PING");
        Thread* first = rt.threads.empty() ? nullptr : rt.threads.front();
        const auto* compiled = first && first->isCompiled
            ? static_cast<const CompiledScript*>(first->compiled) : nullptr;
        // No-JIT builds have already interpreted the hat and advanced to
        // "inc" here. This test targets native-only lazy root initialization.
        if (!compiled || !compiled->entry) {
            check(!rt.jit || !rt.jit->ok(),
                  "available native backend must warm this short broadcast receiver");
            rt.runUntilDone(30);
            check(shared && cast::toNumber(shared->value) == 1,
                  "non-native receiver retains ordinary interpreter execution");
            setJitMode(previousMode);
            return;
        }
        check(compiled->shortSync && !compiled->mayYield,
              "warmed receiver exercises the real short synchronous native path");
        const char* lazyEnv = std::getenv("XYO_LAZY_ROOT_FRAME");
        const bool lazy = (!lazyEnv || lazyEnv[0] != '0') && compiled &&
            compiled->entry && compiled->shortSync && !compiled->mayYield;
        const char* lightEnv = std::getenv("XYO_LIGHT_BROADCAST_RESTART");
        const bool lightRestart = !lightEnv || lightEnv[0] != '0';
        const auto checkRoot = [&](Thread* thread, bool inPlace = false) {
            const bool expectLazy = lazy && (!inPlace || lightRestart);
            check(thread && thread->stack.size() == 1 &&
                      thread->stack.front() == "recv",
                  "lightweight receiver keeps its root ID and scheduling slot");
            check(thread && thread->stackFrames.empty() == expectLazy,
                  "fresh and restarted receivers obey lazy-root setting");
        };
        checkRoot(first);
        for (int i = 0; i < 8; ++i) {
            rt.startBroadcast(u"ping");
            check(rt.threads.size() == 1,
                  "pre-drain rebroadcast does not append duplicate receiver");
            if (!rt.threads.empty()) {
                checkRoot(rt.threads.front(), compiled != nullptr);
                check(rt.threads.front()->pendingShortBroadcast == (lightRestart && lazy),
                      "only pristine lazy native receiver remains marked unentered");
                if (compiled)
                    check(rt.threads.front() == first,
                          "ordinary compiled restart preserves reusable identity");
            }
        }
        check(shared && cast::toNumber(shared->value) == 0,
              "lightweight restart never runs receiver inline at dispatch");
        Thread* pending = rt.threads.front();
        pending->pushReportedValue(Value::num(17));
        check(!pending->pendingShortBroadcast,
              "reported-value mutation invalidates pristine pending state");
        rt.startBroadcast(u"PING");
        check(rt.threads.front() == pending && pending->justReported.isUndefined(),
              "modified pending receiver takes the full payload reset path");
        checkRoot(pending, true);
        StackFrame* frame = pending->peekStackFrame();
        check(frame && !frame->warpMode && !frame->executionContext &&
                  !frame->hasParams && pending->stackFrames.size() == 1,
              "lazy receiver materializes a clean root on demand");
        frame->warpMode = true;
        rt.startBroadcast(u"PING");
        check(rt.threads.front() == pending,
              "materialized receiver retains ordinary reusable identity");
        checkRoot(pending, true);
        check(pending->stackFrames.empty() || !pending->stackFrames.front()->warpMode,
              "materialized frame state forces full reset instead of coalescing");
        // Identity-exposing (and-wait) dispatch must replace even this receiver.
        const auto joined = rt.startBroadcastAndWait(u"PING");
        check(joined.size() == 1 && joined.front() != pending,
              "joined restart receives a distinct identity");
        if (!joined.empty()) {
            checkRoot(joined.front());
            check(!joined.front()->pendingShortBroadcast,
                  "join-exposed receiver cannot be coalesced as an ordinary pending event");
            Thread* joinedIdentity = joined.front();
            rt.startBroadcast(u"PING");
            check(rt.threads.size() == 1 && rt.threads.front() != joinedIdentity,
                  "ordinary restart cannot reset a join-exposed identity");
            check(joinedIdentity->status == STATUS_DONE &&
                      joinedIdentity->stack.empty(),
                  "replaced join handle remains completed");
            checkRoot(rt.threads.front());
        }
        Thread* finalReceiver = rt.threads.front();
        // Keep this test observer alive through the normal retention graph.
        rt.lastStepDoneThreads.push_back(finalReceiver);
        rt.sequencer->stepThread(*finalReceiver);
        check(!finalReceiver->pendingShortBroadcast,
              "entering native receiver invalidates pending coalescing marker");
        const char* slotEnv = std::getenv("XYO_JIT_SHORT_SLOT_DIRECT");
        const bool directSlots = !slotEnv || slotEnv[0] != '0';
        const char* guardEnv = std::getenv("XYO_RT_GUARD_NATIVE_VAR_SLOTS");
        if (!guardEnv || guardEnv[0] != '1') {
            check(finalReceiver->jitVarCache.empty() == directSlots,
                  "short native slot direct mode omits only the redundant helper hash cache");
        }
        check(finalReceiver->jitVarSlots.size() == 1 &&
                  finalReceiver->jitVarSlots.front() == shared,
              "short receiver slot binds the current Stage variable");
        // A direct stepThread completes the payload, not the runtime vector.
        // Let the normal scheduler remove its completed slot before asserting.
        rt.step();
        rt.runUntilDone(30);
        check(rt.threads.empty() && shared &&
                  cast::toNumber(shared->value) == 1,
              "all pre-drain restarts commit exactly one final receiver write");
    }
    setJitMode(previousMode);
}

void testShortSlotInitializerAliases() {
#if defined(XYO_JIT)
    const char* guard = std::getenv("XYO_RT_GUARD_NATIVE_VAR_SLOTS");
    // Guarded per-use mode intentionally resolves changed bindings afresh;
    // this regression covers the unguarded initializer-cache contract.
    if (guard && guard[0] == '1') return;
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", "{}", true) + "," +
        target("Alias", "{}") + "],\"monitors\":[],\"extensions\":[],\"meta\":{}}",
        nullptr);
    check(loaded.ok, "load direct slot initializer alias fixture");
    if (!loaded.ok) return;
    Target* local = rt.getSpriteTargetByName(u"Alias");
    Target* stage = rt.getTargetForStage();
    stage->variables.emplace("existing",
        Variable::create("existing", u"stage", Variable::kScalarType, false));
    local->variables.emplace("z",
        Variable::create("z", u"N", Variable::kScalarType, false));
    CompiledScript native;
    native.entry = +[](JitCtx*) {};
    native.shortSync = true;
    native.mayYield = false;
    Thread thread("probe");
    thread.target = local;
    thread.isCompiled = true;
    thread.compiled = &native;
    JitCtx context{};
    context.thread = &thread;
    context.target = local;
    context.runtime = &rt;
    context.sequencer = rt.sequencer.get();
    static const char existingId[] = "existing";
    static const char16_t stageName[] = u"stage";
    xyo_varslot_init(&context, 0, existingId, stageName, 0);
    check(thread.jitVarSlots[0] == &stage->variables.at("existing"),
          "direct initializer preserves Stage ID binding");
    const char* direct = std::getenv("XYO_JIT_SHORT_SLOT_DIRECT");
    check(thread.jitVarCache.empty() == (!direct || direct[0] != '0'),
          "existing ID skips one-use cache allocation only in direct mode");
    thread.jitVarCache.clear();
    static const char absentId[] = "a";
    static const char16_t firstName[] = u"N";
    static const char16_t secondName[] = u"M";
    Variable* original = &local->variables.at("z");
    xyo_varslot_init(&context, 0, absentId, firstName, 0);
    check(thread.jitVarSlots[0] == original && !thread.jitVarCache.empty(),
          "absent ID name fallback keeps historical helper-cache binding");
    xyo_varslot_init(&context, 1, absentId, secondName, 0);
    Variable* created = &local->variables.at("a");
    check(thread.jitVarSlots[1] == created && created != original,
          "different name with same absent ID creates its own binding");
    check(xyo_jitVar(&context, absentId, firstName, 0) == original,
          "helper preserves original pointer-key alias after later ID creation");
    xyo_varslot_init(&context, 2, absentId, firstName, 0);
    check(thread.jitVarSlots[2] == original,
          "nonempty cache prevents direct ID lookup bypassing an earlier alias");
    char otherId[] = "a";
    char16_t otherName[] = u"N";
    check(xyo_jitVar(&context, otherId, otherName, 0) == created,
          "same-content distinct helper keys retain historical pointer-key behavior");
    thread.jitVarCache.clear();
    xyo_varslot_init(&context, 3, existingId, stageName, 1);
    check(thread.jitVarSlots[3] == &stage->variables.at("existing"),
          "ID binding keeps existing scalar/list collision semantics");
#endif
}

void testShortBroadcastFreshBindings() {
    const JitMode previousMode = jitMode();
    setJitMode(JitMode::HotSpot);
    const std::string receiver =
        "{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
        "\"topLevel\":true,\"next\":\"inc\",\"inputs\":{},"
        "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},\"inc\":" +
        block("data_changevariableby", "append", "{\"VALUE\":[1,[4,\"1\"]]}",
              "{\"VARIABLE\":[\"v\",\"v\"]}") + ",\"append\":" +
        block("data_addtolist", nullptr, "{\"ITEM\":[1,[12,\"v\",\"v\"]]}",
              "{\"LIST\":[\"items\",\"items\"]}") + "}";
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", "{}", true) + "," +
        target("Bindings", receiver) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "load short scalar/list binding fixture");
    if (!loaded.ok) { setJitMode(previousMode); return; }
    Target* original = rt.getSpriteTargetByName(u"Bindings");
    Target* stage = rt.getTargetForStage();
    for (int i = 0; i < 12; ++i) {
        rt.startBroadcast(u"PING");
        rt.runUntilDone(30);
    }
    original->variables.at("v").value = Value::num(10);
    original->variables.at("items").list->clear();
    // Same name but a different Stage ID must not hide a clone-local value.
    auto stageScalar = Variable::create("stage-v", u"v", Variable::kScalarType, false);
    stageScalar.value = Value::num(99);
    stage->variables.emplace("stage-v", std::move(stageScalar));
    Target* clone = original->makeClone();
    check(clone != nullptr, "create clone with independent copied scalar/list state");
    if (!clone) { setJitMode(previousMode); return; }
    rt.addTarget(clone);
    clone->variables.at("v").value = Value::num(40);
    clone->variables.at("items").list->clear();
    rt.startBroadcast(u"PING");
    for (Thread* thread : rt.threads) {
        if (thread->isCompiled) {
            const auto* native = static_cast<const CompiledScript*>(thread->compiled);
            check(native && native->shortSync && !native->mayYield,
                  "scalar/list clone receivers exercise short native slot initialization");
        }
    }
    rt.runUntilDone(30);
    check(cast::toNumber(original->variables.at("v").value) == 11 &&
              cast::toNumber(clone->variables.at("v").value) == 41 &&
              cast::toNumber(stage->variables.at("stage-v").value) == 99,
          "shared native code binds each clone-local scalar independently");
    SList* originalItems = original->variables.at("items").list;
    SList* cloneItems = clone->variables.at("items").list;
    check(originalItems != cloneItems && originalItems->len == 1 && cloneItems->len == 1 &&
              cast::toNumber(originalItems->items[0]) == 11 &&
              cast::toNumber(cloneItems->items[0]) == 41,
          "shared native code appends to each clone-local list independently");

    // Remove the old ID between COMPLETED invocations. Rebinding must use
    // the new local name match rather than retaining an old Variable*.
    clone->variables.erase("v");
    auto replacement = Variable::create("renamed-v", u"v", Variable::kScalarType, false);
    replacement.value = Value::num(100);
    clone->variables.emplace("renamed-v", std::move(replacement));
    Gc::get().collect();
    rt.startBroadcast(u"PING");
    rt.runUntilDone(30);
    check(cast::toNumber(clone->variables.at("renamed-v").value) == 101 &&
              clone->variables.at("items").list->len == 2 &&
              cast::toNumber(clone->variables.at("items").list->items[1]) == 101,
          "fresh entry rebinds erased scalar ID by local name after GC");

    clone->variables.erase("items");
    clone->variables.emplace("renamed-items",
        Variable::create("renamed-items", u"items", Variable::kListType, false));
    // Stage ID wins over local name, exactly like ordinary Scratch lookup.
    rt.startBroadcast(u"PING");
    rt.runUntilDone(30);
    SList* stageItems = stage->variables.at("items").list;
    check(stageItems->len == 1 && cast::toNumber(stageItems->items[0]) == 102 &&
              clone->variables.at("renamed-items").list->len == 0,
          "fresh list binding prefers Stage ID over local name fallback");
    stage->variables.erase("items");
    Gc::get().collect();
    rt.startBroadcast(u"PING");
    rt.runUntilDone(30);
    SList* renamedItems = clone->variables.at("renamed-items").list;
    check(renamedItems->len == 1 && cast::toNumber(renamedItems->items[0]) == 103,
          "fresh list entry falls back to clone-local name when both IDs disappear");
    rt.disposeTarget(clone);
    rt.startBroadcast(u"PING");
    rt.runUntilDone(30);
    check(cast::toNumber(original->variables.at("v").value) == 15,
          "disposed clone cannot contaminate the original receiver binding");
    setJitMode(previousMode);
}

void testPendingBroadcastReceiverOrder() {
    const JitMode previousMode = jitMode();
    setJitMode(JitMode::HotSpot);
    const auto receiver = [](const char* digit) {
        return std::string("{\"recv\":{\"opcode\":\"event_whenbroadcastreceived\","
            "\"topLevel\":true,\"next\":\"set\",\"inputs\":{},"
            "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",null]}},\"set\":") +
            block("data_setvariableto", nullptr, "{\"VALUE\":[2,\"add\"]}",
                  "{\"VARIABLE\":[\"s\",\"s\"]}") +
            ",\"add\":" + block("operator_add", nullptr,
                std::string("{\"NUM1\":[2,\"mul\"],\"NUM2\":[1,[4,\"") + digit + "\"]]}") +
            ",\"mul\":" + block("operator_multiply", nullptr,
                "{\"NUM1\":[1,[12,\"s\",\"s\"]],\"NUM2\":[1,[4,\"10\"]]}") + "}";
    };
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", "{}", true) + "," +
        target("First", receiver("1"), false, 1) + "," +
        target("Second", receiver("2"), false, 2) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "load coalesced receiver execution-order fixture");
    if (loaded.ok) {
        Variable* shared = rt.getTargetForStage()->lookupVariableById("s");
        for (int i = 0; i < 12; ++i) {
            rt.startBroadcast(u"PING");
            rt.runUntilDone(30);
        }
        shared->value = Value::num(0);
        rt.startBroadcast(u"PING");
        check(rt.threads.size() == 2, "ordered event queues two receivers");
        for (int i = 0; i < 8; ++i) rt.startBroadcast(u"PING");
        check(shared && cast::toNumber(shared->value) == 0,
              "repeated ordinary events leave sender-visible state unchanged until scheduling");
        check(rt.threads.size() == 2 &&
                  rt.threads[0]->target->getName() == u"Second" &&
                  rt.threads[1]->target->getName() == u"First",
              "pending coalescing preserves original receiver slots and target order");
        rt.runUntilDone(30);
        check(shared && cast::toNumber(shared->value) == 21,
              "coalesced shared writers execute once each in original event order");
        rt.startBroadcast(u"PING");
        rt.stopAll();
        check(rt.threads.empty(), "stopAll cancels queued short receivers");
        rt.startBroadcast(u"PING");
        rt.runUntilDone(30);
        check(shared && cast::toNumber(shared->value) == 2121,
              "stopped pending event cannot leak writes into a later event");
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

    // Reverse lookup is opt-in. It tracks pointer identity only and never
    // dereferences a possibly replaced Variable from the registry.
    thread.eventWaitArmed = true;
    rt.trackWaitDependencies(thread);
    Variable* gate = target.lookupVariableById("gate");
    const char* registryFlag = std::getenv("XYO_WAIT_WAKE_REGISTRY");
    const bool indexedWake = registryFlag && registryFlag[0] == '1';
    rt.notifyWaitVariableWrite(*gate);
    auto wakeups = rt.takeWaitWakeups();
    check(indexedWake ? wakeups.size() == 1 && wakeups[0] == &thread
                      : wakeups.empty(),
          "reverse-index write notification targets the watched thread");
#if defined(XYO_JIT)
    // Exercise the exact C entry point emitted by LLVM, with the resolved
    // Variable* supplied by the compiled var slot.
    JitCtx ctx{};
    ctx.runtime = &rt;
    xyo_wait_notify_var_store(&ctx, gate);
    wakeups = rt.takeWaitWakeups();
    check(indexedWake ? wakeups.size() == 1 && wakeups[0] == &thread
                      : wakeups.empty(),
          "native LLVM variable-write callback wakes its dependency");
#endif
    rt.notifyWaitVariableWrite(*gate);
    rt.notifyWaitVariableWrite(*gate);
    wakeups = rt.takeWaitWakeups();
    check(indexedWake ? wakeups.size() == 1 && wakeups[0] == &thread
                      : wakeups.empty(),
          "repeated writes coalesce into one pending wakeup");
    rt.untrackWaitDependencies(thread);
    rt.notifyWaitVariableWrite(*gate);
    check(rt.takeWaitWakeups().empty(),
          "untracking prevents stale variable-to-thread wakeups");
    thread.eventWaitArmed = false;
    gate->value = Value::num(1);
    check(!canSkipEventWaitByDependency(thread),
          "changed scalar value wakes the suspended condition");
    gate->value = Value::num(0);
    snapshotEventWaitDependency(thread, site);
    check(canSkipEventWaitByDependency(thread),
          "rearming records the new scalar snapshot");

    // An interpreter can publish DONE without calling retireThread.
    // Compaction must still remove the reverse-watch identity.
    thread.eventWaitArmed = true;
    rt.trackWaitDependencies(thread);
    thread.status = STATUS_DONE;
    thread.runtimeIndex = 0;
    rt.threads.push_back(&thread);
    std::vector<Thread*> finished;
    rt.sequencer->compactFinishedThreads(finished);
    check(rt.threads.empty() && finished.size() == 1 &&
              finished[0] == &thread,
          "directly finished watcher is removed by compaction");
    rt.notifyWaitVariableWrite(*gate);
    check(rt.takeWaitWakeups().empty(),
          "compaction deregisters watches before thread reuse");
    thread.status = STATUS_RUNNING;
    snapshotEventWaitDependency(thread, site);

    // Deletion must remove every reverse-watch edge before the address
    // becomes invalid. A subsequent allocation may reuse that address.
    thread.eventWaitArmed = true;
    rt.trackWaitDependencies(thread);
    const auto beforeErase = rt.schedulerMutationEpoch.load(
        std::memory_order_relaxed);
    target.deleteVariable("gate");
    check(!canSkipEventWaitByDependency(thread),
          "deleted variable invalidates pointers before dereference");
    check(!indexedWake || !thread.eventWatchValid,
          "variable deletion invalidates native wait snapshot");
    const auto removedWakeups = rt.takeWaitWakeups();
    check(indexedWake ? removedWakeups.size() == 1 &&
                            removedWakeups[0] == &thread
                      : removedWakeups.empty(),
          "variable deletion queues a conservative waiter reevaluation");
    check(!indexedWake ||
              rt.schedulerMutationEpoch.load(std::memory_order_relaxed) >
                  beforeErase,
          "variable deletion invalidates the ready index");
    target.variables.emplace("gate", Variable::create(
        "gate", u"gate", Variable::kScalarType, false));
    snapshotEventWaitDependency(thread, site);
    check(canSkipEventWaitByDependency(thread),
          "variable rebound after mutation yields a new safe snapshot");

    // An embedder may bypass Target::deleteVariable() and erase directly.
    // The audit must check the binding generation *before* touching the
    // old Variable*, invalidate the reverse edge, and request reevaluation.
    thread.eventWaitArmed = true;
    rt.trackWaitDependencies(thread);
    const uint64_t auditEpoch = rt.schedulerMutationEpoch.load(
        std::memory_order_relaxed);
    target.variables.erase("gate");
    check(!eventWaitBindingSnapshotCurrent(thread),
          "direct variable erase invalidates the binding-only guard");
    const size_t directErases = rt.validateWaitDependencyBindings();
    check(indexedWake ? directErases == 1 : directErases == 0,
          "binding auditor detects unnotified direct map deletion");
    check(!indexedWake || !thread.eventWatchValid,
          "binding auditor disarms a stale snapshot without dereferencing it");
    const auto auditWakeups = rt.takeWaitWakeups();
    check(indexedWake ? auditWakeups.size() == 1 &&
                            auditWakeups[0] == &thread
                      : auditWakeups.empty(),
          "binding auditor requests fallback evaluation after direct erase");
    check(!indexedWake ||
              rt.schedulerMutationEpoch.load(std::memory_order_relaxed) >
                  auditEpoch,
          "binding auditor invalidates a cached ready index");
    target.variables.emplace("gate", Variable::create(
        "gate", u"gate", Variable::kScalarType, false));
    rt.notifyWaitVariableWrite(*target.lookupVariableById("gate"));
    check(rt.takeWaitWakeups().empty(),
          "rebinding cannot revive a subscription to old variable storage");
    snapshotEventWaitDependency(thread, site);
    check(eventWaitBindingSnapshotCurrent(thread),
          "new variable storage may be watched after a fresh snapshot");
    rt.trackWaitDependencies(thread);

    sprite.blocks->resetCache();
    const size_t editedSource = rt.validateWaitDependencyBindings();
    check(indexedWake ? editedSource == 1 : editedSource == 0,
          "binding auditor invalidates reverse watchers on source edit");
    const auto sourceWakeups = rt.takeWaitWakeups();
    check(indexedWake ? sourceWakeups.size() == 1 &&
                            sourceWakeups[0] == &thread
                      : sourceWakeups.empty(),
          "source edits schedule a conservative conditional reevaluation");
    thread.eventWaitArmed = false;
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
    const char* registryFlag = std::getenv("XYO_WAIT_WAKE_REGISTRY");
    const bool indexedWake = registryFlag && registryFlag[0] == '1';
    thread.eventWaitArmed = true;
    rt.trackWaitDependencies(thread);
    target.deleteVariable("second");
    check(!canSkipEventWaitByDependency(thread),
          "binding deletion invalidates watched pointers before access");
    auto affected = rt.takeWaitWakeups();
    check(indexedWake ? affected.size() == 1 && affected[0] == &thread
                      : affected.empty(),
          "deleting one of two dependencies wakes the watched thread");
    rt.notifyWaitVariableWrite(*a);
    check(rt.takeWaitWakeups().empty(),
          "deleting a dependency untracks remaining subscribed variables");
    thread.eventWaitArmed = false;
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
    testReclaimLargeJoinClosure();
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
    testSchedulerStatusTransitions();
    testWaitResumeOutcomeAudit();
    testNativeWaitResumeOutcomes();
    testAutonomousNativeWaitWriter();
    testNativeWaitFanoutStress();
    testWaitReadyUnnotifiedFallback();
    testWaitNotificationDifferentialAudit();
    testSchedulerRetirementNotification();
    testReadyChangePublication();
    testConcurrentReadyInvalidation();
    testWaitDeadlineIndex();
    testBroadcastReceiverReuse();
    testShortBroadcastReadyCache();
    testShortBroadcastLazyRestart();
    testShortSlotInitializerAliases();
    testShortBroadcastFreshBindings();
    testPendingBroadcastReceiverOrder();
    testNonRestartBroadcastFanout();
    testFreshCloneHatLaunchOrder();
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
