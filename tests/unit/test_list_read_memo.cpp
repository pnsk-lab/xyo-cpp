// Read-only leaf list-read/toNumber memo: exact coercion, raw Values, and yield invalidation.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL list read memo: %s\n", what); }
}
struct EnvGuard {
    std::string key, old;
    bool existed;
    EnvGuard(const char* name, const char* value) : key(name), existed(std::getenv(name) != nullptr) {
        if (existed) old = std::getenv(name);
        put(value);
    }
    void put(const char* value) {
#ifdef _WIN32
        _putenv_s(key.c_str(), value ? value : "");
#else
        if (value) setenv(key.c_str(), value, 1); else unsetenv(key.c_str());
#endif
    }
    ~EnvGuard() { put(existed ? old.c_str() : nullptr); }
};
std::string number(int n) { return "[1,[4,\"" + std::to_string(n) + "\"]]"; }
std::string textInput(const char* s) { return "[1,[10,\"" + std::string(s) + "\"]]"; }
std::string ref(const std::string& id) { return "[2,\"" + id + "\"]"; }
std::string field(const char* kind, const char* id) {
    return "{\"" + std::string(kind) + "\":[\"" + id + "\",\"" + id + "\"]}";
}
struct Graph {
    std::string blocks;
    int serial = 0;
    void add(const std::string& id, const std::string& op, const std::string& next = "",
             const std::string& inputs = "{}", const std::string& fields = "{}",
             const std::string& extra = "") {
        if (!blocks.empty()) blocks += ",";
        blocks += "\"" + id + "\":{\"opcode\":\"" + op + "\",\"next\":" +
            (next.empty() ? "null" : "\"" + next + "\"") + ",\"inputs\":" + inputs +
            ",\"fields\":" + fields + extra + "}";
    }
    std::string reporter(const std::string& op, const std::string& inputs,
                         const std::string& fields = "{}") {
        const std::string id = "r" + std::to_string(serial++);
        add(id, op, "", inputs, fields);
        return ref(id);
    }
    std::string var(const char* id) { return reporter("data_variable", "{}", field("VARIABLE", id)); }
    std::string item() {
        return reporter("data_itemoflist", "{\"INDEX\":" + var("i") + "}", field("LIST", "L"));
    }
};
struct Spec {
    const char* name;
    std::vector<Value> values;
    int trip = 6;
    bool reenter = false;
    bool otherListWrite = false;
    bool nested = false;
};
enum class ListMutation { None, Replace, Resize, Clear };
std::string project(const Spec& s, ListMutation mutation = ListMutation::None) {
    Graph g;
    const bool warp = mutation == ListMutation::None;
    const std::string mut = ",\"mutation\":{\"proccode\":\"read\",\"argumentids\":\"[]\","
        "\"argumentnames\":\"[]\",\"argumentdefaults\":\"[]\",\"warp\":\"" +
        std::string(warp ? "true" : "false") + "\"}";
    g.add("hat", "event_whenflagclicked", "call", "{}", "{}", ",\"topLevel\":true");
    g.add("call", "procedures_call", s.reenter ? "replaceBetween" : "", "{}", "{}", mut);
    if (s.reenter) {
        g.add("replaceBetween", "data_replaceitemoflist", "resetIndex",
              "{\"INDEX\":" + number(1) + ",\"ITEM\":" + textInput("11") + "}", field("LIST", "L"));
        g.add("resetIndex", "data_setvariableto", "callAgain", "{\"VALUE\":" + number(1) + "}", field("VARIABLE", "i"));
        g.add("callAgain", "procedures_call", "", "{}", "{}", mut);
    }
    g.add("def", "procedures_definition", "loop", "{\"custom_block\":[1,\"proto\"]}", "{}", ",\"topLevel\":true");
    g.add("proto", "procedures_prototype", "", "{}", "{}", ",\"shadow\":true" + mut);
    g.add("loop", "control_repeat", "", "{\"TIMES\":" + number(s.trip) +
          ",\"SUBSTACK\":" + ref(s.nested ? "inner" : "sum") + "}");
    if (s.nested) g.add("inner", "control_repeat", "", "{\"TIMES\":" + number(1) + ",\"SUBSTACK\":" + ref("sum") + "}");
    // Only scalar writes in the eligible loop. An out-list observer would
    // itself disable the optimization this fixture is intended to exercise.
    g.add("sum", "data_changevariableby", "last", "{\"VALUE\":" + g.item() + "}", field("VARIABLE", "sum"));
    g.add("last", "data_setvariableto", "compare", "{\"VALUE\":" + g.item() + "}", field("VARIABLE", "last"));
    const std::string eq = g.reporter("operator_equals", "{\"OPERAND1\":" + g.item() +
                                      ",\"OPERAND2\":" + textInput("false") + "}");
    g.add("compare", "control_if", "nextIndex", "{\"CONDITION\":" + eq + ",\"SUBSTACK\":" + ref("hit") + "}");
    g.add("hit", "data_changevariableby", "", "{\"VALUE\":" + number(1) + "}", field("VARIABLE", "matches"));
    g.add("nextIndex", "data_changevariableby", "wrap", "{\"VALUE\":" + number(1) + "}", field("VARIABLE", "i"));
    const std::string len = g.reporter("data_lengthoflist", "{}", field("LIST", "L"));
    const std::string past = g.reporter("operator_gt", "{\"OPERAND1\":" + g.var("i") + ",\"OPERAND2\":" + len + "}");
    g.add("wrap", "control_if", s.otherListWrite ? "otherWrite" : "",
          "{\"CONDITION\":" + past + ",\"SUBSTACK\":" + ref("reset") + "}");
    g.add("reset", "data_setvariableto", "", "{\"VALUE\":" + number(1) + "}", field("VARIABLE", "i"));
    if (s.otherListWrite) g.add("otherWrite", "data_replaceitemoflist", "",
        "{\"INDEX\":" + number(1) + ",\"ITEM\":" + number(1) + "}", field("LIST", "other"));
    if (mutation != ListMutation::None) {
        // A hat is essential: JIT entry lowering begins at root->next.
        g.add("writer", "event_whenbroadcastreceived", "writerOp", "{}", field("BROADCAST_OPTION", "go"), ",\"topLevel\":true");
        if (mutation == ListMutation::Replace) {
            g.add("writerOp", "data_replaceitemoflist", "", "{\"INDEX\":" + number(1) +
                  ",\"ITEM\":" + textInput("19") + "}", field("LIST", "L"));
        } else {
            g.add("writerOp", "data_deletealloflist", mutation == ListMutation::Resize ? "append0" : "", "{}", field("LIST", "L"));
            // A short straight chain grows beyond the initial capacity. This
            // also catches stale promoted items pointers across a real yield.
            if (mutation == ListMutation::Resize)
                for (int i = 0; i < 64; ++i)
                    g.add("append" + std::to_string(i), "data_addtolist", i == 63 ? "" : "append" + std::to_string(i + 1),
                          "{\"ITEM\":" + textInput("19") + "}", field("LIST", "L"));
        }
    }
    return "{\"targets\":[{\"isStage\":true,\"name\":\"Stage\","
        "\"variables\":{\"sum\":[\"sum\",0],\"last\":[\"last\",\"untouched\"],\"matches\":[\"matches\",0],\"i\":[\"i\",1]},"
        "\"lists\":{\"L\":[\"L\",[]],\"other\":[\"other\",[0]]},\"blocks\":{" + g.blocks +
        "},\"costumes\":[],\"sounds\":[]}],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
}
Value text(const char* s) { return Value::string(SStr::fromUtf8(s)); }
std::string encode(Value v) {
    if (v.isString()) return "s:" + SStr::toUtf8(cast::toString(v));
    if (v.isNumber() && std::isnan(v.asNumberUnchecked())) return "n:NaN";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "b:%016llx", static_cast<unsigned long long>(v.bits));
    return buf;
}
using State = std::vector<std::string>;
State run(const Spec& s, JitMode mode, bool memo,
          ListMutation mutation = ListMutation::None, bool collect = false) {
    EnvGuard option("XYO_LIST_READ_MEMO", memo ? "1" : "0");
    setJitMode(mode);
    Runtime rt;
    const bool ok = loadSb3(rt, project(s, mutation), nullptr).ok;
    check(ok && rt.targets.size() == 1, "load read-only memo fixture");
    if (!ok || rt.targets.size() != 1) return {};
    Target* t = rt.targets[0];
    SList* list = t->lookupVariableById("L")->list;
    std::vector<Value> values;
    {
        Gc::DeferCollect defer;
        // Fresh strings per mode ensure the interp run does not warm the
        // numeric caches used by either JIT variant.
        for (Value v : s.values)
            values.push_back(v.isString() && v.bits != val::kEmptyStr
                ? Value::string(SStr::fromUtf8(SStr::toUtf8(cast::toString(v)))) : v);
        // GC cases create their input only after loading, then root it before
        // further allocations; a native Spec vector is not a GC root.
        if (mutation != ListMutation::None) values = {text("7")};
        for (Value v : values) list->push(v);
    }
    State raw;
    std::vector<uint64_t> originalBits;
    for (Value v : values) { raw.push_back(encode(v)); originalBits.push_back(v.bits); }
    // Compute semantic expectations before optional collection can reclaim
    // strings no longer owned by the list. Spec's native vector is not a root.
    State scalarExpected;
    {
        Gc::DeferCollect deferExpected;
        Value expected = Value::num(0), last = text("untouched");
        int matches = 0;
        const Value falseText = text("false");
        auto account = [&](Value v) {
            // Do not pre-populate the tested SStr's numeric cache merely by
            // computing reference results before the generated loop runs.
            const Value copy = v.isString()
                ? Value::string(SStr::fromUtf8(SStr::toUtf8(cast::toString(v)))) : v;
            expected = Value::numCanonical(cast::toNumber(expected) + cast::toNumber(copy));
            last = v;
            if (cast::compare(copy, falseText) == 0) ++matches;
        };
        if (mutation == ListMutation::None) {
            for (int i = 0; i < s.trip; ++i) account(values.empty() ? Value::emptyStr() : values[size_t(i) % values.size()]);
            if (s.reenter) {
                for (int i = 0; i < s.trip; ++i) {
                    const size_t index = values.empty() ? 0 : size_t(i) % values.size();
                    account(index == 0 ? text("11") : values[index]);
                }
                if (!raw.empty()) raw[0] = "s:11";
            }
        } else {
            account(values[0]);
            account(mutation == ListMutation::Clear ? Value::emptyStr() : text("19"));
            raw.assign(mutation == ListMutation::Clear ? 0 : mutation == ListMutation::Resize ? 64 : 1, "s:19");
        }
        scalarExpected = {encode(expected), encode(last), encode(Value::num(matches))};
    }
    rt.greenFlag();
    check(rt.threads.size() == 1, "start only read loop");
    if (rt.threads.size() != 1) return {};
    Thread* main = rt.threads[0];
#ifdef XYO_JIT
    if (mode == JitMode::Eager) check(main->isCompiled, "memo fixture actually compiled");
#endif
    if (mutation != ListMutation::None) {
        rt.sequencer->timer.start();
        rt.sequencer->stepThread(*main);
        check(main->status != STATUS_DONE, "non-warp loop yields between two list reads");
        check(encode(t->lookupVariableById("sum")->value) == encode(Value::num(7)), "first read contributes seven before writer");
        Thread* writer = rt.pushThread("writer", t);
        check(writer != nullptr, "start independent list writer");
        if (writer) {
            for (int pass = 0; pass < 8 && writer->status != STATUS_DONE && !writer->stack.empty(); ++pass)
                rt.sequencer->stepThread(*writer);
            check(writer->status == STATUS_DONE || writer->stack.empty(), "writer completes while reader remains suspended");
        }
        if (collect) {
            const size_t before = Gc::get().collectCount();
            Gc::get().collect();
            check(Gc::get().collectCount() > before, "GC really runs between memoized reads");
        }
    }
    rt.runUntilDone(100);
    check(rt.threads.empty(), "memo fixture terminates");
    State out;
    for (const char* id : {"sum", "last", "matches"}) out.push_back(encode(t->lookupVariableById(id)->value));
    for (size_t i = 0; i < scalarExpected.size(); ++i) {
        if (out[i] != scalarExpected[i])
            std::printf("list memo diagnostic case=%s mode=%d memo=%d mutation=%d field=%zu got=%s expected=%s\n",
                        s.name, int(mode), int(memo), int(mutation), i, out[i].c_str(), scalarExpected[i].c_str());
        check(out[i] == scalarExpected[i], "sum/raw final Value/comparison match independent semantics");
    }
    State actualRaw;
    for (uint32_t i = 0; i < list->len; ++i) {
        actualRaw.push_back(encode(list->items[i]));
        if (mutation == ListMutation::None && i < originalBits.size() && !(s.reenter && i == 0))
            check(list->items[i].bits == originalBits[i], "memo/coercion keeps exact raw list Value bits");
    }
    check(actualRaw == raw, "read/coercion does not rewrite raw list Values");
    out.insert(out.end(), actualRaw.begin(), actualRaw.end());
    return out;
}
// Exercise a real compiled list-read loop while the backing storage changes
// during a scheduler yield. Compare logical states with the interpreter.
State runAdaptiveEpochTransition(JitMode mode, bool promoteBeforeStart) {
    setJitMode(mode);
    Runtime rt;
    Spec spec{"adaptive epoch interleave", {}, 140};
    const bool ok = loadSb3(rt, project(spec, ListMutation::Replace), nullptr).ok;
    check(ok && rt.targets.size() == 1, "adaptive epoch fixture loaded");
    if (!ok || rt.targets.size() != 1) return {};
    Target* t = rt.targets[0];
    SList* list = t->lookupVariableById("L")->list;
    check(list != nullptr, "adaptive epoch fixture list exists");
    if (!list) return {};
    for (int i = 0; i < 512; ++i)
        list->push(Value::numCanonical(double((i % 11) + 1)));
    const auto promote = [&] {
        for (int i = 0; i < 32; ++i) {
            list->insert0(64, Value::numCanonical(double(50 + i)));
            list->erase0(65);
        }
    };
    if (promoteBeforeStart) promote();
    const uint64_t initialEpoch = list->storageEpoch;
    rt.greenFlag();
    check(rt.threads.size() == 1, "adaptive epoch reader started");
    if (rt.threads.size() != 1) return {};
    Thread* reader = rt.threads[0];
#ifdef XYO_JIT
    if (mode == JitMode::Eager)
        check(reader->isCompiled, "adaptive epoch reader compiled");
#endif
    rt.sequencer->timer.start();
    rt.sequencer->stepThread(*reader);
    check(reader->status != STATUS_DONE && !reader->stack.empty(),
          "adaptive epoch reader suspended between reads");
    if (reader->status == STATUS_DONE || reader->stack.empty()) return {};

    // Cover both epoch paths:
    // (1) a list becomes gap while the JIT is suspended;
    // (2) the JIT begins its first read with an already non-flat list.
    if (!promoteBeforeStart) promote();
    check(list->extra != nullptr && list->adaptiveBackend == 5,
          "native edits promote list to gap");
    check(promoteBeforeStart || list->storageEpoch > initialEpoch,
          "interleaved promotion invalidates cached items");
    const uint64_t promotedEpoch = list->storageEpoch;
    Gc::get().collect();
    rt.runUntilDone(4000);
    check(rt.threads.empty(), "adaptive epoch reader finished");
    if (mode == JitMode::Eager) {
        check(list->isContiguous(),
              "adaptive JIT demotes after repeated helper reads");
        check(list->storageEpoch > promotedEpoch,
              "JIT demotion invalidates cached gap representation");
    } else if (list->extra) {
        // The interpreter's direct get0 path does not count adaptive reads.
        list->makeContiguous();
    }
    State result;
    for (const char* id : {"sum", "last", "matches", "i"})
        result.push_back(encode(t->lookupVariableById(id)->value));
    for (uint32_t i = 0; i < list->len; ++i)
        result.push_back(encode(list->get0(i)));
    return result;
}
void compare(const Spec& s, ListMutation mutation = ListMutation::None, bool collect = false) {
    const State interp = run(s, JitMode::Off, false, mutation, collect);
    const State disabled = run(s, JitMode::Eager, false, mutation, collect);
    const State enabled = run(s, JitMode::Eager, true, mutation, collect);
    check(!interp.empty() && disabled == interp, "memo-disabled JIT matches interpreter");
    check(!interp.empty() && enabled == interp, "memo-enabled JIT matches interpreter");
}
} // namespace

