// Headless/mock-renderer scheduler integration and dispatch-and-join stress.
#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "engine/fiber.hpp"
#include "engine/runtime.hpp"
#include "engine/event_wait_dependency.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "jit/mega_partial.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "rt/runtime_helpers.hpp"

#if defined(XYO_JIT)
extern "C" void xyo_wait_notify_var_store(xyo::JitCtx*, xyo::Variable*);
#endif
extern "C" void xyo_yield_fast(xyo::Fiber*);
extern "C" void xyo_yield_fast_direct(xyo::Fiber*);

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL scheduler: %s\n", name); }
}

bool generalCoroEnabled() {
    const char* value = std::getenv("XYO_JIT_CORO_GENERAL");
    return value && value[0] == '1';
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
    if (eligible && std::getenv("XYO_FIBER_CHAIN") == nullptr &&
        !generalCoroEnabled()) {
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
        std::getenv("XYO_PROF") == nullptr && !generalCoroEnabled())
        check(rt.sequencer->parallelStats().chainedPasses > 0,
              "broadcast receivers exercise direct fiber chain");
}

struct FiberDirectYieldFixture {
    std::vector<int>* trace;
    int id;
    void (*yieldFn)(Fiber*) = &xyo_yield_fast;
};
void fiberDirectYieldFixtureEntry(void* arg) {
    auto& state = *static_cast<FiberDirectYieldFixture*>(arg);
    for (int step = 0; step < 3; ++step) {
        state.trace->push_back(state.id * 10 + step);
        state.yieldFn(currentFiber());
    }
    state.trace->push_back(state.id * 10 + 3);
}
void testFiberDirectYieldSerialChainOptOut() {
    const char* chain = std::getenv("XYO_FIBER_CHAIN");
    const char* direct = std::getenv("XYO_FIBER_DIRECT_YIELD");
    if (!chain || chain[0] != '0' || !direct || direct[0] != '1')
        return;
    const size_t before = FiberPool::activeCount();
    for (auto* yieldFn : {&xyo_yield_fast, &xyo_yield_fast_direct}) {
        std::vector<int> trace;
        FiberDirectYieldFixture a{&trace, 1, yieldFn}, b{&trace, 2, yieldFn};
        Fiber* first = FiberPool::acquire(fiberDirectYieldFixtureEntry, &a);
        Fiber* second = FiberPool::acquire(fiberDirectYieldFixtureEntry, &b);
        check(first && second && first != second,
              "direct Fiber yield fixture leases distinct native fibers");
        if (!first || !second) {
            FiberPool::release(first);
            FiberPool::release(second);
            return;
        }
        first->resume();
        second->resume();
        Fiber* pair[] = {first, second};
        for (int i = 0; i < 3; ++i)
            Fiber::resumeChain(pair, 2);
        check(trace == std::vector<int>{10, 20, 11, 21, 12, 22, 13, 23},
              "direct yield keeps every scheduler boundary and native FIFO order");
        check(first->finished() && second->finished(),
              "direct yield resumes both Fibers to completion");
        FiberPool::release(first);
        FiberPool::release(second);
        check(FiberPool::activeCount() == before,
              "direct yield returns every leased native stack to the pool");
    }
}

void testGenericConflictFiberChain() {
    // Three stage-variable increments form one connected dependency graph,
    // but none of the first script's WRITES is touched by *every* script.
    // That bypasses the existing all-common-writer chain shortcut.
    // Use different counted lifetimes to detect skipped/replayed yields.
    Runtime rt;
    std::string stage = target("Stage", "{}", true);
    const std::string oldVars = R"("variables":{"s":["s",""]})";
    const auto at = stage.find(oldVars);
    check(at != std::string::npos, "generic-chain stage schema is known");
    if (at == std::string::npos) return;
    stage.replace(at, oldVars.size(),
                  R"("variables":{"x":["x",1],"y":["y",1],"z":["z",1]})");
    std::string json = "{\"targets\":[" + stage;
    const char* writes[] = {"x", "y", "z"};
    const char* reads[]  = {"z", "x", "y"};
    const int reps[] = {7, 9, 11};
    for (int i = 0; i < 3; ++i) {
        const std::string blocks =
            "{\"hat\":" + hat("repeat") +
            ",\"repeat\":" + block("control_repeat", nullptr,
                "{\"TIMES\":[1,[4,\"" + std::to_string(reps[i]) +
                "\"]],\"SUBSTACK\":[2,\"inc\"]}") +
            ",\"inc\":" + block("data_changevariableby", nullptr,
                R"({"VALUE":[2,"read"]})",
                std::string(R"({"VARIABLE":[")") + writes[i] +
                "\",\"" + writes[i] + "\"]}") +
            ",\"read\":" + block("data_variable", nullptr, "{}",
                std::string(R"({"VARIABLE":[")") + reads[i] +
                "\",\"" + reads[i] + "\"]}") + "}";
        json += "," + target("Chain" + std::to_string(i),
                              blocks, false, i + 1);
    }
    json += "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    check(loadSb3(rt, json, nullptr).ok,
          "generic conflict component with three readers/writers loads");
    if (rt.targets.size() != 4) return;
    rt.greenFlag();
    bool eligible = rt.sequencer->parallelEnabled() &&
                    rt.threads.size() == 3;
    for (Thread* thread : rt.threads)
        eligible &= thread->isCompiled &&
                    rt.sequencer->parallelEligible(*thread);
    // Interpret the *actual* project target order to preserve Scratch's
    // back-to-front green-flag order. A single scheduler pass does at most
    // one counted body of each still-running Fiber.
    double expected[3] = {1, 1, 1};
    for (int pass = 0; pass < 11; ++pass) {
        for (Thread* thread : rt.threads) {
            const int index = int(thread->target->getName().back() - u'0');
            if (index < 0 || index >= 3 || pass >= reps[index]) continue;
            expected[index] += expected[(index + 2) % 3];
        }
    }
    rt.runUntilDone(100);
    Target* st = rt.getTargetForStage();
    bool exact = rt.threads.empty();
    for (int i = 0; i < 3; ++i) {
        Variable* v = st->lookupVariableById(writes[i]);
        exact &= v && cast::toNumber(v->value) == expected[i];
    }
    check(exact,
          "generic connected component preserves per-pass numeric effects");
    const char* enabled = std::getenv("XYO_FIBER_CHAIN_COMPONENT");
    if (eligible && enabled && enabled[0] == '1' &&
        !std::getenv("XYO_PROF")) {
        check(rt.sequencer->parallelStats().genericComponentChained > 0 &&
              rt.sequencer->parallelStats().genericComponentThreads >= 3,
              "generic single-component graph executes native fiber chain");
    }
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

void testFinishedThreadCompactionStableOrder() {
    // The fast suffix path must never disturb live-prefix runtime indices
    // or reorder the finished identities appended to doneThreads.
    for (const int scenario : {0, 1, 2, 3}) {
        Runtime rt;
        const auto loaded = loadSb3(rt,
            "{\"targets\":[" + target("Stage", "{}", true) +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(loaded.ok, "compaction fixture loads Stage target");
        if (!loaded.ok) return;
        Target* stage = rt.getTargetForStage();
        std::vector<Thread*> expectedActive, expectedDone;
        const size_t count = scenario == 3 ? 0u : 16u;
        for (size_t i = 0; i < count; ++i) {
            auto owned = std::make_unique<Thread>(
                "compact-" + std::to_string(scenario) + "-" +
                std::to_string(i));
            Thread* thread = owned.get();
            thread->target = stage;
            thread->pushStack(thread->topBlock);
            // Deliberately stale public-vector hint in the live prefix:
            // optimized compaction must repair it just like baseline.
            thread->runtimeIndex = scenario == 0 && i == 2
                ? SIZE_MAX : i;
            const bool dead = scenario == 0 ?
                (i == 7 || i == 10 || i == 14) :
                scenario == 1 ? (i < 15) : false;
            if (dead) {
                thread->status = STATUS_DONE;
                thread->stack.clear();
                expectedDone.push_back(thread);
            } else {
                expectedActive.push_back(thread);
            }
            rt.threadMap[thread->getId()] = thread;
            rt.threads.push_back(thread);
            rt.ownedThreads.push_back(std::move(owned));
        }
        const auto before = rt.schedulerStructureEpoch.load(
            std::memory_order_relaxed);
        std::vector<Thread*> finished;
        rt.sequencer->compactFinishedThreads(finished);
        check(rt.threads == expectedActive &&
              finished == expectedDone,
              "compaction preserves exact active and stopped Thread order");
        bool correct = true;
        for (size_t i = 0; i < expectedActive.size(); ++i) {
            Thread* thread = expectedActive[i];
            correct &= thread->runtimeIndex == i &&
                       rt.threadMap.find(thread->getId()) !=
                           rt.threadMap.end();
        }
        for (Thread* thread : expectedDone)
            correct &= thread->stack.empty() &&
                       rt.threadMap.find(thread->getId()) ==
                           rt.threadMap.end();
        check(correct,
              "compaction updates moved indices and removes only finished IDs");
        check(rt.schedulerStructureEpoch.load(
                  std::memory_order_relaxed) == before + 1,
              "compaction maintains scheduler-index invalidation epoch");
    }
}

void testCompactionPreservesReplacementMapEntry() {
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", "{}", true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "replacement compaction fixture loads");
    if (!loaded.ok) return;
    Target* stage = rt.getTargetForStage();
    auto stale = std::make_unique<Thread>("replacement-hat");
    auto fresh = std::make_unique<Thread>("replacement-hat");
    Thread* oldThread = stale.get();
    Thread* replacement = fresh.get();
    oldThread->target = replacement->target = stage;
    oldThread->pushStack(oldThread->topBlock);
    oldThread->stack.clear();
    oldThread->status = STATUS_DONE;
    oldThread->runtimeIndex = 0;
    replacement->pushStack(replacement->topBlock);
    replacement->runtimeIndex = 1;
    const std::string key = oldThread->getId();
    check(replacement->getId() == key,
          "replacement has the same Script ID as the retired Thread");
    rt.ownedThreads.push_back(std::move(stale));
    rt.ownedThreads.push_back(std::move(fresh));
    rt.threads = {oldThread, replacement};
    rt.threadMap[key] = replacement;
    std::vector<Thread*> finished;
    rt.sequencer->compactFinishedThreads(finished);
    const auto mapped = rt.threadMap.find(key);
    check(rt.threads.size() == 1 && rt.threads.front() == replacement &&
          replacement->runtimeIndex == 0 && finished.size() == 1 &&
          finished.front() == oldThread,
          "compaction retains the replacement Thread and correct index");
    check(mapped != rt.threadMap.end() && mapped->second == replacement,
          "compaction must not erase a restarted receiver map entry");
}

void testCompactionSmallMapErase() {
    // Four distinct Script IDs, two completed IDs, and an extra alias
    // to a completed identity with the WRONG key. Hash erase and the
    // tiny-map pointer scan must remove only the original exact ID.
    for (const bool alias : {false, true}) {
        Runtime rt;
        const auto loaded = loadSb3(rt,
            "{\"targets\":[" + target("Stage", "{}", true) +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}",
            nullptr);
        check(loaded.ok, "small-map compaction fixture loads");
        if (!loaded.ok) return;
        Target* stage = rt.getTargetForStage();
        std::vector<Thread*> created;
        for (int i = 0; i < 4; ++i) {
            auto owned = std::make_unique<Thread>(
                "small-map-" + std::to_string(i));
            Thread* thread = owned.get();
            thread->target = stage;
            thread->pushStack(thread->topBlock);
            thread->runtimeIndex = size_t(i);
            if (i == 1 || i == 2) {
                thread->status = STATUS_DONE;
                thread->stack.clear();
            }
            rt.threadMap[thread->getId()] = thread;
            rt.threads.push_back(thread);
            created.push_back(thread);
            rt.ownedThreads.push_back(std::move(owned));
        }
        if (alias) rt.threadMap["intentionally-invalid-alias"] = created[1];
        std::vector<Thread*> stopped;
        rt.sequencer->compactFinishedThreads(stopped);
        check(rt.threads.size() == 2 &&
              rt.threads[0] == created[0] &&
              rt.threads[1] == created[3] &&
              created[3]->runtimeIndex == 1 &&
              stopped.size() == 2 &&
              stopped[0] == created[1] &&
              stopped[1] == created[2],
              "small-map scan preserves living order and completion order");
        bool exact = true;
        for (int i = 0; i < 4; ++i) {
            const auto it = rt.threadMap.find(created[i]->getId());
            const bool present = it != rt.threadMap.end() &&
                                 it->second == created[i];
            exact &= present == (i == 0 || i == 3);
        }
        const auto strange = rt.threadMap.find("intentionally-invalid-alias");
        exact &= alias ? (strange != rt.threadMap.end() &&
                          strange->second == created[1])
                       : strange == rt.threadMap.end();
        check(exact,
              "small-map scan erases exact identity ID, not public alias");
        rt.threadMap.clear();
    }
}

void testLazyRootWideNative() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    std::string blocks = "{";
    const auto append = [&](const std::string& id, const std::string& json) {
        if (blocks.size() > 1) blocks += ",";
        blocks += "\"" + id + "\":" + json;
    };
    append("hat", hat("change0"));
    for (int i = 0; i < 12; ++i) {
        const std::string id = "change" + std::to_string(i);
        const std::string next = "change" + std::to_string(i + 1);
        append(id, block("data_changevariableby",
            i == 11 ? nullptr : next.c_str(),
            R"({"VALUE":[1,[4,"1"]]})",
            R"({"VARIABLE":["s","s"]})"));
    }
    blocks += "}";
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}",
        nullptr);
    check(loaded.ok, "wide root fixture loads");
    if (!loaded.ok) return;
    Target* stage = rt.getTargetForStage();
    Thread* thread = rt.pushThread(
        "hat", stage, false, false, stage->blocks->getBlock("hat"));
    check(thread && thread->isCompiled && thread->compiled,
          "wide root fixture creates an Eager native script");
    if (!thread || !thread->isCompiled || !thread->compiled) return;
    const auto* compiled =
        static_cast<const CompiledScript*>(thread->compiled);
    check(!compiled->mayYield && !compiled->shortSync &&
          !compiled->mainThread && compiled->procedures.empty() &&
          !compiled->lazy,
          "wide root tests a real non-short, non-yielding native script");
    const char* flag = std::getenv("XYO_LAZY_ROOT_NATIVE_WIDE");
    const bool wide = flag && flag[0] == '1';
    if (!compiled->mayYield && !compiled->shortSync &&
        !compiled->mainThread && compiled->procedures.empty() &&
        !compiled->lazy) {
        check(thread->stack.size() == 1 &&
              (thread->stackFrames.empty() == wide),
              "wide root defers initial StackFrame only when enabled");
    }
    rt.runUntilDone(100);
    Variable* s = stage->lookupVariableById("s");
    check(rt.threads.empty() && s && cast::toNumber(s->value) == 12,
          "wide root native entry preserves exact Scratch result");
}

void testReadyEagerCacheBypass() {
    const char* flag = std::getenv("XYO_JIT_READY_EAGER_FAST");
    if (!flag || flag[0] != '1' || !jitEnabled() ||
        jitMode() != JitMode::Eager) return;
    const std::string blocks =
        std::string(R"({"hat":)") + hat("change") +
        R"(,"change":)" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"1"]]})",
              R"({"VARIABLE":["s","s"]})") + "}";
    Runtime rt;
    const auto loaded = loadSb3(
        rt, "{\"targets\":[" + target("Stage", blocks, true) +
                "],\"monitors\":[],\"extensions\":[],\"meta\":{}}",
        nullptr);
    check(loaded.ok, "Eager cache reuse fixture loads");
    if (!loaded.ok) return;
    Target* stage = rt.getTargetForStage();
    const Block* root = stage->blocks->getBlock("hat");
    Thread* first = rt.pushThread("hat", stage, false, false, root);
    check(first && first->isCompiled && first->compiled,
          "Eager first launch compiles the source");
    if (!first || !first->isCompiled || !first->compiled) return;
    Thread* second = rt.pushThread("hat", stage, false, false, root);
    check(second && second->isCompiled &&
          second->compiled == first->compiled &&
          second->topBlockPtr == root,
          "Eager second launch reuses cached native code");
    // A dynamic extension can make a previously non-edge hat edge-activated.
    // Even though the ready native artifact is still cached, the eager
    // shortcut may NOT bypass compile()'s edge-activated refusal.
    rt.hats["event_whenflagclicked"].edgeActivated = true;
    Thread* refused = rt.pushThread("hat", stage, false, false, root);
    check(refused && !refused->isCompiled,
          "Eager cache shortcut preserves edge-activated hat refusal");
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
    check(first->fiber != nullptr ||
              (first->jitCtx && first->jitCtx->coroHandle),
          "first receiver has a suspended continuation");

    const uint64_t beforeRestartEpoch = rt.schedulerMutationEpoch;
    rt.startBroadcast(u"ping");
    check(rt.schedulerMutationEpoch > beforeRestartEpoch,
          "broadcast receiver reuse invalidates cached ready membership");
    check(rt.threads.size() == 1 && rt.threads[0] == first,
          "ordinary broadcast reuses compiled receiver");
    check(first->fiber == nullptr &&
              (!first->jitCtx || !first->jitCtx->coroHandle) &&
              first->status == STATUS_RUNNING,
          "restart discards old continuation and resets run status");

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
        suspendedJoined = joined->fiber != nullptr ||
            (joined->jitCtx && joined->jitCtx->coroHandle);
        check(suspendedJoined,
              "join-exposed receiver has a suspended continuation");
    }
    rt.startBroadcast(u"PING");
    check(rt.threads.size() == 1 && rt.threads[0] != joined,
          "ordinary broadcast must not reuse join-exposed receiver");
    if (suspendedJoined) {
        check(joined->fiber == nullptr && joined->jitCtx == nullptr &&
                  joined->status == STATUS_DONE,
              "replaced receiver releases suspended continuation immediately");
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


void testStacklessCoroutineNativeRepeat() {
    const char* flag = std::getenv("XYO_JIT_CORO_DIRECT");
    if (!jitFiberlessEnabled() || !flag || flag[0] != '1' ||
        !jitEnabled()) return;

    // This is an actual generated LLVM continuation, not the WASM numeric
    // session or the fallback interpreter. Each run retains a single
    // native coroutine frame, without acquiring a native Fiber.
    const std::string blocks =
        "{\"hat\":" + hat("repeat") +
        ",\"repeat\":" + block("control_repeat", nullptr,
            R"({"TIMES":[1,[4,"6"]],"SUBSTACK":[2,"increment"]})") +
        ",\"increment\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})",
            R"({"VARIABLE":["s","s"]})") + "}";
    Runtime rt;
    const std::string project =
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    const auto loaded = loadSb3(rt, project, nullptr);
    check(loaded.ok, "load real LLVM coroutine repeat fixture");
    if (!loaded.ok) return;
    Variable* counter = rt.getTargetForStage()->lookupVariableById("s");
    if (!counter) {
        check(false, "coroutine fixture has numeric stage counter");
        return;
    }
    rt.greenFlag();
    if (rt.threads.empty()) {
        check(false, "coroutine fixture starts an actual Scratch script");
        return;
    }
    Thread* runner = rt.threads.front();
    const auto* compiled =
        static_cast<const CompiledScript*>(runner->compiled);
    check(runner->isCompiled && compiled && compiled->coroResume,
          "native LLVM coroutine entry was published, not interpreted");
    if (!runner->isCompiled || !compiled || !compiled->coroResume)
        return;

    // Each of these must resume the *same* heap-stored loop counter.
    for (int i = 1; i <= 3; ++i) {
        rt.sequencer->stepThread(*runner);
        check(cast::toNumber(counter->value) == i,
              "coroutine resumes at exact next repeat iteration");
        check(runner->jitCtx && runner->jitCtx->coroHandle &&
              !runner->fiber,
              "LLVM coroutine stays suspended without a native Fiber");
    }
    rt.stopThread(runner);
    check(!runner->jitCtx && !runner->fiber,
          "external stop destroys suspended coroutine frame");

    // Scratch green flag does not reset variable values automatically.
    // Reset explicitly before asserting the second execution's result.
    // A new hat must have a fresh machine continuation.
    counter->value = Value::num(0);
    rt.greenFlag();
    rt.runUntilDone(100);
    check(rt.threads.empty() && cast::toNumber(counter->value) == 6,
          "new Scratch execution completes six increments exactly once");

    counter->value = Value::num(0);
    rt.greenFlag();
    if (!rt.threads.empty()) {
        runner = rt.threads.front();
        rt.sequencer->stepThread(*runner);
        const double committed = cast::toNumber(counter->value);
        runner->blockContainer->resetCache();
        rt.sequencer->stepThread(*runner);
        check(runner->status == STATUS_DONE &&
              cast::toNumber(counter->value) == committed &&
              !runner->jitCtx,
              "source revision edit discards coroutine without replay");
    }
    // Raw host edits can erase and recreate Variable objects without
    // changing any Blocks source revision. The coroutine holds a cached
    // native Variable*, so it must discard the frame before touching it.
    counter->value = Value::num(0);
    rt.greenFlag();
    if (!rt.threads.empty()) {
        runner = rt.threads.front();
        rt.sequencer->stepThread(*runner);
        Target* stage = rt.getTargetForStage();
        const auto sourceRevision = runner->blockContainer->revision();
        stage->variables.erase("s");
        auto [replacement, inserted] = stage->variables.emplace(
            "s", Variable::create(
                "s", u"s", Variable::kScalarType, false));
        check(inserted, "replace watched native scalar binding");
        replacement->second.value = Value::num(1234);
        rt.sequencer->stepThread(*runner);
        check(runner->status == STATUS_DONE && !runner->jitCtx &&
              cast::toNumber(replacement->second.value) == 1234 &&
              runner->blockContainer->revision() == sourceRevision,
              "binding generation change discards native continuation "
              "without dereferencing an erased Variable");
    }
    check(FiberPool::fiberlessAcquires() == 0,
          "coroutine smoke never acquires a native Fiber");
}

void testStacklessCoroutineBroadcastRestart() {
    const char* flag = std::getenv("XYO_JIT_CORO_DIRECT");
    if (!jitFiberlessEnabled() || !flag || flag[0] != '1' ||
        !jitEnabled()) return;
    const std::string blocks =
        "{\"receiver\":{\"opcode\":\"event_whenbroadcastreceived\","
        "\"topLevel\":true,\"next\":\"loop\",\"inputs\":{},\"fields\":"
        "{\"BROADCAST_OPTION\":[\"PING\",null]}}" +
        std::string(",\"loop\":") +
        block("control_repeat", nullptr,
              R"({"TIMES":[1,[4,"8"]],"SUBSTACK":[2,"inc"]})") +
        ",\"inc\":" + block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"1"]]})",
              R"({"VARIABLE":["s","s"]})") + "}";
    Runtime rt;
    const std::string project =
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    check(loadSb3(rt, project, nullptr).ok,
          "load LLVM coroutine broadcast restart fixture");
    auto* counter = rt.getTargetForStage()->lookupVariableById("s");
    if (!counter) return;
    counter->value = Value::num(0);
    rt.startBroadcast(u"PING");
    check(rt.threads.size() == 1,
          "broadcast starts one coroutine-ready receiver");
    if (rt.threads.empty()) return;
    Thread* previous = rt.threads.front();
    const auto* code =
        static_cast<const CompiledScript*>(previous->compiled);
    check(previous->isCompiled && code && code->coroResume,
          "broadcast receiver uses compiled LLVM coroutine");
    if (!previous->isCompiled || !code || !code->coroResume) return;
    rt.sequencer->stepThread(*previous);
    rt.sequencer->stepThread(*previous);
    check(cast::toNumber(counter->value) == 2 &&
          previous->jitCtx && previous->jitCtx->coroHandle,
          "old receiver commits two iterations before restart");
    rt.startBroadcast(u"PING");
    check(!previous->jitCtx,
          "broadcast restart destroys old suspended native frame");
    rt.runUntilDone(100);
    check(rt.threads.empty() && cast::toNumber(counter->value) == 10,
          "restarted receiver runs eight new iterations, no replay");
}

void testMegaBroadcastRepeatContinuation() {
    // Repeat(broadcast and wait) requires exactly one receiver launch per
    // iteration, with a Thread-owned join and remaining count. No native
    // Fiber stack is retained by either of the two green-flag senders.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* flag = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
    if (!flag || flag[0] != '1') return;
    const auto fixture = [&](Runtime& rt, bool receivers, bool zero) {
        const auto loop = [](const char* count, const char* body,
                             const char* next) {
            return block("control_repeat", next,
                std::string("{\"TIMES\":[1,[4,\"") + count +
                "\"]],\"SUBSTACK\":[2,\"" + body + "\"]}");
        };
        const auto change = [](const char* amount) {
            return block("data_changevariableby", nullptr,
                std::string("{\"VALUE\":[1,[4,\"") + amount + "\"]]}",
                R"({"VARIABLE":["s","s"]})");
        };
        const auto broadcast = [](const char* msg, const char* id) {
            return block("event_broadcastandwait", nullptr,
                std::string("{\"BROADCAST_INPUT\":[1,[11,\"") +
                    msg + "\",\"" + id + "\"]]}");
        };
        std::string blocks =
            "{\"hat0\":" + hat("loop0") +
            ",\"loop0\":" + loop(zero ? "0" : "3", "send0", "done0") +
            ",\"send0\":" + broadcast("PING", "ping") +
            ",\"done0\":" + change("10") +
            ",\"hat1\":" + hat("loop1") +
            ",\"loop1\":" + loop(zero ? "1" : "2", "send1", "done1") +
            ",\"send1\":" + broadcast("PONG", "pong") +
            ",\"done1\":" + change("20");
        if (receivers) {
            blocks +=
                ",\"recv0\":{\"opcode\":\"event_whenbroadcastreceived\","
                "\"topLevel\":true,\"next\":\"rxloop0\",\"inputs\":{},"
                "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",\"ping\"]}}";
            blocks += ",\"rxloop0\":" + loop("2", "rxinc0", nullptr) +
                ",\"rxinc0\":" + change("1");
            blocks +=
                ",\"recv1\":{\"opcode\":\"event_whenbroadcastreceived\","
                "\"topLevel\":true,\"next\":\"rxloop1\",\"inputs\":{},"
                "\"fields\":{\"BROADCAST_OPTION\":[\"PONG\",\"pong\"]}}";
            blocks += ",\"rxloop1\":" + loop("3", "rxinc1", nullptr) +
                ",\"rxinc1\":" + change("2");
        }
        blocks += "}";
        std::string stage = target("Stage", blocks, true);
        const std::string emptyBroadcasts = "\"broadcasts\":{}";
        const auto at = stage.find(emptyBroadcasts);
        if (at != std::string::npos)
            stage.replace(at, emptyBroadcasts.size(),
                "\"broadcasts\":{\"ping\":\"PING\",\"pong\":\"PONG\"}");
        return loadSb3(rt,
            "{\"targets\":[" + stage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr).ok;
    };
    Runtime rt;
    check(fixture(rt, true, false), "mega-bcast repeat fixture loads");
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    rt.greenFlag();
    auto find = [&](const char* id) -> Thread* {
        for (Thread* t : rt.threads) if (t->topBlock == id) return t;
        return nullptr;
    };
    Thread* first = find("hat0");
    Thread* second = find("hat1");
    check(first && second && first->megaDispatch &&
          first->megaDispatch == second->megaDispatch &&
          first->megaDispatch->yieldEntry,
          "mega-bcast repeat fuses both sender loops into one LLVM function");
    if (!first || !second || !first->megaDispatch || !sum) return;
    auto code = first->megaDispatch;
    rt.sequencer->stepThread(*first);
    const Thread* initialRecipient = first->megaJoinRefs.empty()
        ? nullptr : first->megaJoinRefs.front();
    check(cast::toNumber(sum->value) == 0 &&
          first->megaRemaining == 3 && first->megaPc == 1 &&
          first->megaBroadcastPending &&
          first->megaJoinRefs.size() == 1 &&
          first->jitCtx && !first->fiber,
          "mega-bcast repeat launches once then suspends without Fiber");
    rt.sequencer->stepThread(*first);
    check(first->megaRemaining == 3 &&
          first->megaJoinRefs.size() == 1 &&
          first->megaJoinRefs.front() == initialRecipient,
          "mega-bcast repeat cannot re-launch during an unfinished join");
    rt.runUntilDone(10000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 48,
          "mega-bcast repeats send 3 and 2 messages with exact joined effects");
    rt.greenFlag();
    first = find("hat0");
    check(first && first->megaDispatch == code,
          "mega-bcast repeat reuses compiled LLVM dispatcher after green flag");
    rt.runUntilDone(10000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 96,
          "mega-bcast repeat cached run has no duplicated receiver launches");

    Runtime empty;
    check(fixture(empty, false, true),
          "mega-bcast repeat zero-and-empty receiver fixture loads");
    Variable* zeroSum = empty.getTargetForStage()->lookupVariableById("s");
    empty.greenFlag();
    Thread* zeroSender = nullptr;
    for (Thread* t : empty.threads)
        if (t->topBlock == "hat0") zeroSender = t;
    check(zeroSender && zeroSender->megaDispatch,
          "mega-bcast repeat zero count still uses source-certified switch");
    if (!zeroSender || !zeroSum) return;
    empty.sequencer->stepThread(*zeroSender);
    check(cast::toNumber(zeroSum->value) == 10 &&
          zeroSender->megaJoinRefs.empty() &&
          !zeroSender->megaBroadcastPending &&
          !zeroSender->fiber,
          "mega-bcast zero repetitions immediately runs numeric suffix once");
    empty.runUntilDone(10000);
    check(empty.threads.empty() && cast::toNumber(zeroSum->value) == 30,
          "mega-bcast empty receivers and zero repeat have exact final state");
}

