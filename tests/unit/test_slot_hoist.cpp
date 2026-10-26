// Stable Variable* handles must not freeze variable values or list storage.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/slist.hpp"
#include "engine/block_utility.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#ifdef XYO_JIT
#include "aot/aot.hpp"
#endif

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL slot hoist: %s\n", what); }
}
struct SlotReloadEnv {
    bool had = std::getenv("XYO_JIT_SLOT_RELOAD") != nullptr;
    std::string old = had ? std::getenv("XYO_JIT_SLOT_RELOAD") : "";
    explicit SlotReloadEnv(bool reload) { set(reload ? "1" : nullptr); }
    ~SlotReloadEnv() { set(had ? old.c_str() : nullptr); }
    static void set(const char* value) {
#ifdef _WIN32
        _putenv_s("XYO_JIT_SLOT_RELOAD", value ? value : "");
#else
        if (value) setenv("XYO_JIT_SLOT_RELOAD", value, 1);
        else unsetenv("XYO_JIT_SLOT_RELOAD");
#endif
    }
};
std::string number(int n) { return "[1,[4,\"" + std::to_string(n) + "\"]]"; }
std::string reporter(const char* id) { return "[2,\"" + std::string(id) + "\"]"; }
std::string variable(const std::string& name, const std::string& id = "") {
    return "{\"VARIABLE\":[\"" + name + "\",\"" + (id.empty() ? name : id) + "\"]}";
}
struct Graph {
    std::string text;
    void add(const std::string& id, const std::string& opcode,
             const std::string& next = "", const std::string& inputs = "{}",
             const std::string& fields = "{}", const std::string& extra = "") {
        if (!text.empty()) text += ",";
        text += "\"" + id + "\":{\"opcode\":\"" + opcode + "\",\"next\":" +
            (next.empty() ? "null" : "\"" + next + "\"") + ",\"inputs\":" +
            inputs + ",\"fields\":" + fields + extra + "}";
    }
    void change(const std::string& id, const std::string& next,
                const std::string& name, const std::string& input) {
        add(id, "data_changevariableby", next, "{\"VALUE\":" + input + "}", variable(name));
    }
    static std::string mutation(const std::string& name) {
        return ",\"mutation\":{\"proccode\":\"" + name +
            "\",\"argumentids\":\"[]\",\"argumentnames\":\"[]\","
            "\"argumentdefaults\":\"[]\",\"warp\":\"true\"}";
    }
    void call(const std::string& id, const std::string& name, const std::string& next) {
        add(id, "procedures_call", next, "{}", "{}", mutation(name));
    }
    void definition(const std::string& name, const std::string& next) {
        add(name + "Def", "procedures_definition", next,
            "{\"custom_block\":[1,\"" + name + "Proto\"]}");
        add(name + "Proto", "procedures_prototype", "", "{}", "{}",
            ",\"shadow\":true" + mutation(name));
    }
};
std::string sprite(const char* name, int seed) {
    Graph g;
    g.add("hat", "event_whenflagclicked", "cold", "{}", "{}", ",\"topLevel\":true");
    g.add("cold", "control_if", "loop",
          "{\"CONDITION\":[2,\"never\"],\"SUBSTACK\":[2,\"cold0\"]}");
    g.add("never", "operator_equals", "", "{\"OPERAND1\":" + number(0) +
          ",\"OPERAND2\":" + number(1) + "}");
    std::string vars = "\"x\":[\"x\"," + std::to_string(seed) +
        "],\"sum\":[\"sum\",0],\"depth\":[\"depth\",0],\"alias\":[\"alias\",3],"
        "\"procOnly\":[\"procOnly\",0]";
    // Slots from unreachable branches are still initialized unconditionally;
    // using many forces vector growth before the final initialization finishes.
    for (int i = 0; i < 96; ++i) {
        std::string id = "cold" + std::to_string(i);
        vars += ",\"" + id + "\":[\"" + id + "\"," + std::to_string(i) + "]";
        g.add(id, "data_setvariableto", i == 95 ? "" : "cold" + std::to_string(i + 1),
              "{\"VALUE\":" + number(-1) + "}", variable(id));
    }
    g.add("loop", "control_repeat", "clone",
          "{\"TIMES\":" + number(3) + ",\"SUBSTACK\":[2,\"incX\"]}");
    g.change("incX", "leafCall", "x", number(1));
    g.call("leafCall", "leaf", "setSum");
    g.add("setSum", "data_setvariableto", "push", "{\"VALUE\":[2,\"probe\"]}", variable("sum"));
    g.add("probe", "slot_probe");
    g.add("push", "data_addtolist", "aliasInc", "{\"ITEM\":[2,\"readX\"]}",
          "{\"LIST\":[\"items\",\"items\"]}");
    g.add("readX", "data_variable", "", "{}", variable("x"));
    // Different ID resolves to the SAME Variable* by name. No noalias allowed.
    g.add("aliasInc", "data_changevariableby", "recCall", "{\"VALUE\":" + number(1) + "}",
          variable("alias", "missingAlias"));
    g.call("recCall", "recursive", "wait");
    g.add("wait", "control_wait", "", "{\"DURATION\":" + number(0) + "}");
    g.add("clone", "control_create_clone_of", "", "{\"CLONE_OPTION\":[1,[10,\"_myself_\"]]}");
    g.add("cloneHat", "control_start_as_clone", "cloneInc", "{}", "{}", ",\"topLevel\":true");
    g.change("cloneInc", "", "x", number(7));
    g.definition("leaf", "leafInc");
    g.change("leafInc", "nestedCall", "sum", reporter("readLeafX"));
    g.add("readLeafX", "data_variable", "", "{}", variable("x"));
    g.call("nestedCall", "nested", "");
    g.definition("nested", "nestedInc");
    g.change("nestedInc", "", "procOnly", number(1));
    g.definition("recursive", "recIf");
    g.add("recIf", "control_if", "", "{\"CONDITION\":[2,\"recTest\"],\"SUBSTACK\":[2,\"depthInc\"]}");
    g.add("recTest", "operator_lt", "", "{\"OPERAND1\":[2,\"readDepth\"],\"OPERAND2\":" + number(2) + "}");
    g.add("readDepth", "data_variable", "", "{}", variable("depth"));
    g.change("depthInc", "recX", "depth", number(1));
    g.change("recX", "recurse", "x", number(1));
    g.call("recurse", "recursive", "depthDec");
    g.change("depthDec", "", "depth", number(-1));
    return "{\"isStage\":false,\"name\":\"" + std::string(name) + "\",\"variables\":{" + vars +
        "},\"lists\":{\"items\":[\"items\",[]]},\"broadcasts\":{},\"blocks\":{" + g.text +
        "},\"costumes\":[],\"sounds\":[],\"currentCostume\":0}";
}
std::string project() {
    return "{\"targets\":[{\"isStage\":true,\"name\":\"Stage\",\"variables\":{},\"lists\":{},"
        "\"blocks\":{},\"costumes\":[],\"sounds\":[]}," + sprite("A", 1) + "," +
        sprite("B", 100) + "],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
}
Value probe(const ArgValues&, BlockUtility& u) {
    auto& frame = u.stackFrame();
    if (!frame.has("waited")) {
        frame["waited"] = Value::boolean(true);
        u.yieldTick();
        return Value::undefined();
    }
    Variable* x = u.target()->lookupVariableById("x");
    x->value = Value::numCanonical(cast::toNumber(x->value) + 10);
    // Reallocation/lazy list creation must remain visible through real loads.
    Variable* items = u.target()->lookupVariableById("items");
    if (!items->list) items->list = SList::create();
    items->list->ensure(items->list->cap + 1);
    Gc::get().collect();
    return x->value;
}
void prepare(Runtime& rt, const std::string& json) {
    rt.registerPrimitive("slot_probe", probe);
    check(loadSb3(rt, json, nullptr).ok, "load multi-script slot fixture");
    for (Target* t : rt.targets)
        if (!t->isStage) t->lookupVariableById("items")->list = nullptr;
}
double value(Target* t, const char* id) {
    Variable* v = t->lookupVariableById(id);
    return v ? cast::toNumber(v->value) : -99999;
}
void verify(Runtime& rt) {
    check(rt.threads.empty(), "yielding recursive/clone scripts complete");
    check(rt.targets.size() == 5, "both original targets create one clone");
    for (Target* t : rt.targets) {
        if (t->isStage) continue;
        const int seed = t->getName() == u"A" ? 1 : 100;
        check(value(t, "x") == seed + (t->isOriginal ? 39 : 46), "target-local handles and post-callback values");
        check(value(t, "sum") == seed + 37, "compat reporter mutation is visible");
        check(value(t, "procOnly") == 3, "nested/procedure-only slot initialized");
        check(value(t, "depth") == 0, "recursive invocation handles remain valid");
        check(value(t, "alias") == 6, "distinct IDs may alias by name");
        check(value(t, "cold0") == 0 && value(t, "cold95") == 95, "cold-branch values untouched");
        SList* list = t->lookupVariableById("items")->list;
        check(list && list->len == 3, "lazy/reallocated list pointer is reloaded");
        if (list && list->len == 3)
            for (unsigned i = 0; i < 3; ++i)
                check(cast::toNumber(list->items[i]) == seed + 11 + 13 * i, "list items use current scalar value");
    }
}
void run(JitMode mode, bool reload) {
    SlotReloadEnv env(reload);
    setJitMode(mode);
    Runtime rt;
    prepare(rt, project());
    rt.greenFlag();
#ifdef XYO_JIT
    if (mode == JitMode::Eager)
        for (Thread* t : rt.threads) check(t->isCompiled, "eager fixture actually compiled");
#endif
    rt.runUntilDone(200);
    verify(rt);
}
#ifdef XYO_JIT
void testRestart() {
    SlotReloadEnv env(false);
    setJitMode(JitMode::Eager);
    Runtime rt;
    prepare(rt, project());
    rt.greenFlag();
    if (rt.threads.empty()) { check(false, "restart has a compiled thread"); return; }
    Thread* old = rt.threads[0];
    Target* target = old->target;
    check(old->isCompiled, "restart source really compiled");
    if (!old->isCompiled) return;
    // Resume exactly once: the compat probe yields before changing x. Restart
    // uses a NEW Thread, not entry reexecution on the suspended slot vector.
    rt.sequencer->jitStepThread(*old);
    check(!old->jitVarSlots.empty(), "suspended native invocation initialized slots");
    Variable** oldArray = old->jitVarSlots.data();
    Thread* fresh = rt.restartThread(old);
    rt.stopThread(old);
    check(fresh != old && fresh->jitVarSlots.empty(), "restart owns a fresh slot vector");
    rt.runUntilDone(200);
    check(rt.threads.empty(), "restarted script and clones complete");
    check(!fresh->jitVarSlots.empty() && fresh->jitVarSlots.data() != oldArray,
          "restart cannot replace the suspended thread's slot array");
    const int seed = target->getName() == u"A" ? 1 : 100;
    check(value(target, "x") == seed + 40, "restart runs from new entry after one prior increment");
}