int test_list_read_memo_main() {
    const JitMode saved = jitMode();
    EnvGuard gpu("XYO_GPU", "0"), parallel("XYO_PAR", "0"), parfor("XYO_PARFOR", "0");
    {
        // Table Values are temporarily unrooted between fresh Runtime instances.
        // Explicit collection tests below run outside this deferral scope.
        Gc::DeferCollect defer;
        const double inf = std::numeric_limits<double>::infinity();
        const std::vector<Spec> cases = {
            {"repeated numeric string", {text("007")}},
            {"alternating numeric strings", {text("7"), text("19")}},
            {"nonnumeric string", {text("hello")}},
            {"whitespace string", {text(" ")}},
            {"empty string singleton", {Value::emptyStr()}},
            {"heap empty string", {text("")}},
            {"false spelling", {text("false"), text("FALSE")}},
            {"true bool", {Value::boolean(true)}},
            {"false bool", {Value::boolean(false)}},
            {"numeric NaN", {Value::nan()}},
            {"string NaN memoized as zero", {text("NaN")}},
            {"numeric infinities", {Value::num(inf), Value::num(-inf)}},
            {"opposite infinities then repeated finite string recover", {Value::num(inf), Value::num(-inf), text("7"), text("7")}, 4},
            {"string infinities then finite memo recover", {text("Infinity"), text("-Infinity"), text("7"), text("7")}, 4},
            {"finite hit interleaved with invalidating nonfinite strings", {text("7"), text("7"), text("Infinity"), text("7"), text("-Infinity"), text("7")}},
            {"finite bool cache around NaN", {Value::boolean(true), Value::boolean(true), Value::nan(), Value::boolean(true), Value::boolean(true)}, 5},
            {"initial undefined memo key", {Value::undefined()}},
            {"mixed coercions", {text("7"), Value::boolean(true), text("hello"), Value::nan(), text(" "), Value::num(3)}},
            {"plain numeric fast path", {Value::num(7), Value::num(-2), Value::num(-0.0)}},
            {"zero trip preserves initial scalar", {text("7")}, 0},
            {"same procedure loop reentry after external replace", {text("7")}, 3, true},
            {"other-list write disables memo", {text("7")}, 4, false, true},
            {"nested loop disables memo", {text("7")}, 4, false, false, true}
        };
        for (const Spec& s : cases) compare(s);
    }
    // Each input is immediately installed in and rooted by its Runtime list.
    // No suite-wide DeferCollect is active for these actual GC checks.
    for (ListMutation m : {ListMutation::Replace, ListMutation::Resize, ListMutation::Clear}) {
        // Recreate the input per Runtime so a prior run's collection cannot
        // leave a dangling native Spec vector in the next run.
        State states[3];
        for (int variant = 0; variant < 3; ++variant) {
            Spec s{"non-warp writer plus collection", {}, 2};
            states[variant] = run(s, variant == 0 ? JitMode::Off : JitMode::Eager, variant == 2, m, true);
        }
        check(!states[0].empty() && states[1] == states[0], "writer/GC memo-disabled JIT matches interp");
        check(!states[0].empty() && states[2] == states[0], "writer/GC memo-enabled JIT matches interp");
    }
    if (xyo_list_storage_mode == 7) {
        for (bool alreadyPromoted : {false, true}) {
            const State interp =
                runAdaptiveEpochTransition(JitMode::Off, alreadyPromoted);
            const State eager =
                runAdaptiveEpochTransition(JitMode::Eager, alreadyPromoted);
            check(!interp.empty() && eager == interp,
                  "adaptive epoch JIT matches interpreter across GC and transitions");
        }
    }
    setJitMode(saved);
    std::printf("list read memo: %d checks, %d failures\n", checks, failures);
    return failures;
}