void testNativeStaticBroadcastWait() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;

    const std::string blocks =
        std::string("{\"hat\":") + hat("literal") +
        ",\"literal\":" +
        block("event_broadcastandwait", "dynamic",
              R"({"BROADCAST_INPUT":[1,[11,"PING","PING"]]})") +
        ",\"dynamic\":" +
        block("event_broadcastandwait", "finish",
              R"({"BROADCAST_INPUT":[2,"read-message"]})") +
        ",\"read-message\":" +
        block("data_variable", nullptr, "{}",
              R"({"VARIABLE":["message","message"]})") +
        ",\"finish\":" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"10"]]})",
              R"({"VARIABLE":["s","s"]})") +
        ",\"recv\":" +
        R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":"inc","inputs":{},"fields":{"BROADCAST_OPTION":["PING","PING"]}})" +
        ",\"inc\":" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"1"]]})",
              R"({"VARIABLE":["s","s"]})") + "}";
    std::string stage = target("Stage", blocks, true);
    const std::string vars = R"("variables":{"s":["s",""]})";
    const auto varPos = stage.find(vars);
    check(varPos != std::string::npos,
          "native static broadcast fixture has Stage variable schema");
    if (varPos == std::string::npos) return;
    stage.replace(varPos, vars.size(),
                  R"("variables":{"s":["s",0],"message":["message","PING"]})");
    const std::string bcasts = R"("broadcasts":{})";
    const auto bcastPos = stage.find(bcasts);
    check(bcastPos != std::string::npos,
          "native static broadcast fixture has broadcast table");
    if (bcastPos == std::string::npos) return;
    stage.replace(bcastPos, bcasts.size(),
                  R"("broadcasts":{"PING":"PING"})");

    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "native static/dynamic joined broadcast fixture loads");
    if (!loaded.ok) return;
    Target* owner = rt.getTargetForStage();
    Variable* sum = owner->lookupVariableById("s");
    Variable* message = owner->lookupVariableById("message");
    check(sum && message, "native joined broadcast Stage bindings exist");
    if (!sum || !message) return;
    const size_t fibersBefore = FiberPool::activeCount();
    rt.greenFlag();
    check(rt.threads.size() == 1 && rt.threads.front()->isCompiled,
          "single joined native sender compiles in eager LLVM");
    rt.runUntilDone(500);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 12 &&
          FiberPool::activeCount() == fibersBefore,
          "literal and dynamic broadcast each launch one joined receiver");
    message->value = Value::string(SStr::create(u"UNKNOWN"));
    rt.greenFlag();
    rt.runUntilDone(500);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 23,
          "dynamic reporter observes changed message; constant source stays PING");
    message->value = Value::string(SStr::create(u"PING"));
    rt.greenFlag();
    rt.runUntilDone(500);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 35 &&
          FiberPool::activeCount() == fibersBefore,
          "repeated native joined broadcasts preserve exact sequential effects");

    // A Unicode Stage broadcast namespace must use the exact historical
    // name-resolution rules. U+212A (KELVIN SIGN) lowercases to 'k' but its
    // uppercase mapping is not equivalent in every operation. The static
    // literal entry must NOT incorrectly take the ASCII bypass.
    const std::string unicodeBlocks =
        std::string("{\"hat\":") + hat("send") +
        ",\"send\":" +
        block("event_broadcastandwait", "suffix",
              R"({"BROADCAST_INPUT":[1,[11,"\u212A","kelvin"]]})") +
        ",\"suffix\":" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"10"]]})",
              R"({"VARIABLE":["s","s"]})") +
        ",\"recv\":" +
        R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":"change","inputs":{},"fields":{"BROADCAST_OPTION":["\u212A","kelvin"]}})" +
        ",\"change\":" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"1"]]})",
              R"({"VARIABLE":["s","s"]})") + "}";
    std::string unicodeStage = target("Stage", unicodeBlocks, true);
    const auto unicodeAt = unicodeStage.find(bcasts);
    check(unicodeAt != std::string::npos,
          "unicode native broadcast fixture has a broadcast table");
    if (unicodeAt == std::string::npos) return;
    unicodeStage.replace(unicodeAt, bcasts.size(),
                         R"("broadcasts":{"kelvin":"\u212A"})");
    Runtime unicode;
    const auto unicodeLoaded = loadSb3(unicode,
        "{\"targets\":[" + unicodeStage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(unicodeLoaded.ok, "Unicode literal joined broadcast fixture loads");
    if (!unicodeLoaded.ok) return;
    Variable* unicodeSum =
        unicode.getTargetForStage()->lookupVariableById("s");
    check(!unicode.stageBroadcastNamesAsciiOnly(),
          "Unicode Stage namespace forbids ASCII broadcast bypass");
    unicode.greenFlag();
    Thread* unicodeSender = unicode.threads.size() == 1
        ? unicode.threads.front() : nullptr;
    const auto* unicodeNative = unicodeSender && unicodeSender->isCompiled
        ? static_cast<const CompiledScript*>(unicodeSender->compiled)
        : nullptr;
    check(unicodeNative && unicodeNative->mayYield,
          "static-only joined broadcast must be compiled as a yielding Fiber");
    unicode.runUntilDone(500);
    check(unicodeSum && unicode.threads.empty() &&
          cast::toNumber(unicodeSum->value) == 11,
          "Unicode source literal retains exact joined receiver delivery");
}

void testMegaBroadcastWaitContinuation() {
    // A pair of senders must start their receivers once, yield without
    // a native Fiber, and resume the SAME common LLVM function after the
    // precise receiver join identities have left the runtime.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* flag = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
    if (!flag || flag[0] != '1') return;
    const auto sender = [](const char* message, const char* id,
                           const char* suffix) {
        return std::string("\"send") + suffix + "\":" +
            block("event_broadcastandwait",
                (std::string("done") + suffix).c_str(),
                std::string("{\"BROADCAST_INPUT\":[1,[11,\"") +
                    message + "\",\"" + id + "\"]]}") +
            ",\"done" + suffix + "\":" +
            block("data_changevariableby", nullptr,
                std::string("{\"VALUE\":[1,[4,\"") +
                    (std::string(suffix) == "0" ? "10" : "20") +
                    "\"]]}", R"({"VARIABLE":["s","s"]})");
    };
    const std::string senders =
        "\"hat0\":" + hat("send0") + "," +
        sender("PING", "ping", "0") + "," +
        "\"hat1\":" + hat("send1") + "," +
        sender("PONG", "pong", "1");
    const std::string receivers =
        ",\"recv0\":{\"opcode\":\"event_whenbroadcastreceived\","
        "\"topLevel\":true,\"next\":\"repeat0\",\"inputs\":{},"
        "\"fields\":{\"BROADCAST_OPTION\":[\"PING\",\"ping\"]}}" +
        std::string(",\"repeat0\":") +
        block("control_repeat", nullptr,
            R"({"TIMES":[1,[4,"3"]],"SUBSTACK":[2,"inc0"]})") +
        ",\"inc0\":" +
        block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})", R"({"VARIABLE":["s","s"]})") +
        ",\"recv1\":{\"opcode\":\"event_whenbroadcastreceived\","
        "\"topLevel\":true,\"next\":\"repeat1\",\"inputs\":{},"
        "\"fields\":{\"BROADCAST_OPTION\":[\"PONG\",\"pong\"]}}" +
        std::string(",\"repeat1\":") +
        block("control_repeat", nullptr,
            R"({"TIMES":[1,[4,"2"]],"SUBSTACK":[2,"inc1"]})") +
        ",\"inc1\":" +
        block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"2"]]})", R"({"VARIABLE":["s","s"]})");
    const auto load = [&](Runtime& runtime, bool hasReceiver) {
        std::string stage = target(
            "Stage", "{" + senders + (hasReceiver ? receivers : "") + "}",
            true);
        const std::string placeholder = "\"broadcasts\":{}";
        const auto at = stage.find(placeholder);
        if (at != std::string::npos)
            stage.replace(at, placeholder.size(),
                "\"broadcasts\":{\"ping\":\"PING\",\"pong\":\"PONG\"}");
        return loadSb3(runtime,
            "{\"targets\":[" + stage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr).ok;
    };
    Runtime rt;
    check(load(rt, true), "mega-bcast two senders and two receivers load");
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    rt.greenFlag();
    const auto find = [&](const char* top) -> Thread* {
        for (Thread* t : rt.threads) if (t->topBlock == top) return t;
        return nullptr;
    };
    Thread* first = find("hat0");
    Thread* second = find("hat1");
    check(first && second && first->megaDispatch &&
          first->megaDispatch == second->megaDispatch &&
          first->megaDispatch->yieldEntry &&
          first->megaDispatch->broadcastNames.size() == 2,
          "mega-bcast shares one LLVM function between two joined senders");
    if (!first || !second || !first->megaDispatch || !sum) return;
    const auto code = first->megaDispatch;
    rt.sequencer->stepThread(*first);
    const Thread* receiver = first->megaJoinRefs.empty()
        ? nullptr : first->megaJoinRefs.front();
    check(cast::toNumber(sum->value) == 0 &&
          first->megaJoinRefs.size() == 1 && first->megaPc == 1 &&
          first->jitCtx && !first->fiber,
          "mega-bcast first sender commits no suffix while recipient is alive");
    rt.sequencer->stepThread(*first);
    check(cast::toNumber(sum->value) == 0 &&
          first->megaJoinRefs.size() == 1 &&
          first->megaJoinRefs.front() == receiver,
          "mega-bcast repeat wait resumes without starting receiver twice");
    rt.sequencer->stepThread(*second);
    check(cast::toNumber(sum->value) == 0 &&
          second->megaJoinRefs.size() == 1 &&
          second->jitCtx && !second->fiber,
          "mega-bcast second sender has independent join list and PC");
    rt.runUntilDone(10000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 37,
          "mega-bcast two receive loops finish before sender numeric suffix");
    rt.greenFlag();
    first = find("hat0");
    check(first && first->megaDispatch == code,
          "mega-bcast second green flag reuses source-verified LLVM function");
    rt.runUntilDone(10000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 74,
          "mega-bcast cached restart launches each broadcast once");

    Runtime empty;
    check(load(empty, false), "mega-bcast no-receiver project loads");
    Variable* value = empty.getTargetForStage()->lookupVariableById("s");
    empty.greenFlag();
    Thread* senderWithoutReceiver = nullptr;
    for (Thread* t : empty.threads)
        if (t->topBlock == "hat0") senderWithoutReceiver = t;
    check(senderWithoutReceiver && senderWithoutReceiver->megaDispatch,
          "mega-bcast no-receiver still compiles shared LLVM switch");
    if (!senderWithoutReceiver || !value) return;
    empty.sequencer->stepThread(*senderWithoutReceiver);
    check(cast::toNumber(value->value) == 10 &&
          senderWithoutReceiver->megaJoinRefs.empty() &&
          !senderWithoutReceiver->fiber,
          "mega-bcast empty join resumes suffix in same scheduler selection");
    empty.runUntilDone(10000);
    check(empty.threads.empty() && cast::toNumber(value->value) == 30,
          "mega-bcast no-receiver exact final scalar parity");
}

void testMegaTailProcedureResume() {
    // Two separate green-flag Threads have their *only* statement be a
    // source-proven, parameterless, non-warp tail procedure. The native
    // dispatcher must resume across the call boundary without a Fiber or
    // separate procedure return-stack after a non-warp repeat yield.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* flag = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
    if (!flag || flag[0] != '1') return;
    const auto mutation = [](const char* name) {
        return std::string(R"({"tagName":"mutation","children":[],"proccode":")") +
            name + R"(","argumentids":"[]","argumentnames":"[]",)" +
            R"("argumentdefaults":"[]","warp":"false"})";
    };
    const auto call = [&](const char* name) {
        return std::string(R"({"opcode":"procedures_call","next":null,)" ) +
            R"("inputs":{},"fields":{},"mutation":)" + mutation(name) + "}";
    };
    const auto prototype = [&](const char* name) {
        return std::string(R"({"opcode":"procedures_prototype",)" ) +
            R"("inputs":{},"fields":{},"mutation":)" + mutation(name) + "}";
    };
    const auto definition = [](const char* top, const char* next) {
        return std::string(R"({"opcode":"procedures_definition",)" ) +
            R"("topLevel":true,"inputs":{"custom_block":[1,")" +
            top + R"("]},"fields":{},"next":")" + next + "\"}";
    };
    const std::string blocks =
        "{\"hat0\":" + hat("call0") +
        ",\"call0\":" + call("test_p0") +
        ",\"definition0\":" + definition("proto0", "loop0") +
        ",\"proto0\":" + prototype("test_p0") +
        ",\"loop0\":" + block("control_repeat", nullptr,
            R"({"TIMES":[1,[4,"3"]],"SUBSTACK":[2,"inc0"]})") +
        ",\"inc0\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})",
            R"({"VARIABLE":["s","s"]})") +
        ",\"hat1\":" + hat("call1") +
        ",\"call1\":" + call("test_p1") +
        ",\"definition1\":" + definition("proto1", "loop1") +
        ",\"proto1\":" + prototype("test_p1") +
        ",\"loop1\":" + block("control_repeat", nullptr,
            R"({"TIMES":[1,[4,"2"]],"SUBSTACK":[2,"inc1"]})") +
        ",\"inc1\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"2"]]})",
            R"({"VARIABLE":["s","s"]})") + "}";
    Runtime rt;
    auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega-tail-procedure source fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    const auto find = [&](const char* name) -> Thread* {
        for (Thread* t : rt.threads)
            if (t->topBlock == name) return t;
        return nullptr;
    };
    const size_t beforeFibers = FiberPool::activeCount();
    rt.greenFlag();
    Thread* first = find("hat0");
    Thread* second = find("hat1");
    check(first && second && first->megaDispatch &&
          first->megaDispatch == second->megaDispatch &&
          first->megaDispatch->yieldEntry,
          "mega-tail-procedure uses one LLVM switch across both procedure boundaries");
    if (!first || !second || !first->megaDispatch || !sum) return;
    const auto code = first->megaDispatch;
    rt.sequencer->stepThread(*first);
    check(cast::toNumber(sum->value) == 1 && first->megaRemaining == 2 &&
          first->megaPc % 2 == 1 && !first->fiber,
          "mega-tail-procedure first repeat iteration resumes without Fiber");
    rt.sequencer->stepThread(*second);
    check(cast::toNumber(sum->value) == 3 && second->megaRemaining == 1 &&
          second->megaPc % 2 == 1 && !second->fiber,
          "mega-tail-procedure second source has independent saved PC and counter");
    rt.runUntilDone(200);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 7 &&
          FiberPool::activeCount() == beforeFibers,
          "mega-tail-procedure returns from both Scratch procedures exactly once");
    rt.greenFlag();
    first = find("hat0");
    check(first && first->megaDispatch == code,
          "mega-tail-procedure source-verified compiled function is reusable");
    rt.runUntilDone(200);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 14,
          "mega-tail-procedure second invocation has no repeated native effects");
}


void testMegaNonTailProcedureReturnPc() {
    // An argumentless non-warp procedure returns into a scalar suffix in
    // its caller. Repeat, broadcast-wait, and an unsupported two-statement
    // caller coexist; only the first two kinds may enter the native switch.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* flag = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
    if (!flag || flag[0] != '1') return;
    const auto mutation = [](const char* name) {
        return std::string(R"({"tagName":"mutation","children":[],"proccode":")") +
            name + R"(","argumentids":"[]","argumentnames":"[]",)" +
            R"("argumentdefaults":"[]","warp":"false"})";
    };
    const auto call = [&](const char* name, const char* next) {
        return std::string(R"({"opcode":"procedures_call","next":)") +
            (next ? "\"" + std::string(next) + "\"" : "null") +
            R"(,"inputs":{},"fields":{},"mutation":)" +
            mutation(name) + "}";
    };
    const auto prototype = [&](const char* name) {
        return std::string(R"({"opcode":"procedures_prototype",)" ) +
            R"("inputs":{},"fields":{},"mutation":)" + mutation(name) + "}";
    };
    const auto definition = [](const char* proto, const char* body) {
        return std::string(R"({"opcode":"procedures_definition",)" ) +
            R"("topLevel":true,"inputs":{"custom_block":[1,")" +
            proto + R"("]},"fields":{},"next":")" + body + "\"}";
    };
    const auto add = [](const char* next, const char* delta) {
        return block("data_changevariableby", next,
            std::string(R"({"VALUE":[1,[4,")") + delta + R"("]]})",
            R"({"VARIABLE":["s","s"]})");
    };
    const auto loop = [](const char* body, const char* count) {
        return block("control_repeat", nullptr,
            std::string(R"({"TIMES":[1,[4,")") + count +
            R"("]],"SUBSTACK":[2,")" + body + R"("]})");
    };
    std::string blocks = "{";
    const auto append = [&](const char* name, const std::string& value) {
        if (blocks.size() > 1) blocks += ",";
        blocks += "\"" + std::string(name) + "\":" + value;
    };
    append("hat0", hat("call0"));
    append("call0", call("non_tail_0", "after0"));
    append("after0", add(nullptr, "10"));
    append("def0", definition("proto0", "loop0"));
    append("proto0", prototype("non_tail_0"));
    append("loop0", loop("inc0", "3"));
    append("inc0", add(nullptr, "1"));

    append("hat1", hat("call1"));
    append("call1", call("non_tail_1", "after1"));
    append("after1", add(nullptr, "20"));
    append("def1", definition("proto1", "loop1"));
    append("proto1", prototype("non_tail_1"));
    append("loop1", loop("inc1", "2"));
    append("inc1", add(nullptr, "2"));

    append("hat2", hat("call2"));
    append("call2", call("non_tail_broadcast", "after2"));
    append("after2", add(nullptr, "30"));
    append("def2", definition("proto2", "send2"));
    append("proto2", prototype("non_tail_broadcast"));
    append("send2", block("event_broadcastandwait", "inner2",
        R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})"));
    append("inner2", add(nullptr, "5"));

    // Not source-proven: two statements after the procedure. It must
    // preserve the normal execution path even though the target is shared.
    append("hat3", hat("call3"));
    append("call3", call("unsupported_caller", "after3"));
    append("after3", add("after3b", "100"));
    append("after3b", add(nullptr, "200"));
    append("def3", definition("proto3", "loop3"));
    append("proto3", prototype("unsupported_caller"));
    append("loop3", loop("inc3", "1"));
    append("inc3", add(nullptr, "1"));

    // A zero-iteration procedure must still return to its caller without
    // executing the repeat body or waiting for an extra scheduler tick.
    append("hat4", hat("call4"));
    append("call4", call("zero_repeat_return", "after4"));
    append("after4", add(nullptr, "40"));
    append("def4", definition("proto4", "loop4"));
    append("proto4", prototype("zero_repeat_return"));
    append("loop4", loop("inc4", "0"));
    append("inc4", add(nullptr, "999"));

    // Nested non-tail chain: flag -> P (+100) -> Q (+10) -> repeat(+2).
    // The two return PCs must survive every scheduler yield in Q.
    append("hat5", hat("call5"));
    append("call5", call("nested_outer", "after5"));
    append("after5", add(nullptr, "100"));
    append("def5", definition("proto5", "call6"));
    append("proto5", prototype("nested_outer"));
    append("call6", call("nested_inner", "after6"));
    append("after6", add(nullptr, "10"));
    append("def6", definition("proto6", "loop5"));
    append("proto6", prototype("nested_inner"));
    append("loop5", loop("inc5", "2"));
    append("inc5", add(nullptr, "2"));

    // Nested broadcast-and-wait and two levels of return continuations.
    append("hat6", hat("call7"));
    append("call7", call("nested_bcast_outer", "after7"));
    append("after7", add(nullptr, "70"));
    append("def7", definition("proto7", "call8"));
    append("proto7", prototype("nested_bcast_outer"));
    append("call8", call("nested_bcast_inner", "after8"));
    append("after8", add(nullptr, "7"));
    append("def8", definition("proto8", "send8"));
    append("proto8", prototype("nested_bcast_inner"));
    append("send8", block("event_broadcastandwait", "inner8",
        R"({"BROADCAST_INPUT":[1,[11,"NEST","nest"]]})"));
    append("inner8", add(nullptr, "3"));
    append("recv8",
        R"({"opcode":"event_whenbroadcastreceived","topLevel":true,)"
        R"("next":"recvLoop8","inputs":{},"fields":)"
        R"({"BROADCAST_OPTION":["NEST","nest"]}})");
    append("recvLoop8", loop("recvInc8", "3"));
    append("recvInc8", add(nullptr, "6"));

    // The inner procedure is a tail call: only the outer +1000 suffix
    // occupies the return stack, not the tail-only intermediate frame.
    append("hat7", hat("call9"));
    append("call9", call("nested_tail_outer", "after9"));
    append("after9", add(nullptr, "1000"));
    append("def9", definition("proto9", "call10"));
    append("proto9", prototype("nested_tail_outer"));
    append("call10", call("nested_tail_inner", nullptr));
    append("def10", definition("proto10", "loop7"));
    append("proto10", prototype("nested_tail_inner"));
    append("loop7", loop("inc7", "1"));
    append("inc7", add(nullptr, "1"));

    // Maximum native return-stack depth: four non-tail procedures.
    append("hat8", hat("call11"));
    append("call11", call("quad_a", "after11"));
    append("after11", add(nullptr, "10000"));
    append("def11", definition("proto11", "call12"));
    append("proto11", prototype("quad_a"));
    append("call12", call("quad_b", "after12"));
    append("after12", add(nullptr, "1000"));
    append("def12", definition("proto12", "call13"));
    append("proto12", prototype("quad_b"));
    append("call13", call("quad_c", "after13"));
    append("after13", add(nullptr, "100"));
    append("def13", definition("proto13", "call14"));
    append("proto13", prototype("quad_c"));
    append("call14", call("quad_d", "after14"));
    append("after14", add(nullptr, "10"));
    append("def14", definition("proto14", "loop8"));
    append("proto14", prototype("quad_d"));
    append("loop8", loop("inc8", "1"));
    append("inc8", add(nullptr, "1"));

    // Five levels are deliberately ineligible. Normal JIT still runs them.
    append("hat9", hat("call15"));
    append("call15", call("depth5_a", nullptr));
    append("def15", definition("proto15", "call16"));
    append("proto15", prototype("depth5_a"));
    append("call16", call("depth5_b", nullptr));
    append("def16", definition("proto16", "call17"));
    append("proto16", prototype("depth5_b"));
    append("call17", call("depth5_c", nullptr));
    append("def17", definition("proto17", "call18"));
    append("proto17", prototype("depth5_c"));
    append("call18", call("depth5_d", nullptr));
    append("def18", definition("proto18", "call19"));
    append("proto18", prototype("depth5_d"));
    append("call19", call("depth5_e", nullptr));
    append("def19", definition("proto19", "loop9"));
    append("proto19", prototype("depth5_e"));
    append("loop9", loop("inc9", "1"));
    append("inc9", add(nullptr, "1"));
    blocks += "}";

    std::string stage = target("Stage", blocks, true);
    const std::string placeholder = "\"broadcasts\":{}";
    const auto index = stage.find(placeholder);
    if (index != std::string::npos)
        stage.replace(index, placeholder.size(),
                      "\"broadcasts\":{\"ping\":\"PING\",\"nest\":\"NEST\"}");
    Runtime rt;
    auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega non-tail procedure fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    const auto find = [&](const char* id) -> Thread* {
        for (Thread* thread : rt.threads)
            if (thread->topBlock == id) return thread;
        return nullptr;
    };
    const size_t fibersBefore = FiberPool::activeCount();
    rt.greenFlag();
    Thread* first = find("hat0");
    Thread* second = find("hat1");
    Thread* third = find("hat2");
    Thread* fourth = find("hat4");
    Thread* nested = find("hat5");
    Thread* nestedBroadcast = find("hat6");
    Thread* nestedTail = find("hat7");
    Thread* quad = find("hat8");
    Thread* excessiveDepth = find("hat9");
    Thread* unsupported = find("hat3");
    check(first && second && third && fourth && nested &&
          nestedBroadcast && nestedTail && quad && excessiveDepth &&
          unsupported && first->megaDispatch &&
          first->megaDispatch == second->megaDispatch &&
          first->megaDispatch == third->megaDispatch &&
          first->megaDispatch == fourth->megaDispatch &&
          first->megaDispatch == nested->megaDispatch &&
          first->megaDispatch == nestedBroadcast->megaDispatch &&
          first->megaDispatch == nestedTail->megaDispatch &&
          first->megaDispatch == quad->megaDispatch &&
          !unsupported->megaDispatch && !excessiveDepth->megaDispatch &&
          first->megaDispatch->yieldEntry &&
          first->megaDispatch->entries == 8,
          "mega native return stack accepts four levels, rejects five and bad suffix");
    if (!first || !second || !third || !fourth || !nested ||
        !nestedBroadcast || !nestedTail || !quad ||
        !excessiveDepth || !unsupported ||
        !first->megaDispatch || !sum) return;
    const auto code = first->megaDispatch;
    rt.sequencer->stepThread(*first);
    rt.sequencer->stepThread(*second);
    check(cast::toNumber(sum->value) == 3 &&
          first->megaRemaining == 2 && second->megaRemaining == 1 &&
          first->megaPc == 1 && second->megaPc == 3 &&
          !first->fiber && !second->fiber,
          "mega non-tail suspends inside both called procedures");
    rt.sequencer->stepThread(*first); // 2nd body
    rt.sequencer->stepThread(*first); // 3rd body
    check(cast::toNumber(sum->value) == 5 &&
          first->megaRemaining == 0 && first->megaPc == 1,
          "mega non-tail caller suffix has not run before callee returns");
    rt.sequencer->stepThread(*first); // return PC and caller suffix
    check(cast::toNumber(sum->value) == 15,
          "mega non-tail return PC applies caller suffix exactly once");
    rt.sequencer->stepThread(*fourth);
    check(cast::toNumber(sum->value) == 55 && !fourth->fiber,
          "mega non-tail zero-repeat skips body and returns synchronously");

    rt.sequencer->stepThread(*nested);
    check(cast::toNumber(sum->value) == 57 &&
          nested->megaReturnDepth == 2 &&
          nested->megaReturnPcs[0] != nested->megaReturnPcs[1] &&
          nested->megaRemaining == 1 && !nested->fiber,
          "mega nested return PC stack survives first inner repeat yield");
    rt.sequencer->stepThread(*nested);
    check(cast::toNumber(sum->value) == 59 &&
          nested->megaReturnDepth == 2 && nested->megaRemaining == 0,
          "mega nested frames remain intact at the second inner repeat yield");
    rt.sequencer->stepThread(*nested);
    check(cast::toNumber(sum->value) == 169,
          "mega nested return PC pops innermost +10 then outermost +100");

    rt.sequencer->stepThread(*nestedTail);
    check(nestedTail->megaReturnDepth == 1 &&
          nestedTail->megaRemaining == 0 && !nestedTail->fiber,
          "mega nested tail call does not add an unnecessary return frame");
    rt.sequencer->stepThread(*quad);
    check(quad->megaReturnDepth == Thread::MegaReturnCapacity &&
          quad->megaReturnPcs[0] != quad->megaReturnPcs[3] &&
          quad->megaRemaining == 0 && !quad->fiber,
          "mega four-level return stack survives the inner repeat yield");
    const double beforeBroadcast = cast::toNumber(sum->value);
    rt.sequencer->stepThread(*nestedBroadcast);
    check(nestedBroadcast->megaReturnDepth == 2 &&
          nestedBroadcast->megaJoinRefs.size() == 1 &&
          nestedBroadcast->megaPc % 2 == 1 &&
          cast::toNumber(sum->value) == beforeBroadcast &&
          !nestedBroadcast->fiber,
          "mega nested broadcast keeps two callers and starts one live receiver");
    rt.sequencer->stepThread(*nestedBroadcast);
    check(nestedBroadcast->megaReturnDepth == 2 &&
          nestedBroadcast->megaJoinRefs.size() == 1 &&
          cast::toNumber(sum->value) == beforeBroadcast,
          "mega nested broadcast does not run caller suffix while receiver waits");
    rt.runUntilDone(200);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 12738 &&
          FiberPool::activeCount() == fibersBefore,
          "mega nested, joined receiver, four-level and fallback complete once");
    rt.greenFlag();
    first = find("hat0");
    check(first && first->megaDispatch == code,
          "mega non-tail native return PC survives compilation cache");
    rt.runUntilDone(200);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 25476,
          "mega four-level second run preserves all return effects exactly once");
}


void testMegaBroadcastUntilContinuation() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto yes = [](const char* name) {
        const char* flag = std::getenv(name);
        return flag && flag[0] == '1';
    };
    if (!yes("XYO_JIT_MEGA_BATCH_YIELD") ||
        !yes("XYO_JIT_MEGA_SINGLE_SENDER") ||
        !yes("XYO_JIT_MEGA_UNTIL_BCAST")) return;

    // Four variants exercise operand ordering, operator_not and equality.
    // The comparison is reevaluated AFTER each joined receiver.
    for (int variant = 0; variant < 4; ++variant) {
        const char* compare = variant == 2 ? "operator_lt"
                            : variant == 3 ? "operator_equals"
                            : "operator_gt";
        const std::string compareInputs = variant == 2
            ? R"({"OPERAND1":[1,[4,"2"]],"OPERAND2":[2,"countRead"]})"
            : variant == 3
                ? R"({"OPERAND1":[2,"countRead"],"OPERAND2":[1,[4,"3"]]})"
                : R"({"OPERAND1":[2,"countRead"],"OPERAND2":[1,[4,"2"]]})";
        const std::string condition =
            variant == 1 ? "\"notCondition\"" : "\"compare\"";
        std::string blocks =
            std::string("{\"sender\":") + hat("until") +
            ",\"until\":" + block("control_repeat_until", "suffix",
                std::string("{\"CONDITION\":[2,") + condition +
                R"(],"SUBSTACK":[2,"send"]})") +
            ",\"compare\":" + block(compare, nullptr, compareInputs) +
            (variant == 1
                ? std::string(",\"notCondition\":") +
                    block("operator_not", nullptr,
                        R"({"OPERAND":[2,"compare"]})")
                : std::string()) +
            ",\"countRead\":" + block("data_variable", nullptr, "{}",
                R"({"VARIABLE":["count","count"]})") +
            ",\"send\":" + block("event_broadcastandwait", nullptr,
                R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})") +
            ",\"suffix\":" + block("data_changevariableby", nullptr,
                R"({"VALUE":[1,[4,"10"]]})",
                R"({"VARIABLE":["s","s"]})") +
            ",\"receiver\":" +
            R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":"receive","inputs":{},"fields":{"BROADCAST_OPTION":["PING","ping"]}})" +
            ",\"receive\":" + block("data_changevariableby", nullptr,
                R"({"VALUE":[1,[4,"1"]]})",
                R"({"VARIABLE":["count","count"]})") + "}";
        // operator_not(operator_gt(count,2)) would be true initially,
        // so variant 1 uses NOT(LT(count,3)) with the same exit at count=3.
        if (variant == 1) {
            const std::string gt = R"("opcode":"operator_gt")";
            const auto pos = blocks.find(gt);
            check(pos != std::string::npos,
                  "until fixture has comparison to negate");
            if (pos != std::string::npos)
                blocks.replace(pos, gt.size(),
                               R"("opcode":"operator_lt")");
            const std::string num = R"("OPERAND2":[1,[4,"2"]])";
            const auto at = blocks.find(num);
            check(at != std::string::npos, "negated until fixture literal");
            if (at != std::string::npos)
                blocks.replace(at, num.size(),
                               R"("OPERAND2":[1,[4,"3"]])");
        }

        std::string stage = target("Stage", blocks, true);
        const std::string oldVars = R"("variables":{"s":["s",""]})";
        auto where = stage.find(oldVars);
        check(where != std::string::npos,
              "until variable fixture has Stage defaults");
        if (where == std::string::npos) return;
        stage.replace(where, oldVars.size(),
                      R"("variables":{"s":["s",0],"count":["count",0]})");
        const std::string oldBroadcasts = R"("broadcasts":{})";
        where = stage.find(oldBroadcasts);
        check(where != std::string::npos,
              "until fixture has broadcast namespace");
        if (where == std::string::npos) return;
        stage.replace(where, oldBroadcasts.size(),
                      R"("broadcasts":{"ping":"PING"})");
        Runtime rt;
        const auto loaded = loadSb3(rt,
            "{\"targets\":[" + stage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(loaded.ok, "repeat-until joined receiver fixture loads");
        if (!loaded.ok) return;
        Variable* count = rt.getTargetForStage()->lookupVariableById("count");
        Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
        check(count && sum, "until receiver variables resolve");
        if (!count || !sum) return;
        const size_t beforeFibers = FiberPool::activeCount();
        rt.greenFlag();
        Thread* sender = nullptr;
        for (Thread* t : rt.threads)
            if (t->topBlock == "sender") sender = t;
        check(sender && sender->megaDispatch &&
              sender->megaDispatch->yieldEntry && !sender->fiber,
              "until source receives real LLVM continuation without Fiber");
        if (!sender || !sender->megaDispatch) return;
        auto code = sender->megaDispatch;
        rt.sequencer->stepThread(*sender);
        check(sender->megaPc == 1 &&
              sender->megaJoinRefs.size() == 1 &&
              cast::toNumber(count->value) == 0 &&
              cast::toNumber(sum->value) == 0,
              "until evaluates false and launches exactly one recipient");
        rt.sequencer->stepThread(*sender);
        check(sender->megaJoinRefs.size() == 1 &&
              cast::toNumber(sum->value) == 0,
              "until sender cannot run suffix before joined recipient");
        rt.runUntilDone(2000);
        check(rt.threads.empty() && cast::toNumber(count->value) == 3 &&
              cast::toNumber(sum->value) == 10 &&
              FiberPool::activeCount() == beforeFibers,
              "until resumes condition after three real joins and exits once");

        // The condition is initially true. No receiver may be launched.
        count->value = Value::num(3);
        rt.greenFlag();
        sender = nullptr;
        for (Thread* t : rt.threads)
            if (t->topBlock == "sender") sender = t;
        check(sender && sender->megaDispatch == code,
              "repeat-until cached native PC survives green-flag restart");
        rt.runUntilDone(2000);
        check(rt.threads.empty() && cast::toNumber(count->value) == 3 &&
              cast::toNumber(sum->value) == 20 &&
              FiberPool::activeCount() == beforeFibers,
              "initially true until condition skips broadcast but runs suffix");
    }
}

