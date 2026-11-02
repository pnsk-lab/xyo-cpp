// Numeric promotion and boolean lowering preserve Value identity and branch semantics.
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
    if (!ok) { ++failures; std::printf("FAIL numeric promotion: %s\n", what); }
}
struct EnvGuard {
    std::string key, previous;
    bool existed;
    EnvGuard(const char* k) : key(k), existed(std::getenv(k) != nullptr) {
        if (existed) previous = std::getenv(k);
        put("0");
    }
    void put(const char* value) {
#ifdef _WIN32
        _putenv_s(key.c_str(), value ? value : "");
#else
        if (value) setenv(key.c_str(), value, 1); else unsetenv(key.c_str());
#endif
    }
    ~EnvGuard() { put(existed ? previous.c_str() : nullptr); }
};
std::string number(int n) { return "[1,[4,\"" + std::to_string(n) + "\"]]"; }
std::string ref(const std::string& id) { return "[2,\"" + id + "\"]"; }
std::string field(const char* kind, const std::string& id) {
    return "{\"" + std::string(kind) + "\":[\"" + id + "\",\"" + id + "\"]}";
}
struct Graph {
    std::string blocks;
    int serial = 0;
    void add(const std::string& id, const std::string& op, const std::string& next = "",
             const std::string& inputs = "{}", const std::string& fields = "{}",
             bool top = false) {
        if (!blocks.empty()) blocks += ",";
        blocks += "\"" + id + "\":{\"opcode\":\"" + op + "\",\"next\":" +
            (next.empty() ? "null" : "\"" + next + "\"") + ",\"inputs\":" + inputs +
            ",\"fields\":" + fields + (top ? ",\"topLevel\":true" : "") + "}";
    }
    std::string reporter(const std::string& op, const std::string& inputs,
                         const std::string& fields = "{}") {
        const std::string id = "r" + std::to_string(serial++);
        add(id, op, "", inputs, fields);
        return ref(id);
    }
    std::string var(const char* name) { return reporter("data_variable", "{}", field("VARIABLE", name)); }
    std::string source() { return reporter("data_itemoflist", "{\"INDEX\":" + number(1) + "}", field("LIST", "source")); }
    void observe(const char* id, const char* next, int slot, const std::string& value) {
        add(id, "data_replaceitemoflist", next,
            "{\"INDEX\":" + number(slot) + ",\"ITEM\":" + value + "}", field("LIST", "out"));
    }
};
struct Case {
    const char* name;
    Value input;
    bool set = false;
    bool change = true;
    bool skippedChange = false;
    int iterations = 1;
    bool dynamicDelta = false;
    Value initial = Value::undefined(); // undefined means fixture's raw "001"
};
std::string project(const Case& c, bool yieldWriter = false) {
    Graph g;
    g.add("hat", "event_whenflagclicked", "loop", "{}", "{}", true);
    g.add("loop", "control_repeat", "final", "{\"TIMES\":" + number(c.iterations) +
          ",\"SUBSTACK\":" + ref("before") + "}");
    // Every reporter occurrence has one parent; reusing reporter IDs would
    // corrupt the interpreter input-cache parentValues, not test numeric promotion.
    g.observe("before", c.set ? "set" : "maybeChange", 1, g.var("x"));
    if (c.set) g.add("set", "data_setvariableto", "maybeChange",
                    "{\"VALUE\":" + g.source() + "}", field("VARIABLE", "x"));
    const std::string delta = c.dynamicDelta ? g.source() : number(1);
    if (c.skippedChange) {
        const std::string no = g.reporter("operator_equals", "{\"OPERAND1\":" + number(0) +
                                         ",\"OPERAND2\":" + number(1) + "}");
        g.add("maybeChange", "control_if", "raw", "{\"CONDITION\":" + no +
              ",\"SUBSTACK\":" + ref("inc") + "}");
        g.add("inc", "data_changevariableby", "", "{\"VALUE\":" + delta + "}", field("VARIABLE", "x"));
    } else if (c.change) {
        g.add("maybeChange", "data_changevariableby", "raw", "{\"VALUE\":" + delta + "}", field("VARIABLE", "x"));
    } else {
        // Still retain an eligible finite change in the loop graph, but do
        // not execute it. This exercises the raw initial-value PHI arm.
        const std::string no = g.reporter("operator_equals", "{\"OPERAND1\":" + number(0) +
                                         ",\"OPERAND2\":" + number(1) + "}");
        g.add("maybeChange", "control_if", "raw", "{\"CONDITION\":" + no +
              ",\"SUBSTACK\":" + ref("inc") + "}");
        g.add("inc", "data_changevariableby", "", "{\"VALUE\":" + delta + "}", field("VARIABLE", "x"));
    }
    g.observe("raw", "copy", 2, g.var("x"));
    g.add("copy", "data_setvariableto", "cmp", "{\"VALUE\":" + g.var("x") + "}", field("VARIABLE", "copy"));
    // Compare x only after an executed finite change made it a number. Raw
    // NaN/whitespace comparison through knownNumD has a separate pre-existing
    // issue; raw preservation here is checked by list ITEM and final Values.
    const bool safeCompare = c.change && !c.skippedChange && !c.dynamicDelta;
    const std::string cmpLeft = safeCompare ? g.var("x") : number(0);
    const std::string cmpRight = safeCompare ? g.source() : number(1);
    const std::string eq = g.reporter("operator_equals", "{\"OPERAND1\":" + cmpLeft +
                                    ",\"OPERAND2\":" + cmpRight + "}");
    g.observe("cmp", "arithmetic", 3, eq);
    const std::string sum = g.reporter("operator_add", "{\"NUM1\":" + g.var("x") + ",\"NUM2\":" + number(2) + "}");
    g.observe("arithmetic", "", 4, sum);
    g.observe("final", "", 5, g.var("x"));
    if (yieldWriter) {
        // Explicitly push this script only after the loop's first yield.
        g.add("writer", "event_whenbroadcastreceived", "writerSet", "{}", field("BROADCAST_OPTION", "go"), true);
        g.add("writerSet", "data_setvariableto", "", "{\"VALUE\":" + g.source() + "}", field("VARIABLE", "x"));
    }
    return "{\"targets\":[{\"isStage\":true,\"name\":\"Stage\","
        "\"variables\":{\"x\":[\"x\",\"001\"],\"copy\":[\"copy\",\"untouched\"]},"
        "\"lists\":{\"source\":[\"source\",[0]],\"out\":[\"out\",[0,0,0,0,0]]},"
        "\"blocks\":{" + g.blocks + "},\"costumes\":[],\"sounds\":[]}],"
        "\"monitors\":[],\"extensions\":[],\"meta\":{}}";
}
std::string encode(Value v) {
    if (v.isString()) return "s:" + SStr::toUtf8(cast::toString(v));
    // Hardware arithmetic in interp may retain a negative NaN, whereas JIT
    // canonicalizes it. Scratch cannot observe numeric NaN's sign/payload.
    // Keep type identity (string "NaN" remains s:NaN) and exact bits for all
    // non-NaN numbers, including negative zero.
    if (v.isNumber() && std::isnan(v.asNumberUnchecked())) return "n:NaN";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "b:%016llx", static_cast<unsigned long long>(v.bits));
    return buf;
}
using State = std::vector<std::string>;
State run(const Case& c, JitMode mode, bool yieldWriter = false) {
    setJitMode(mode);
    Runtime rt;
    const bool loaded = loadSb3(rt, project(c, yieldWriter), nullptr).ok;
    check(loaded && rt.targets.size() == 1, c.name);
    if (!loaded || rt.targets.size() != 1) return {};
    Target* t = rt.targets[0];
    // Install arbitrary Values, including true numeric NaN/Infinity, instead
    // of accidentally testing only text literals with similar spellings.
    t->lookupVariableById("source")->list->items[0] = c.input;
    if (c.initial.bits != Value::undefined().bits)
        t->lookupVariableById("x")->value = c.initial;
    rt.greenFlag();
    check(rt.threads.size() == 1, "only the green-flag script starts");
    if (rt.threads.size() != 1) return {};
    Thread* main = rt.threads[0];
#ifdef XYO_JIT
    if (mode == JitMode::Eager) check(main->isCompiled, "eager loop actually compiled");
#endif
    if (yieldWriter) {
        rt.sequencer->timer.start();
        rt.sequencer->stepThread(*main);
        check(cast::toNumber(t->lookupVariableById("x")->value) == 2,
              "non-warp first iteration spills changed number at yield");
        check(main->status != STATUS_DONE, "non-warp loop suspended before its next iteration");
        Thread* writer = rt.pushThread("writer", t);
        check(writer != nullptr, "start competing writer script");
        if (writer) {
            for (int pass = 0; pass < 8 && writer->status != STATUS_DONE && !writer->stack.empty(); ++pass)
                rt.sequencer->stepThread(*writer);
            check(writer->status == STATUS_DONE || writer->stack.empty(), "competing writer finishes before main resumes");
        }
        if (encode(t->lookupVariableById("x")->value) != encode(c.input))
            std::printf("numeric promotion writer diagnostic mode=%d compiled=%d got=%s expected=%s\n", int(mode),
                        writer ? int(writer->isCompiled) : -1,
                        encode(t->lookupVariableById("x")->value).c_str(), encode(c.input).c_str());
        check(encode(t->lookupVariableById("x")->value) == encode(c.input),
              "competing script installs raw string during yield");
    }
    rt.runUntilDone(20);
    check(rt.threads.empty(), "small loop completes");
    State out;
    for (Value v : {t->lookupVariableById("x")->value, t->lookupVariableById("copy")->value})
        out.push_back(encode(v));
    SList* list = t->lookupVariableById("out")->list;
    for (uint32_t i = 0; i < list->len; ++i) out.push_back(encode(list->items[i]));
    if (!yieldWriter) {
        Value expected = c.initial.bits == Value::undefined().bits
            ? Value::string(SStr::fromUtf8("001")) : c.initial;
        for (int i = 0; i < c.iterations; ++i) {
            if (c.set) expected = c.input;
            if (c.change && !c.skippedChange)
                expected = Value::numCanonical(cast::toNumber(expected) +
                    (c.dynamicDelta ? cast::toNumber(c.input) : 1.0));
        }
        if (out[0] != encode(expected))
            std::printf("numeric promotion change diagnostic mode=%d case=%s delta=%s initial=%s got=%s expected=%s\n", int(mode),
                        c.name, encode(c.input).c_str(), encode(c.initial).c_str(), out[0].c_str(), encode(expected).c_str());
        check(out[0] == encode(expected), "final variable has independently expected Value representation (numeric NaN normalized)");
        check(out[6] == encode(expected), "loop-exit Value observation matches independent expectation");
        if (c.iterations) {
            check(out[1] == encode(expected), "copy assignment observes materialized Value");
            check(out[3] == encode(expected), "list ITEM observes materialized Value");
            check(out[5] == encode(Value::numCanonical(cast::toNumber(expected) + 2.0)),
                  "numeric shadow matches materialized result, including NaN coerced back to zero");
        }
    }
    if (yieldWriter) {
        check(out[0] == encode(Value::num(41)), "reload after another script's write feeds numeric shadow");
        check(out[2] == encode(c.input), "Value read after reload preserves original string spelling");
    }
    return out;
}
std::string truthProject() {
    Graph g;
    auto compare = [&](const char* op, int rhs) {
        return g.reporter(op, "{\"OPERAND1\":" + g.var("n") + ",\"OPERAND2\":" + number(rhs) + "}");
    };
    auto yes = [&]() { return compare("operator_lt", 2); };
    auto no = [&]() { return compare("operator_gt", 2); };
    auto neg = [&](const std::string& v) { return g.reporter("operator_not", "{\"OPERAND\":" + v + "}"); };
    auto binary = [&](const char* op, const std::string& a, const std::string& b) {
        return g.reporter(op, "{\"OPERAND1\":" + a + ",\"OPERAND2\":" + b + "}");
    };
    // Numeric n is changed to 1. Comparing n avoids the unrelated existing
    // raw NaN/whitespace comparison issue in promoted mixed-type variables.
    std::vector<std::string> predicates;
    predicates.push_back(g.var("x"));
    predicates.push_back(neg(g.var("x")));
    predicates.push_back(binary("operator_and", g.var("x"), yes()));
    predicates.push_back(binary("operator_and", g.var("x"), no()));
    predicates.push_back(binary("operator_or", g.var("x"), yes()));
    predicates.push_back(binary("operator_or", g.var("x"), no()));
    predicates.push_back(neg(binary("operator_and", g.var("x"), yes())));
    predicates.push_back(neg(binary("operator_or", g.var("x"), no())));
    predicates.push_back(yes());
    predicates.push_back(no());
    predicates.push_back(compare("operator_equals", 1));
    predicates.push_back(neg(yes()));
    predicates.push_back(binary("operator_and", no(), g.var("x")));
    predicates.push_back(binary("operator_or", yes(), g.var("x")));
    predicates.push_back(binary("operator_and", g.var("x"), neg(g.var("x"))));
    predicates.push_back(binary("operator_or", g.var("x"), neg(g.var("x"))));
    g.add("hat", "event_whenflagclicked", "loop", "{}", "{}", true);
    g.add("loop", "control_repeat", "", "{\"TIMES\":" + number(1) + ",\"SUBSTACK\":" + ref("set") + "}");
    g.add("set", "data_setvariableto", "inc", "{\"VALUE\":" + g.source() + "}", field("VARIABLE", "x"));
    g.add("inc", "data_changevariableby", "cond0", "{\"VALUE\":" + number(1) + "}", field("VARIABLE", "n"));
    for (size_t i = 0; i < predicates.size(); ++i) {
        const std::string id = "cond" + std::to_string(i);
        const std::string next = i + 1 == predicates.size() ? "" : "cond" + std::to_string(i + 1);
        const std::string trueId = "yes" + std::to_string(i), falseId = "no" + std::to_string(i);
        g.add(id, "control_if_else", next, "{\"CONDITION\":" + predicates[i] +
              ",\"SUBSTACK\":" + ref(trueId) + ",\"SUBSTACK2\":" + ref(falseId) + "}");
        // Branch outcomes, rather than reporter output alone, force emitTruthyV
        // to consume every predicate and both dynamic kTrue/kFalse input tags.
        g.observe(trueId.c_str(), "", int(i + 1), number(1));
        g.observe(falseId.c_str(), "", int(i + 1), number(0));
    }
    return "{\"targets\":[{\"isStage\":true,\"name\":\"Stage\","
        "\"variables\":{\"x\":[\"x\",0],\"n\":[\"n\",0]},"
        "\"lists\":{\"source\":[\"source\",[0]],\"out\":[\"out\",[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]]},"
        "\"blocks\":{" + g.blocks + "},\"costumes\":[],\"sounds\":[]}],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
}
State truthRun(Value input, JitMode mode) {
    setJitMode(mode);
    Runtime rt;
    const bool loaded = loadSb3(rt, truthProject(), nullptr).ok;
    check(loaded && rt.targets.size() == 1, "load boolean branch fixture");
    if (!loaded || rt.targets.size() != 1) return {};
    Target* t = rt.targets[0];
    t->lookupVariableById("source")->list->items[0] = input;
    rt.greenFlag();
#ifdef XYO_JIT
    if (mode == JitMode::Eager)
        for (Thread* th : rt.threads) check(th->isCompiled, "boolean branch fixture actually compiled");
#endif
    rt.runUntilDone(20);
    check(rt.threads.empty(), "boolean branch fixture terminates");
    const bool v = cast::toBoolean(input);
    const bool expected[] = {v, !v, v, false, true, v, !v, !v,
                             true, false, true, false, false, true, false, true};
    SList* out = t->lookupVariableById("out")->list;
    check(out->len == 16, "every boolean branch has a result slot");
    State result;
    if (out->len != 16) return result;
    for (uint32_t i = 0; i < out->len; ++i) {
        if (cast::toNumber(out->items[i]) != int(expected[i]))
            std::printf("numeric promotion truthy diagnostic mode=%d input=%s predicate=%u got=%s expected=%d\n",
                        int(mode), encode(input).c_str(), i, encode(out->items[i]).c_str(), int(expected[i]));
        check(cast::toNumber(out->items[i]) == int(expected[i]), "dynamic boolean/comparison branch takes expected arm");
        result.push_back(encode(out->items[i]));
    }
    check(encode(t->lookupVariableById("x")->value) == encode(input),
          "truth conversion leaves dynamic variable Value unchanged");
    return result;
}