void runAot(bool reload) {
    SlotReloadEnv env(reload);
    setJitMode(JitMode::Eager);
    const std::string json = project();
    Runtime source;
    prepare(source, json);
    const uint64_t hash = aot::hashBytes(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    aot::BuildResult result;
    std::string why;
    const bool built = aot::compileProject(source, hash, result, &why);
    check(built, "compile shared-module AOT fixture");
    if (!built) { std::printf("slot AOT: %s\n", why.c_str()); return; }
    check(result.scripts >= 4, "AOT includes both targets' main and clone entries");
    Runtime rt;
    prepare(rt, json);
    const bool loaded = rt.jit && rt.jit->loadAot(rt, result.object.data(), result.object.size(), hash, &why);
    check(loaded, "load AOT handles for distinct scripts sharing index zero");
    if (!loaded) return;
    rt.greenFlag();
    for (Thread* t : rt.threads) {
        JitEngine::ScriptStats stats;
        const bool found = rt.jit->scriptStats(t->topBlockPtr, &stats);
        check(t->isCompiled && found && stats.aot, "AOT executed without eager fallback");
    }
    rt.runUntilDone(200);
    verify(rt);
}
#endif
} // namespace
int test_slot_hoist_main() {
    const auto saved = jitMode();
    run(JitMode::Off, false);
    run(JitMode::Eager, false);
    run(JitMode::Eager, true);
#ifdef XYO_JIT
    testRestart();
    runAot(false);
    runAot(true);
#endif
    setJitMode(saved);
    std::printf("slot hoist: %d checks, %d failures\n", checks, failures);
    return failures;
}