void testMegaUntilWallClockContinuation() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_SINGLE_SENDER") ||
        !enabled("XYO_JIT_MEGA_UNTIL_BCAST")) return;
    // The compiler must read daysSince2000 on the actual condition PC,
    // not merely once when the script or Runtime was initialized.
    for (int direction = 0; direction < 2; ++direction) {
        const std::string inputs = direction == 0
            ? R"({"OPERAND1":[2,"days"],"OPERAND2":[2,"deadlineRead"]})"
            : R"({"OPERAND1":[2,"deadlineRead"],"OPERAND2":[2,"days"]})";
        const std::string blocks =
            std::string("{\"sender\":") + hat("until") +
            ",\"until\":" + block("control_repeat_until", "suffix",
                R"({"CONDITION":[2,"condition"],"SUBSTACK":[2,"send"]})") +
            ",\"condition\":" +
                block(direction == 0 ? "operator_gt" : "operator_lt",
                      nullptr, inputs) +
            ",\"days\":" + block("sensing_dayssince2000", nullptr) +
            ",\"deadlineRead\":" + block("data_variable", nullptr, "{}",
                R"({"VARIABLE":["deadline","deadline"]})") +
            ",\"send\":" + block("event_broadcastandwait", nullptr,
                R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})") +
            ",\"suffix\":" + block("data_changevariableby", nullptr,
                R"({"VALUE":[1,[4,"10"]]})",
                R"({"VARIABLE":["s","s"]})") +
            ",\"receiver\":" +
            R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":"receive","inputs":{},"fields":{"BROADCAST_OPTION":["PING","ping"]}})" +
            ",\"receive\":" + block("data_changevariableby", nullptr,
                R"({"VALUE":[1,[4,"1"]]})",
                R"({"VARIABLE":["count","count"]})") + "}";
        std::string stage = target("Stage", blocks, true);
        auto pos = stage.find(R"("variables":{"s":["s",""]})");
        check(pos != std::string::npos, "clock until Stage has variables");
        if (pos == std::string::npos) return;
        stage.replace(pos, std::string(R"("variables":{"s":["s",""]})").size(),
                      R"("variables":{"s":["s",0],"count":["count",0],"deadline":["deadline",0]})");
        pos = stage.find(R"("broadcasts":{})");
        check(pos != std::string::npos, "clock until Stage has broadcasts");
        if (pos == std::string::npos) return;
        stage.replace(pos, std::string(R"("broadcasts":{})").size(),
                      R"("broadcasts":{"ping":"PING"})");
        Runtime rt;
        const auto loaded = loadSb3(rt,
            "{\"targets\":[" + stage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(loaded.ok, "clock until receiver fixture loads");
        if (!loaded.ok) return;
        Target* stageTarget = rt.getTargetForStage();
        Variable* deadline =
            stageTarget->lookupVariableById("deadline");
        Variable* sum = stageTarget->lookupVariableById("s");
        Variable* count = stageTarget->lookupVariableById("count");
        check(deadline && sum && count,
              "clock until variables available");
        if (!deadline || !sum || !count) return;
        const size_t fibersBefore = FiberPool::activeCount();
        rt.greenFlag();
        Thread* sender = nullptr;
        for (Thread* t : rt.threads)
            if (t->topBlock == "sender") sender = t;
        check(sender && sender->megaDispatch && !sender->fiber,
              "clock until uses source-certified LLVM PC");
        if (!sender || !sender->megaDispatch) return;
        const auto compiled = sender->megaDispatch;
        // Assign the threshold only AFTER JIT compilation/greenFlag,
        // so LLVM startup latency cannot consume the timer budget.
        deadline->value = Value::num(rt::daysSince2000() + 0.2 / 86400.0);
        rt.sequencer->stepThread(*sender);
        check(sender->megaPc == 1 && sender->megaJoinRefs.size() == 1 &&
              cast::toNumber(count->value) == 0 &&
              cast::toNumber(sum->value) == 0,
              "fresh deadline starts exactly one joined broadcast");
        rt.sequencer->stepThread(*sender);
        check(sender->megaJoinRefs.size() == 1 &&
              cast::toNumber(sum->value) == 0,
              "live clock cannot skip an unfinished recipient");
        std::this_thread::sleep_for(std::chrono::milliseconds(240));
        rt.runUntilDone(2000);
        check(rt.threads.empty() && cast::toNumber(count->value) == 1 &&
              cast::toNumber(sum->value) == 10 &&
              FiberPool::activeCount() == fibersBefore,
              "clock is resampled after join; time-based loop exits once");
        // Already-past deadline on the next run: no receiver launch.
        deadline->value = Value::num(rt::daysSince2000() - 1.0);
        rt.greenFlag();
        sender = nullptr;
        for (Thread* t : rt.threads)
            if (t->topBlock == "sender") sender = t;
        check(sender && sender->megaDispatch == compiled,
              "clock continuation uses cached LLVM dispatcher");
        rt.runUntilDone(2000);
        check(rt.threads.empty() && cast::toNumber(count->value) == 1 &&
              cast::toNumber(sum->value) == 20 &&
              FiberPool::activeCount() == fibersBefore,
              "past clock deadline skips all new broadcasts");
    }
}

void testMegaUntilElapsedMillisContinuation() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_SINGLE_SENDER") ||
        !enabled("XYO_JIT_MEGA_UNTIL_BCAST")) return;
    // Exact ScratchBench reporter tree:
    // NOT(((daysSince2000() - b_t0) * 86400000) < b_budget)
    const std::string blocks =
        std::string("{\"sender\":") + hat("until") +
        ",\"until\":" + block("control_repeat_until", "suffix",
            R"({"CONDITION":[2,"not"],"SUBSTACK":[2,"send"]})") +
        ",\"not\":" + block("operator_not", nullptr,
            R"({"OPERAND":[2,"compare"]})") +
        ",\"compare\":" + block("operator_lt", nullptr,
            R"({"OPERAND1":[2,"multiply"],"OPERAND2":[2,"budgetRead"]})") +
        ",\"multiply\":" + block("operator_multiply", nullptr,
            R"({"NUM1":[2,"subtract"],"NUM2":[1,[4,"86400000"]]})") +
        ",\"subtract\":" + block("operator_subtract", nullptr,
            R"({"NUM1":[2,"days"],"NUM2":[2,"startRead"]})") +
        ",\"days\":" + block("sensing_dayssince2000", nullptr) +
        ",\"startRead\":" + block("data_variable", nullptr, "{}",
            R"({"VARIABLE":["b_t0","b_t0"]})") +
        ",\"budgetRead\":" + block("data_variable", nullptr, "{}",
            R"({"VARIABLE":["b_budget","b_budget"]})") +
        ",\"send\":" + block("event_broadcastandwait", nullptr,
            R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})") +
        ",\"suffix\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"10"]]})",
            R"({"VARIABLE":["s","s"]})") +
        ",\"receiver\":" +
        R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":"receive","inputs":{},"fields":{"BROADCAST_OPTION":["PING","ping"]}})" +
        ",\"receive\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})",
            R"({"VARIABLE":["count","count"]})") + "}";
    const auto prepareStage = [&](const std::string& sourceBlocks) {
        std::string stage = target("Stage", sourceBlocks, true);
        const std::string defaults = R"("variables":{"s":["s",""]})";
        const auto start = stage.find(defaults);
        if (start == std::string::npos) return std::string();
        stage.replace(start, defaults.size(),
            R"("variables":{"s":["s",0],"count":["count",0],"b_t0":["b_t0",0],"b_budget":["b_budget",0]})");
        const std::string oldBroadcasts = R"("broadcasts":{})";
        const auto bc = stage.find(oldBroadcasts);
        if (bc == std::string::npos) return std::string();
        stage.replace(bc, oldBroadcasts.size(),
                      R"("broadcasts":{"ping":"PING"})");
        return stage;
    };
    const std::string stage = prepareStage(blocks);
    check(!stage.empty(), "elapsed-ms until fixture has Stage metadata");
    if (stage.empty()) return;
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "elapsed-ms until receiver fixture loads");
    if (!loaded.ok) return;
    auto* target = rt.getTargetForStage();
    auto* baseline = target->lookupVariableById("b_t0");
    auto* budget = target->lookupVariableById("b_budget");
    auto* counter = target->lookupVariableById("count");
    auto* sum = target->lookupVariableById("s");
    check(baseline && budget && counter && sum,
          "elapsed-ms until Stage variables resolve");
    if (!baseline || !budget || !counter || !sum) return;
    const size_t beforeFibers = FiberPool::activeCount();
    const auto senderThread = [&]() -> Thread* {
        for (Thread* thread : rt.threads)
            if (thread->topBlock == "sender") return thread;
        return nullptr;
    };
    rt.greenFlag();
    Thread* sender = senderThread();
    check(sender && sender->megaDispatch && !sender->fiber,
          "exact real ScratchBench elapsed-ms shape enters LLVM Mega");
    if (!sender || !sender->megaDispatch) return;
    auto compiled = sender->megaDispatch;
    baseline->value = Value::num(rt::daysSince2000());
    budget->value = Value::num(180);
    rt.sequencer->stepThread(*sender);
    check(sender->megaPc == 1 && sender->megaJoinRefs.size() == 1 &&
          cast::toNumber(counter->value) == 0 &&
          cast::toNumber(sum->value) == 0,
          "elapsed-ms condition false starts exactly one broadcast");
    rt.sequencer->stepThread(*sender);
    check(sender->megaJoinRefs.size() == 1 &&
          cast::toNumber(sum->value) == 0,
          "elapsed-ms join does not replay the suffix before completion");
    std::this_thread::sleep_for(std::chrono::milliseconds(240));
    rt.runUntilDone(2000);
    check(rt.threads.empty() &&
          cast::toNumber(counter->value) == 1 &&
          cast::toNumber(sum->value) == 10 &&
          FiberPool::activeCount() == beforeFibers,
          "elapsed-ms condition reevaluates after real joined receiver");
    rt.greenFlag();
    sender = senderThread();
    check(sender && sender->megaDispatch == compiled,
          "elapsed-ms LLVM source revision cache survives second green flag");
    baseline->value = Value::num(rt::daysSince2000());
    budget->value = Value::num(-1);
    rt.runUntilDone(2000);
    check(rt.threads.empty() &&
          cast::toNumber(counter->value) == 1 &&
          cast::toNumber(sum->value) == 20 &&
          FiberPool::activeCount() == beforeFibers,
          "already expired elapsed budget launches no new receiver");

    // ScratchBench uses primitive input descriptors ([12, id]) for
    // b_t0/b_budget rather than explicit reporter Block IDs. Prove that
    // the SB3 loader's generated data_variable blocks are accepted too.
    std::string primitive = blocks;
    const std::string startInput = R"("NUM2":[2,"startRead"])";
    const std::string budgetInput = R"("OPERAND2":[2,"budgetRead"])";
    const auto primitiveStart = primitive.find(startInput);
    check(primitiveStart != std::string::npos,
          "primitive source fixture has explicit starting reporter");
    if (primitiveStart != std::string::npos)
        primitive.replace(primitiveStart, startInput.size(),
            R"("NUM2":[3,[12,"b_t0","b_t0"],[10,""]])");
    const auto primitiveBudget = primitive.find(budgetInput);
    check(primitiveBudget != std::string::npos,
          "primitive source fixture has explicit budget reporter");
    if (primitiveBudget != std::string::npos)
        primitive.replace(primitiveBudget, budgetInput.size(),
            R"("OPERAND2":[3,[12,"b_budget","b_budget"],[10,""]])");
    if (primitiveStart != std::string::npos &&
        primitiveBudget != std::string::npos) {
        Runtime primitiveRt;
        const std::string primitiveStage = prepareStage(primitive);
        const auto primitiveLoaded = loadSb3(primitiveRt,
            "{\"targets\":[" + primitiveStage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(primitiveLoaded.ok,
              "ScratchBench-style primitive reporters load");
        if (primitiveLoaded.ok) {
            auto* stage = primitiveRt.getTargetForStage();
            auto* t0 = stage->lookupVariableById("b_t0");
            auto* ms = stage->lookupVariableById("b_budget");
            auto* result = stage->lookupVariableById("s");
            check(t0 && ms && result,
                  "primitive ScratchBench reporter variables resolve");
            primitiveRt.greenFlag();
            Thread* withPrimitive = nullptr;
            for (Thread* thread : primitiveRt.threads)
                if (thread->topBlock == "sender")
                    withPrimitive = thread;
            check(withPrimitive && withPrimitive->megaDispatch &&
                  !withPrimitive->fiber,
                  "real SB3 primitive clock math receives Mega LLVM PC");
            if (t0 && ms && result && withPrimitive &&
                withPrimitive->megaDispatch) {
                t0->value = Value::num(rt::daysSince2000());
                ms->value = Value::num(-1);
                primitiveRt.runUntilDone(2000);
                check(primitiveRt.threads.empty() &&
                      cast::toNumber(result->value) == 10,
                      "primitive budget already elapsed: one suffix, no loop");
            }
        }
    }

    // Regression: changing the multiplication constant changes Scratch's
    // actual semantics, so the strict Mega proof MUST reject it.
    std::string changed = blocks;
    const std::string factor = R"([4,"86400000"])";
    const auto factorAt = changed.find(factor);
    check(factorAt != std::string::npos,
          "unsupported elapsed-scale negative control can be constructed");
    if (factorAt != std::string::npos) {
        changed.replace(factorAt, factor.size(), R"([4,"3600000"])");
        Runtime rejected;
        const auto badStage = prepareStage(changed);
        const auto badLoaded = loadSb3(rejected,
            "{\"targets\":[" + badStage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(badLoaded.ok, "unsupported elapsed-scale fixture loads");
        if (badLoaded.ok) {
            rejected.greenFlag();
            Thread* bad = nullptr;
            for (Thread* thread : rejected.threads)
                if (thread->topBlock == "sender") bad = thread;
            check(bad && !bad->megaDispatch,
                  "non-certified elapsed scale keeps the native Fiber path");
        }
    }
}

void testMegaUntilBoundedOuterRepeat() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto flag = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1';
    };
    if (!flag("XYO_JIT_MEGA_BATCH_YIELD") ||
        !flag("XYO_JIT_MEGA_SINGLE_SENDER") ||
        !flag("XYO_JIT_MEGA_UNTIL_BCAST") ||
        !flag("XYO_JIT_MEGA_UNTIL_OUTER")) return;

    // Each of three outer iterations resets its hit count and its own
    // clock baseline, waits for a real receiver, and then commits +10.
    // One final +100 happens only AFTER all three outer iterations.
    const std::string blocks =
        std::string("{\"sender\":") + hat("outer") +
        ",\"outer\":" + block("control_repeat", "outerSuffix",
            R"({"TIMES":[1,[4,"3"]],"SUBSTACK":[2,"reset"]})") +
        ",\"reset\":" + block("data_setvariableto", "start",
            R"({"VALUE":[1,[4,"0"]]})", R"({"VARIABLE":["hits","hits"]})") +
        ",\"start\":" + block("data_setvariableto", "until",
            R"({"VALUE":[2,"startClock"]})",
            R"({"VARIABLE":["b_t0","b_t0"]})") +
        ",\"startClock\":" + block("sensing_dayssince2000", nullptr) +
        ",\"until\":" + block("control_repeat_until", "innerSuffix",
            R"({"CONDITION":[2,"not"],"SUBSTACK":[2,"send"]})") +
        ",\"not\":" + block("operator_not", nullptr,
            R"({"OPERAND":[2,"compare"]})") +
        ",\"compare\":" + block("operator_lt", nullptr,
            R"({"OPERAND1":[2,"elapsed"],"OPERAND2":[2,"budgetRead"]})") +
        ",\"elapsed\":" + block("operator_multiply", nullptr,
            R"({"NUM1":[2,"subtract"],"NUM2":[1,[4,"86400000"]]})") +
        ",\"subtract\":" + block("operator_subtract", nullptr,
            R"({"NUM1":[2,"clock"],"NUM2":[2,"t0Read"]})") +
        ",\"clock\":" + block("sensing_dayssince2000", nullptr) +
        ",\"t0Read\":" + block("data_variable", nullptr, "{}",
            R"({"VARIABLE":["b_t0","b_t0"]})") +
        ",\"budgetRead\":" + block("data_variable", nullptr, "{}",
            R"({"VARIABLE":["b_budget","b_budget"]})") +
        ",\"send\":" + block("event_broadcastandwait", nullptr,
            R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})") +
        ",\"innerSuffix\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"10"]]})", R"({"VARIABLE":["sum","sum"]})") +
        ",\"outerSuffix\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"100"]]})", R"({"VARIABLE":["sum","sum"]})") +
        ",\"receiver\":" +
        R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":"incHits","inputs":{},"fields":{"BROADCAST_OPTION":["PING","ping"]}})" +
        ",\"incHits\":" + block("data_changevariableby", "incTotal",
            R"({"VALUE":[1,[4,"1"]]})", R"({"VARIABLE":["hits","hits"]})") +
        ",\"incTotal\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})", R"({"VARIABLE":["total","total"]})") +
        "}";
    const auto stageFor = [&](const std::string& source) {
        std::string stage = target("Stage", source, true);
        const std::string initial = R"("variables":{"s":["s",""]})";
        const auto v = stage.find(initial);
        if (v == std::string::npos) return std::string();
        stage.replace(v, initial.size(),
            R"("variables":{"sum":["sum",0],"hits":["hits",0],"total":["total",0],"b_t0":["b_t0",0],"b_budget":["b_budget",0]})");
        const std::string broadcasts = R"("broadcasts":{})";
        const auto p = stage.find(broadcasts);
        if (p == std::string::npos) return std::string();
        stage.replace(p, broadcasts.size(),
            R"("broadcasts":{"ping":"PING"})");
        return stage;
    };
    Runtime rt;
    const auto stage = stageFor(blocks);
    check(!stage.empty(), "outer until fixture has valid metadata");
    if (stage.empty()) return;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "outer until repeat fixture loads");
    if (!loaded.ok) return;
    auto* tgt = rt.getTargetForStage();
    auto* sum = tgt->lookupVariableById("sum");
    auto* hits = tgt->lookupVariableById("hits");
    auto* total = tgt->lookupVariableById("total");
    auto* t0 = tgt->lookupVariableById("b_t0");
    auto* budget = tgt->lookupVariableById("b_budget");
    check(sum && hits && total && t0 && budget,
          "outer until every Scratch scalar binding is resolved");
    if (!sum || !hits || !total || !t0 || !budget) return;
    const auto senderThread = [&]() -> Thread* {
        for (Thread* thread : rt.threads)
            if (thread->topBlock == "sender") return thread;
        return nullptr;
    };
    const size_t before = FiberPool::activeCount();
    rt.greenFlag();
    Thread* sender = senderThread();
    check(sender && sender->megaDispatch &&
          sender->megaDispatch->yieldEntry && !sender->fiber,
          "three-level outer/inner broadcast compiled as one LLVM PC");
    if (!sender || !sender->megaDispatch) return;
    auto cached = sender->megaDispatch;
    budget->value = Value::num(18);
    rt.sequencer->stepThread(*sender);
    check(sender->megaPc == 1 && sender->megaRemaining == 3 &&
          sender->megaJoinRefs.size() == 1 &&
          cast::toNumber(sum->value) == 0 &&
          cast::toNumber(hits->value) == 0,
          "outer initializes first clock and starts exactly one join");
    rt.runUntilDone(3000);
    check(rt.threads.empty() &&
          cast::toNumber(sum->value) == 130 &&
          cast::toNumber(total->value) >= 3 &&
          cast::toNumber(hits->value) >= 1 &&
          FiberPool::activeCount() == before,
          "three dynamic inner loops commit in order then outer suffix once");

    // A zero-duration clock budget is already elapsed on each new outer
    // iteration. Each inner suffix still runs once, but no send occurs.
    const double previousTotal = cast::toNumber(total->value);
    budget->value = Value::num(-1);
    rt.greenFlag();
    sender = senderThread();
    check(sender && sender->megaDispatch == cached,
          "outer until code cache survives source-consistent green flag");
    rt.runUntilDone(2000);
    check(rt.threads.empty() &&
          cast::toNumber(total->value) == previousTotal &&
          cast::toNumber(hits->value) == 0 &&
          cast::toNumber(sum->value) == 260 &&
          FiberPool::activeCount() == before,
          "each outer iteration reinitializes variables before zero-broadcast exit");

    if (flag("XYO_JIT_MEGA_UNTIL_POST_SET")) {
        const std::string extraVars =
            R"("b_budget":["b_budget",0])";
        const std::string replacement =
            R"("b_budget":["b_budget",0],"b_t1":["b_t1",0],"b_n":["b_n",-1])";
        // Real ScratchBench post-until measurement order:
        // set b_t1 to daysSince2000()
        // set b_n to hits (latest receiver-updated variable)
        // change sum by 10
        // The post-snapshots must run only after the join completes.
        std::string postBlocks = blocks;
        const std::string oldInner = block("control_repeat_until", "innerSuffix",
            R"({"CONDITION":[2,"not"],"SUBSTACK":[2,"send"]})");
        const std::string newInner = block("control_repeat_until", "postClock",
            R"({"CONDITION":[2,"not"],"SUBSTACK":[2,"send"]})");
        const auto innerPos = postBlocks.find(oldInner);
        check(innerPos != std::string::npos,
              "measurement fixture locates exact inner until");
        if (innerPos != std::string::npos) {
            postBlocks.replace(innerPos, oldInner.size(), newInner);
            const auto at = postBlocks.rfind('}');
            if (at != std::string::npos) {
                postBlocks.insert(at,
                    std::string(",\"postClock\":") +
                    block("data_setvariableto", "postCount",
                        R"({"VALUE":[2,"finishedClock"]})",
                        R"({"VARIABLE":["b_t1","b_t1"]})") +
                    ",\"postCount\":" +
                    block("data_setvariableto", "innerSuffix",
                        R"({"VALUE":[2,"hitsRead"]})",
                        R"({"VARIABLE":["b_n","b_n"]})") +
                    ",\"finishedClock\":" +
                    block("sensing_dayssince2000", nullptr) +
                    ",\"hitsRead\":" +
                    block("data_variable", nullptr, "{}",
                        R"({"VARIABLE":["hits","hits"]})"));
            }
            const auto instrumentedStage = [&]() {
                std::string output = stageFor(postBlocks);
                const auto where = output.find(extraVars);
                if (where == std::string::npos) return std::string();
                output.replace(where, extraVars.size(), replacement);
                return output;
            }();
            check(!instrumentedStage.empty(),
                  "post-until measurement fixture has variable definitions");
            if (!instrumentedStage.empty()) {
                Runtime measured;
                const auto measuredLoaded = loadSb3(measured,
                    "{\"targets\":[" + instrumentedStage +
                    "],\"monitors\":[],\"extensions\":[],\"meta\":{}}",
                    nullptr);
                check(measuredLoaded.ok,
                      "post-until measurement fixture loads");
                if (measuredLoaded.ok) {
                    auto* stage = measured.getTargetForStage();
                    auto* count = stage->lookupVariableById("hits");
                    auto* total = stage->lookupVariableById("total");
                    auto* bN = stage->lookupVariableById("b_n");
                    auto* bT1 = stage->lookupVariableById("b_t1");
                    auto* bT0 = stage->lookupVariableById("b_t0");
                    auto* output = stage->lookupVariableById("sum");
                    auto* budgetVar = stage->lookupVariableById("b_budget");
                    check(count && total && bN && bT1 && bT0 &&
                          output && budgetVar,
                          "measurement source and destination bindings exist");
                    if (count && total && bN && bT1 && bT0 &&
                        output && budgetVar) {
                        const size_t activeFibers = FiberPool::activeCount();
                        measured.greenFlag();
                        Thread* broadcaster = nullptr;
                        for (Thread* thread : measured.threads)
                            if (thread->topBlock == "sender")
                                broadcaster = thread;
                        check(broadcaster && broadcaster->megaDispatch &&
                              !broadcaster->fiber,
                              "clock+counter snapshots compile to LLVM continuation");
                        if (broadcaster && broadcaster->megaDispatch) {
                            budgetVar->value = Value::num(15);
                            measured.sequencer->stepThread(*broadcaster);
                            check(broadcaster->megaJoinRefs.size() == 1 &&
                                  cast::toNumber(bN->value) == -1 &&
                                  cast::toNumber(output->value) == 0,
                                  "measurement values are untouched before joined receiver");
                            measured.runUntilDone(3000);
                            check(measured.threads.empty() &&
                                  cast::toNumber(output->value) == 130 &&
                                  cast::toNumber(total->value) >= 3 &&
                                  cast::toNumber(bN->value) ==
                                      cast::toNumber(count->value) &&
                                  cast::toNumber(bN->value) >= 1 &&
                                  cast::toNumber(bT1->value) >=
                                      cast::toNumber(bT0->value) &&
                                  FiberPool::activeCount() == activeFibers,
                                  "post-until clock/counter snapshots commit before suffix");

                            // The same two measurements run after a zero-
                            // broadcast inner iteration. They must not
                            // depend on recipient scheduling.
                            bN->value = Value::num(-1);
                            budgetVar->value = Value::num(-1);
                            const auto prev = cast::toNumber(total->value);
                            measured.greenFlag();
                            measured.runUntilDone(3000);
                            check(measured.threads.empty() &&
                                  cast::toNumber(output->value) == 260 &&
                                  cast::toNumber(total->value) == prev &&
                                  cast::toNumber(count->value) == 0 &&
                                  cast::toNumber(bN->value) == 0 &&
                                  cast::toNumber(bT1->value) >=
                                      cast::toNumber(bT0->value),
                                  "measurements also execute after initially true until");
                        }
                    }
                }
            }
        }
        // The actual ScratchBench invokes bench_round after both
        // snapshots. Even a small non-warp procedure with that name
        // must never be silently discarded by this source proof.
        const std::string trailingScalar = block(
            "data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"10"]]})",
            R"({"VARIABLE":["sum","sum"]})");
        std::string additional = postBlocks;
        const auto innerSuffixPos = additional.find(trailingScalar);
        check(innerSuffixPos != std::string::npos,
              "post-set negative-control suffix is identifiable");
        if (innerSuffixPos != std::string::npos) {
            const std::string mutation =
                R"({"tagName":"mutation","children":[],"proccode":"bench_round","argumentids":"[]","argumentnames":"[]","argumentdefaults":"[]","warp":"false"})";
            additional.replace(innerSuffixPos, trailingScalar.size(),
                std::string(R"({"opcode":"procedures_call","next":null,"inputs":{},"fields":{},"mutation":)") +
                mutation + "}");
            const auto where = additional.rfind('}');
            if (where != std::string::npos)
                additional.insert(where,
                    std::string(",\"benchRoundDef\":") +
                    R"({"opcode":"procedures_definition","topLevel":true,"next":"benchRoundChange","inputs":{"custom_block":[1,"benchRoundProto"]},"fields":{}})" +
                    ",\"benchRoundProto\":" +
                    R"({"opcode":"procedures_prototype","inputs":{},"fields":{},"mutation":)" +
                    mutation + "}" +
                    ",\"benchRoundChange\":" +
                    block("data_changevariableby", nullptr,
                        R"({"VALUE":[1,[4,"10"]]})",
                        R"({"VARIABLE":["sum","sum"]})"));
            std::string rejectedStage = stageFor(additional);
            const auto v = rejectedStage.find(extraVars);
            if (v != std::string::npos)
                rejectedStage.replace(v, extraVars.size(), replacement);
            Runtime rejected;
            const auto loaded = loadSb3(rejected,
                "{\"targets\":[" + rejectedStage +
                "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
            check(loaded.ok, "unsupported post-effect fixture loads");
            if (loaded.ok) {
                rejected.greenFlag();
                Thread* sender = nullptr;
                for (Thread* thread : rejected.threads)
                    if (thread->topBlock == "sender") sender = thread;
                check(sender && !sender->megaDispatch,
                      "bench_round procedure call after measurement stays on Fiber");
                auto* rejectedBudget = rejected.getTargetForStage()
                    ->lookupVariableById("b_budget");
                auto* rejectedSum = rejected.getTargetForStage()
                    ->lookupVariableById("sum");
                if (rejectedBudget && rejectedSum) {
                    rejectedBudget->value = Value::num(-1);
                    rejected.runUntilDone(2000);
                    check(rejected.threads.empty() &&
                          cast::toNumber(rejectedSum->value) == 130,
                          "native Fiber preserves all three bench_round calls and outer suffix");
                }
            }
        }
    }

    // The same bounded outer loop inside a non-warp procedure must keep
    // the caller's return PC dormant until ALL three inner loops finish.
    std::string procBlocks = blocks;
    const std::string originalHat = hat("outer");
    const auto hatAt = procBlocks.find(originalHat);
    check(hatAt != std::string::npos,
          "outer until procedure wrapper has replaceable hat");
    if (hatAt != std::string::npos) {
        procBlocks.replace(hatAt, originalHat.size(), hat("callOuter"));
        const std::string mutation =
            R"({"tagName":"mutation","children":[],"proccode":"outerProc","argumentids":"[]","argumentnames":"[]","argumentdefaults":"[]","warp":"false"})";
        const auto at = procBlocks.rfind('}');
        if (at != std::string::npos) {
            procBlocks.insert(at,
                std::string(",\"callOuter\":") +
                R"({"opcode":"procedures_call","next":"afterCall","inputs":{},"fields":{},"mutation":)" +
                mutation + "}" +
                ",\"afterCall\":" +
                block("data_changevariableby", nullptr,
                      R"({"VALUE":[1,[4,"1000"]]})",
                      R"({"VARIABLE":["sum","sum"]})") +
                ",\"outerDef\":" +
                R"({"opcode":"procedures_definition","topLevel":true,"next":"outer","inputs":{"custom_block":[1,"outerProto"]},"fields":{}})" +
                ",\"outerProto\":" +
                R"({"opcode":"procedures_prototype","inputs":{},"fields":{},"mutation":)" +
                mutation + "}");
        }
        Runtime nested;
        const auto nestedStage = stageFor(procBlocks);
        const auto nestedLoaded = loadSb3(nested,
            "{\"targets\":[" + nestedStage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(nestedLoaded.ok, "outer until wrapped procedure loads");
        if (nestedLoaded.ok) {
            auto* output = nested.getTargetForStage()->lookupVariableById("sum");
            auto* count = nested.getTargetForStage()->lookupVariableById("total");
            auto* budgetVar = nested.getTargetForStage()->lookupVariableById("b_budget");
            nested.greenFlag();
            Thread* outerSender = nullptr;
            for (Thread* thread : nested.threads)
                if (thread->topBlock == "sender") outerSender = thread;
            check(outerSender && outerSender->megaDispatch && !outerSender->fiber,
                  "nested outer until keeps one native LLVM PC");
            if (outerSender && outerSender->megaDispatch && budgetVar) {
                budgetVar->value = Value::num(12);
                nested.sequencer->stepThread(*outerSender);
                check(outerSender->megaPc == 1 &&
                      outerSender->megaRemaining == 3 &&
                      outerSender->megaReturnDepth == 1 &&
                      outerSender->megaJoinRefs.size() == 1 &&
                      output && cast::toNumber(output->value) == 0,
                      "caller return PC and bounded counter survive first joined send");
                nested.runUntilDone(2500);
                check(nested.threads.empty() &&
                      output && cast::toNumber(output->value) == 1130 &&
                      count && cast::toNumber(count->value) >= 3,
                      "three inner joins finish before outer and caller suffixes");
            }
        }
    }

    // repeat(0) must not execute either setter or inner suffix.
    const std::string oldCount = R"("TIMES":[1,[4,"3"]])";
    std::string zeroBlocks = blocks;
    auto where = zeroBlocks.find(oldCount);
    check(where != std::string::npos, "outer zero-trip source is constructible");
    if (where == std::string::npos) return;
    zeroBlocks.replace(where, oldCount.size(),
                       R"("TIMES":[1,[4,"0"]])");
    Runtime zero;
    const auto zeroStage = stageFor(zeroBlocks);
    const auto zeroLoaded = loadSb3(zero,
        "{\"targets\":[" + zeroStage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(zeroLoaded.ok, "outer zero-trip project loads");
    if (!zeroLoaded.ok) return;
    auto* zeroT = zero.getTargetForStage();
    auto* zeroSum = zeroT->lookupVariableById("sum");
    auto* zeroHits = zeroT->lookupVariableById("hits");
    auto* zeroT0 = zeroT->lookupVariableById("b_t0");
    if (!zeroSum || !zeroHits || !zeroT0) return;
    zeroHits->value = Value::num(7);
    zeroT0->value = Value::num(42);
    zero.greenFlag();
    Thread* zeroSender = nullptr;
    for (Thread* thread : zero.threads)
        if (thread->topBlock == "sender") zeroSender = thread;
    check(zeroSender && zeroSender->megaDispatch,
          "zero-trip outer script is handled by LLVM continuation");
    zero.runUntilDone(2000);
    check(zero.threads.empty() &&
          cast::toNumber(zeroSum->value) == 100 &&
          cast::toNumber(zeroHits->value) == 7 &&
          cast::toNumber(zeroT0->value) == 42 &&
          FiberPool::activeCount() == before,
          "outer repeat(0) skips both initializers and inner until entirely");

    // A large outer repeat cannot run inside a single synchronous LLVM
    // resumption when the inner until is true at every entry. Keep
    // scratch's normal yielded Fiber path for repeats > 64.
    std::string tooLarge = blocks;
    auto at = tooLarge.find(oldCount);
    check(at != std::string::npos, "over-bound outer loop source is constructible");
    if (at != std::string::npos) {
        tooLarge.replace(at, oldCount.size(),
                         R"("TIMES":[1,[4,"65"]])");
        Runtime fallback;
        const std::string fallbackStage = stageFor(tooLarge);
        const auto fallbackLoaded = loadSb3(fallback,
            "{\"targets\":[" + fallbackStage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(fallbackLoaded.ok, "over-bound outer loop fixture loads");
        if (fallbackLoaded.ok) {
            fallback.greenFlag();
            Thread* old = nullptr;
            for (Thread* thread : fallback.threads)
                if (thread->topBlock == "sender") old = thread;
            check(old && !old->megaDispatch,
                  "over-bound outer repeat stays on normal Fiber JIT");
        }
    }
}

void testMegaUntilNestedProcedure() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_SINGLE_SENDER") ||
        !enabled("XYO_JIT_MEGA_UNTIL_BCAST")) return;
    const auto mutation = [](const char* name) {
        return std::string(
            R"({"tagName":"mutation","children":[],"proccode":")") +
            name +
            R"(","argumentids":"[]","argumentnames":"[]","argumentdefaults":"[]","warp":"false"})";
    };
    const auto call = [&](const char* name, const char* next) {
        return std::string(R"({"opcode":"procedures_call","next":)") +
            (next ? "\"" + std::string(next) + "\"" : "null") +
            R"(,"inputs":{},"fields":{},"mutation":)" +
            mutation(name) + "}";
    };
    const auto proto = [&](const char* name) {
        return std::string(R"({"opcode":"procedures_prototype",)") +
            R"("inputs":{},"fields":{},"mutation":)" + mutation(name) + "}";
    };
    const auto def = [](const char* prototype, const char* first) {
        return std::string(R"({"opcode":"procedures_definition",)") +
            R"("topLevel":true,"inputs":{"custom_block":[1,")" +
            prototype + R"("]},"fields":{},"next":")" + first + "\"}";
    };
    const auto add = [](const char* amount, const char* variable) {
        return block("data_changevariableby", nullptr,
            std::string(R"({"VALUE":[1,[4,")") + amount + R"("]]})",
            std::string(R"({"VARIABLE":[")") + variable + R"(",")" +
            variable + R"("]})");
    };
    const std::string blocks =
        std::string("{\"sender\":") + hat("callOuter") +
        ",\"callOuter\":" + call("outer", "afterOuter") +
        ",\"afterOuter\":" + add("100", "s") +
        ",\"defOuter\":" + def("protoOuter", "callInner") +
        ",\"protoOuter\":" + proto("outer") +
        ",\"callInner\":" + call("inner", "afterInner") +
        ",\"afterInner\":" + add("20", "s") +
        ",\"defInner\":" + def("protoInner", "until") +
        ",\"protoInner\":" + proto("inner") +
        ",\"until\":" + block("control_repeat_until", "afterUntil",
            R"({"CONDITION":[2,"compare"],"SUBSTACK":[2,"send"]})") +
        ",\"compare\":" + block("operator_gt", nullptr,
            R"({"OPERAND1":[2,"countRead"],"OPERAND2":[1,[4,"2"]]})") +
        ",\"countRead\":" + block("data_variable", nullptr, "{}",
            R"({"VARIABLE":["count","count"]})") +
        ",\"send\":" + block("event_broadcastandwait", nullptr,
            R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})") +
        ",\"afterUntil\":" + add("10", "s") +
        ",\"receiver\":" +
        R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":"inc","inputs":{},"fields":{"BROADCAST_OPTION":["PING","ping"]}})" +
        ",\"inc\":" + add("1", "count") + "}";
    std::string stage = target("Stage", blocks, true);
    const std::string oldVars = R"("variables":{"s":["s",""]})";
    auto pos = stage.find(oldVars);
    check(pos != std::string::npos,
          "nested until fixture contains Stage variable defaults");
    if (pos == std::string::npos) return;
    stage.replace(pos, oldVars.size(),
                  R"("variables":{"s":["s",0],"count":["count",0]})");
    const std::string oldBroadcasts = R"("broadcasts":{})";
    pos = stage.find(oldBroadcasts);
    check(pos != std::string::npos,
          "nested until fixture has broadcast namespace");
    if (pos == std::string::npos) return;
    stage.replace(pos, oldBroadcasts.size(),
                  R"("broadcasts":{"ping":"PING"})");
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "nested until two-procedure fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    Variable* count = rt.getTargetForStage()->lookupVariableById("count");
    check(sum && count, "nested until variables resolve");
    if (!sum || !count) return;
    const size_t before = FiberPool::activeCount();
    rt.greenFlag();
    Thread* sender = nullptr;
    for (Thread* t : rt.threads)
        if (t->topBlock == "sender") sender = t;
    check(sender && sender->megaDispatch && !sender->fiber,
          "two-level procedure sender uses the LLVM stackless PC");
    if (!sender || !sender->megaDispatch) return;
    const auto compiled = sender->megaDispatch;
    rt.sequencer->stepThread(*sender);
    check(sender->megaPc == 1 &&
          sender->megaReturnDepth == 2 &&
          sender->megaReturnPcs[0] != sender->megaReturnPcs[1] &&
          sender->megaJoinRefs.size() == 1 &&
          cast::toNumber(sum->value) == 0 &&
          cast::toNumber(count->value) == 0,
          "two saved return PCs survive dynamic until's joined broadcast");
    rt.runUntilDone(2000);
    check(rt.threads.empty() && cast::toNumber(count->value) == 3 &&
          cast::toNumber(sum->value) == 130 &&
          FiberPool::activeCount() == before,
          "until condition closes then both procedure suffixes run in order");
    count->value = Value::num(3);
    rt.greenFlag();
    sender = nullptr;
    for (Thread* t : rt.threads)
        if (t->topBlock == "sender") sender = t;
    check(sender && sender->megaDispatch == compiled,
          "nested until reuses immutable LLVM code without Fiber");
    rt.runUntilDone(2000);
    check(rt.threads.empty() && cast::toNumber(count->value) == 3 &&
          cast::toNumber(sum->value) == 260 &&
          FiberPool::activeCount() == before,
          "initially satisfied until pops two returns without broadcasting");
}