// for_each writes its loop variable implicitly; promotion must observe that
// write, preserve changes inside the body, and leave it alone for zero trips.
State foreachRun(const char* limit, int bodyKind, JitMode mode) {
    Graph g;
    g.add("hat", "event_whenflagclicked", "loop", "{}", "{}", true);
    const std::string limitInput = "[1,[4,\"" + std::string(limit) + "\"]]";
    g.add("loop", "control_for_each", "",
          "{\"VALUE\":" + limitInput + ",\"SUBSTACK\":" + ref("first") + "}",
          field("VARIABLE", "i"));
    std::string body = "first";
    if (bodyKind == 1) {
        g.add("first", "data_changevariableby", "sum",
              "{\"VALUE\":" + number(10) + "}", field("VARIABLE", "i"));
        body = "sum";
    } else if (bodyKind == 2) {
        g.add("first", "data_setvariableto", "sum",
              "{\"VALUE\":[1,[10,\"text\"]]}", field("VARIABLE", "i"));
        body = "sum";
    }
    if (bodyKind == 3) {
        g.add("first", "data_changevariableby", "",
              "{\"VALUE\":" + number(7) + "}", field("VARIABLE", "sum"));
    } else {
        g.add(body, "data_changevariableby", "",
              "{\"VALUE\":" + g.var("i") + "}", field("VARIABLE", "sum"));
    }
    std::string proj =
        "{\"targets\":[{\"isStage\":true,\"name\":\"Stage\","
        "\"variables\":{\"i\":[\"i\",\"raw\"],\"sum\":[\"sum\",0]},"
        "\"lists\":{},\"blocks\":{" + g.blocks + "},\"costumes\":[],\"sounds\":[]}],"
        "\"monitors\":[],\"extensions\":[],\"meta\":{}}";
    setJitMode(mode);
    Runtime rt;
    const bool loaded = loadSb3(rt, proj, nullptr).ok;
    check(loaded, "for_each fixture loads");
    if (!loaded) return {};
    Target* t = rt.targets[0];
    rt.greenFlag();
#ifdef XYO_JIT
    if (mode == JitMode::Eager)
        for (Thread* th : rt.threads)
            check(th->isCompiled, "for_each fixture is JIT-compiled");
#endif
    rt.runUntilDone(20);
    check(rt.threads.empty(), "for_each fixture terminates");
    return {encode(t->lookupVariableById("i")->value),
            encode(t->lookupVariableById("sum")->value)};
}
void foreachCases() {
    struct Test {
        const char* limit;
        int body;
        const char* finalText;  // nullptr means numeric
        double finalNumber;
        double sum;
    };
    const Test cases[] = {
        {"0",   0, "raw",  0,  0},  // zero trips: preserve initial text
        {"3.5", 0, nullptr, 4, 10}, // fractional bound: four iterations
        {"3",   1, nullptr, 13, 36},// body also changes the loop variable
        {"3",   2, "text", 0,  0},  // body writes a non-numeric string
        {"3",   3, nullptr, 3, 21}, // implicit loop write without body reading i
        {"-2",  0, "raw",  0,  0},
    };
    for (const auto& c : cases) {
        const State interp = foreachRun(c.limit, c.body, JitMode::Off);
        const State eager = foreachRun(c.limit, c.body, JitMode::Eager);
        check(interp.size() == 2 && interp == eager,
              "for_each: interpreter/eager output agree");
        if (interp.size() != 2) continue;
        if (interp[1] != encode(Value::num(c.sum)))
            std::printf("for_each sum mismatch limit=%s kind=%d got=%s expected=%s\n",
                        c.limit, c.body, interp[1].c_str(),
                        encode(Value::num(c.sum)).c_str());
        check(interp[1] == encode(Value::num(c.sum)), "for_each: expected sum");
        if (c.finalText)
            check(interp[0] == encode(Value::string(SStr::fromUtf8(c.finalText))),
                  "for_each: expected final string variable");
        else
            check(interp[0] == encode(Value::num(c.finalNumber)),
                  "for_each: expected final numeric variable");
    }
}

