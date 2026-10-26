// Arithmetic NaN/coercion regression: exact boxed bits, not just numeric equality.
#include <cstdio>
#include <cmath>
#include <memory>
#include <limits>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/json.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"
#ifdef XYO_JIT
#include "aot/aot.hpp"
#endif

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL nan codegen: %s\n", what); }
}
// Independent evaluator: public Scratch casts at EVERY arithmetic boundary,
// never JIT helpers or JIT-generated expected results. Tree emission allocates
// fresh reporter IDs per occurrence, avoiding interpreter parentValues aliases.
struct Expr;
using E = std::shared_ptr<Expr>;
struct Expr { char op; std::string literal; E left, right; };
E variable(char which) { return std::make_shared<Expr>(Expr{which, {}, {}, {}}); }
E literal(const std::string& value) {
    // JSON numeric shadow payloads really are boxed numeric constants. A quoted
    // math_number payload remains a string in the loader, so use raw finite
    // numbers here and text shadows for the string conversion cases below.
    bool numeric = false;
    for (const char* n : {"0", "-0", "1", "2", "3", "-2", "0.5",
                          "1.7976931348623157e308", "-1.7976931348623157e308",
                          "5e-324", "-5e-324"})
        if (value == n) numeric = true;
    return std::make_shared<Expr>(Expr{numeric ? 'n' : 'l', value, {}, {}});
}
E binary(char op, E a, E b) { return std::make_shared<Expr>(Expr{op, {}, a, b}); }
E mathop(const std::string& name, E input) {
    return std::make_shared<Expr>(Expr{'m', name, input, {}});
}
Value text(const std::string& s) { return Value::string(SStr::fromUtf8(s)); }
Value arithmetic(char op, Value a, Value b) {
    const double x = cast::toNumber(a), y = cast::toNumber(b);
    switch (op) {
    case '+': return Value::numCanonical(x + y);
    case '-': return Value::numCanonical(x - y);
    case '*': return Value::numCanonical(x * y);
    default: return Value::numCanonical(x / y);
    }
}
Value evaluate(const E& e, Value x, Value y) {
    if (e->op == 'x') return x;
    if (e->op == 'y') return y;
    if (e->op == 'l') return text(e->literal);
    if (e->op == 'n') return Value::num(cast::toNumber(text(e->literal)));
    Value a = evaluate(e->left, x, y);
    if (e->op == 'm') {
        // Independent public-cast/std math reference, not xyo_mathop. Handle
        // unary nodes before touching right: every emitted reporter is a tree.
        const double n = cast::toNumber(a);
        double result;
        if (e->literal == "abs") result = std::fabs(n);
        else if (e->literal == "floor") result = std::floor(n);
        else if (e->literal == "ceiling") result = std::ceil(n);
        else result = std::sqrt(n);
        return Value::numCanonical(result);
    }
    Value b = evaluate(e->right, x, y);
    return arithmetic(e->op, a, b);
}
const std::vector<std::string>& literals() {
    static const std::vector<std::string> values = {
        "0", "-0", "1", "-2", "0.5", "1.7976931348623157e308",
        "-1.7976931348623157e308", "5e-324", "-5e-324", " 2 ", "0x10",
        "not-a-number", "NaN", "Infinity", "-Infinity", "1e999",
        // Force string shadows (not numeric-recognized spellings above),
        // including ES whitespace and BOM decoded through project JSON.
        "", " ", " \t\r\n ", "+0", "-0.0", "1.0", "1x", "1.0px",
        "\u00a0", "\ufeff", "\u00a0-0.0\ufeff", "\u20032\u2029",
        "\"1\"", "\\1", "0b11", "0o17", "true", "false", "003"
    };
    return values;
}
std::vector<E> expressions() {
    std::vector<E> out;
    for (char op : {'+', '-', '*', '/'}) {
        for (const auto& value : literals()) {
            out.push_back(binary(op, variable('x'), literal(value)));
            out.push_back(binary(op, literal(value), variable('x')));
        }
        out.push_back(binary(op, variable('x'), variable('y')));
    }
    // Boxing-only mathop regression. These exact operations avoid unrelated
    // libm/trig rounding differences. The common project harness stores each
    // result, then consumes both stored and immediate results through +7.
    // Dynamic inputs below include signed zero, infinities, NaN, strings/bools.
    for (const char* name : {"abs", "floor", "ceiling", "sqrt"}) {
        out.push_back(mathop(name, variable('x')));
        out.push_back(binary('+', mathop(name, variable('x')), literal("1")));
        out.push_back(binary('*', mathop(name, variable('x')), variable('y')));
        out.push_back(mathop(name, binary('/', variable('x'), variable('y'))));
    }
    // Domain-error NaN must be boxed canonically on storage, but coerced to
    // zero at the NEXT math/arithmetic boundary (not propagated unchecked).
    out.push_back(mathop("sqrt", literal("-1")));
    out.push_back(mathop("abs", mathop("sqrt", literal("-1"))));
    // Explicit negative fractions also exercise the signed-zero ceil result.
    out.push_back(mathop("ceiling", literal("-0.5")));
    out.push_back(mathop("floor", literal("-0.5")));
    // Non-NaN does not mean finite: overflow in either child can make a parent
    // NaN. Underflow can likewise produce an unsafe zero divisor.
    out.push_back(binary('-', binary('*', variable('x'), literal("2")),
                              binary('*', variable('x'), literal("2"))));
    out.push_back(binary('*', binary('+', variable('x'), literal("1")), literal("0")));
    out.push_back(binary('/', literal("0"), binary('*', variable('x'), literal("0.5"))));
    out.push_back(binary('/', binary('+', variable('x'), literal("1")), variable('y')));
    out.push_back(binary('+', binary('/', variable('x'), literal("2")), literal("3")));
    out.push_back(binary('*', binary('-', literal("1"), variable('x')), literal("-2")));
    // Explicit dangerous forms, independent of the selected dynamic input.
    out.push_back(binary('+', literal("Infinity"), literal("-Infinity")));
    out.push_back(binary('-', literal("Infinity"), literal("Infinity")));
    out.push_back(binary('*', literal("0"), literal("Infinity")));
    out.push_back(binary('*', literal("-0"), literal("-Infinity")));
    out.push_back(binary('/', literal("0"), literal("0")));
    out.push_back(binary('/', literal("Infinity"), literal("Infinity")));
    return out;
}
std::string blockRef(const std::string& id) { return "[2,\"" + id + "\"]"; }
std::string field(const std::string& id) { return "{\"VARIABLE\":[\"" + id + "\",\"" + id + "\"]}"; }
struct Graph {
    std::string blocks;
    int serial = 0;
    std::string fresh() { return "r" + std::to_string(serial++); }
    void add(const std::string& id, const std::string& op, const std::string& next,
             const std::string& inputs = "{}", const std::string& fields = "{}",
             const std::string& extra = "") {
        if (!blocks.empty()) blocks += ",";
        blocks += "\"" + id + "\":{\"opcode\":\"" + op + "\",\"next\":" +
            (next.empty() ? "null" : "\"" + next + "\"") + ",\"inputs\":" + inputs +
            ",\"fields\":" + fields + extra + "}";
    }
    std::string read(const std::string& var) {
        std::string id = fresh(); add(id, "data_variable", "", "{}", field(var)); return blockRef(id);
    }
    std::string emit(const E& e) {
        if (e->op == 'x' || e->op == 'y') return read(std::string(1, e->op));
        if (e->op == 'l') return "[1,[10," + jsonStringify(JsonValue(e->literal)) + "]]";
        if (e->op == 'n') return "[1,[4," + e->literal + "]]";
        if (e->op == 'm') {
            std::string input = emit(e->left), id = fresh();
            add(id, "operator_mathop", "", "{\"NUM\":" + input + "}",
                "{\"OPERATOR\":[" + jsonStringify(JsonValue(e->literal)) + ",null]}");
            return blockRef(id);
        }
        std::string a = emit(e->left), b = emit(e->right), id = fresh();
        const char* op = e->op == '+' ? "operator_add" : e->op == '-' ? "operator_subtract" :
                         e->op == '*' ? "operator_multiply" : "operator_divide";
        add(id, op, "", "{\"NUM1\":" + a + ",\"NUM2\":" + b + "}");
        return blockRef(id);
    }
    void append(const std::string& id, const std::string& next, const std::string& input) {
        add(id, "data_addtolist", next, "{\"ITEM\":" + input + "}",
            "{\"LIST\":[\"results\",\"results\"]}");
    }
    std::string plusSeven(const std::string& input) {
        std::string id = fresh();
        add(id, "operator_add", "", "{\"NUM1\":" + input + ",\"NUM2\":[1,[4,7]]}");
        return blockRef(id);
    }
};
std::string project(const std::vector<E>& exprs) {
    Graph g;
    g.add("hat", "event_whenflagclicked", "s0", "{}", "{}", ",\"topLevel\":true");
    size_t row = 0;
    for (const auto& e : exprs) {
        std::string id = "s" + std::to_string(row++), next = "s" + std::to_string(row);
        g.add(id, "data_setvariableto", id + "a", "{\"VALUE\":" + g.emit(e) + "}", field("tmp"));
        g.append(id + "a", id + "b", g.read("tmp"));
        g.append(id + "b", id + "c", g.plusSeven(g.read("tmp")));
        g.append(id + "c", next, g.plusSeven(g.emit(e)));
    }
    for (const auto& delta : literals()) {
        std::string id = "s" + std::to_string(row++), next = "s" + std::to_string(row);
        g.add(id, "data_setvariableto", id + "change", "{\"VALUE\":" + g.read("x") + "}", field("tmp"));
        g.add(id + "change", "data_changevariableby", id + "a",
              "{\"VALUE\":" + g.emit(literal(delta)) + "}", field("tmp"));
        g.append(id + "a", id + "b", g.read("tmp"));
        g.append(id + "b", next, g.plusSeven(g.read("tmp")));
    }
    // Native function fact boundaries: safe and unsafe caller SSA values feed
    // the same parameter. No caller fact may leak to the callee's argument.
    const std::string mutation = ",\"mutation\":{\"proccode\":\"consume %s\","
        "\"argumentids\":\"[\\\"arg\\\"]\",\"argumentnames\":\"[\\\"a\\\"]\","
        "\"argumentdefaults\":\"[\\\"\\\"]\",\"warp\":\"true\"}";
    for (const E& e : {binary('+', variable('x'), literal("1")),
                       binary('*', variable('x'), literal("0")),
                       binary('/', variable('x'), variable('y'))}) {
        std::string id = "s" + std::to_string(row++), next = "s" + std::to_string(row);
        g.add(id, "procedures_call", next, "{\"arg\":" + g.emit(e) + "}", "{}", mutation);
    }
    g.add("s" + std::to_string(row), "data_setvariableto", "literalOut",
          "{\"VALUE\":[1,[10,\"003\"]]}", field("kept"));
    g.add("literalOut", "data_addtolist", "doneSet", "{\"ITEM\":[1,[10,\"003\"]]}",
          "{\"LIST\":[\"literalOutputs\",\"literalOutputs\"]}");
    g.add("doneSet", "data_setvariableto", "", "{\"VALUE\":[1,[4,1]]}", field("done"));
    g.add("def", "procedures_definition", "procSet", "{\"custom_block\":[1,\"proto\"]}");
    g.add("proto", "procedures_prototype", "", "{}", "{}", ",\"shadow\":true" + mutation);
    g.add("argRead", "argument_reporter_string_number", "", "{}", "{\"VALUE\":[\"a\",null]}");
    g.add("argMath", "operator_add", "", "{\"NUM1\":" + blockRef("argRead") + ",\"NUM2\":[1,[4,1]]}");
    g.add("procSet", "data_setvariableto", "procOut", "{\"VALUE\":" + blockRef("argMath") + "}", field("tmp"));
    g.append("procOut", "", g.read("tmp"));
    return "{\"targets\":[{\"isStage\":true,\"name\":\"Stage\",\"variables\":{"
        "\"x\":[\"x\",0],\"y\":[\"y\",0],\"tmp\":[\"tmp\",0],\"done\":[\"done\",0],"
        "\"kept\":[\"kept\",0]},\"lists\":{\"results\":[\"results\",[]],"
        "\"literalOutputs\":[\"literalOutputs\",[]]},\"blocks\":{" + g.blocks +
        "},\"costumes\":[],\"sounds\":[]}],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
}
std::vector<Value> expected(const std::vector<E>& exprs, Value x, Value y) {
    std::vector<Value> out;
    const Value seven = Value::num(7);
    for (const auto& e : exprs) {
        Value v = evaluate(e, x, y);
        out.push_back(v);
        out.push_back(arithmetic('+', v, seven));
        out.push_back(arithmetic('+', v, seven));
    }
    for (const auto& delta : literals()) {
        Value v = arithmetic('+', x, text(delta));
        out.push_back(v); out.push_back(arithmetic('+', v, seven));
    }
    for (const E& e : {binary('+', variable('x'), literal("1")),
                       binary('*', variable('x'), literal("0")),
                       binary('/', variable('x'), variable('y'))})
        out.push_back(arithmetic('+', evaluate(e, x, y), Value::num(1)));
    return out;
}
struct Input { Value x, y; };
std::vector<Input> inputs() {
    const double inf = std::numeric_limits<double>::infinity();
    const double max = std::numeric_limits<double>::max();
    const double tiny = std::numeric_limits<double>::denorm_min();
    std::vector<Input> out;
    for (double n : {0.0, -0.0, 1.0, -1.0, 0.5, -2.5, max, -max, tiny, -tiny,
                     std::numeric_limits<double>::min(), inf, -inf})
        out.push_back({Value::num(n), Value::num(n)});
    out.push_back({Value::nan(), Value::nan()});
    out.push_back({Value::num(inf), Value::num(-inf)});
    out.push_back({Value::num(-inf), Value::num(inf)});
    out.push_back({Value::num(0), Value::num(inf)});
    out.push_back({Value::num(inf), Value::num(0)});
    for (const char* s : {"0", "-0", " 2.5 ", "0x10", "no-number", "", "NaN",
                          "Infinity", "-Infinity", "1e999", "5e-324"})
        out.push_back({text(s), text(s)});
    out.push_back({Value::boolean(false), Value::boolean(true)});
    out.push_back({Value::boolean(true), Value::boolean(false)});
    out.push_back({Value::undefined(), Value::undefined()});
    return out;
}
using Bits = std::vector<std::vector<uint64_t>>;
Bits run(JitMode mode, const std::vector<E>& exprs, const Bits* interp, bool aotMode = false) {
    setJitMode(mode);
    const std::string json = project(exprs);
#ifdef XYO_JIT
    constexpr uint64_t projectHash = 0x4e414e434f455243ull;
    aot::BuildResult build;
    std::string why;
    if (aotMode) {
        // Destroy the emitting Runtime before loading the object into a fresh
        // Runtime: no pinned host string/variable pointer may survive via AOT.
        Runtime source;
        const bool loaded = loadSb3(source, json, nullptr).ok;
        check(loaded, "load separate AOT source runtime");
        if (!loaded) return {};
        const bool compiled = aot::compileProject(source, projectHash, build, &why);
        if (!compiled) std::printf("nan AOT compile: %s\n", why.c_str());
        check(compiled && build.scripts > 0, "AOT compile generated arithmetic project");
        if (!compiled || build.scripts == 0) return {};
    }
#else
    (void)aotMode;
#endif
    Runtime rt;
    check(loadSb3(rt, json, nullptr).ok, "load generated arithmetic project");
#ifdef XYO_JIT
    if (aotMode) {
        const bool loaded = rt.jit && rt.jit->loadAot(rt, build.object.data(),
                                                   build.object.size(), projectHash, &why);
        if (!loaded) std::printf("nan AOT load: %s\n", why.c_str());
        check(loaded, "load arithmetic AOT into fresh runtime");
        if (!loaded) return {};
    }
#endif
    if (rt.targets.size() != 1) { check(false, "one target loaded"); return {}; }
    Target* t = rt.targets[0];
    Variable* x = t->lookupVariableById("x"), *y = t->lookupVariableById("y");
    Variable* done = t->lookupVariableById("done");
    SList* result = t->lookupVariableById("results")->list;
    Variable* kept = t->lookupVariableById("kept");
    SList* literalOutputs = t->lookupVariableById("literalOutputs")->list;
    // Root inputs across greenFlag / compilation, which can allocate/collect.
    std::vector<Input> in;
    Gc::get().addRootProvider(&in, [&in](Gc& gc) {
        for (const auto& v : in) { gc.markValue(v.x); gc.markValue(v.y); }
    });
    { Gc::DeferCollect defer; in = inputs(); }
    Bits bits;
    for (size_t i = 0; i < in.size(); ++i) {
        x->value = in[i].x; y->value = in[i].y;
        done->value = Value::num(0); result->clear();
        kept->value = Value::num(0); literalOutputs->clear();
        std::vector<Value> want;
        { Gc::DeferCollect defer; want = expected(exprs, x->value, y->value); }
        rt.greenFlag();
#ifdef XYO_JIT
        if (mode == JitMode::Eager)
            check(rt.threads.size() == 1 && rt.threads[0]->isCompiled,
                  "eager arithmetic script actually compiled, not fallback");
        if (aotMode) {
            JitEngine::ScriptStats stats;
            check(rt.jit->scriptStats(t->blocks->getBlock("hat"), &stats) &&
                  stats.compiled && stats.aot, "arithmetic script actually uses loaded AOT");
        }
#endif
        rt.runUntilDone(100);
        check(rt.threads.empty() && done->value.bits == Value::num(1).bits, "script completed");
        check(result->len == want.size(), "all arithmetic outputs present");
        auto is003 = [](Value v) {
            return v.isString() && v.asStrPtr() && v.asStrPtr()->view() == u"003";
        };
        check(is003(kept->value), "stored literal retains string type and original 003 spelling");
        check(literalOutputs->len == 1 && is003(literalOutputs->items[0]),
              "list literal retains string type and original 003 spelling");
        bits.emplace_back();
        for (size_t j = 0; j < result->len && j < want.size(); ++j) {
            Value observed = result->items[j];
            const bool numericNaN = observed.isNumber() && std::isnan(observed.asNumberUnchecked());
            // The interpreter legitimately stores the platform's negative raw
            // NaN. Normalize NaNs ONLY for differential/reference comparison;
            // finite values, infinities and both zero signs stay bit-exact.
            if (mode == JitMode::Eager && numericNaN)
                check(observed.bits == val::kNan, "compiled stored arithmetic NaN is canonical");
            uint64_t got = numericNaN ? val::kNan : observed.bits;
            bits.back().push_back(got);
            if (got != want[j].bits)
                std::printf("nan diagnostic mode=%d input=%zu output=%zu got=%016llx expected=%016llx\n",
                    int(mode), i, j, static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(want[j].bits));
            check(got == want[j].bits, "exact reference Value bits (signed zero / canonical NaN)");
        }
        if (interp) check(i < interp->size() && bits.back() == (*interp)[i],
                          "JIT output bits match actual interpreter execution");
    }
    Gc::get().removeRootProvider(&in);
    return bits;
}
} // namespace

int test_nan_codegen_main() {
    checks = failures = 0;
    const JitMode saved = jitMode();
    const auto exprs = expressions();
    const Bits interp = run(JitMode::Off, exprs, nullptr);
    run(JitMode::Eager, exprs, &interp);
#ifdef XYO_JIT
    run(JitMode::Eager, exprs, &interp, true);
#endif
    setJitMode(saved);
    std::printf("nan codegen: %d checks, %d failures\n", checks, failures);
    return failures;
}