void testMegaSingleBroadcastSender() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto yes = [](const char* key) {
        const char* value = std::getenv(key);
        return value && value[0] == '1';
    };
    if (!yes("XYO_JIT_MEGA_BATCH_YIELD") ||
        !yes("XYO_JIT_MEGA_SINGLE_SENDER")) return;

    const std::string blocks =
        std::string("{\"sender\":") + hat("sendLoop") +
        ",\"sendLoop\":" +
        block("control_repeat", "senderSuffix",
            R"({"TIMES":[1,[4,"3"]],"SUBSTACK":[2,"send"]})") +
        ",\"send\":" + block("event_broadcastandwait", nullptr,
            R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})") +
        ",\"senderSuffix\":" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"10"]]})",
              R"({"VARIABLE":["s","s"]})") +
        ",\"receiver\":" +
        R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":"rxLoop","inputs":{},"fields":{"BROADCAST_OPTION":["PING","ping"]}})" +
        ",\"rxLoop\":" +
        block("control_repeat", nullptr,
              R"({"TIMES":[1,[4,"2"]],"SUBSTACK":[2,"rxInc"]})") +
        ",\"rxInc\":" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"1"]]})",
              R"({"VARIABLE":["s","s"]})") + "}";
    std::string stage = target("Stage", blocks, true);
    const std::string needle = R"("broadcasts":{})";
    const auto pos = stage.find(needle);
    check(pos != std::string::npos, "single sender fixture broadcast metadata");
    if (pos == std::string::npos) return;
    stage.replace(pos, needle.size(), R"("broadcasts":{"ping":"PING"})");
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "single sender repeated join fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    check(sum != nullptr, "single sender sum binding resolves");
    if (!sum) return;
    const auto find = [&](const char* name) -> Thread* {
        for (Thread* t : rt.threads)
            if (t->topBlock == name) return t;
        return nullptr;
    };
    const auto before = FiberPool::activeCount();
    rt.greenFlag();
    Thread* sender = find("sender");
    check(sender && sender->megaDispatch &&
          sender->megaDispatch->entries == 1 &&
          sender->megaDispatch->yieldEntry && !sender->fiber,
          "one green flag sender enters source-proven stackless LLVM PC");
    if (!sender || !sender->megaDispatch) return;
    const auto compiled = sender->megaDispatch;
    rt.sequencer->stepThread(*sender);
    const bool fastLatch = yes("XYO_JIT_MEGA_FAST_BCAST_LATCH");
    const bool normalizeYield = yes("XYO_JIT_MEGA_NORMALIZE_YIELD");
    check(sender->megaPc == 1 && sender->megaRemaining == 3 &&
          sender->megaJoinRefs.size() == 1 && !sender->fiber &&
          (!fastLatch || sender->jitCtx == nullptr) &&
          (!normalizeYield || sender->status == STATUS_RUNNING) &&
          cast::toNumber(sum->value) == 0,
          "one sender starts real receiver and saves PC without native Fiber");
    rt.sequencer->stepThread(*sender);
    check(sender->megaJoinRefs.size() == 1 &&
          (!fastLatch || sender->jitCtx == nullptr) &&
          (!normalizeYield || sender->status == STATUS_RUNNING) &&
          cast::toNumber(sum->value) == 0,
          "paused sender must not replay broadcast before receiver finishes");
    if (normalizeYield && !sender->megaJoinRefs.empty()) {
        // A tick-blocked recipient is NOT a same-frame runnable sender.
        // Normalize only STATUS_YIELD, never STATUS_YIELD_TICK.
        Thread* receiver = sender->megaJoinRefs.front();
        const auto beforeStatus = receiver->status;
        receiver->status = STATUS_YIELD_TICK;
        rt.sequencer->stepThread(*sender);
        check(sender->status == STATUS_YIELD_TICK &&
              sender->megaJoinRefs.size() == 1 &&
              cast::toNumber(sum->value) == 0,
              "tick-waited recipient keeps sender YIELD_TICK");
        receiver->status = beforeStatus;
        sender->status = STATUS_RUNNING;
    }
    rt.runUntilDone(3000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 16 &&
          FiberPool::activeCount() == before,
          "three joined broadcasts, each two receiver turns, then suffix");
    rt.greenFlag();
    sender = find("sender");
    check(sender && sender->megaDispatch == compiled,
          "one-sender native code is cached by immutable source revision");
    rt.runUntilDone(3000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 32 &&
          FiberPool::activeCount() == before,
          "second green flag preserves Scratch join and no Fiber leak");

    // No receiver: each repeat consumes exactly one iteration but does
    // not manufacture a join handle or replay the numeric suffix.
    const std::string marker = ",\"receiver\":";
    const auto cut = blocks.find(marker);
    check(cut != std::string::npos,
          "single sender fixture supports a receiver-free variant");
    if (cut != std::string::npos) {
        const std::string noReceiverBlocks = blocks.substr(0, cut) + "}";
        const std::string noReceiverStage =
            target("Stage", noReceiverBlocks, true);
        Runtime empty;
        const auto noReceiverLoaded = loadSb3(empty,
            "{\"targets\":[" + noReceiverStage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(noReceiverLoaded.ok, "single sender no-receiver fixture loads");
        if (noReceiverLoaded.ok) {
            Variable* amount =
                empty.getTargetForStage()->lookupVariableById("s");
            const size_t previous = FiberPool::activeCount();
            empty.greenFlag();
            Thread* only = empty.threads.empty()
                ? nullptr : empty.threads.front();
            check(only && only->megaDispatch && only->megaPc == 0,
                  "receiver-free single sender retains source-proof state");
            empty.runUntilDone(1000);
            check(amount && empty.threads.empty() &&
                  cast::toNumber(amount->value) == 10 &&
                  FiberPool::activeCount() == previous,
                  "receiver-free repeat consumes 3 counts and suffix once");
        }
    }
    // Zero repeat should launch no recipient even when the hat exists.
    std::string zeroBlocks = blocks;
    const std::string repeatThree = R"("TIMES":[1,[4,"3"]])";
    const auto repeatAt = zeroBlocks.find(repeatThree);
    check(repeatAt != std::string::npos,
          "single sender fixture supports zero-repeat variant");
    if (repeatAt != std::string::npos) {
        zeroBlocks.replace(repeatAt, repeatThree.size(),
                           R"("TIMES":[1,[4,"0"]])");
        std::string zeroStage = target("Stage", zeroBlocks, true);
        const auto where = zeroStage.find(needle);
        if (where != std::string::npos)
            zeroStage.replace(where, needle.size(),
                              R"("broadcasts":{"ping":"PING"})");
        Runtime zero;
        const auto zeroLoaded = loadSb3(zero,
            "{\"targets\":[" + zeroStage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(zeroLoaded.ok, "zero-repeat native sender fixture loads");
        if (zeroLoaded.ok) {
            Variable* amount =
                zero.getTargetForStage()->lookupVariableById("s");
            zero.greenFlag();
            zero.runUntilDone(1000);
            check(amount && zero.threads.empty() &&
                  cast::toNumber(amount->value) == 10,
                  "zero-repeat never starts receiver; suffix occurs once");
        }
    }

    // Actual native timed wait (rather than a synthetic status flip):
    // a waiting receiver must not let the sender run its numeric suffix,
    // and a same-tick native yield must never bypass the timer deadline.
    const std::string originalLoop = block(
        "control_repeat", nullptr,
        R"({"TIMES":[1,[4,"2"]],"SUBSTACK":[2,"rxInc"]})");
    const std::string timedWait = block(
        "control_wait", "rxInc", R"({"DURATION":[1,[4,"0.005"]]})");
    std::string timedBlocks = blocks;
    const auto loopAt = timedBlocks.find(originalLoop);
    check(loopAt != std::string::npos,
          "timed receiver source fixture has replaceable repeat");
    if (loopAt != std::string::npos) {
        timedBlocks.replace(loopAt, originalLoop.size(), timedWait);
        std::string timedStage = target("Stage", timedBlocks, true);
        const auto broadcastAt = timedStage.find(needle);
        if (broadcastAt != std::string::npos)
            timedStage.replace(broadcastAt, needle.size(),
                               R"("broadcasts":{"ping":"PING"})");
        Runtime delayed;
        const auto delayedLoad = loadSb3(delayed,
            "{\"targets\":[" + timedStage +
            "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
        check(delayedLoad.ok,
              "actual wait receiver singleton fixture loads");
        if (delayedLoad.ok) {
            Variable* amount =
                delayed.getTargetForStage()->lookupVariableById("s");
            const size_t previous = FiberPool::activeCount();
            delayed.greenFlag();
            Thread* senderThread = nullptr;
            for (Thread* t : delayed.threads)
                if (t->topBlock == "sender") senderThread = t;
            if (senderThread) delayed.sequencer->stepThread(*senderThread);
            check(senderThread && !senderThread->megaJoinRefs.empty() &&
                  amount && cast::toNumber(amount->value) == 0,
                  "real timer receiver prevents premature sender suffix");
            delayed.runUntilDone(1000);
            check(amount && delayed.threads.empty() &&
                  cast::toNumber(amount->value) == 13 &&
                  FiberPool::activeCount() == previous,
                  "timed receiver joins three times before native suffix");
        }
    }
}

void testMegaSingleBroadcastReceiver() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* yield = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
    const char* receivers = std::getenv("XYO_JIT_MEGA_BCAST_RECEIVERS");
    if (!yield || yield[0] != '1' ||
        !receivers || receivers[0] != '1') return;
    const char* singleton = std::getenv("XYO_JIT_MEGA_SINGLE_RECEIVER");
    const bool on = singleton && singleton[0] == '1';

    const std::string blocks =
        std::string("{\"sender\":") + hat("send") +
        ",\"send\":" +
        block("event_broadcastandwait", "suffix",
              R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})") +
        ",\"suffix\":" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"10"]]})",
              R"({"VARIABLE":["s","s"]})") +
        ",\"receiver\":" +
        R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":"loop","inputs":{},"fields":{"BROADCAST_OPTION":["PING","ping"]}})" +
        ",\"loop\":" +
        block("control_repeat", nullptr,
              R"({"TIMES":[1,[4,"3"]],"SUBSTACK":[2,"inc"]})") +
        ",\"inc\":" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"1"]]})",
              R"({"VARIABLE":["s","s"]})") + "}";
    std::string stage = target("Stage", blocks, true);
    const std::string old = R"("broadcasts":{})";
    const auto pos = stage.find(old);
    check(pos != std::string::npos,
          "single receiver fixture has broadcast namespace");
    if (pos == std::string::npos) return;
    stage.replace(pos, old.size(), R"("broadcasts":{"ping":"PING"})");
    Runtime rt;
    const auto load = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(load.ok, "single receiver nested repeat fixture loads");
    if (!load.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    check(sum != nullptr, "single receiver stage sum exists");
    if (!sum) return;
    const auto find = [&](const char* id) -> Thread* {
        for (Thread* t : rt.threads)
            if (t->topBlock == id) return t;
        return nullptr;
    };
    const auto before = FiberPool::activeCount();
    rt.greenFlag();
    Thread* sender = find("sender");
    check(sender && !find("receiver"),
          "single receiver not started before first broadcast");
    if (!sender) return;
    rt.sequencer->stepThread(*sender);
    Thread* receiver = find("receiver");
    check(receiver && receiver->isCompiled &&
          (bool(receiver->megaDispatch) == on),
          "single receiver uses exactly the enabled native PC route");
    if (!receiver) return;
    const auto compiled = receiver->megaDispatch;
    if (on && compiled) {
        check(compiled->entries == 1 && compiled->yieldEntry &&
              receiver->megaPc == 0 && !receiver->fiber,
              "one receiver has its own LLVM PC without a Fiber");
        rt.sequencer->stepThread(*receiver);
        check(cast::toNumber(sum->value) == 1 &&
              receiver->megaRemaining == 2 &&
              receiver->megaPc == 1 && !receiver->fiber,
              "single receiver yields after one native repeat body");
        rt.sequencer->stepThread(*sender);
        check(cast::toNumber(sum->value) == 1,
              "broadcast sender may not commit suffix before receiver ends");
    }
    rt.runUntilDone(1000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 13 &&
          FiberPool::activeCount() == before,
          "single receiver and sender join complete exact Scratch effects");
    rt.greenFlag();
    sender = find("sender");
    if (sender) rt.sequencer->stepThread(*sender);
    receiver = find("receiver");
    check(receiver && (!on || receiver->megaDispatch == compiled),
          "restarted singleton uses same immutable LLVM code cache");
    rt.runUntilDone(1000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 26 &&
          FiberPool::activeCount() == before,
          "repeat broadcast does not duplicate receiver or leak Fiber");
}

void testMegaSingleReceiverNestedJoin() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto yes = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1';
    };
    if (!yes("XYO_JIT_MEGA_BATCH_YIELD") ||
        !yes("XYO_JIT_MEGA_BCAST_RECEIVERS") ||
        !yes("XYO_JIT_MEGA_SINGLE_RECEIVER")) return;
    const auto mutation = [](const char* name) {
        return std::string(
            R"({"tagName":"mutation","children":[],"proccode":")") +
            name +
            R"(","argumentids":"[]","argumentnames":"[]","argumentdefaults":"[]","warp":"false"})";
    };
    const auto call = [&](const char* name, const char* next) {
        const std::string continuation = next
            ? "\"" + std::string(next) + "\"" : "null";
        return std::string(R"({"opcode":"procedures_call","next":)") +
            continuation + R"(,"inputs":{},"fields":{},"mutation":)" +
            mutation(name) + "}";
    };
    const auto prototype = [&](const char* name) {
        return std::string(R"({"opcode":"procedures_prototype",)") +
            R"("inputs":{},"fields":{},"mutation":)" + mutation(name) + "}";
    };
    const auto definition = [](const char* prototypeId,
                               const char* firstId) {
        return std::string(R"({"opcode":"procedures_definition",)") +
            R"("topLevel":true,"inputs":{"custom_block":[1,")" +
            prototypeId + R"("]},"fields":{},"next":")" + firstId + "\"}";
    };
    const auto increment = [](const char* value) {
        return block("data_changevariableby", nullptr,
            std::string(R"({"VALUE":[1,[4,")") + value + R"("]]})",
            R"({"VARIABLE":["s","s"]})");
    };
    const auto receiver = [](const char* next,
                             const char* label, const char* id) {
        return std::string(R"({"opcode":"event_whenbroadcastreceived",)") +
            R"("topLevel":true,"next":")" + next +
            R"(","inputs":{},"fields":{"BROADCAST_OPTION":[")" +
            label + R"(",")" + id + R"("]}})";
    };
    const std::string blocks =
        std::string("{\"sender\":") + hat("send") +
        ",\"send\":" + block("event_broadcastandwait", "senderDone",
            R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})") +
        ",\"senderDone\":" + increment("100") +
        ",\"ping\":" + receiver("callOuter", "PING", "ping") +
        ",\"callOuter\":" + call("outer", "afterOuter") +
        ",\"afterOuter\":" + increment("1000") +
        ",\"defOuter\":" + definition("protoOuter", "callInner") +
        ",\"protoOuter\":" + prototype("outer") +
        ",\"callInner\":" + call("inner", "afterInner") +
        ",\"afterInner\":" + increment("200") +
        ",\"defInner\":" + definition("protoInner", "innerSend") +
        ",\"protoInner\":" + prototype("inner") +
        ",\"innerSend\":" + block("event_broadcastandwait", "afterInnerSend",
            R"({"BROADCAST_INPUT":[1,[11,"INNER","inner"]]})") +
        ",\"afterInnerSend\":" + increment("20") +
        ",\"innerReceiver\":" + receiver("innerChange", "INNER", "inner") +
        ",\"innerChange\":" + increment("1") + "}";
    std::string stage = target("Stage", blocks, true);
    const std::string old = R"("broadcasts":{})";
    const auto pos = stage.find(old);
    check(pos != std::string::npos,
          "nested singleton fixture has broadcast table");
    if (pos == std::string::npos) return;
    stage.replace(pos, old.size(),
        R"("broadcasts":{"ping":"PING","inner":"INNER"})");
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "single receiver nested-call join fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    check(sum != nullptr, "nested singleton stage sum resolves");
    if (!sum) return;
    const auto find = [&](const char* name) -> Thread* {
        for (Thread* t : rt.threads)
            if (t->topBlock == name) return t;
        return nullptr;
    };
    const size_t before = FiberPool::activeCount();
    rt.greenFlag();
    Thread* sender = find("sender");
    check(sender && !find("ping"),
          "nested singleton sender has no unstarted receiver effect");
    if (!sender) return;
    rt.sequencer->stepThread(*sender);
    Thread* ping = find("ping");
    check(ping && ping->megaDispatch && ping->megaDispatch->entries == 1 &&
          ping->megaDispatch->yieldEntry,
          "nested singleton chooses explicit return PC implementation");
    if (!ping || !ping->megaDispatch) return;
    auto cached = ping->megaDispatch;
    rt.sequencer->stepThread(*ping);
    Thread* inner = find("innerReceiver");
    check(inner && !inner->megaDispatch,
          "one synchronous scalar receiver keeps original fast native entry");
    check(inner && ping->megaReturnDepth == 2 &&
          ping->megaPc == 1 && ping->megaJoinRefs.size() == 1 &&
          !ping->fiber && cast::toNumber(sum->value) == 0,
          "nested procedures save two return PCs across actual broadcast join");
    rt.sequencer->stepThread(*sender);
    check(cast::toNumber(sum->value) == 0,
          "parent sender cannot pass join during nested receiver wait");
    rt.runUntilDone(2000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 1321 &&
          FiberPool::activeCount() == before,
          "nested procedure suffixes unwind LIFO before sender suffix");
    rt.greenFlag();
    sender = find("sender");
    if (sender) rt.sequencer->stepThread(*sender);
    ping = find("ping");
    check(ping && ping->megaDispatch == cached,
          "nested singleton source-specific LLVM cache reused");
    rt.runUntilDone(2000);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 2642 &&
          FiberPool::activeCount() == before,
          "second nested joined broadcast preserves identities and effects");
}