void truthCases() {
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<Value> inputs = {
        Value::boolean(false), Value::boolean(true), Value::num(0), Value::num(-0.0),
        Value::num(1), Value::num(-2), Value::nan(), Value::num(inf), Value::num(-inf),
        Value::emptyStr(), Value::string(SStr::fromUtf8("false")), Value::string(SStr::fromUtf8("FALSE")),
        Value::string(SStr::fromUtf8("0")), Value::string(SStr::fromUtf8("00")),
        Value::string(SStr::fromUtf8(" ")), Value::string(SStr::fromUtf8("hello"))
    };
    for (Value input : inputs) {
        const State expected = truthRun(input, JitMode::Off);
        const State got = truthRun(input, JitMode::Eager);
        check(!expected.empty() && expected == got, "dynamic truthy/not/and/or/compare branch states match interp/eager");
    }
}
Value text(const char* s) { return Value::string(SStr::fromUtf8(s)); }
} // namespace

int test_numeric_promotion_main() {
    const JitMode saved = jitMode();
    // Case inputs contain strings not yet owned by a Runtime list. Keep these
    // tiny table-driven fixtures alive across the fresh Runtime instances.
    Gc::DeferCollect defer;
    // GPU offload would bypass the CPU promoted-value lowering under test.
    EnvGuard gpu("XYO_GPU"), parallel("XYO_PAR");
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<Case> cases = {
        {"initial string unchanged", text("ignored"), false, false},
        {"initial numeric string then finite change", text("ignored")},
        {"arbitrary string set stays raw", text(" 001 "), true, false},
        {"nonnumeric string then finite change", text("hello"), true},
        {"numeric NaN set stays raw", Value::nan(), true, false},
        {"numeric NaN then finite change", Value::nan(), true},
        {"positive infinity set stays raw", Value::num(inf), true, false},
        {"negative infinity then finite change", Value::num(-inf), true},
        {"string Infinity set stays raw", text("Infinity"), true, false},
        {"string Infinity then finite change", text("Infinity"), true},
        {"false branch leaves initial string raw", text("ignored"), false, true, true},
        {"zero iterations leave string untouched", text("ignored"), false, true, false, 0},
        {"set after prior numeric change restores raw arm", text("03"), true, true, false, 2},
        // VALUE is a live list reporter, not a constant or numeric shadow.
        {"dynamic finite positive delta", Value::num(7), false, true, false, 1, true},
        {"dynamic finite negative delta", Value::num(-7), false, true, false, 1, true},
        {"dynamic positive infinity delta", Value::num(inf), false, true, false, 1, true},
        {"dynamic negative infinity delta", Value::num(-inf), false, true, false, 1, true},
        {"dynamic numeric NaN delta coerces to zero", Value::nan(), false, true, false, 1, true},
        {"dynamic true delta", Value::boolean(true), false, true, false, 1, true},
        {"dynamic false delta", Value::boolean(false), false, true, false, 1, true},
        {"dynamic numeric string delta", text("7"), false, true, false, 1, true},
        {"dynamic signed numeric string delta", text(" -7 "), false, true, false, 1, true},
        {"dynamic nonnumeric delta coerces to zero", text("hello"), false, true, false, 1, true},
        {"dynamic empty delta coerces to zero", Value::emptyStr(), false, true, false, 1, true},
        {"dynamic string infinity delta", text("Infinity"), false, true, false, 1, true},
        {"infinite current plus finite positive delta", Value::num(7), false, true, false, 1, true, Value::num(inf)},
        {"negative infinite current plus finite delta", Value::num(-7), false, true, false, 1, true, Value::num(-inf)},
        {"positive infinity plus negative infinity produces numeric NaN", Value::num(-inf), false, true, false, 1, true, Value::num(inf)},
        {"negative infinity plus positive infinity produces numeric NaN", Value::num(inf), false, true, false, 1, true, Value::num(-inf)},
        {"NaN from opposite infinity feeds following dynamic change", Value::num(-inf), false, true, false, 2, true, Value::num(inf)},
        {"numeric NaN current plus finite delta", Value::num(7), false, true, false, 1, true, Value::nan()},
        {"string current plus dynamic finite delta", Value::num(7), false, true, false, 1, true, text("004")},
        {"false branch skips dynamic infinite delta and keeps raw current", Value::num(inf), false, true, true, 1, true, text("raw")},
        {"zero iteration skips dynamic delta", Value::num(inf), false, true, false, 0, true, text("raw")}
    };
    for (const Case& c : cases) {
        const State expected = run(c, JitMode::Off);
        const State got = run(c, JitMode::Eager);
        check(!expected.empty() && got == expected, c.name);
    }
    const Case interleave{"non-warp yield reload", text("040"), false, true, false, 2};
    const State expected = run(interleave, JitMode::Off, true);
    const State got = run(interleave, JitMode::Eager, true);
    check(!expected.empty() && got == expected, "interleaved script states match interp/eager with exact Value tags");
    truthCases();
    foreachCases();
    setJitMode(saved);
    std::printf("numeric promotion: %d checks, %d failures\n", checks, failures);
    return failures;
}