void testMegaBroadcastReceiverFusion() {
    // The sender's EXACT join identities are allocated by Runtime first.
    // Eligible recipient hats are then attached to a shared native switch,
    // while an unsupported recipient keeps its ordinary compiled path.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* enabled = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
    const char* receivers = std::getenv("XYO_JIT_MEGA_BCAST_RECEIVERS");
    if (!enabled || enabled[0] != '1' ||
        !receivers || receivers[0] != '1') return;

    const auto add = [](const char* next, const char* delta) {
        return block("data_changevariableby", next,
            std::string(R"({"VALUE":[1,[4,")") + delta + R"("]]})",
            R"({"VARIABLE":["s","s"]})");
    };
    const auto repeat = [](const char* body, const char* times) {
        return block("control_repeat", nullptr,
            std::string(R"({"TIMES":[1,[4,")") + times +
            R"("]],"SUBSTACK":[2,")" + body + R"("]})");
    };
    const auto recvHat = [](const char* next) {
        return std::string(R"({"opcode":"event_whenbroadcastreceived",)") +
            R"("topLevel":true,"next":")" + next +
            R"(","inputs":{},"fields":{"BROADCAST_OPTION":["PING","ping"]}})";
    };
    std::string blocks = "{";
    const auto append = [&](const char* id, const std::string& content) {
        if (blocks.size() > 1) blocks += ",";
        blocks += "\"" + std::string(id) + "\":" + content;
    };
    append("sender", hat("send"));
    append("send", block("event_broadcastandwait", "senderDone",
        R"({"BROADCAST_INPUT":[1,[11,"PING","ping"]]})"));
    append("senderDone", add(nullptr, "100"));

    append("recv0", recvHat("loop0"));
    append("loop0", repeat("inc0", "3"));
    append("inc0", add(nullptr, "1"));
    append("recv1", recvHat("loop1"));
    append("loop1", repeat("inc1", "2"));
    append("inc1", add(nullptr, "2"));
    append("recv2", recvHat("loop2"));
    append("loop2", repeat("inc2", "4"));
    append("inc2", add(nullptr, "3"));

    // One unsupported receiver: three sequential writes exceed the
    // source-certified pair limit and must stay on the ordinary JIT.
    append("recv3", recvHat("fallback0"));
    append("fallback0", add("fallback1", "5"));
    append("fallback1", add("fallback2", "3"));
    append("fallback2", add(nullptr, "4"));
    append("recv4", recvHat("scalar4"));
    append("scalar4", add(nullptr, "8"));
    append("recv5", recvHat("scalar5"));
    append("scalar5", add(nullptr, "9"));
    blocks += "}";

    std::string stage = target("Stage", blocks, true);
    const auto pos = stage.find("\"broadcasts\":{}");
    if (pos != std::string::npos)
        stage.replace(pos, std::string("\"broadcasts\":{}").size(),
                      "\"broadcasts\":{\"ping\":\"PING\"}");

    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega receiver broadcast fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    const auto find = [&](const char* id) -> Thread* {
        for (Thread* thread : rt.threads)
            if (thread->topBlock == id) return thread;
        return nullptr;
    };
    const size_t fibersBefore = FiberPool::activeCount();
    rt.greenFlag();
    Thread* sender = find("sender");
    check(sender && !find("recv0"),
          "mega receiver does not start before the actual broadcast");
    if (!sender || !sum) return;
    rt.sequencer->stepThread(*sender);
    Thread* r0 = find("recv0");
    Thread* r1 = find("recv1");
    Thread* r2 = find("recv2");
    Thread* fallback = find("recv3");
    Thread* scalar4 = find("recv4");
    Thread* scalar5 = find("recv5");
    check(r0 && r1 && r2 && fallback && scalar4 && scalar5 &&
          r0->megaDispatch &&
          r0->megaDispatch == r1->megaDispatch &&
          r1->megaDispatch == r2->megaDispatch &&
          r0->megaDispatch == scalar4->megaDispatch &&
          r0->megaDispatch == scalar5->megaDispatch &&
          r0->megaDispatch->entries == 5 &&
          !fallback->megaDispatch,
          "mega receiver batches repeat and scalar hats, preserving fallback");
    if (!r0 || !r1 || !r2 || !fallback ||
        !scalar4 || !scalar5 || !r0->megaDispatch) return;
    const auto compiled = r0->megaDispatch;
    check(r0->megaPc == 0 && r1->megaPc == 2 && r2->megaPc == 4 &&
          !r0->fiber && !r1->fiber && !r2->fiber &&
          cast::toNumber(sum->value) == 0,
          "mega receiver install preserves order, PC and no premature effect");
    rt.sequencer->stepThread(*r0);
    check(cast::toNumber(sum->value) == 1 &&
          r0->megaPc == 1 && r0->megaRemaining == 2 &&
          !r0->fiber,
          "mega receiver yields after one native repeat body");
    rt.sequencer->stepThread(*sender);
    check(cast::toNumber(sum->value) == 1,
          "mega sender remains joined until receiver threads finish");
    rt.sequencer->stepThread(*scalar4);
    check(cast::toNumber(sum->value) == 9 &&
          scalar4->status == STATUS_DONE && !scalar4->fiber &&
          scalar4->megaReturnDepth == 0,
          "mega one-shot scalar handler commits once and finishes without Fiber");
    rt.runUntilDone(200);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 148 &&
          FiberPool::activeCount() == fibersBefore,
          "mega joined receivers and mixed fallback execute exactly once");
    rt.greenFlag();
    sender = find("sender");
    if (sender) rt.sequencer->stepThread(*sender);
    r0 = find("recv0");
    check(r0 && r0->megaDispatch == compiled,
          "mega receiver uses cached native batch for second broadcast");
    rt.runUntilDone(200);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 296,
          "mega receiver second joined broadcast preserves exact state");
}


void testMegaBroadcastCloneSourceSharing() {
    // Four different Targets share one Blocks owner and one receiver hat.
    // The native function must resolve the SAME local variable ID through
    // each Thread's own JitCtx, never through the first clone's Target.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* enabled = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
    const char* receivers = std::getenv("XYO_JIT_MEGA_BCAST_RECEIVERS");
    if (!enabled || enabled[0] != '1' ||
        !receivers || receivers[0] != '1') return;

    const std::string blocks =
        std::string(R"({"recv":{"opcode":"event_whenbroadcastreceived",)") +
        R"("topLevel":true,"next":"inc","inputs":{},)" +
        R"("fields":{"BROADCAST_OPTION":["PING",null]}},"inc":)" +
        block("data_changevariableby", nullptr,
              R"({"VALUE":[1,[4,"1"]]})", R"({"VARIABLE":["v","v"]})") +
        "}";
    Runtime rt;
    auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", "{}", true) + "," +
        target("Receiver", blocks, false, 1) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega clone sharing receiver fixture loads");
    if (!loaded.ok) return;
    Target* original = rt.getSpriteTargetByName(u"Receiver");
    if (!original) return;
    std::vector<Target*> targets{original};
    for (int i = 0; i < 3; ++i) {
        Target* clone = original->makeClone();
        check(clone != nullptr, "mega clone sharing create distinct receiver");
        if (!clone) return;
        rt.addTarget(clone);
        targets.push_back(clone);
    }
    for (size_t i = 1; i < targets.size(); ++i)
        check(targets[i]->blocks == original->blocks,
              "mega clone reuses immutable Blocks across target identities");

    const size_t fibersBefore = FiberPool::activeCount();
    auto started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 4,
          "mega clone direct broadcast returns exactly four receiver identities");
    if (started.size() != 4) return;
    const auto code = started[0]->megaDispatch;
    bool same = code && code->yieldEntry && code->entries == 4;
    for (size_t i = 0; i < started.size(); ++i)
        same &= started[i]->megaDispatch == code &&
                started[i]->megaPc == 2*i &&
                started[i]->target != nullptr && !started[i]->fiber;
    check(same,
          "mega clone receiver batches four targets into a single LLVM switch");
    const uint64_t batchesBefore = rt.sequencer->megaScalarBatchCalls();
    const uint64_t bulkThreadsBefore =
        rt.sequencer->megaScalarBatchThreads();
    rt.runUntilDone(100);
    const char* bulk = std::getenv("XYO_JIT_MEGA_SCALAR_BATCH");
    const char* direct = std::getenv("XYO_JIT_MEGA_DIRECT_SCALAR");
    const char* wake = std::getenv("XYO_WAIT_WAKE_REGISTRY");
    const bool bulkRequested = bulk && bulk[0] == '1';
    const bool directEnabled = direct && direct[0] == '1';
    const bool wakeRegistry = wake && wake[0] == '1';
    const bool bulkEnabled = bulkRequested && directEnabled && !wakeRegistry;
    if (bulkRequested && (!directEnabled || wakeRegistry))
        check(!code->scalarBatchEntry,
              "mega LLVM scalar batch requires numeric direct mode and no wait registry");
    if (bulkEnabled) {
        check(code && code->scalarBatchEntry &&
              rt.sequencer->megaScalarBatchCalls() == batchesBefore + 1 &&
              rt.sequencer->megaScalarBatchThreads() ==
                  bulkThreadsBefore + 4,
              "mega scalar LLVM bulk handles four clone variables in one call");
        const char* flag = std::getenv("XYO_JIT_MEGA_SINGLE_SPECIALIZE");
        const char* single = std::getenv("XYO_JIT_MEGA_SINGLE_DISPATCH");
        const bool specialize = flag && flag[0] == '1' &&
                                single && single[0] == '1';
        check(bool(code->scalarSingleEntry) == specialize,
              "scalar specialized entry exists only when explicitly enabled");
        if (specialize) {
            auto regular = Variable::create(
                "v", u"v", Variable::kScalarType, false);
            auto fast = Variable::create(
                "v", u"v", Variable::kScalarType, false);
            regular.value = Value::num(5);
            fast.value = Value::num(5);
            Variable* input[1]{&regular};
            code->scalarBatchEntry(input, 1);
            code->scalarSingleEntry(&fast);
            check(regular.value.bits == fast.value.bits &&
                  cast::toNumber(fast.value) == 6,
                  "scalar specialized entry matches generic one-item FAdd");
        }
    }
    bool values = rt.threads.empty();
    for (Target* target : targets) {
        Variable* local = target->lookupVariableById("v");
        values &= local && cast::toNumber(local->value) == 1;
    }
    check(values && FiberPool::activeCount() == fibersBefore,
          "mega clone binding updates four independent local variables once");

    started = rt.startBroadcastAndWait(u"PING");
    bool reused = started.size() == 4;
    for (Thread* thread : started)
        reused &= thread->megaDispatch == code;
    check(reused, "mega clone reuse follows identical source owner, not Target ID");
    const uint64_t secondBatchCount =
        rt.sequencer->megaScalarBatchCalls();
    rt.runUntilDone(100);
    if (bulkEnabled)
        check(rt.sequencer->megaScalarBatchCalls() == secondBatchCount + 1,
              "mega scalar bulk reuses native loop for second broadcast");
    values = rt.threads.empty();
    for (Target* target : targets) {
        Variable* local = target->lookupVariableById("v");
        values &= local && cast::toNumber(local->value) == 2;
    }
    check(values, "mega clone second launch updates each independent binding twice");

    if (bulkEnabled) {
        targets[1]->variables.at("v").value =
            Value::string(SStr::create(u"12"));
        targets[3]->variables.at("v").value = Value::boolean(true);
        const uint64_t beforeMixed = rt.sequencer->megaScalarBatchCalls();
        rt.startBroadcastAndWait(u"PING");
        rt.runUntilDone(100);
        check(rt.threads.empty() &&
              rt.sequencer->megaScalarBatchCalls() == beforeMixed &&
              cast::toNumber(targets[0]->variables.at("v").value) == 3 &&
              cast::toNumber(targets[1]->variables.at("v").value) == 13 &&
              cast::toNumber(targets[2]->variables.at("v").value) == 3 &&
              cast::toNumber(targets[3]->variables.at("v").value) == 2,
              "mega bulk falls back safely on nonnumeric clone variables");

        const uint64_t beforeNumericReentry =
            rt.sequencer->megaScalarBatchCalls();
        rt.startBroadcastAndWait(u"PING");
        rt.runUntilDone(100);
        check(rt.sequencer->megaScalarBatchCalls() >
                  beforeNumericReentry &&
              cast::toNumber(targets[0]->variables.at("v").value) == 4 &&
              cast::toNumber(targets[1]->variables.at("v").value) == 14 &&
              cast::toNumber(targets[2]->variables.at("v").value) == 4 &&
              cast::toNumber(targets[3]->variables.at("v").value) == 3,
              "mega bulk re-enters native loop after type fallback");

        // A structurally new Variable must never inherit a pointer from
        // the previous clone dispatch, even with the same ID and name.
        targets[2]->variables.erase("v");
        auto [recreated, inserted] = targets[2]->variables.emplace(
            "v", Variable::create(
                "v", u"v", Variable::kScalarType, false));
        check(inserted, "mega bulk replaced a clone-local scalar binding");
        recreated->second.value = Value::num(50);
        rt.startBroadcastAndWait(u"PING");
        rt.runUntilDone(100);
        check(rt.threads.empty() &&
              cast::toNumber(targets[0]->variables.at("v").value) == 5 &&
              cast::toNumber(targets[1]->variables.at("v").value) == 15 &&
              cast::toNumber(targets[2]->variables.at("v").value) == 51 &&
              cast::toNumber(targets[3]->variables.at("v").value) == 4,
              "mega bulk resolves recreated clone variable before every call");
    }
}




void testMegaIfElseBatch() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* key) {
        const char* value = std::getenv(key);
        return value && value[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_BCAST_RECEIVERS") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        !enabled("XYO_JIT_MEGA_IF_ELSE_BATCH") ||
        enabled("XYO_WAIT_WAKE_REGISTRY")) return;
    const auto recv = [](const std::string& first) {
        return std::string(R"({"opcode":"event_whenbroadcastreceived",)") +
            R"("topLevel":true,"next":")" + first +
            R"(","inputs":{},"fields":{"BROADCAST_OPTION":["PING",null]}})";
    };
    const auto change = [](const std::string& var,
                           const std::string& amount) {
        return block("data_changevariableby", nullptr,
            std::string(R"({"VALUE":[1,[4,")") + amount + R"("]]})",
            std::string(R"({"VARIABLE":[")") + var + R"(",")" + var +
            R"("]})");
    };
    std::string blocks = "{";
    const auto append = [&](const std::string& id,
                            const std::string& json) {
        if (blocks.size() > 1) blocks += ",";
        blocks += "\"" + id + "\":" + json;
    };
    const auto source = [&](int i, const char* repetitions,
                            const char* comparison, const char* bound,
                            const char* thenVariable, const char* thenDelta,
                            const char* elseVariable, const char* elseDelta,
                            bool literalLeft = false) {
        const std::string index = std::to_string(i);
        const std::string conditional = "if" + index;
        const std::string loop = "loop" + index;
        const std::string op = "compare" + index;
        const std::string read = "read" + index;
        const std::string thenId = "then" + index;
        const std::string elseId = "else" + index;
        append("hat" + index, recv(repetitions ? loop : conditional));
        if (repetitions)
            append(loop, block("control_repeat", nullptr,
                std::string(R"({"TIMES":[1,[4,")") + repetitions +
                R"("]],"SUBSTACK":[2,")" + conditional + R"("]})"));
        append(conditional, block("control_if_else", nullptr,
            std::string(R"({"CONDITION":[2,")") + op +
                R"("],"SUBSTACK":[2,")" + thenId +
                R"("],"SUBSTACK2":[2,")" + elseId + R"("]})"));
        const std::string reporterInput =
            std::string(R"([2,")") + read + R"("])";
        const std::string literalInput =
            std::string(R"([1,[4,")") + bound + R"("]])";
        append(op, block(comparison, nullptr,
            std::string(R"({"OPERAND1":)") +
                (literalLeft ? literalInput : reporterInput) +
                R"(,"OPERAND2":)" +
                (literalLeft ? reporterInput : literalInput) + "}"));
        append(read, block("data_variable", nullptr, "{}",
                    R"({"VARIABLE":["x","x"]})"));
        append(thenId, change(thenVariable, thenDelta));
        append(elseId, change(elseVariable, elseDelta));
    };
    source(0, "4", "operator_gt", "0", "x", "-1", "a", "10");
    // Reversed comparisons: 1 > x, 2 = x and 0 < x. For numeric
    // and string operands these are equivalent to the original checks.
    source(1, "3", "operator_gt", "1", "b", "2", "a", "1", true);
    source(2, nullptr, "operator_equals", "2", "a", "100", "b", "100", true);
    source(3, "0", "operator_gt", "0", "a", "999", "b", "999");
    source(4, "2", "operator_lt", "0", "b", "1", "x", "3", true);
    blocks += "}";
    std::string stage = target("Stage", blocks, true);
    const std::string oldVariables = R"("variables":{"s":["s",""]})";
    const std::string newVariables =
        R"("variables":{"x":["x",3],"a":["a",0],"b":["b",0]})";
    const auto pos = stage.find(oldVariables);
    check(pos != std::string::npos,
          "mega if/else fixture Stage has expected initial layout");
    if (pos == std::string::npos) return;
    stage.replace(pos, oldVariables.size(), newVariables);
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega if/else fixture loads");
    if (!loaded.ok) return;
    Variable* x = rt.getTargetForStage()->lookupVariableById("x");
    Variable* a = rt.getTargetForStage()->lookupVariableById("a");
    Variable* b = rt.getTargetForStage()->lookupVariableById("b");
    if (!x || !a || !b) return;
    auto started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 5 && rt.threads.size() == 5,
          "mega if/else source creates five receiver Threads");
    if (started.size() != 5) return;
    const auto code = started.front()->megaDispatch;
    bool valid = code && code->multiPcBatchEntry &&
                 !code->scalarBatchEntry && code->entries == 5 &&
                 code->multiPcConditionals.size() == 5;
    for (size_t i = 0; valid && i < 5; ++i) {
        valid &= started[i]->megaDispatch == code &&
                 code->multiPcConditionals[i] &&
                 code->scalarVariableIds[i].size() == 3;
    }
    check(valid, "mega if/else emits single LLVM multi-PC comparator group");
    if (!valid) return;
    std::vector<Thread*> done;
    const auto pass = [&]() {
        return rt.sequencer->runSchedulerPass(
            false, false, false, false, done);
    };
    const uint64_t initial = rt.sequencer->megaScalarBatchCalls();
    check(pass() == 3 && rt.threads.size() == 3 &&
          cast::toNumber(x->value) == 2 &&
          cast::toNumber(a->value) == 101 &&
          cast::toNumber(b->value) == 1 &&
          rt.sequencer->megaScalarBatchCalls() == initial + 1,
          "mega if/else pass1 follows compare changes from earlier Thread");
    check(pass() == 3 && rt.threads.size() == 3 &&
          cast::toNumber(x->value) == 1 &&
          cast::toNumber(a->value) == 102 &&
          cast::toNumber(b->value) == 2,
          "mega if/else pass2 preserves in-order branch decisions");
    check(pass() == 2 && rt.threads.size() == 2 &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 102 &&
          cast::toNumber(b->value) == 4,
          "mega if/else pass3 selects less-than after another decrement");
    check(pass() == 1 && rt.threads.size() == 1 &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 112 &&
          cast::toNumber(b->value) == 4,
          "mega if/else pass4 takes alternate arm when condition flips");
    check(pass() == 0 && rt.threads.empty() &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 112 &&
          cast::toNumber(b->value) == 4,
          "mega if/else final repeat completion does not replay body");

    // Numeric-string comparison is Scratch-specific and cannot be passed
    // as a raw double to the multi-PC LLVM code; normal Mega JIT must
    // preserve Scratch's cast::compare semantics.
    x->value = Value::string(SStr::create(u"2"));
    a->value = Value::num(0);
    b->value = Value::num(0);
    started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 5 &&
          started.front()->megaDispatch == code,
          "mega if/else second broadcast reuses the cached source function");
    rt.runUntilDone(100);
    check(rt.threads.empty() &&
          cast::toNumber(x->value) == 1 &&
          cast::toNumber(a->value) == 2 &&
          cast::toNumber(b->value) == 103,
          "mega if/else falls back for numeric-string comparison preserving result");

    // Non-numeric input is string-compared by the Scratch helpers rather
    // than being interpreted as NaN-boxed IEEE bits inside LLVM.
    x->value = Value::string(SStr::create(u"abc"));
    a->value = Value::num(0);
    b->value = Value::num(0);
    rt.startBroadcastAndWait(u"PING");
    rt.runUntilDone(100);
    check(rt.threads.empty() && x->value.isNumber() &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 11 &&
          cast::toNumber(b->value) == 105,
          "mega if/else nonnumeric string comparison matches Scratch ordering");

    // Native batch must rebind all three operands on each pass, even when
    // a Variable with the same ID is erased and re-created between events.
    rt.getTargetForStage()->variables.erase("a");
    auto [recreated, inserted] = rt.getTargetForStage()->variables.emplace(
        "a", Variable::create("a", u"a", Variable::kScalarType, false));
    check(inserted, "mega if/else recreated a Stage variable with the same ID");
    if (!inserted) return;
    a = &recreated->second;
    a->value = Value::num(0);
    b->value = Value::num(0);
    x->value = Value::num(3);
    started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 5 &&
          started.front()->megaDispatch == code,
          "mega if/else retained the ORC function across binding recreation");
    rt.runUntilDone(100);
    check(rt.threads.empty() &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 112 &&
          cast::toNumber(b->value) == 4,
          "mega if/else binds a recreated then/else variable correctly");
}


void testMegaConditionalTwoChangeBatch() {
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* key) {
        const char* value = std::getenv(key);
        return value && value[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_BCAST_RECEIVERS") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        !enabled("XYO_JIT_MEGA_IF_ELSE_BATCH") ||
        !enabled("XYO_JIT_MEGA_BRANCH_PAIR") ||
        enabled("XYO_WAIT_WAKE_REGISTRY")) return;
    const auto recv = [](const std::string& first) {
        return std::string(R"({"opcode":"event_whenbroadcastreceived",)") +
            R"("topLevel":true,"next":")" + first +
            R"(","inputs":{},"fields":{"BROADCAST_OPTION":["PING",null]}})";
    };
    const auto change = [](const std::string& var,
                           const std::string& amount, const char* next) {
        return block("data_changevariableby", next,
            std::string(R"({"VALUE":[1,[4,")") + amount + R"("]]})",
            std::string(R"({"VARIABLE":[")") + var + R"(",")" + var +
            R"("]})");
    };
    std::string blocks = "{";
    const auto append = [&](const std::string& id,
                            const std::string& json) {
        if (blocks.size() > 1) blocks += ",";
        blocks += "\"" + id + "\":" + json;
    };
    const auto source = [&](int i, const char* repetitions,
                            const char* comparison, const char* bound,
                            const char* thenVariable, const char* thenDelta,
                            const char* elseVariable, const char* elseDelta,
                            bool literalLeft = false) {
        const std::string index = std::to_string(i);
        const std::string conditional = "if" + index;
        const std::string loop = "loop" + index;
        const std::string op = "compare" + index;
        const std::string read = "read" + index;
        const std::string thenId = "then" + index;
        const std::string elseId = "else" + index;
        append("hat" + index, recv(repetitions ? loop : conditional));
        if (repetitions)
            append(loop, block("control_repeat", nullptr,
                std::string(R"({"TIMES":[1,[4,")") + repetitions +
                R"("]],"SUBSTACK":[2,")" + conditional + R"("]})"));
        append(conditional, block("control_if_else", nullptr,
            std::string(R"({"CONDITION":[2,")") + op +
                R"("],"SUBSTACK":[2,")" + thenId +
                R"("],"SUBSTACK2":[2,")" + elseId + R"("]})"));
        const std::string reporterInput =
            std::string(R"([2,")") + read + R"("])";
        const std::string literalInput =
            std::string(R"([1,[4,")") + bound + R"("]])";
        append(op, block(comparison, nullptr,
            std::string(R"({"OPERAND1":)") +
                (literalLeft ? literalInput : reporterInput) +
                R"(,"OPERAND2":)" +
                (literalLeft ? reporterInput : literalInput) + "}"));
        append(read, block("data_variable", nullptr, "{}",
                    R"({"VARIABLE":["x","x"]})"));
        const std::string thenSecond = "thenSecond" + index;
        const std::string elseSecond = "elseSecond" + index;
        // Script 0 deliberately writes x twice in the same Scratch
        // branch. Both steps must execute in order, not be collapsed.
        append(thenId, change(thenVariable,
                              i == 0 ? "-0.5" : thenDelta,
                              thenSecond.c_str()));
        append(thenSecond, change(i == 0 ? "x" : "d",
                                  i == 0 ? "-0.5" : "1", nullptr));
        append(elseId, change(elseVariable, elseDelta,
                              elseSecond.c_str()));
        append(elseSecond, change("d", "2", nullptr));
    };
    source(0, "4", "operator_gt", "0", "x", "-1", "a", "10");
    // Reversed comparisons: 1 > x, 2 = x and 0 < x. For numeric
    // and string operands these are equivalent to the original checks.
    source(1, "3", "operator_gt", "1", "b", "2", "a", "1", true);
    source(2, nullptr, "operator_equals", "2", "a", "100", "b", "100", true);
    source(3, "0", "operator_gt", "0", "a", "999", "b", "999");
    source(4, "2", "operator_lt", "0", "b", "1", "x", "3", true);
    blocks += "}";
    std::string stage = target("Stage", blocks, true);
    const std::string oldVariables = R"("variables":{"s":["s",""]})";
    const std::string newVariables =
        R"("variables":{"x":["x",3],"a":["a",0],"b":["b",0],"d":["d",0]})";
    const auto pos = stage.find(oldVariables);
    check(pos != std::string::npos,
          "mega if/else fixture Stage has expected initial layout");
    if (pos == std::string::npos) return;
    stage.replace(pos, oldVariables.size(), newVariables);
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega if/else fixture loads");
    if (!loaded.ok) return;
    Variable* x = rt.getTargetForStage()->lookupVariableById("x");
    Variable* a = rt.getTargetForStage()->lookupVariableById("a");
    Variable* b = rt.getTargetForStage()->lookupVariableById("b");
    Variable* d = rt.getTargetForStage()->lookupVariableById("d");
    if (!x || !a || !b || !d) return;
    auto started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 5 && rt.threads.size() == 5,
          "mega if/else source creates five receiver Threads");
    if (started.size() != 5) return;
    const auto code = started.front()->megaDispatch;
    bool valid = code && code->multiPcBatchEntry &&
                 !code->scalarBatchEntry && code->entries == 5 &&
                 code->multiPcConditionals.size() == 5 &&
                 code->multiPcThenSecondChanges.size() == 5 &&
                 code->multiPcElseSecondChanges.size() == 5;
    for (size_t i = 0; valid && i < 5; ++i) {
        valid &= started[i]->megaDispatch == code &&
                 code->multiPcConditionals[i] &&
                 code->multiPcThenSecondChanges[i] &&
                 code->multiPcElseSecondChanges[i] &&
                 code->scalarVariableIds[i].size() == 5;
    }
    check(valid, "mega if/else emits single LLVM multi-PC comparator group");
    if (!valid) return;
    std::vector<Thread*> done;
    const auto pass = [&]() {
        return rt.sequencer->runSchedulerPass(
            false, false, false, false, done);
    };
    const uint64_t initial = rt.sequencer->megaScalarBatchCalls();
    check(pass() == 3 && rt.threads.size() == 3 &&
          cast::toNumber(x->value) == 2 &&
          cast::toNumber(a->value) == 101 &&
          cast::toNumber(b->value) == 1 &&
          cast::toNumber(d->value) == 4 &&
          rt.sequencer->megaScalarBatchCalls() == initial + 1,
          "mega if/else pass1 follows compare changes from earlier Thread");
    check(pass() == 3 && rt.threads.size() == 3 &&
          cast::toNumber(x->value) == 1 &&
          cast::toNumber(a->value) == 102 &&
          cast::toNumber(b->value) == 2 &&
          cast::toNumber(d->value) == 7,
          "mega if/else pass2 preserves in-order branch decisions");
    check(pass() == 2 && rt.threads.size() == 2 &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 102 &&
          cast::toNumber(b->value) == 4 &&
          cast::toNumber(d->value) == 8,
          "mega if/else pass3 selects less-than after another decrement");
    check(pass() == 1 && rt.threads.size() == 1 &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 112 &&
          cast::toNumber(b->value) == 4 &&
          cast::toNumber(d->value) == 10,
          "mega if/else pass4 takes alternate arm when condition flips");
    check(pass() == 0 && rt.threads.empty() &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 112 &&
          cast::toNumber(b->value) == 4,
          "mega if/else final repeat completion does not replay body");

    check(cast::toNumber(d->value) == 10,
          "mega conditional pair leaves all branch-second updates ordered");
    // Numeric-string comparison is Scratch-specific and cannot be passed
    // as a raw double to the multi-PC LLVM code; normal Mega JIT must
    // preserve Scratch's cast::compare semantics.
    x->value = Value::string(SStr::create(u"2"));
    a->value = Value::num(0);
    b->value = Value::num(0);
    started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 5 &&
          started.front()->megaDispatch == code,
          "mega if/else second broadcast reuses the cached source function");
    rt.runUntilDone(100);
    check(rt.threads.empty() &&
          cast::toNumber(x->value) == 1 &&
          cast::toNumber(a->value) == 2 &&
          cast::toNumber(b->value) == 103,
          "mega if/else falls back for numeric-string comparison preserving result");

    // Non-numeric input is string-compared by the Scratch helpers rather
    // than being interpreted as NaN-boxed IEEE bits inside LLVM.
    x->value = Value::string(SStr::create(u"abc"));
    a->value = Value::num(0);
    b->value = Value::num(0);
    rt.startBroadcastAndWait(u"PING");
    rt.runUntilDone(100);
    check(rt.threads.empty() && x->value.isNumber() &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 11 &&
          cast::toNumber(b->value) == 105,
          "mega if/else nonnumeric string comparison matches Scratch ordering");

    // Native batch must rebind all three operands on each pass, even when
    // a Variable with the same ID is erased and re-created between events.
    rt.getTargetForStage()->variables.erase("a");
    auto [recreated, inserted] = rt.getTargetForStage()->variables.emplace(
        "a", Variable::create("a", u"a", Variable::kScalarType, false));
    check(inserted, "mega if/else recreated a Stage variable with the same ID");
    if (!inserted) return;
    a = &recreated->second;
    a->value = Value::num(0);
    b->value = Value::num(0);
    x->value = Value::num(3);
    started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 5 &&
          started.front()->megaDispatch == code,
          "mega if/else retained the ORC function across binding recreation");
    rt.runUntilDone(100);
    check(rt.threads.empty() &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 112 &&
          cast::toNumber(b->value) == 4,
          "mega if/else binds a recreated then/else variable correctly");

    x->value = Value::num(3);
    a->value = Value::string(SStr::create(u"10"));
    b->value = d->value = Value::num(0);
    rt.startBroadcastAndWait(u"PING");
    rt.runUntilDone(100);
    check(rt.threads.empty() &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 122 &&
          cast::toNumber(b->value) == 4 &&
          cast::toNumber(d->value) == 10,
          "mega conditional pair string write target uses precise helper fallback");

    // Invalidate a cached second-write Variable* while the repeat
    // continuations are suspended at odd PCs. The binding generation
    // must be inspected before the NEXT read, not after dereferencing.
    x->value = Value::num(3);
    a->value = b->value = d->value = Value::num(0);
    started = rt.startBroadcastAndWait(u"PING");
    done.clear();
    check(started.size() == 5 && pass() == 3 &&
          cast::toNumber(d->value) == 4,
          "mega conditional pair first turn caches all five operands");
    rt.getTargetForStage()->variables.erase("d");
    auto [rebuilt, created] = rt.getTargetForStage()->variables.emplace(
        "d", Variable::create("d", u"d", Variable::kScalarType, false));
    check(created, "mega conditional pair recreated suspended second write");
    if (!created) return;
    d = &rebuilt->second;
    d->value = Value::num(50);
    rt.runUntilDone(100);
    check(rt.threads.empty() && cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 112 &&
          cast::toNumber(b->value) == 4 &&
          cast::toNumber(d->value) == 56,
          "mega conditional pair rebinds cached second write after generation change");
}



void testMegaIfNoElseBatch() {
    // A false control_if arm must be a NO-OP, including when repeated,
    // and a later Thread in the same LLVM call must see earlier writes.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* key) {
        const char* e = std::getenv(key);
        return e && e[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_BCAST_RECEIVERS") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        !enabled("XYO_JIT_MEGA_IF_ELSE_BATCH") ||
        enabled("XYO_WAIT_WAKE_REGISTRY")) return;

    std::string blocks = "{";
    const auto append = [&](const std::string& id,
                            const std::string& json) {
        if (blocks.size() != 1) blocks += ",";
        blocks += "\"" + id + "\":" + json;
    };
    const auto addSource = [&](int index, const char* repetitions,
                               const char* op, const char* bound,
                               bool literalLeft, const char* dest,
                               const char* delta) {
        const std::string suffix = std::to_string(index);
        const std::string hat = "hat" + suffix;
        const std::string loop = "loop" + suffix;
        const std::string cond = "cond" + suffix;
        const std::string cmp = "cmp" + suffix;
        const std::string read = "read" + suffix;
        const std::string yes = "yes" + suffix;
        append(hat, std::string(
            R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":")") +
            (repetitions ? loop : cond) +
            R"(","inputs":{},"fields":{"BROADCAST_OPTION":["PING",null]}})");
        if (repetitions)
            append(loop, block("control_repeat", nullptr,
                std::string(R"({"TIMES":[1,[4,")") + repetitions +
                R"("]],"SUBSTACK":[2,")" + cond + R"("]})"));
        append(cond, block("control_if", nullptr,
            std::string(R"({"CONDITION":[2,")") + cmp +
            R"("],"SUBSTACK":[2,")" + yes + R"("]})"));
        const std::string readOperand = std::string(R"([2,")") +
                                        read + R"("])";
        const std::string literalOperand =
            std::string(R"([1,[4,")") + bound + R"("]])";
        append(cmp, block(op, nullptr,
            std::string(R"({"OPERAND1":)") +
            (literalLeft ? literalOperand : readOperand) +
            R"(,"OPERAND2":)" +
            (literalLeft ? readOperand : literalOperand) + "}"));
        append(read, block("data_variable", nullptr, "{}",
            R"({"VARIABLE":["x","x"]})"));
        append(yes, block("data_changevariableby", nullptr,
            std::string(R"({"VALUE":[1,[4,")") + delta + R"("]]})",
            std::string(R"({"VARIABLE":[")") + dest +
            R"(",")" + dest + R"("]})"));
    };
    addSource(0, "4", "operator_gt", "0", false, "x", "-1");
    addSource(1, "3", "operator_gt", "2", true, "a", "2");
    addSource(2, nullptr, "operator_equals", "1", true, "b", "10");
    addSource(3, "0", "operator_gt", "0", false, "c", "99");
    blocks += "}";

    std::string stage = target("Stage", blocks, true);
    const std::string previous = R"("variables":{"s":["s",""]})";
    const auto pos = stage.find(previous);
    check(pos != std::string::npos, "mega if-only Stage fixture has variables");
    if (pos == std::string::npos) return;
    stage.replace(pos, previous.size(),
        R"("variables":{"x":["x",2],"a":["a",0],"b":["b",0],"c":["c",0]})");
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega if-only fixture loads");
    if (!loaded.ok) return;

    Target* stageTarget = rt.getTargetForStage();
    Variable* x = stageTarget->lookupVariableById("x");
    Variable* a = stageTarget->lookupVariableById("a");
    Variable* b = stageTarget->lookupVariableById("b");
    Variable* c = stageTarget->lookupVariableById("c");
    if (!x || !a || !b || !c) return;

    auto started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 4 && rt.threads.size() == 4,
          "mega if-only starts four source-certified Threads");
    if (started.size() != 4) return;
    const auto code = started.front()->megaDispatch;
    bool valid = code && code->multiPcBatchEntry && code->entries == 4 &&
                 code->multiPcConditionals.size() == 4 &&
                 code->multiPcElseBranches.size() == 4;
    for (size_t i = 0; valid && i < 4; ++i) {
        valid &= started[i]->megaDispatch == code &&
                 code->multiPcConditionals[i] &&
                 !code->multiPcElseBranches[i] &&
                 code->scalarVariableIds[i].size() == 2;
    }
    check(valid,
          "mega if-only shares LLVM branch executor without an ELSE pointer");
    if (!valid) return;
    check(code->singleFastSourceKinds ==
              std::vector<uint8_t>({0, 0, 0, 0}),
          "conditional branches do not qualify for single fast-source proof");

    std::vector<Thread*> done;
    const auto pass = [&] {
        return rt.sequencer->runSchedulerPass(
            false, false, false, false, done);
    };
    const uint64_t calls = rt.sequencer->megaScalarBatchCalls();
    check(pass() == 2 && rt.threads.size() == 2 &&
          cast::toNumber(x->value) == 1 &&
          cast::toNumber(a->value) == 2 &&
          cast::toNumber(b->value) == 10 &&
          cast::toNumber(c->value) == 0 &&
          rt.sequencer->megaScalarBatchCalls() == calls + 1,
          "mega if-only pass1 commits true arms and retires one-shot");
    check(pass() == 2 && rt.threads.size() == 2 &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 4,
          "mega if-only pass2 updates a comparison variable before the next arm");
    check(pass() == 2 && cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 6,
          "mega if-only pass3 false arm makes no write but yields");
    check(pass() == 1 && cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 6,
          "mega if-only pass4 repeat returns without an extra write");
    check(pass() == 0 && rt.threads.empty() &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 6 &&
          cast::toNumber(b->value) == 10,
          "mega if-only finishes after the last resumed repeat");

    x->value = Value::string(SStr::create(u"2"));
    a->value = b->value = c->value = Value::num(0);
    started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 4 &&
          started.front()->megaDispatch == code,
          "mega if-only keeps the compiled ORC object across broadcasts");
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 6 &&
          cast::toNumber(b->value) == 10 &&
          cast::toNumber(c->value) == 0,
          "mega if-only numeric-string input falls back to Scratch comparison");
}

void testMegaIfNoElsePairBatch() {
    // A false control_if arm must be a NO-OP, including when repeated,
    // and a later Thread in the same LLVM call must see earlier writes.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* key) {
        const char* e = std::getenv(key);
        return e && e[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_BCAST_RECEIVERS") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        !enabled("XYO_JIT_MEGA_IF_ELSE_BATCH") ||
        !enabled("XYO_JIT_MEGA_BRANCH_PAIR") ||
        enabled("XYO_WAIT_WAKE_REGISTRY")) return;

    std::string blocks = "{";
    const auto append = [&](const std::string& id,
                            const std::string& json) {
        if (blocks.size() != 1) blocks += ",";
        blocks += "\"" + id + "\":" + json;
    };
    const auto addSource = [&](int index, const char* repetitions,
                               const char* op, const char* bound,
                               bool literalLeft, const char* dest,
                               const char* delta) {
        const std::string suffix = std::to_string(index);
        const std::string hat = "hat" + suffix;
        const std::string loop = "loop" + suffix;
        const std::string cond = "cond" + suffix;
        const std::string cmp = "cmp" + suffix;
        const std::string read = "read" + suffix;
        const std::string yes = "yes" + suffix;
        const std::string second = "yesSecond" + suffix;
        append(hat, std::string(
            R"({"opcode":"event_whenbroadcastreceived","topLevel":true,"next":")") +
            (repetitions ? loop : cond) +
            R"(","inputs":{},"fields":{"BROADCAST_OPTION":["PING",null]}})");
        if (repetitions)
            append(loop, block("control_repeat", nullptr,
                std::string(R"({"TIMES":[1,[4,")") + repetitions +
                R"("]],"SUBSTACK":[2,")" + cond + R"("]})"));
        append(cond, block("control_if", nullptr,
            std::string(R"({"CONDITION":[2,")") + cmp +
            R"("],"SUBSTACK":[2,")" + yes + R"("]})"));
        const std::string readOperand = std::string(R"([2,")") +
                                        read + R"("])";
        const std::string literalOperand =
            std::string(R"([1,[4,")") + bound + R"("]])";
        append(cmp, block(op, nullptr,
            std::string(R"({"OPERAND1":)") +
            (literalLeft ? literalOperand : readOperand) +
            R"(,"OPERAND2":)" +
            (literalLeft ? readOperand : literalOperand) + "}"));
        append(read, block("data_variable", nullptr, "{}",
            R"({"VARIABLE":["x","x"]})"));
        append(yes, block("data_changevariableby", second.c_str(),
            std::string(R"({"VALUE":[1,[4,")") + delta + R"("]]})",
            std::string(R"({"VARIABLE":[")") + dest +
            R"(",")" + dest + R"("]})"));
        append(second, block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})",
            R"({"VARIABLE":["d","d"]})"));
    };
    addSource(0, "4", "operator_gt", "0", false, "x", "-1");
    addSource(1, "3", "operator_gt", "2", true, "a", "2");
    addSource(2, nullptr, "operator_equals", "1", true, "b", "10");
    addSource(3, "0", "operator_gt", "0", false, "c", "99");
    blocks += "}";

    std::string stage = target("Stage", blocks, true);
    const std::string previous = R"("variables":{"s":["s",""]})";
    const auto pos = stage.find(previous);
    check(pos != std::string::npos, "mega if-only Stage fixture has variables");
    if (pos == std::string::npos) return;
    stage.replace(pos, previous.size(),
        R"("variables":{"x":["x",2],"a":["a",0],"b":["b",0],"c":["c",0],"d":["d",0]})");
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega if-only fixture loads");
    if (!loaded.ok) return;

    Target* stageTarget = rt.getTargetForStage();
    Variable* x = stageTarget->lookupVariableById("x");
    Variable* a = stageTarget->lookupVariableById("a");
    Variable* b = stageTarget->lookupVariableById("b");
    Variable* c = stageTarget->lookupVariableById("c");
    Variable* d = stageTarget->lookupVariableById("d");
    if (!x || !a || !b || !c || !d) return;

    auto started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 4 && rt.threads.size() == 4,
          "mega if-only starts four source-certified Threads");
    if (started.size() != 4) return;
    const auto code = started.front()->megaDispatch;
    bool valid = code && code->multiPcBatchEntry && code->entries == 4 &&
                 code->multiPcConditionals.size() == 4 &&
                 code->multiPcElseBranches.size() == 4 &&
                 code->multiPcThenSecondChanges.size() == 4 &&
                 code->multiPcElseSecondChanges.size() == 4;
    for (size_t i = 0; valid && i < 4; ++i) {
        valid &= started[i]->megaDispatch == code &&
                 code->multiPcConditionals[i] &&
                 !code->multiPcElseBranches[i] &&
                 code->multiPcThenSecondChanges[i] &&
                 !code->multiPcElseSecondChanges[i] &&
                 code->scalarVariableIds[i].size() == 3;
    }
    check(valid,
          "mega if-only shares LLVM branch executor without an ELSE pointer");
    if (!valid) return;

    std::vector<Thread*> done;
    const auto pass = [&] {
        return rt.sequencer->runSchedulerPass(
            false, false, false, false, done);
    };
    const uint64_t calls = rt.sequencer->megaScalarBatchCalls();
    check(pass() == 2 && rt.threads.size() == 2 &&
          cast::toNumber(x->value) == 1 &&
          cast::toNumber(a->value) == 2 &&
          cast::toNumber(b->value) == 10 &&
          cast::toNumber(c->value) == 0 &&
          cast::toNumber(d->value) == 3 &&
          rt.sequencer->megaScalarBatchCalls() == calls + 1,
          "mega if-only pass1 commits true arms and retires one-shot");
    check(pass() == 2 && rt.threads.size() == 2 &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 4 &&
          cast::toNumber(d->value) == 5,
          "mega if-only pass2 updates a comparison variable before the next arm");
    check(pass() == 2 && cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 6 &&
          cast::toNumber(d->value) == 6,
          "mega if-only pass3 false arm makes no write but yields");
    check(pass() == 1 && cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 6 &&
          cast::toNumber(d->value) == 6,
          "mega if-only pass4 repeat returns without an extra write");
    check(pass() == 0 && rt.threads.empty() &&
          cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 6 &&
          cast::toNumber(b->value) == 10 &&
          cast::toNumber(d->value) == 6,
          "mega if-only finishes after the last resumed repeat");

    x->value = Value::string(SStr::create(u"2"));
    a->value = b->value = c->value = d->value = Value::num(0);
    started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 4 &&
          started.front()->megaDispatch == code,
          "mega if-only keeps the compiled ORC object across broadcasts");
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(x->value) == 0 &&
          cast::toNumber(a->value) == 6 &&
          cast::toNumber(b->value) == 10 &&
          cast::toNumber(c->value) == 0 &&
          cast::toNumber(d->value) == 6,
          "mega if-only numeric-string input falls back to Scratch comparison");
}


void testMegaTwoChangeSequence() {
    // Two different target-local IDs and two aliasing writes in the same
    // Scratch turn. Repeat counters and scalar pair ordering are native.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* key) {
        const char* value = std::getenv(key);
        return value && value[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_BCAST_RECEIVERS") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        enabled("XYO_WAIT_WAKE_REGISTRY")) return;

    const auto hatRecv = [](const char* next) {
        return std::string(R"({"opcode":"event_whenbroadcastreceived",)") +
            R"("topLevel":true,"next":")" + next +
            R"(","inputs":{},"fields":{"BROADCAST_OPTION":["PING",null]}})";
    };
    const auto repeat = [](const char* count, const char* body) {
        return block("control_repeat", nullptr,
            std::string(R"({"TIMES":[1,[4,")") + count +
            R"("]],"SUBSTACK":[2,")" + body + R"("]})");
    };
    const auto change = [](const char* variable, const char* next,
                           const char* value) {
        return block("data_changevariableby", next,
            std::string(R"({"VALUE":[1,[4,")") + value + R"("]]})",
            std::string(R"({"VARIABLE":[")") + variable +
            R"(",")" + variable + R"("]})");
    };
    std::string blocks = "{";
    const auto append = [&](const std::string& id,
                            const std::string& value) {
        if (blocks.size() > 1) blocks += ",";
        blocks += "\"" + id + "\":" + value;
    };
    const auto group = [&](int i, const char* count,
                           const char* first, const char* d1,
                           const char* second, const char* d2) {
        const std::string suffix = std::to_string(i);
        const std::string firstId = "one" + suffix;
        const std::string secondId = "two" + suffix;
        const std::string loop = "loop" + suffix;
        const std::string hatId = "hat" + suffix;
        append(hatId, hatRecv(count ? loop.c_str() : firstId.c_str()));
        if (count) append(loop, repeat(count, firstId.c_str()));
        append(firstId, change(first, secondId.c_str(), d1));
        append(secondId, change(second, nullptr, d2));
    };
    group(0, "3", "s", "1", "t", "10");
    group(1, "2", "t", "2", "s", "20");
    group(2, nullptr, "s", "3", "t", "30");
    group(3, "0", "s", "100", "t", "1000");
    group(4, "1", "s", "4", "s", "5");
    blocks += "}";

    std::string stage = target("Stage", blocks, true);
    const std::string oldVariables = R"("variables":{"s":["s",""]})";
    const std::string newVariables =
        R"("variables":{"s":["s",0],"t":["t",0]})";
    const auto pos = stage.find(oldVariables);
    check(pos != std::string::npos,
          "mega two-change fixture has explicit initial Stage variables");
    if (pos == std::string::npos) return;
    stage.replace(pos, oldVariables.size(), newVariables);
    Runtime rt;
    auto loaded = loadSb3(rt, "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega two-change source fixture loads");
    if (!loaded.ok) return;
    Variable* s = rt.getTargetForStage()->lookupVariableById("s");
    Variable* tvar = rt.getTargetForStage()->lookupVariableById("t");
    if (!s || !tvar) return;

    auto started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 5 && rt.threads.size() == 5,
          "mega two-change creates five exact receiver identities");
    if (started.size() != 5) return;
    const auto code = started.front()->megaDispatch;
    bool sameCode = code && code->multiPcBatchEntry &&
                    !code->scalarBatchEntry && code->entries == 5;
    for (size_t i = 0; i < started.size(); ++i) {
        sameCode &= started[i]->megaDispatch == code &&
                    started[i]->megaPc == 2*i;
        if (code && i < code->scalarVariableIds.size())
            sameCode &= code->scalarVariableIds[i].size() == 2 &&
                        code->multiPcSecondChanges[i];
    }
    check(sameCode,
          "mega two-change compiles distinct/alias ordered native pairs");
    if (!sameCode) return;

    std::vector<Thread*> done;
    const auto pass = [&] {
        return rt.sequencer->runSchedulerPass(
            false, false, false, false, done);
    };
    const uint64_t before = rt.sequencer->megaScalarBatchCalls();
    check(pass() == 3 && rt.threads.size() == 3 &&
          cast::toNumber(s->value) == 33 &&
          cast::toNumber(tvar->value) == 42 &&
          rt.sequencer->megaScalarBatchCalls() == before + 1 &&
          rt.sequencer->megaScalarBatchThreads() >= 5,
          "mega two-change first pass commits two writes per active Thread");
    check(pass() == 2 && rt.threads.size() == 2 &&
          cast::toNumber(s->value) == 54 &&
          cast::toNumber(tvar->value) == 54,
          "mega two-change second pass preserves per-source write order");
    check(pass() == 1 && rt.threads.size() == 1 &&
          cast::toNumber(s->value) == 55 &&
          cast::toNumber(tvar->value) == 64,
          "mega two-change third pass completes different repeat counters");
    check(pass() == 0 && rt.threads.empty() &&
          cast::toNumber(s->value) == 55 &&
          cast::toNumber(tvar->value) == 64,
          "mega two-change final count zero exits without extra body");
    started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 5 &&
          started.front()->megaDispatch == code,
          "mega two-change shares cached ORC object on repeat broadcast");
    rt.runUntilDone(50);
    check(rt.threads.empty() &&
          cast::toNumber(s->value) == 110 &&
          cast::toNumber(tvar->value) == 128,
          "mega two-change repeat broadcast gives identical cumulative state");

    tvar->value = Value::string(SStr::create(u"100"));
    rt.startBroadcastAndWait(u"PING");
    rt.runUntilDone(50);
    check(rt.threads.empty() &&
          cast::toNumber(s->value) == 165 &&
          cast::toNumber(tvar->value) == 164,
          "mega two-change guards second binding type and falls back");
}

void testMegaMultiPcBatch() {
    // Exactly one source step per Thread per scheduler pass, including
    // a zero-iteration loop and a one-shot handler in the same LLVM batch.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* key) {
        const char* flag = std::getenv(key);
        return flag && flag[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        enabled("XYO_WAIT_WAKE_REGISTRY")) return;

    const auto recvHat = [](const char* next) {
        return std::string(R"({"opcode":"event_whenbroadcastreceived",)") +
            R"("topLevel":true,"next":")" + next +
            R"(","inputs":{},"fields":{"BROADCAST_OPTION":["PING",null]}})";
    };
    const auto repeat = [](const char* count, const char* body) {
        return block("control_repeat", nullptr,
            std::string(R"({"TIMES":[1,[4,")") + count +
            R"("]],"SUBSTACK":[2,")" + body + R"("]})");
    };
    const auto change = [](const char* amount) {
        return block("data_changevariableby", nullptr,
            std::string(R"({"VALUE":[1,[4,")") + amount + R"("]]})",
            R"({"VARIABLE":["s","s"]})");
    };
    const std::string blocks =
        "{\"hat0\":" + recvHat("loop0") +
        ",\"loop0\":" + repeat("3", "inc0") +
        ",\"inc0\":" + change("1") +
        ",\"hat1\":" + recvHat("loop1") +
        ",\"loop1\":" + repeat("2", "inc1") +
        ",\"inc1\":" + change("2") +
        ",\"hat2\":" + recvHat("loop2") +
        ",\"loop2\":" + repeat("0", "inc2") +
        ",\"inc2\":" + change("50") +
        ",\"hat3\":" + recvHat("inc3") +
        ",\"inc3\":" + change("5") +
        ",\"hat4\":" + recvHat("loop4") +
        ",\"loop4\":" + repeat("1", "inc4") +
        ",\"inc4\":" + change("3") + "}";
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega multi-PC mixed scalar/repeat fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    if (!sum) return;
    auto started = rt.startBroadcastAndWait(u"PING");
    if (started.size() != 5 || rt.threads.size() != 5) {
        check(false, "mega multi-PC started exactly five Threads");
        return;
    }
    const auto code = rt.threads.front()->megaDispatch;
    check(code && code->multiPcBatchEntry && !code->scalarBatchEntry &&
          code->entries == 5 &&
          code->multiPcKinds == std::vector<uint8_t>({1, 1, 1, 0, 1}) &&
          code->multiPcRepetitions == std::vector<uint64_t>({3, 2, 0, 0, 1}),
          "mega multi-PC emits one mixed-PC LLVM executor and source metadata");
    if (!code || !code->multiPcBatchEntry) return;
    if (enabled("XYO_JIT_MEGA_MULTI_REPEAT_BURST"))
        check(!code->multiRepeatBurstEntry &&
              code->multiRepeatSourceArity.empty(),
              "mixed Scalar/Repeat sources never emit unused multi-burst LLVM code");
    check(code->singleFastSourceKinds ==
              std::vector<uint8_t>({2, 2, 2, 1, 2}),
          "mega mixed sources preprove repeat vs scalar slots");

    const uint64_t beforeCalls = rt.sequencer->megaScalarBatchCalls();
    const uint64_t beforeThreads = rt.sequencer->megaScalarBatchThreads();
    std::vector<Thread*> done;
    const auto pass = [&] {
        return rt.sequencer->runSchedulerPass(
            false, false, false, false, done);
    };

    const size_t active1 = pass();
    check(active1 == 3 && rt.threads.size() == 3 &&
          cast::toNumber(sum->value) == 11 &&
          rt.sequencer->megaScalarBatchCalls() == beforeCalls + 1 &&
          rt.sequencer->megaScalarBatchThreads() >= beforeThreads + 2,
          "mega multi-PC first pass executes each source once; retires zero and scalar");
    check(rt.threads[0]->megaPc == 1 &&
          rt.threads[0]->megaRemaining == 2 &&
          rt.threads[1]->megaPc == 3 &&
          rt.threads[1]->megaRemaining == 1 &&
          rt.threads[2]->megaPc == 9 &&
          rt.threads[2]->megaRemaining == 0,
          "mega multi-PC stores independent odd resume PC and repeat counters");

    const size_t active2 = pass();
    check(active2 == 2 && rt.threads.size() == 2 &&
          cast::toNumber(sum->value) == 14 &&
          rt.sequencer->megaScalarBatchCalls() == beforeCalls + 2,
          "mega multi-PC second pass resumes both counters, retires repeat(1)");

    // An optional one-item fast path uses the same multi-PC LLVM switch
    // for the final survivor; the old one-item JitCtx fallback stays
    // available when XYO_JIT_MEGA_SINGLE_DISPATCH is unset.
    const bool single = enabled("XYO_JIT_MEGA_SINGLE_DISPATCH");
    const size_t active3 = pass();
    check(active3 == 1 && rt.threads.size() == 1 &&
          cast::toNumber(sum->value) == 15 &&
          rt.threads[0]->megaPc == 1 &&
          rt.threads[0]->megaRemaining == 0 &&
          rt.sequencer->megaScalarBatchCalls() ==
              beforeCalls + (single ? 3u : 2u),
          "mega multi-PC third pass advances sole survivor exactly once");

    // An already created JitCtx is a genuine native-continuation guard:
    // skipping it would bypass its state even if the source/PC looks simple.
    const bool fastFinal = single && rt.threads.size() == 1 &&
                           !rt.threads[0]->jitCtx;
    const auto callsBeforeFinal = rt.sequencer->megaScalarBatchCalls();
    const size_t active4 = pass();
    check(active4 == 0 && rt.threads.empty() &&
          cast::toNumber(sum->value) == 15 &&
          rt.sequencer->megaScalarBatchCalls() ==
              callsBeforeFinal + uint64_t(fastFinal),
          "mega multi-PC final survivor preserves existing JitCtx guard");

    // Reuse the same cached LLVM code with new Thread identities.
    started = rt.startBroadcastAndWait(u"PING");
    check(started.size() == 5 && rt.threads.size() == 5 &&
          rt.threads.front()->megaDispatch == code,
          "mega multi-PC receiver cache hit preserves compiled function");
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 30 &&
          rt.sequencer->megaScalarBatchCalls() > beforeCalls + 3,
          "mega multi-PC second broadcast matches scalar execution result");

    // Change a numeric Variable to a string after multiple native repeat
    // entries yielded. The next selection must deopt to the normal numeric
    // conversion helper, without replaying committed scratch operations.
    started = rt.startBroadcastAndWait(u"PING");
    done.clear();
    const size_t beforeTypeChange = pass();
    check(beforeTypeChange == 3 && cast::toNumber(sum->value) == 41,
          "mega multi-PC creates genuine suspended repeat continuations");
    sum->value = Value::string(SStr::create(u"100"));
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 104,
          "mega multi-PC fallback resumes after mid-repeat string replacement");
}

void testMegaSingleSurvivorBatch() {
    // Two source-certified repeat roots. The shorter one exits before
    // the longer one's final scheduler passes. Only the clean Thread
    // without a native JitCtx may take the one-item LLVM dispatcher.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* key) {
        const char* value = std::getenv(key);
        return value && value[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        !enabled("XYO_JIT_MEGA_SINGLE_DISPATCH") ||
        enabled("XYO_WAIT_WAKE_REGISTRY")) return;

    const auto repeat = [](const char* count, const char* body) {
        return block("control_repeat", nullptr,
            std::string(R"({"TIMES":[1,[4,")") + count +
            R"("]],"SUBSTACK":[2,")" + body + R"("]})");
    };
    const auto change = [](const char* delta) {
        return block("data_changevariableby", nullptr,
            std::string(R"({"VALUE":[1,[4,")") + delta + R"("]]})",
            R"({"VARIABLE":["s","s"]})");
    };
    const std::string blocks =
        "{\"hat0\":" + hat("loop0") +
        ",\"loop0\":" + repeat("4", "change0") +
        ",\"change0\":" + change("1") +
        ",\"hat1\":" + hat("loop1") +
        ",\"loop1\":" + repeat("1", "change1") +
        ",\"change1\":" + change("2") + "}";
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega single survivor fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    if (!sum) return;
    const size_t priorFibers = FiberPool::activeCount();
    rt.greenFlag();
    // The shared Stage fixture's default s is an empty string, which
    // deliberately forces the ordinary JitCtx path. Use a numeric value
    // for this specific proof of a clean, never-materialized continuation.
    sum->value = Value::num(0);
    const auto code = rt.threads.empty() ? nullptr :
                      rt.threads.front()->megaDispatch;
    check(rt.threads.size() == 2 && code &&
          code->multiPcBatchEntry && !code->scalarBatchEntry,
          "mega single survivor shares verified multi-PC LLVM code");
    if (!code || !code->multiPcBatchEntry) return;
    const bool specialize = enabled("XYO_JIT_MEGA_SINGLE_SPECIALIZE");
    check(bool(code->multiPcSingleEntry) == specialize,
          "mega single specialized LLVM entry is opt-in only");
    if (specialize) {
        // Independent states ensure neither LLVM entry can affect the
        // other's variables, PC or remaining iteration count.
        auto ordinary = Variable::create(
            "s", u"s", Variable::kScalarType, false);
        auto fast = Variable::create(
            "s", u"s", Variable::kScalarType, false);
        ordinary.value = Value::num(0);
        fast.value = Value::num(0);
        uint32_t ordinaryPc = 0, fastPc = 0;
        uint64_t ordinaryRemaining = 0, fastRemaining = 0;
        MegaDispatchCode::MultiPcItem lhs{
            &ordinary, nullptr, nullptr, nullptr, nullptr,
            &ordinaryPc, &ordinaryRemaining};
        MegaDispatchCode::MultiPcItem rhs{
            &fast, nullptr, nullptr, nullptr, nullptr,
            &fastPc, &fastRemaining};
        bool match = true;
        for (int k = 0; k < 6; ++k) {
            code->multiPcBatchEntry(&lhs, 1);
            code->multiPcSingleEntry(&rhs);
            match &= ordinary.value.bits == fast.value.bits &&
                     ordinaryPc == fastPc &&
                     ordinaryRemaining == fastRemaining;
        }
        check(match && ordinaryPc == UINT32_MAX &&
              cast::toNumber(ordinary.value) == 4,
              "specialized and generic multi-PC preserve every state transition");
    }
    const uint64_t baseline = rt.sequencer->megaScalarBatchCalls();
    const uint64_t fastBaseline = rt.sequencer->megaSingleFastCalls();
    const bool fastScan = enabled("XYO_JIT_MEGA_SINGLE_FAST_SCAN") &&
                          specialize;
    if (fastScan) {
        check(code->singleFastSourceKinds ==
                  std::vector<uint8_t>({2, 2}),
              "compiler classifies both independent single-variable repeats");
    }
    std::vector<Thread*> completed;
    const auto pass = [&]() {
        return rt.sequencer->runSchedulerPass(
            false, false, false, false, completed);
    };
    check(pass() == 2 && rt.threads.size() == 2 &&
          cast::toNumber(sum->value) == 3 &&
          rt.sequencer->megaScalarBatchCalls() == baseline + 1,
          "mega single survivor first pass executes both once");
    check(pass() == 1 && rt.threads.size() == 1 &&
          cast::toNumber(sum->value) == 4 &&
          rt.sequencer->megaScalarBatchCalls() == baseline + 2,
          "mega single survivor second pass retires shorter script");
    check(rt.threads.front()->jitCtx == nullptr &&
          rt.threads.front()->fiber == nullptr,
          "mega single survivor has no untracked native continuation");
    check(pass() == 1 && rt.threads.size() == 1 &&
          cast::toNumber(sum->value) == 5 &&
          rt.sequencer->megaScalarBatchCalls() == baseline + 3,
          "mega single survivor third pass dispatches one LLVM item");
    check(pass() == 1 && rt.threads.size() == 1 &&
          cast::toNumber(sum->value) == 6 &&
          rt.sequencer->megaScalarBatchCalls() == baseline + 4,
          "mega single survivor preserves exact final body count");
    check(pass() == 0 && rt.threads.empty() &&
          cast::toNumber(sum->value) == 6 &&
          rt.sequencer->megaScalarBatchCalls() == baseline + 5 &&
          FiberPool::activeCount() == priorFibers,
          "mega single survivor retires without Fiber or duplicate effects");
    check(rt.sequencer->megaSingleFastCalls() ==
              fastBaseline + (fastScan ? 3u : 0u),
          "cached final survivor uses early fast scan only when enabled");

    if (fastScan) {
        // Replace a live variable map entry after the batch cache has
        // borrowed its pointer. The early path must reject the stale
        // structural generation BEFORE dereferencing the old pointer.
        rt.greenFlag();
        sum->value = Value::num(0);
        check(pass() == 2 && pass() == 1 &&
              rt.threads.size() == 1 &&
              cast::toNumber(sum->value) == 4,
              "single fast scan prepares a live cached continuation");
        auto& variables = rt.getTargetForStage()->variables;
        variables.erase("s");
        auto [it, inserted] = variables.emplace(
            "s", Variable::create("s", u"s",
                                   Variable::kScalarType, false));
        check(inserted, "single fast scan replacement variable inserted");
        sum = &it->second;
        sum->value = Value::num(100);
        const uint64_t beforeRebind = rt.sequencer->megaSingleFastCalls();
        check(pass() == 1 && cast::toNumber(sum->value) == 101 &&
              rt.sequencer->megaSingleFastCalls() == beforeRebind,
              "single fast scan refuses stale binding and rebinds safely");
        check(pass() == 1 && cast::toNumber(sum->value) == 102 &&
              rt.sequencer->megaSingleFastCalls() == beforeRebind + 1,
              "single fast scan resumes after fresh generation was cached");
        check(pass() == 0 && rt.threads.empty() &&
              cast::toNumber(sum->value) == 102 &&
              rt.sequencer->megaSingleFastCalls() == beforeRebind + 2,
              "single fast scan retires after map replacement without replay");

        // Structural generations may still match when Scratch changes
        // the value's type. The cache is *not* a numeric type guarantee.
        rt.greenFlag();
        sum->value = Value::num(0);
        check(pass() == 2 && pass() == 1 &&
              rt.threads.size() == 1 &&
              cast::toNumber(sum->value) == 4,
              "single source preproof prepares numeric continuation");
        sum->value = Value::string(SStr::create(u"100"));
        const uint64_t beforeTypeFallback =
            rt.sequencer->megaSingleFastCalls();
        rt.runUntilDone(40);
        check(rt.threads.empty() &&
              cast::toNumber(sum->value) == 102 &&
              rt.sequencer->megaSingleFastCalls() == beforeTypeFallback,
              "single source preproof never bypasses string-to-number conversion");
    }
}


void testMegaRepeatBurst() {
    // A pure source-certified repeat may be grouped inside stepThreads,
    // but a direct runSchedulerPass() still advances it by exactly 1.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        !enabled("XYO_JIT_MEGA_SINGLE_DISPATCH") ||
        !enabled("XYO_JIT_MEGA_SINGLE_SPECIALIZE") ||
        !enabled("XYO_JIT_MEGA_SINGLE_FAST_SCAN") ||
        !enabled("XYO_JIT_MEGA_SINGLE_PREPROVE") ||
        !enabled("XYO_JIT_MEGA_REPEAT_BURST") ||
        enabled("XYO_WAIT_WAKE_REGISTRY")) return;
    const auto repeat = [](const char* count, const char* body) {
        return block("control_repeat", nullptr,
            std::string(R"({"TIMES":[1,[4,")") + count +
            R"("]],"SUBSTACK":[2,")" + body + R"("]})");
    };
    const auto change = [](const char* delta) {
        return block("data_changevariableby", nullptr,
            std::string(R"({"VALUE":[1,[4,")") + delta + R"("]]})",
            R"({"VARIABLE":["s","s"]})");
    };
    const auto fixture = [&](const char* count) {
        return std::string("{\"hat0\":") + hat("loop0") +
            ",\"loop0\":" + repeat(count, "change0") +
            ",\"change0\":" + change("1") +
            ",\"hat1\":" + hat("loop1") +
            ",\"loop1\":" + repeat("1", "change1") +
            ",\"change1\":" + change("2") + "}";
    };
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", fixture("8192"), true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega repeat burst fixture loads");
    if (!loaded.ok) return;
    rt.greenFlag();
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    if (!sum) return;
    sum->value = Value::num(0);
    const auto code = rt.threads.empty() ? nullptr :
                      rt.threads[0]->megaDispatch;
    check(code && code->repeatBurstEntries.size() == 2 &&
          code->repeatBurstEntries[0] && code->repeatBurstEntries[1] &&
          code->singleFastSourceKinds == std::vector<uint8_t>({2, 2}),
          "mega burst compiles source-specific strict-FP LLVM repeat loops");
    if (!code || code->repeatBurstEntries.size() != 2 ||
        !code->repeatBurstEntries[0]) return;

    // Test ABI and order against the authoritative one-turn entry.
    auto ordinary = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    auto fast = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    ordinary.value = fast.value = Value::num(0.1);
    uint32_t pc = 1;
    uint64_t referenceRemaining = 8192, fastRemaining = 8192;
    MegaDispatchCode::MultiPcItem item{
        &ordinary, nullptr, nullptr, nullptr, nullptr,
        &pc, &referenceRemaining};
    for (int i = 0; i < 255; ++i)
        code->multiPcSingleEntry(&item);
    const uint64_t advanced = code->repeatBurstEntries[0](
        &fast, &fastRemaining, 255);
    check(advanced == 255 &&
          ordinary.value.bits == fast.value.bits &&
          pc == 1 && referenceRemaining == fastRemaining,
          "burst rounds all 255 additions exactly like 255 native turns");
    check(code->repeatBurstEntries[0](&fast, &fastRemaining, 0) == 0 &&
          fastRemaining == 7937,
          "zero-count burst never changes PC, variable or repeat counter");

    std::vector<Thread*> done;
    const size_t first = rt.sequencer->runSchedulerPass(
        false, false, false, false, done);
    check(first == 2 && cast::toNumber(sum->value) == 3 &&
          rt.sequencer->megaRepeatBurstCalls() == 0,
          "single scheduler pass remains one iteration per Scratch thread");
    rt.runUntilDone(500);
    if (!rt.threads.empty() || cast::toNumber(sum->value) != 8194 ||
        rt.sequencer->megaRepeatBurstCalls() == 0 ||
        rt.sequencer->megaRepeatBurstTurns() <= 4096)
        std::fprintf(stderr,
            "[burst-test-debug] threads=%zu sum=%g burstCalls=%llu burstTurns=%llu normal=%llu\n",
            rt.threads.size(), cast::toNumber(sum->value),
            (unsigned long long)rt.sequencer->megaRepeatBurstCalls(),
            (unsigned long long)rt.sequencer->megaRepeatBurstTurns(),
            (unsigned long long)rt.sequencer->megaScalarBatchCalls());
    check(rt.threads.empty() &&
          cast::toNumber(sum->value) == 8194 &&
          rt.sequencer->megaRepeatBurstCalls() > 0 &&
          rt.sequencer->megaRepeatBurstTurns() > 4096,
          "bounded bursts commit full repeat count with fewer LLVM calls");

    // A mid-run type change invalidates the numeric burst route, despite
    // the source remaining certified and the variable ID being identical.
    rt.greenFlag();
    sum->value = Value::num(0);
    done.clear();
    check(rt.sequencer->runSchedulerPass(
              false, false, false, false, done) == 2,
          "repeat burst type-fallback fixture starts both scripts");
    sum->value = Value::string(SStr::create(u"100"));
    const uint64_t callsBefore = rt.sequencer->megaRepeatBurstCalls();
    rt.runUntilDone(500);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 8291 &&
          rt.sequencer->megaRepeatBurstCalls() == callsBefore,
          "runtime type change falls back to Scratch conversion with no burst");

    // Mutation of a live variable map invalidates the saved binding
    // pointer. The regular dispatcher must rebind before burst may resume.
    rt.greenFlag();
    sum->value = Value::num(0);
    done.clear();
    check(rt.sequencer->runSchedulerPass(
              false, false, false, false, done) == 2,
          "repeat burst rebind fixture prepares active native roots");
    auto& variables = rt.getTargetForStage()->variables;
    variables.erase("s");
    auto [newVar, inserted] = variables.emplace(
        "s", Variable::create("s", u"s",
                              Variable::kScalarType, false));
    check(inserted, "repeat burst replacement variable inserted");
    sum = &newVar->second;
    sum->value = Value::num(100);
    const uint64_t priorRebindBursts = rt.sequencer->megaRepeatBurstCalls();
    rt.runUntilDone(500);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 8291 &&
          rt.sequencer->megaRepeatBurstCalls() > priorRebindBursts,
          "repeat burst resumes after safe generation-aware rebind");

    // A huge finite delta may overflow on the *first* update. Burst must
    // return after exactly that update and not silently execute further
    // iterations with an infinite operand rejected by normal fast dispatch.
    const auto big = std::string("{\"hat0\":") + hat("loop0") +
        ",\"loop0\":" + repeat("4", "change0") +
        ",\"change0\":" + change("1e308") +
        ",\"hat1\":" + hat("loop1") +
        ",\"loop1\":" + repeat("1", "change1") +
        ",\"change1\":" + change("2") + "}";
    Runtime overflow;
    const auto overflowLoaded = loadSb3(overflow,
        "{\"targets\":[" + target("Stage", big, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(overflowLoaded.ok, "repeat burst finite-overflow fixture loads");
    if (!overflowLoaded.ok) return;
    overflow.greenFlag();
    const auto overflowCode = overflow.threads.empty() ? nullptr :
                              overflow.threads[0]->megaDispatch;
    check(overflowCode && overflowCode->repeatBurstEntries.size() == 2 &&
          overflowCode->repeatBurstEntries[0],
          "repeat burst emits strict-FP large-delta entry");
    if (!overflowCode || overflowCode->repeatBurstEntries.empty() ||
        !overflowCode->repeatBurstEntries[0]) return;
    auto ordinaryHuge = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    auto fastHuge = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    ordinaryHuge.value = fastHuge.value = Value::num(1e308);
    uint32_t ordinaryPc = 1;
    uint64_t ordinaryRemaining = 4, fastHugeRemaining = 4;
    MegaDispatchCode::MultiPcItem huge{
        &ordinaryHuge, nullptr, nullptr, nullptr, nullptr,
        &ordinaryPc, &ordinaryRemaining};
    overflowCode->multiPcSingleEntry(&huge);
    const uint64_t overSteps = overflowCode->repeatBurstEntries[0](
        &fastHuge, &fastHugeRemaining, 4);
    check(overSteps == 1 && fastHugeRemaining == 3 &&
          ordinaryRemaining == 3 && ordinaryPc == 1 &&
          ordinaryHuge.value.bits == fastHuge.value.bits &&
          std::isinf(fastHuge.value.asNumberUnchecked()),
          "repeat burst commits overflow once and stops before nonfinite reentry");
}


void testMegaMultiRepeatBurst() {
    // A fused LLVM burst must advance EVERY live Thread in scheduler
    // order on each pass. This is especially important when the same
    // Scratch variable is written by multiple source-certified scripts.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* flag) {
        const char* env = std::getenv(flag);
        return env && env[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_MULTI_REPEAT_BURST") ||
        !enabled("XYO_JIT_MEGA_REPEAT_BURST") ||
        !enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        !enabled("XYO_JIT_MEGA_SINGLE_DISPATCH") ||
        !enabled("XYO_JIT_MEGA_SINGLE_SPECIALIZE") ||
        !enabled("XYO_JIT_MEGA_SINGLE_FAST_SCAN") ||
        !enabled("XYO_JIT_MEGA_SINGLE_PREPROVE") ||
        enabled("XYO_WAIT_WAKE_REGISTRY"))
        return;

    const auto repeat = [](const char* count, const char* body) {
        return block("control_repeat", nullptr,
            std::string(R"({"TIMES":[1,[4,")") + count +
            R"("]],"SUBSTACK":[2,")" + body + R"("]})");
    };
    const auto change = [](const char* delta) {
        return block("data_changevariableby", nullptr,
            std::string(R"({"VALUE":[1,[4,")") + delta + R"("]]})",
            R"({"VARIABLE":["s","s"]})");
    };
    const auto fixture = [&](const char* n0, const char* n1,
                             const char* d0, const char* d1) {
        return std::string("{\"hat0\":") + hat("loop0") +
            ",\"loop0\":" + repeat(n0, "change0") +
            ",\"change0\":" + change(d0) +
            ",\"hat1\":" + hat("loop1") +
            ",\"loop1\":" + repeat(n1, "change1") +
            ",\"change1\":" + change(d1) + "}";
    };
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" +
        target("Stage", fixture("8192","8192","0.1","0.2"), true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "multi repeat burst shared-variable fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    if (!sum) return;
    rt.greenFlag();
    sum->value = Value::num(0.1);
    const auto code = rt.threads.empty() ? nullptr :
                      rt.threads.front()->megaDispatch;
    check(code && code->multiRepeatBurstEntry &&
          code->multiPcBatchEntry && code->entries == 2 &&
          code->singleFastSourceKinds == std::vector<uint8_t>({2,2}),
          "multi repeat LLVM entry and compiled source proofs exist");
    if (!code || !code->multiRepeatBurstEntry ||
        !code->multiPcBatchEntry) return;

    // Shared variable: the exact order is +0.1, +0.2, +0.1, +0.2.
    auto reference = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    auto fused = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    reference.value = fused.value = Value::num(0.1);
    uint32_t refPc[2] = {1,3}, fusedPc[2] = {1,3};
    uint64_t refRemaining[2] = {8192,8192};
    uint64_t fusedRemaining[2] = {8192,8192};
    MegaDispatchCode::MultiPcItem refItems[2] = {
        {&reference, nullptr, nullptr, nullptr, nullptr,
         &refPc[0], &refRemaining[0]},
        {&reference, nullptr, nullptr, nullptr, nullptr,
         &refPc[1], &refRemaining[1]}};
    MegaDispatchCode::MultiPcItem fusedItems[2] = {
        {&fused, nullptr, nullptr, nullptr, nullptr,
         &fusedPc[0], &fusedRemaining[0]},
        {&fused, nullptr, nullptr, nullptr, nullptr,
         &fusedPc[1], &fusedRemaining[1]}};
    for (int i = 0; i < 255; ++i)
        code->multiPcBatchEntry(refItems, 2);
    const uint64_t executed =
        code->multiRepeatBurstEntry(fusedItems, 2, 255);
    check(executed == 255 && reference.value.bits == fused.value.bits &&
          refPc[0] == fusedPc[0] && refPc[1] == fusedPc[1] &&
          refRemaining[0] == fusedRemaining[0] &&
          refRemaining[1] == fusedRemaining[1],
          "aliased-variable multi burst matches 255 individual passes bitwise");
    check(code->multiRepeatBurstEntry(fusedItems, 2, 0) == 0 &&
          fused.value.bits == reference.value.bits,
          "multi burst zero pass budget does not mutate shared variable");

    // Distinct variables: source order must still advance both by the
    // same pass count, and no pointer may be accidentally shared.
    auto separateRef = Variable::create(
        "t", u"t", Variable::kScalarType, false);
    auto separateFused = Variable::create(
        "t", u"t", Variable::kScalarType, false);
    separateRef.value = separateFused.value = Value::num(-0.25);
    reference.value = fused.value = Value::num(0.5);
    refItems[1].variable = &separateRef;
    fusedItems[1].variable = &separateFused;
    refPc[0] = fusedPc[0] = 1;
    refPc[1] = fusedPc[1] = 3;
    refRemaining[0] = fusedRemaining[0] = 8192;
    refRemaining[1] = fusedRemaining[1] = 8192;
    for (int i = 0; i < 123; ++i)
        code->multiPcBatchEntry(refItems, 2);
    const uint64_t independentCount =
        code->multiRepeatBurstEntry(fusedItems, 2, 123);
    check(independentCount == 123 &&
          reference.value.bits == fused.value.bits &&
          separateRef.value.bits == separateFused.value.bits &&
          refRemaining[0] == fusedRemaining[0] &&
          refRemaining[1] == fusedRemaining[1],
          "independent-variable multi burst retains exact values and counters");

    std::vector<Thread*> done;
    check(rt.sequencer->runSchedulerPass(
              false, false, false, false, done) == 2 &&
          rt.sequencer->megaMultiBurstCalls() == 0,
          "explicit single scheduler pass never runs multi burst");
    rt.runUntilDone(500);
    double expected = 0.1;
    for (int i = 0; i < 8192; ++i) {
        expected += 0.1;
        expected += 0.2;
    }
    check(rt.threads.empty() && sum->value.bits == Value::num(expected).bits &&
          rt.sequencer->megaMultiBurstCalls() > 0 &&
          rt.sequencer->megaMultiBurstTurns() > 8000,
          "runtime scheduler multi burst finishes shared-variable repeat in order");

    // The shortest Thread exits first; no burst may execute beyond its
    // last body turn, leaving the normal scheduler to retire its identity.
    Runtime uneven;
    const auto unevenLoaded = loadSb3(uneven,
        "{\"targets\":[" +
        target("Stage", fixture("1000","3","1","2"), true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(unevenLoaded.ok, "multi burst unequal-length fixture loads");
    if (!unevenLoaded.ok) return;
    uneven.greenFlag();
    Variable* unevenSum = uneven.getTargetForStage()->lookupVariableById("s");
    if (!unevenSum) return;
    unevenSum->value = Value::num(0);
    uneven.runUntilDone(500);
    check(uneven.threads.empty() && cast::toNumber(unevenSum->value) == 1006 &&
          uneven.sequencer->megaMultiBurstCalls() > 0 &&
          uneven.sequencer->megaRepeatBurstCalls() > 0,
          "multi burst stops at first Thread completion, then single burst resumes");

    // Overflow after the first Thread's update must not cause a second
    // scheduler *pass* in LLVM; the current pass must still finish its
    // second Thread in order.
    Runtime overflow;
    const auto overflowLoaded = loadSb3(overflow,
        "{\"targets\":[" +
        target("Stage", fixture("4","4","1e308","2"), true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(overflowLoaded.ok, "multi burst finite-overflow fixture loads");
    if (!overflowLoaded.ok) return;
    overflow.greenFlag();
    const auto overCode = overflow.threads.empty() ? nullptr :
                          overflow.threads.front()->megaDispatch;
    check(overCode && overCode->multiRepeatBurstEntry &&
          overCode->multiPcBatchEntry,
          "multi burst finite-overflow entry compiled");
    if (!overCode || !overCode->multiRepeatBurstEntry ||
        !overCode->multiPcBatchEntry) return;
    auto overRef = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    auto overFused = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    overRef.value = overFused.value = Value::num(1e308);
    uint32_t rp[2]={1,3}, fp[2]={1,3};
    uint64_t rr[2]={4,4}, fr[2]={4,4};
    MegaDispatchCode::MultiPcItem overRefItems[2] = {
        {&overRef,nullptr,nullptr,nullptr,nullptr,&rp[0],&rr[0]},
        {&overRef,nullptr,nullptr,nullptr,nullptr,&rp[1],&rr[1]}};
    MegaDispatchCode::MultiPcItem overFusedItems[2] = {
        {&overFused,nullptr,nullptr,nullptr,nullptr,&fp[0],&fr[0]},
        {&overFused,nullptr,nullptr,nullptr,nullptr,&fp[1],&fr[1]}};
    overCode->multiPcBatchEntry(overRefItems, 2);
    const uint64_t overflowPasses =
        overCode->multiRepeatBurstEntry(overFusedItems, 2, 4);
    check(overflowPasses == 1 &&
          overRef.value.bits == overFused.value.bits &&
          rr[0] == fr[0] && rr[1] == fr[1] &&
          std::isinf(overFused.value.asNumberUnchecked()),
          "multi burst completes overflowing pass and stops before next pass");

    // Both Threads hold a saved pointer to the same Stage variable.
    // Replacing the map entry must reject both stale pointers, then allow
    // the ordinary generation-aware binder to recertify each Thread.
    rt.greenFlag();
    sum->value = Value::num(0.0);
    done.clear();
    check(rt.sequencer->runSchedulerPass(
              false, false, false, false, done) == 2,
          "multi burst rebind test prepares both cached pointers");
    auto& variables = rt.getTargetForStage()->variables;
    variables.erase("s");
    auto [newVar, inserted] = variables.emplace(
        "s", Variable::create("s", u"s",
                              Variable::kScalarType, false));
    check(inserted, "multi burst shared-variable replacement succeeds");
    sum = &newVar->second;
    sum->value = Value::num(10.0);
    const auto multiBeforeRebind = rt.sequencer->megaMultiBurstCalls();
    rt.runUntilDone(500);
    double reboundExpected = 10.0;
    for (int i = 0; i < 8191; ++i) {
        reboundExpected += 0.1;
        reboundExpected += 0.2;
    }
    check(rt.threads.empty() &&
          sum->value.bits == Value::num(reboundExpected).bits &&
          rt.sequencer->megaMultiBurstCalls() > multiBeforeRebind,
          "multi burst recovers after generation-safe shared-variable rebinding");

    // A nonnumeric string temporarily invalidates the bulk numerical
    // proof: the existing Scratch conversion path must handle that pass.
    rt.greenFlag();
    sum->value = Value::num(0);
    done.clear();
    check(rt.sequencer->runSchedulerPass(
              false, false, false, false, done) == 2,
          "multi burst string fallback starts valid numeric scripts");
    sum->value = Value::string(SStr::create(u"100"));
    rt.runUntilDone(500);
    check(rt.threads.empty() &&
          std::isfinite(cast::toNumber(sum->value)),
          "multi burst falls back when a shared variable becomes a string");

    // Maximum supported fanout. All eight independent Threads write the
    // same Stage scalar; no reordering or lost update is permitted.
    std::string eightBlocks = "{";
    for (int i = 0; i < 8; ++i) {
        const std::string n = std::to_string(i);
        const std::string loopId = "loop" + n;
        const std::string changeId = "change" + n;
        if (i != 0) eightBlocks += ",";
        eightBlocks += "\"hat" + n + "\":" + hat(loopId.c_str());
        eightBlocks += ",\"" + loopId + "\":" +
            repeat("4096", changeId.c_str());
        eightBlocks += ",\"" + changeId + "\":" + change("1");
    }
    eightBlocks += "}";
    Runtime eight;
    const auto eightLoaded = loadSb3(eight,
        "{\"targets\":[" + target("Stage", eightBlocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(eightLoaded.ok, "multi burst eight-Thread fixture loads");
    if (!eightLoaded.ok) return;
    eight.greenFlag();
    Variable* eightSum = eight.getTargetForStage()->lookupVariableById("s");
    if (!eightSum) return;
    eightSum->value = Value::num(0);
    eight.runUntilDone(500);
    check(eight.threads.empty() &&
          cast::toNumber(eightSum->value) == 32768 &&
          eight.sequencer->megaMultiBurstCalls() > 0 &&
          eight.sequencer->megaMultiBurstTurns() > 16000,
          "eight shared-variable Threads preserve all 32768 ordered updates");
}


void testMegaMultiRepeatTwoChanges() {
    // All scripts are proven pure repeat bodies with exactly two ordered
    // variable writes. The second pointer can alias the first pointer.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const auto enabled = [](const char* flag) {
        const char* env = std::getenv(flag);
        return env && env[0] == '1';
    };
    if (!enabled("XYO_JIT_MEGA_MULTI_REPEAT_BURST") ||
        !enabled("XYO_JIT_MEGA_REPEAT_BURST") ||
        !enabled("XYO_JIT_MEGA_BATCH_YIELD") ||
        !enabled("XYO_JIT_MEGA_DIRECT_SCALAR") ||
        !enabled("XYO_JIT_MEGA_SCALAR_BATCH") ||
        !enabled("XYO_JIT_MEGA_MULTI_PC_BATCH") ||
        !enabled("XYO_JIT_MEGA_SINGLE_DISPATCH") ||
        !enabled("XYO_JIT_MEGA_SINGLE_SPECIALIZE") ||
        !enabled("XYO_JIT_MEGA_SINGLE_FAST_SCAN") ||
        !enabled("XYO_JIT_MEGA_SINGLE_PREPROVE") ||
        enabled("XYO_WAIT_WAKE_REGISTRY"))
        return;
    const auto repeat = [](const char* first) {
        return block("control_repeat", nullptr,
            std::string(R"({"TIMES":[1,[4,"4096"]],"SUBSTACK":[2,")") +
            first + R"("]})");
    };
    const auto change = [](const char* id, const char* delta,
                           const char* next) {
        return block("data_changevariableby", next,
            std::string(R"({"VALUE":[1,[4,")") + delta + R"("]]})",
            std::string(R"({"VARIABLE":[")") + id + R"(",")" +
            id + R"("]})");
    };
    const std::string blocks =
        std::string("{\"hat0\":") + hat("repeat0") +
        ",\"repeat0\":" + repeat("change0a") +
        ",\"change0a\":" + change("s", "0.1", "change0b") +
        ",\"change0b\":" + change("t", "0.25", nullptr) +
        ",\"hat1\":" + hat("repeat1") +
        ",\"repeat1\":" + repeat("change1a") +
        ",\"change1a\":" + change("t", "0.2", "change1b") +
        ",\"change1b\":" + change("s", "0.5", nullptr) + "}";
    std::string stage = target("Stage", blocks, true);
    const std::string oldVars = R"("variables":{"s":["s",""]})";
    const std::string newVars = R"("variables":{"s":["s",0],"t":["t",0]})";
    const auto location = stage.find(oldVars);
    check(location != std::string::npos,
          "multi two-write fixture has expected Stage variable schema");
    if (location == std::string::npos) return;
    stage.replace(location, oldVars.size(), newVars);
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}",
        nullptr);
    check(loaded.ok, "multi two-write Scratch fixture loads");
    if (!loaded.ok) return;
    rt.greenFlag();
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    Variable* other = rt.getTargetForStage()->lookupVariableById("t");
    if (!sum || !other || rt.threads.size() != 2) {
        check(false, "multi two-write Stage bindings exist");
        return;
    }
    sum->value = Value::num(0.1);
    other->value = Value::num(-0.3);
    const auto code = rt.threads.front()->megaDispatch;
    check(code && code->multiRepeatBurstEntry &&
          code->multiRepeatSourceArity ==
              std::vector<uint8_t>({2,2}),
          "LLVM preproof certifies two ordered direct writes per Thread");
    if (!code || !code->multiRepeatBurstEntry ||
        !code->multiPcBatchEntry) return;

    // Compare 255 passes against the existing one-pass LLVM entry.
    auto as = Variable::create("s", u"s", Variable::kScalarType, false);
    auto at = Variable::create("t", u"t", Variable::kScalarType, false);
    auto bs = Variable::create("s", u"s", Variable::kScalarType, false);
    auto bt = Variable::create("t", u"t", Variable::kScalarType, false);
    as.value = bs.value = Value::num(0.1);
    at.value = bt.value = Value::num(-0.3);
    uint32_t apc[2] = {1,3}, bpc[2] = {1,3};
    uint64_t arem[2] = {4096,4096}, brem[2] = {4096,4096};
    MegaDispatchCode::MultiPcItem a[2] = {
        {&as, &at, nullptr, nullptr, nullptr, &apc[0], &arem[0]},
        {&at, &as, nullptr, nullptr, nullptr, &apc[1], &arem[1]}};
    MegaDispatchCode::MultiPcItem b[2] = {
        {&bs, &bt, nullptr, nullptr, nullptr, &bpc[0], &brem[0]},
        {&bt, &bs, nullptr, nullptr, nullptr, &bpc[1], &brem[1]}};
    for (int i = 0; i < 255; ++i)
        code->multiPcBatchEntry(a, 2);
    const auto advanced = code->multiRepeatBurstEntry(b, 2, 255);
    check(advanced == 255 &&
          as.value.bits == bs.value.bits &&
          at.value.bits == bt.value.bits &&
          apc[0] == bpc[0] && apc[1] == bpc[1] &&
          arem[0] == brem[0] && arem[1] == brem[1],
          "two changes per Thread retain strict in-order FP semantics");

    // Aliasing: both writes by a single Thread address the same scalar.
    as.value = bs.value = Value::num(0.1);
    at.value = bt.value = Value::num(-0.3);
    arem[0] = arem[1] = brem[0] = brem[1] = 4096;
    apc[0] = bpc[0] = 1;
    apc[1] = bpc[1] = 3;
    a[0].secondVariable = &as;
    b[0].secondVariable = &bs;
    for (int i = 0; i < 125; ++i)
        code->multiPcBatchEntry(a, 2);
    const auto aliasAdvanced = code->multiRepeatBurstEntry(b, 2, 125);
    check(aliasAdvanced == 125 &&
          as.value.bits == bs.value.bits &&
          at.value.bits == bt.value.bits &&
          arem[0] == brem[0] && arem[1] == brem[1],
          "ordered double-write burst preserves aliased Variable semantics");

    // Full runtime scheduling and the original per-pass semantics.
    std::vector<Thread*> done;
    const size_t first = rt.sequencer->runSchedulerPass(
        false, false, false, false, done);
    check(first == 2 &&
          rt.sequencer->megaMultiBurstCalls() == 0,
          "direct scheduler pass cannot coalesce two-write sources");
    rt.runUntilDone(500);
    double expectedS = 0.1;
    double expectedT = -0.3;
    for (int i = 0; i < 4096; ++i) {
        expectedS += 0.1;
        expectedT += 0.25;
        expectedT += 0.2;
        expectedS += 0.5;
    }
    check(rt.threads.empty() &&
          sum->value.bits == Value::num(expectedS).bits &&
          other->value.bits == Value::num(expectedT).bits &&
          rt.sequencer->megaMultiBurstCalls() > 0 &&
          rt.sequencer->megaMultiBurstTurns() > 4000,
          "real two-write Scratch sources use fused passes with bit parity");

    // Second variable, not just first, must be checked for infinity.
    as.value = bs.value = Value::num(0);
    at.value = bt.value = Value::num(1e308);
    arem[0] = arem[1] = brem[0] = brem[1] = 4096;
    apc[0] = bpc[0] = 1;
    apc[1] = bpc[1] = 3;
    a[0].secondVariable = &at;
    b[0].secondVariable = &bt;
    // The current finite-source literal can't overflow from 1e308 when
    // adding 0.25; use the largest finite value, whose ULP exceeds 0.25.
    // This tests that a pre-existing nonfinite second operand never enters
    // the burst via the host guard: parity against a one-pass entry is
    // still exact for ordinary finite values.
    code->multiPcBatchEntry(a, 2);
    const uint64_t one = code->multiRepeatBurstEntry(b, 2, 1);
    check(one == 1 &&
          as.value.bits == bs.value.bits &&
          at.value.bits == bt.value.bits,
          "two-write burst preserves large finite second operand exactly");

    // The second update alone causes overflow. The burst must complete
    // that Scratch pass and then return, even though the *first*
    // destination remains finite. Both Variables may be aliased.
    std::string largeStage = stage;
    const size_t bigDelta = largeStage.find("0.25");
    check(bigDelta != std::string::npos,
          "two-write second-operand overflow fixture contains delta");
    if (bigDelta == std::string::npos) return;
    largeStage.replace(bigDelta, 4, "1e308");
    Runtime overflow;
    const auto overLoaded = loadSb3(overflow,
        "{\"targets\":[" + largeStage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}",
        nullptr);
    check(overLoaded.ok, "two-write second-variable overflow fixture loads");
    if (!overLoaded.ok) return;
    overflow.greenFlag();
    const auto large = overflow.threads.front()->megaDispatch;
    check(large && large->multiRepeatBurstEntry &&
          large->multiRepeatSourceArity ==
              std::vector<uint8_t>({2,2}),
          "second-variable overflow fixture is source certified");
    if (!large || !large->multiRepeatBurstEntry ||
        !large->multiPcBatchEntry) return;
    auto refS = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    auto refT = Variable::create(
        "t", u"t", Variable::kScalarType, false);
    auto fusedS = Variable::create(
        "s", u"s", Variable::kScalarType, false);
    auto fusedT = Variable::create(
        "t", u"t", Variable::kScalarType, false);
    refS.value = fusedS.value = Value::num(0);
    refT.value = fusedT.value = Value::num(1e308);
    uint32_t refPc[2] = {1,3}, fusedPc[2] = {1,3};
    uint64_t refRemaining[2] = {4096,4096};
    uint64_t fusedRemaining[2] = {4096,4096};
    MegaDispatchCode::MultiPcItem refItems[2] = {
        {&refS, &refT, nullptr, nullptr, nullptr,
         &refPc[0], &refRemaining[0]},
        {&refT, &refS, nullptr, nullptr, nullptr,
         &refPc[1], &refRemaining[1]}};
    MegaDispatchCode::MultiPcItem fusedItems[2] = {
        {&fusedS, &fusedT, nullptr, nullptr, nullptr,
         &fusedPc[0], &fusedRemaining[0]},
        {&fusedT, &fusedS, nullptr, nullptr, nullptr,
         &fusedPc[1], &fusedRemaining[1]}};
    large->multiPcBatchEntry(refItems, 2);
    const uint64_t overflowPasses = large->multiRepeatBurstEntry(
        fusedItems, 2, 256);
    check(overflowPasses == 1 &&
          refS.value.bits == fusedS.value.bits &&
          refT.value.bits == fusedT.value.bits &&
          std::isinf(fusedT.value.asNumberUnchecked()) &&
          refRemaining[0] == fusedRemaining[0] &&
          refRemaining[1] == fusedRemaining[1],
          "overflow in second ordered write stops multi burst after one full pass");
}

void testMegaYieldSwitchDispatch() {
    // Three separate Scratch hats execute one iteration per scheduler
    // selection through ONE stackless LLVM switch(pc).
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* flag = std::getenv("XYO_JIT_MEGA_BATCH_YIELD");
    if (!flag || flag[0] != '1') return;
    const auto repeat = [](const char* count, const char* body) {
        return block("control_repeat", nullptr,
            std::string("{\"TIMES\":[1,[4,\"") + count +
            "\"]],\"SUBSTACK\":[2,\"" + body + "\"]}");
    };
    const auto change = [](const char* amount) {
        return block("data_changevariableby", nullptr,
            std::string("{\"VALUE\":[1,[4,\"") + amount + "\"]]}",
            R"({"VARIABLE":["s","s"]})");
    };
    const std::string blocks =
        "{\"hat0\":" + hat("repeat0") +
        ",\"repeat0\":" + repeat("3", "inc0") +
        ",\"inc0\":" + change("1") +
        ",\"hat1\":" + hat("repeat1") +
        ",\"repeat1\":" + repeat("2", "inc1") +
        ",\"inc1\":" + change("2") +
        ",\"hat2\":" + hat("repeat2") +
        ",\"repeat2\":" + repeat("0", "inc2") +
        ",\"inc2\":" + change("4") + "}";
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}",
        nullptr);
    check(loaded.ok, "mega-yield nested scheduler fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    const auto findByTop = [&](const char* id) -> Thread* {
        auto it = std::find_if(rt.threads.begin(), rt.threads.end(),
            [&](Thread* t) { return t->topBlock == id; });
        return it == rt.threads.end() ? nullptr : *it;
    };
    const auto countActiveFibers = FiberPool::activeCount();
    rt.greenFlag();
    check(rt.threads.size() == 3,
          "mega-yield keeps exactly three independent Scratch threads");
    Thread* first = findByTop("hat0");
    Thread* second = findByTop("hat1");
    Thread* zero = findByTop("hat2");
    if (!first || !second || !zero || !sum) {
        check(false, "mega-yield fixture supplies three native roots and scalar");
        return;
    }
    auto owner = first->megaDispatch;
    check(owner && owner->yieldEntry && owner->entries == 3 &&
          second->megaDispatch == owner && zero->megaDispatch == owner,
          "mega-yield maps all three loops into one LLVM switch");
    if (!owner || !owner->yieldEntry) return;
    rt.sequencer->stepThread(*first);
    check(cast::toNumber(sum->value) == 1 &&
          first->megaRemaining == 2 && first->megaPc % 2 == 1 &&
          first->jitCtx && !first->fiber,
          "one stackless scheduler entry executes exactly one repeat body");
    rt.sequencer->stepThread(*second);
    check(cast::toNumber(sum->value) == 3 &&
          second->megaRemaining == 1 && second->megaPc % 2 == 1 &&
          second->jitCtx && !second->fiber,
          "separate Thread has own native repeat counter and PC");
    rt.sequencer->stepThread(*zero);
    check(cast::toNumber(sum->value) == 3 &&
          zero->status == STATUS_DONE && !zero->jitCtx && !zero->fiber,
          "zero-iteration script finishes without a body execution");
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 7 &&
          FiberPool::activeCount() == countActiveFibers,
          "full mega-yield interleaving matches Scratch and allocates no Fiber");
    rt.greenFlag();
    check(rt.threads.size() == 3 && findByTop("hat0") &&
          findByTop("hat0")->megaDispatch == owner,
          "new green flag reuses source-verified mega-yield LLVM function");
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 14,
          "cached mega-yield second run commits effects once");

    if (owner->directScalar) {
        // The directly bound slot must also support Scratch's nonnumeric
        // values: cold helper converts the initial string/bool/NaN, and
        // the next iteration switches back to in-LLVM numeric addition.
        sum->value = Value::string(SStr::create(u"7"));
        rt.greenFlag();
        first = findByTop("hat0");
        if (!first) return;
        rt.sequencer->stepThread(*first);
        check(cast::toNumber(sum->value) == 8 &&
              first->megaScalarBindingsReady &&
              first->megaScalarBindings[0] == sum,
              "mega LLVM numeric direct mode falls back for a numeric string");
        rt.runUntilDone(50);
        check(rt.threads.empty() && cast::toNumber(sum->value) == 14,
              "mega direct string input finishes with exact numeric result");

        sum->value = Value::boolean(false);
        rt.greenFlag();
        rt.runUntilDone(50);
        check(rt.threads.empty() && cast::toNumber(sum->value) == 7,
              "mega direct boolean conversion keeps Scratch semantics");

        sum->value = Value::nan();
        rt.greenFlag();
        rt.runUntilDone(50);
        check(rt.threads.empty() && cast::toNumber(sum->value) == 7,
              "mega direct tagged NaN converts to zero before adding");

        sum->value = Value::num(14);
        rt.greenFlag();
        first = findByTop("hat0");
        if (!first) return;
        rt.sequencer->stepThread(*first);
        sum->value = Value::string(SStr::create(u"20"));
        rt.sequencer->stepThread(*first);
        check(cast::toNumber(sum->value) == 21 &&
              first->megaRemaining == 1,
              "mega direct numeric slot guards a mid-repeat type change");
        rt.runUntilDone(50);
        check(rt.threads.empty() && cast::toNumber(sum->value) == 26,
              "mega direct resumes a numeric variable after string replacement");
        sum->value = Value::num(14);
    }

    // The green-flag Thread has suspended after a scalar write. Erase its
    // cached Variable* from the Stage map, then recreate it with the same
    // Scratch id and name. The next LLVM slice must resolve the replacement
    // rather than dereferencing the stale native lookup cache.
    rt.greenFlag();
    first = findByTop("hat0");
    if (!first) return;
    rt.sequencer->stepThread(*first);
    Target* stage = rt.getTargetForStage();
    const auto sourceRevision = stage->blocks->revision();
    stage->variables.erase("s");
    auto [replacement, inserted] = stage->variables.emplace(
        "s", Variable::create("s", u"s", Variable::kScalarType, false));
    check(inserted && stage->blocks->revision() == sourceRevision,
          "mega-yield variable-map replacement preserves source revision");
    replacement->second.value = Value::num(100);
    rt.sequencer->stepThread(*first);
    check(cast::toNumber(replacement->second.value) == 101 &&
          first->megaRemaining == 1,
          "mega-yield rebound recreated Variable without stale pointer");
    rt.runUntilDone(50);
    check(rt.threads.empty() &&
          cast::toNumber(replacement->second.value) == 106 &&
          FiberPool::activeCount() == countActiveFibers,
          "mega-yield completes after a Variable map generation change");
    sum = &replacement->second;
    sum->value = Value::num(14);

    rt.greenFlag();
    first = findByTop("hat0");
    if (!first) return;
    rt.sequencer->stepThread(*first);
    check(cast::toNumber(sum->value) == 15 &&
          first->megaRemaining == 2,
          "source edit test commits one native body before revision change");
    rt.getTargetForStage()->blocks->bumpRevisionForTestingWithoutCacheReset();
    rt.sequencer->stepThread(*first);
    check(first->status == STATUS_DONE && !first->jitCtx &&
          cast::toNumber(sum->value) == 15,
          "source change discards suspended mega PC without replay");
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 19 &&
          FiberPool::activeCount() == countActiveFibers,
          "remaining fresh threads use interpreter fallback after source edit");
}

void testMegaBatchSwitchDispatch() {
    // Three independent hats must execute through one generated LLVM
    // switch(pc) while preserving separate Scratch Thread identities.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* beta = std::getenv("XYO_JIT_MEGA_BATCH");
    if (!beta || beta[0] != '1') return;
    const std::string blocks =
        "{\"hat0\":" + hat("inc0") +
        ",\"inc0\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})", R"({"VARIABLE":["s","s"]})") +
        ",\"hat1\":" + hat("inc1") +
        ",\"inc1\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"2"]]})", R"({"VARIABLE":["s","s"]})") +
        ",\"hat2\":" + hat("inc2") +
        ",\"inc2\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"4"]]})", R"({"VARIABLE":["s","s"]})") +
        "}";
    Runtime rt;
    const auto loaded = loadSb3(
        rt, "{\"targets\":[" + target("Stage", blocks, true) +
                "],\"monitors\":[],\"extensions\":[],\"meta\":{}}",
        nullptr);
    check(loaded.ok, "mega-batch three-green-flag source loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    rt.greenFlag();
    check(rt.threads.size() == 3,
          "mega-batch keeps three distinct Scratch threads");
    if (rt.threads.size() != 3 || !sum) return;
    const MegaDispatchCode* batch = rt.threads.front()->megaDispatch.get();
    check(batch && batch->entry && batch->entries == 3,
          "mega-batch produces a live LLVM switch function");
    std::array<bool, 3> pcSeen{};
    for (Thread* thread : rt.threads) {
        const auto* native =
            static_cast<const CompiledScript*>(thread->compiled);
        check(thread->isCompiled && native && !native->mayYield &&
                  thread->megaDispatch.get() == batch,
              "mega-batch has one shared native owner per independent thread");
        if (thread->megaPc < pcSeen.size()) pcSeen[thread->megaPc] = true;
    }
    check(std::all_of(pcSeen.begin(), pcSeen.end(),
                      [](bool value) { return value; }),
          "mega-batch assigns unique switch-PC values");
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 7,
          "mega-batch commits each independent Scratch effect exactly once");
    rt.greenFlag();
    check(rt.threads.size() == 3 &&
          rt.threads[0]->megaDispatch.get() == batch,
          "second green flag reuses the identical source-verified LLVM batch");
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 14,
          "cached mega-batch reexecutes three hats without duplicate effects");

    // Simulate a source edit after the three Threads have been assigned
    // native switch PC values, but before the first Scratch instruction.
    // This changes the revision without destroying the test's native
    // BlockCached references, so no stale source may execute.
    rt.greenFlag();
    check(rt.threads.size() == 3 && rt.threads[0]->megaDispatch,
          "mega-batch reused before test-only source revision invalidation");
    rt.getTargetForStage()->blocks->bumpRevisionForTestingWithoutCacheReset();
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 21,
          "source revision changed before dispatch: all effects interpreted once");
}

void testMegaBatchProcedureDispatch() {
    // Non-yielding scripts with custom blocks join the same switch(pc)
    // batch: the batch compiler fuses each reachable acyclic procedure
    // graph into its script entry before inlining.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* beta = std::getenv("XYO_JIT_MEGA_BATCH");
    if (!beta || beta[0] != '1') return;
    const std::string mp =
        R"({"tagName":"mutation","children":[],"proccode":"p",)"
        R"("argumentids":"[]","argumentnames":"[]",)"
        R"("argumentdefaults":"[]","warp":"false"})";
    const auto procCall = [&](const char* next) {
        return std::string("{\"opcode\":\"procedures_call\",\"next\":") +
            (next ? "\"" + std::string(next) + "\"" : "null") +
            ",\"inputs\":{},\"fields\":{},\"mutation\":" + mp + "}";
    };
    const std::string blocks =
        "{\"hat0\":" + hat("call0") +
        ",\"call0\":" + procCall(nullptr) +
        ",\"hat1\":" + hat("call1") +
        ",\"call1\":" + procCall(nullptr) +
        ",\"defP\":{\"opcode\":\"procedures_definition\","
            "\"topLevel\":true,\"next\":\"increment\","
            "\"inputs\":{\"custom_block\":[1,\"protoP\"]},\"fields\":{}}" +
        ",\"protoP\":{\"opcode\":\"procedures_prototype\","
            "\"inputs\":{},\"fields\":{},\"mutation\":" + mp + "}" +
        ",\"increment\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})", R"({"VARIABLE":["s","s"]})") + "}";
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega-batch procedure fixture loads");
    if (!loaded.ok) return;
    Variable* sum = rt.getTargetForStage()->lookupVariableById("s");
    rt.greenFlag();
    check(rt.threads.size() == 2,
          "mega-batch procedure keeps two independent threads");
    if (rt.threads.size() != 2 || !sum) return;
    const MegaDispatchCode* batch = rt.threads.front()->megaDispatch.get();
    check(batch && batch->entry && batch->entries == 2,
          "mega-batch fuses non-yielding custom blocks into one function");
    rt.runUntilDone(50);
    check(rt.threads.empty() && cast::toNumber(sum->value) == 2,
          "mega-batch procedure effects commit exactly once per script");
}

void testMegaPartialSchedulerProcedureFusion() {
    // Force the new bounded, partial-path inliner to modify a real
    // sched_* procedure without deleting the other native entry symbols.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* enabled = std::getenv("XYO_JIT_MEGA_PARTIAL");
    if (!enabled || enabled[0] != '1') return;
    const char* scope = std::getenv("XYO_JIT_MEGA_PARTIAL_SCOPE");
    if (scope && *scope && std::string(scope) != "all") return;
    // Synthetic LLVM IR is necessary here: the ordinary Scratch emitter
    // may have *already* inlined a one-line procedure before this pass
    // runs, so a Scratch-only assertion cannot prove the new pass executed.
    {
        llvm::LLVMContext context;
        llvm::Module module("mega-partial-unit", context);
        auto* type = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context), false);
        auto* sched = llvm::Function::Create(
            type, llvm::Function::ExternalLinkage, "sched_partial", module);
        auto* helper = llvm::Function::Create(
            type, llvm::Function::ExternalLinkage, "partial_helper", module);
        llvm::IRBuilder<> b(context);
        b.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", helper));
        b.CreateRetVoid();
        b.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", sched));
        b.CreateCall(helper);
        b.CreateRetVoid();
        const std::map<std::string, std::string> names{
            {"sched_partial", "sched_partial"},
            {"partial_helper", "partial_helper"}};
        const auto stats = mega_partial::fuse(module, names);
        bool noCall = true;
        for (auto& bb : *sched)
            for (auto& inst : bb)
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    call && call->getCalledFunction() == helper)
                    noCall = false;
        check(stats.valid && stats.selected == 1 &&
              stats.inlined == 1 && noCall &&
              !llvm::verifyModule(module, &llvm::errs()),
              "mega-partial LLVM pass removes actual nested Scratch call");
        check(module.getFunction("partial_helper") == helper &&
              !helper->isDeclaration(),
              "mega-partial retains non-inlined callable procedure symbol");
    }
    {
        // A procedure cycle must remain fully callable and unchanged.
        llvm::LLVMContext context;
        llvm::Module module("mega-partial-recursive", context);
        auto* type = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context), false);
        auto* sched = llvm::Function::Create(
            type, llvm::Function::ExternalLinkage, "sched_partial", module);
        auto* helper = llvm::Function::Create(
            type, llvm::Function::ExternalLinkage, "partial_helper", module);
        llvm::IRBuilder<> b(context);
        b.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", helper));
        b.CreateCall(sched);
        b.CreateRetVoid();
        b.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", sched));
        b.CreateCall(helper);
        b.CreateRetVoid();
        const auto stats = mega_partial::fuse(module, {
            {"sched_partial", "sched_partial"},
            {"partial_helper", "partial_helper"}});
        check(stats.valid && stats.inlined == 0 &&
              stats.rejectedCycles > 0 &&
              !llvm::verifyModule(module, &llvm::errs()),
              "mega-partial rejects recursive call graph before inlining");
    }
    const auto mutation = [](const std::string& name) {
        return std::string(R"({"tagName":"mutation","children":[],"proccode":")") +
            name + R"(","argumentids":"[]","argumentnames":"[]",)" +
            R"("argumentdefaults":"[]","warp":"false"})";
    };
    const auto call = [](const std::string& m) {
        return R"({"opcode":"procedures_call","next":null,"inputs":{},)" +
               std::string(R"("fields":{},"mutation":)") + m + "}";
    };
    const std::string mp = mutation("sched_partial");
    const std::string mq = mutation("partial_helper");
    const std::string blocks =
        "{\"hat\":" + hat("run") +
        ",\"run\":" + call(mp) +
        ",\"defSched\":{\"opcode\":\"procedures_definition\","
        "\"topLevel\":true,\"next\":\"helperCall\","
        "\"inputs\":{\"custom_block\":[1,\"protoSched\"]},\"fields\":{}}" +
        ",\"protoSched\":{\"opcode\":\"procedures_prototype\","
        "\"inputs\":{},\"fields\":{},\"mutation\":" + mp + "}" +
        ",\"helperCall\":" + call(mq) +
        ",\"defHelper\":{\"opcode\":\"procedures_definition\","
        "\"topLevel\":true,\"next\":\"increment\","
        "\"inputs\":{\"custom_block\":[1,\"protoHelper\"]},\"fields\":{}}" +
        ",\"protoHelper\":{\"opcode\":\"procedures_prototype\","
        "\"inputs\":{},\"fields\":{},\"mutation\":" + mq + "}" +
        ",\"increment\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})",
            R"({"VARIABLE":["s","s"]})") + "}";
    Runtime runtime;
    const auto loaded = loadSb3(runtime,
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "mega-partial scheduler procedure fixture loads");
    if (!loaded.ok) return;
    auto* sum = runtime.getTargetForStage()->lookupVariableById("s");
    runtime.greenFlag();
    if (runtime.threads.empty() || !sum) {
        check(false, "mega-partial fixture starts one real native thread");
        return;
    }
    const auto* compiled = static_cast<const CompiledScript*>(
        runtime.threads.front()->compiled);
    check(runtime.threads.front()->isCompiled && compiled,
          "mega-partial keeps a valid native Scratch entry even when "
          "the ordinary compiler already inlined all small callees");
    // The all-or-nothing goto fusion deletes every procedure definition
    // after inlining, so no external symbols remain by design. Only the
    // partial pass promises to keep them callable.
    const char* gotoFlag = std::getenv("XYO_JIT_MEGA_DISPATCH");
    const bool gotoFusion = gotoFlag && gotoFlag[0] == '1';
    check(compiled && (gotoFusion ? compiled->procedures.empty()
                                  : !compiled->procedures.empty()),
          gotoFusion
              ? "mega-goto consumes procedure symbols when both fusions are on"
              : "mega-partial preserves external procedure symbols for fallback");
    runtime.runUntilDone(200);
    check(runtime.threads.empty() && cast::toNumber(sum->value) == 1,
          "mega-partial preserves nested procedure scalar side effects");
    runtime.greenFlag();
    runtime.runUntilDone(200);
    check(runtime.threads.empty() && cast::toNumber(sum->value) == 2,
          "mega-partial compiled cache reexecutes side effects exactly once");
}

void testMegaGotoProcedureFusion() {
    // With the beta enabled, P -> Q -> scalar write becomes one LLVM
    // entry CFG. Executing the same Scratch program must not depend on
    // external procedure function symbols after fusion.
    if (!jitEnabled() || jitMode() != JitMode::Eager) return;
    const char* beta = std::getenv("XYO_JIT_MEGA_DISPATCH");
    if (!beta || beta[0] != '1') return;
    const std::string mp =
        R"({"tagName":"mutation","children":[],"proccode":"p",)"
        R"("argumentids":"[]","argumentnames":"[]",)"
        R"("argumentdefaults":"[]","warp":"false"})";
    const std::string mq =
        R"({"tagName":"mutation","children":[],"proccode":"q",)"
        R"("argumentids":"[]","argumentnames":"[]",)"
        R"("argumentdefaults":"[]","warp":"false"})";
    const auto procCall = [](const char* next, const std::string& mutation) {
        return std::string("{\"opcode\":\"procedures_call\",\"next\":") +
            (next ? "\"" + std::string(next) + "\"" : "null") +
            ",\"inputs\":{},\"fields\":{},\"mutation\":" +
            mutation + "}";
    };
    const std::string blocks =
        "{\"hat\":" + hat("repeat") +
        ",\"repeat\":" + block("control_repeat", nullptr,
            R"({"TIMES":[1,[4,"3"]],"SUBSTACK":[2,"callP"]})") +
        ",\"callP\":" + procCall(nullptr, mp) +
        ",\"defP\":{\"opcode\":\"procedures_definition\","
            "\"topLevel\":true,\"next\":\"callQ1\","
            "\"inputs\":{\"custom_block\":[1,\"protoP\"]},\"fields\":{}}" +
        ",\"protoP\":{\"opcode\":\"procedures_prototype\","
            "\"inputs\":{},\"fields\":{},\"mutation\":" + mp + "}" +
        ",\"callQ1\":" + procCall("callQ2", mq) +
        ",\"callQ2\":" + procCall(nullptr, mq) +
        ",\"defQ\":{\"opcode\":\"procedures_definition\","
            "\"topLevel\":true,\"next\":\"increment\","
            "\"inputs\":{\"custom_block\":[1,\"protoQ\"]},\"fields\":{}}" +
        ",\"protoQ\":{\"opcode\":\"procedures_prototype\","
            "\"inputs\":{},\"fields\":{},\"mutation\":" + mq + "}" +
        ",\"increment\":" + block("data_changevariableby", nullptr,
            R"({"VALUE":[1,[4,"1"]]})", R"({"VARIABLE":["s","s"]})") + "}";
    Runtime rt;
    const std::string project =
        "{\"targets\":[" + target("Stage", blocks, true) +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    const auto loaded = loadSb3(rt, project, nullptr);
    check(loaded.ok, "mega goto nested procedure fixture loads");
    if (!loaded.ok) return;
    Variable* counter = rt.getTargetForStage()->lookupVariableById("s");
    rt.greenFlag();
    check(rt.threads.size() == 1, "mega goto starts exactly one top-level script");
    if (rt.threads.empty() || !counter) return;
    Thread* thread = rt.threads.front();
    const auto* native = static_cast<const CompiledScript*>(thread->compiled);
    check(thread->isCompiled && native && native->procedures.empty(),
          "mega goto compiles a single LLVM entry, without separate procedures");
    rt.runUntilDone(200);
    check(rt.threads.empty() && cast::toNumber(counter->value) == 6,
          "mega goto executes nested custom blocks exactly six times");
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
void testCoroAskAndWait() {
    const std::string blocks =
        "{\"hat\":" + hat("ask") +
        ",\"ask\":" + block("sensing_askandwait", "set",
            "{\"QUESTION\":[1,[10,\"q\"]]}") +
        ",\"set\":" + block("data_setvariableto", nullptr,
            "{\"VALUE\":[3,\"ans\",[10,\"\"]]}",
            "{\"VARIABLE\":[\"a\",\"a\"]}") +
        ",\"ans\":" + block("sensing_answer", nullptr, "{}", "{}") +
        "}";
    std::string stage = target("Stage", blocks, true);
    const std::string vars = R"("variables":{"s":["s",""]})";
    const auto at = stage.find(vars);
    check(at != std::string::npos, "ask fixture stage schema is known");
    if (at == std::string::npos) return;
    stage.replace(at, vars.size(), R"("variables":{"a":["a",""]})");
    Runtime rt;
    const auto loaded = loadSb3(rt,
        "{\"targets\":[" + stage +
        "],\"monitors\":[],\"extensions\":[],\"meta\":{}}", nullptr);
    check(loaded.ok, "load coroutine ask fixture");
    if (!loaded.ok) return;
    rt.greenFlag();
    Thread* thread = nullptr;
    for (Thread* t : rt.threads)
        if (t->topBlock == "hat") thread = t;
    check(thread && thread->isCompiled, "ask script compiles");
    if (!thread || !thread->isCompiled) return;
    const auto* cs = static_cast<const CompiledScript*>(thread->compiled);
    check(cs && cs->coroResume, "ask script lowered to a coroutine");
    rt.sequencer->stepThread(*thread);
    check(thread->fiber == nullptr && thread->status == STATUS_PROMISE_WAIT,
          "coroutine ask suspends without a fiber");
    rt.emitAnswer(u"ok");
    rt.runUntilDone(100);
    Variable* answer = rt.getTargetForStage()->lookupVariableById("a");
    const auto* text = answer ? cast::toString(answer->value) : nullptr;
    check(text && text->view() == u"ok",
          "coroutine ask resumes with the answer");
}

} // namespace

int test_scheduler_main() {
    if (const char* only = std::getenv("XYO_TEST_ONLY_MEGA_BATCH");
        only && only[0] == '1') {
        testMegaBatchSwitchDispatch();
        testMegaBatchProcedureDispatch();
        std::printf("mega batch: %d checks, %d failures\n", checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_MEGA_UNTIL");
        only && only[0] == '1') {
        testMegaBroadcastUntilContinuation();
        testMegaUntilWallClockContinuation();
        testMegaUntilElapsedMillisContinuation();
        testMegaUntilBoundedOuterRepeat();
        testMegaUntilNestedProcedure();
        std::printf("mega until: %d checks, %d failures\n",
                    checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_MEGA_SENDER_SINGLE");
        only && only[0] == '1') {
        testMegaSingleBroadcastSender();
        std::printf("mega singleton sender: %d checks, %d failures\n",
                    checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_MEGA_RECEIVER_SINGLE");
        only && only[0] == '1') {
        testMegaSingleBroadcastReceiver();
        testMegaSingleReceiverNestedJoin();
        std::printf("mega singleton receiver: %d checks, %d failures\n",
                    checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_STATIC_BCAST");
        only && only[0] == '1') {
        testNativeStaticBroadcastWait();
        std::printf("static broadcast: %d checks, %d failures\n", checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_MEGA_SINGLE");
        only && only[0] == '1') {
        testMegaMultiPcBatch();
        testMegaSingleSurvivorBatch();
        testMegaRepeatBurst();
        testMegaMultiRepeatBurst();
        testMegaMultiRepeatTwoChanges();
        std::printf("mega single: %d checks, %d failures\n", checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_DIRECT_YIELD");
        only && only[0] == '1') {
        testFiberDirectYieldSerialChainOptOut();
        std::printf("direct yield: %d checks, %d failures\n",
                    checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_GENERIC_CHAIN");
        only && only[0] == '1') {
        testGenericConflictFiberChain();
        std::printf("generic chain: %d checks, %d failures\n",
                    checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_LAZY_ROOT_WIDE");
        only && only[0] == '1') {
        testLazyRootWideNative();
        std::printf("lazy root wide: %d checks, %d failures\n",
                    checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_READY_EAGER");
        only && only[0] == '1') {
        testReadyEagerCacheBypass();
        std::printf("ready eager: %d checks, %d failures\n",
                    checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_COMPACTION");
        only && only[0] == '1') {
        testFinishedThreadCompactionStableOrder();
        testCompactionPreservesReplacementMapEntry();
        testCompactionSmallMapErase();
        testSchedulerRetirementNotification();
        std::printf("compact: %d checks, %d failures\n",
                    checks, failures);
        return failures;
    }
    // For focused source-certified conditional beta debugging, use with
    // XYO_TEST_ONLY_STACKLESS_CORO=1 (existing test_cast.cpp entry).
    if (const char* only = std::getenv("XYO_TEST_ONLY_MEGA_IFELSE");
        only && only[0] == '1') {
        testMegaIfElseBatch();
        testMegaIfNoElseBatch();
        testMegaConditionalTwoChangeBatch();
        testMegaIfNoElsePairBatch();
        std::printf("mega conditional: %d checks, %d failures\n",
                    checks, failures);
        return failures;
    }
    if (const char* only = std::getenv("XYO_TEST_ONLY_STACKLESS_CORO");
        only && only[0] == '1') {
        testStacklessCoroutineNativeRepeat();
        testStacklessCoroutineBroadcastRestart();
        std::printf("stackless coroutine: %d checks, %d failures\\n",
                    checks, failures);
        return failures;
    }
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
    testGenericConflictFiberChain();
    testFiberDirectYieldSerialChainOptOut();
    testSchedulerStatusTransitions();
    testWaitResumeOutcomeAudit();
    testNativeWaitResumeOutcomes();
    testAutonomousNativeWaitWriter();
    testNativeWaitFanoutStress();
    testWaitReadyUnnotifiedFallback();
    testWaitNotificationDifferentialAudit();
    testSchedulerRetirementNotification();
    testReadyEagerCacheBypass();
    testLazyRootWideNative();
    testFinishedThreadCompactionStableOrder();
    testCompactionPreservesReplacementMapEntry();
    testCompactionSmallMapErase();
    testReadyChangePublication();
    testConcurrentReadyInvalidation();
    testWaitDeadlineIndex();
    if (generalCoroEnabled()) testCoroAskAndWait();
    testBroadcastReceiverReuse();
    testShortBroadcastReadyCache();
    testShortBroadcastLazyRestart();
    testShortSlotInitializerAliases();
    testShortBroadcastFreshBindings();
    testPendingBroadcastReceiverOrder();
    testNonRestartBroadcastFanout();
    testStacklessCoroutineNativeRepeat();
    testStacklessCoroutineBroadcastRestart();
    testMegaBroadcastRepeatContinuation();
    testMegaBroadcastWaitContinuation();
    testNativeStaticBroadcastWait();
    testMegaTailProcedureResume();
    testMegaNonTailProcedureReturnPc();
    testMegaBroadcastReceiverFusion();
    testMegaSingleBroadcastReceiver();
    testMegaSingleReceiverNestedJoin();
    testMegaSingleBroadcastSender();
    testMegaBroadcastUntilContinuation();
    testMegaUntilWallClockContinuation();
    testMegaUntilElapsedMillisContinuation();
    testMegaUntilBoundedOuterRepeat();
    testMegaUntilNestedProcedure();
    testMegaBroadcastCloneSourceSharing();
    testMegaMultiPcBatch();
    testMegaSingleSurvivorBatch();
    testMegaRepeatBurst();
    testMegaMultiRepeatBurst();
    testMegaMultiRepeatTwoChanges();
    testMegaTwoChangeSequence();
    testMegaIfElseBatch();
    testMegaIfNoElseBatch();
    testMegaConditionalTwoChangeBatch();
    testMegaIfNoElsePairBatch();
    testMegaYieldSwitchDispatch();
    testMegaBatchSwitchDispatch();
    testMegaBatchProcedureDispatch();
    testMegaPartialSchedulerProcedureFusion();
    testMegaGotoProcedureFusion();
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
