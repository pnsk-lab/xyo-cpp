// List bounds lowering: ordered FP guards, truncation, and same-class aliases.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL list codegen bounds: %s\n", what); }
}
struct Case { Value index; int expected; }; // -1 invalid, 0 random, 1..3 valid
Value text(const char* s) { return Value::string(SStr::fromUtf8(s)); }
std::vector<Case> cases() {
    std::vector<Case> out;
    auto num = [&](double n, int expected) { out.push_back({Value::numCanonical(n), expected}); };
    const double inf = std::numeric_limits<double>::infinity();
    num(0.0, -1); num(-0.0, -1); num(0.9, -1); num(-0.9, -1);
    num(-1, -1); num(1, 1); num(1.9, 1); num(2, 2); num(2.9, 2);
    num(3, 3); num(3.9, 3); num(std::nextafter(4.0, 0.0), 3);
    num(4, -1); num(std::nextafter(1.0, 0.0), -1);
    num(std::numeric_limits<double>::quiet_NaN(), -1);
    num(inf, -1); num(-inf, -1);
    num(std::numeric_limits<double>::max(), -1);
    num(-std::numeric_limits<double>::max(), -1);
    num(1e30, -1); num(-1e30, -1);
    num(2147483648.0, -1); num(4294967296.0, -1);
    const struct { const char* input; int expected; } strings[] = {
        {"last", 3}, {"random", 0}, {"any", 0}, {"1", 1}, {"2.9", 2},
        {"3.9", 3}, {"03", 3}, {" 3 ", 3}, {"0x3", 3}, {"4", -1},
        {"0", -1}, {"-0", -1}, {"-1", -1}, {"NaN", -1},
        {"Infinity", -1}, {"-Infinity", -1}, {"1e999", -1},
        {"1e30", -1}, {"", -1}, {"no-index", -1}
    };
    for (auto c : strings) out.push_back({text(c.input), c.expected});
    return out;
}
std::string number(int n) { return "[1,[4,\"" + std::to_string(n) + "\"]]"; }
std::string blockRef(const std::string& id) { return "[2,\"" + id + "\"]"; }
std::string field(const char* kind, const std::string& name, const std::string& id) {
    return "{\"" + std::string(kind) + "\":[\"" + name + "\",\"" + id + "\"]}";
}
struct Graph {
    std::string blocks;
    void add(const std::string& id, const std::string& op, const std::string& next = "",
             const std::string& inputs = "{}", const std::string& fields = "{}",
             const std::string& extra = "") {
        if (!blocks.empty()) blocks += ",";
        blocks += "\"" + id + "\":{\"opcode\":\"" + op + "\",\"next\":" +
            (next.empty() ? "null" : "\"" + next + "\"") + ",\"inputs\":" + inputs +
            ",\"fields\":" + fields + extra + "}";
    }
    void item(const std::string& id, const std::string& listId, const std::string& index) {
        add(id, "data_itemoflist", "", "{\"INDEX\":" + index + "}", field("LIST", "items", listId));
    }
    void append(const std::string& id, const std::string& next, const std::string& item) {
        add(id, "data_addtolist", next, "{\"ITEM\":" + item + "}", field("LIST", "results", "results"));
    }
};
std::string project(int count, int arithmeticIndex = 0) {
    Graph g;
    g.add("hat", "event_whenflagclicked", "call", "{}", "{}", ",\"topLevel\":true");
    const std::string mutation = ",\"mutation\":{\"proccode\":\"test\",\"argumentids\":\"[]\","
        "\"argumentnames\":\"[]\",\"argumentdefaults\":\"[]\",\"warp\":\"true\"}";
    g.add("call", "procedures_call", "", "{}", "{}", mutation);
    g.add("def", "procedures_definition", "loop", "{\"custom_block\":[1,\"proto\"]}");
    g.add("proto", "procedures_prototype", "", "{}", "{}", ",\"shadow\":true" + mutation);
    g.add("loop", "control_for_each", "", "{\"VALUE\":" + number(count) +
          ",\"SUBSTACK\":" + blockRef("reset1") + "}", field("VARIABLE", "k", "k"));
    // Each reporter occurrence has exactly one parent. Sharing a reporter ID
    // would overwrite BlockCached::parentValues in the interpreter cache.
    for (const char* id : {"readKScalar", "readKBefore", "readKReplace", "readKMarker"})
        g.add(id, "data_variable", "", "{}", field("VARIABLE", "k", "k"));
    g.add("readIndexBefore", "data_itemoflist", "", "{\"INDEX\":" + blockRef("readKBefore") + "}",
          field("LIST", "indices", "indices"));
    g.add("readIndexReplace", "data_itemoflist", "", "{\"INDEX\":" + blockRef("readKReplace") + "}",
          field("LIST", "indices", "indices"));
    for (int i = 1; i <= 3; ++i)
        g.add("reset" + std::to_string(i), "data_replaceitemoflist",
              i == 3 ? "scalarSet" : "reset" + std::to_string(i + 1),
              "{\"INDEX\":" + number(i) + ",\"ITEM\":" + number(i * 10) + "}",
              field("LIST", "items", "items"));
    // Missing IDs resolve by name to the very same scalar/list Variable*. The
    // optimizer must not attach per-ID noalias assumptions to either access.
    g.add("scalarSet", "data_setvariableto", "scalarChange", "{\"VALUE\":" + blockRef("readKScalar") + "}",
          field("VARIABLE", "x", "x"));
    g.add("scalarChange", "data_changevariableby", "before", "{\"VALUE\":" + number(1) + "}",
          field("VARIABLE", "x", "missingScalar"));
    std::string beforeIndex = "readIndexBefore", replaceIndex = "readIndexReplace";
    if (arithmeticIndex) {
        // Distinct reporter nodes for each parent. Math results exercise the
        // same numeric index fast path as 3D2's floor-based texture lookup.
        if (arithmeticIndex >= 3) {
            const char* name = arithmeticIndex == 3 ? "floor" :
                               arithmeticIndex == 4 ? "ceiling" : "sqrt";
            g.add("mathBefore", "operator_mathop", "", "{\"NUM\":" + blockRef(beforeIndex) + "}",
                  field("OPERATOR", name, ""));
            g.add("mathReplace", "operator_mathop", "", "{\"NUM\":" + blockRef(replaceIndex) + "}",
                  field("OPERATOR", name, ""));
            beforeIndex = "mathBefore";
            replaceIndex = "mathReplace";
        }
        // Mode 6 adds one after sqrt: a domain NaN is coerced to zero at
        // this arithmetic boundary, so the resulting index must be one.
        if (arithmeticIndex < 3 || arithmeticIndex == 6) {
            const char* op = arithmeticIndex == 2 ? "operator_multiply" : "operator_add";
            const auto operand = number(arithmeticIndex == 6 ? 1 : 0);
            g.add("arithmeticBefore", op, "", "{\"NUM1\":" + blockRef(beforeIndex) +
                  ",\"NUM2\":" + operand + "}");
            g.add("arithmeticReplace", op, "", "{\"NUM1\":" + blockRef(replaceIndex) +
                  ",\"NUM2\":" + operand + "}");
            beforeIndex = "arithmeticBefore";
            replaceIndex = "arithmeticReplace";
        }
    }
    g.item("beforeItem", "missingList", blockRef(beforeIndex));
    g.append("before", "replace", blockRef("beforeItem"));
    g.add("marker", "operator_add", "", "{\"NUM1\":" + number(100) + ",\"NUM2\":" + blockRef("readKMarker") + "}");
    g.add("replace", "data_replaceitemoflist", "after1",
          "{\"INDEX\":" + blockRef(replaceIndex) + ",\"ITEM\":" + blockRef("marker") + "}",
          field("LIST", "items", "missingList"));
    for (int i = 1; i <= 3; ++i) {
        g.item("afterItem" + std::to_string(i), i == 2 ? "missingList" : "items", number(i));
        g.append("after" + std::to_string(i), i == 3 ? "scalarOut" : "after" + std::to_string(i + 1),
                 blockRef("afterItem" + std::to_string(i)));
    }
    g.add("readScalar", "data_variable", "", "{}", field("VARIABLE", "x", "x"));
    g.append("scalarOut", "", blockRef("readScalar"));
    return "{\"targets\":[{\"isStage\":true,\"name\":\"Stage\",\"variables\":{},\"lists\":{},"
        "\"blocks\":{},\"costumes\":[],\"sounds\":[]},{\"isStage\":false,\"name\":\"Test\","
        "\"variables\":{\"k\":[\"k\",0],\"x\":[\"x\",0]},"
        "\"lists\":{\"items\":[\"items\",[10,20,30]],\"indices\":[\"indices\",[]],"
        "\"results\":[\"results\",[]]},\"blocks\":{" + g.blocks +
        "},\"costumes\":[],\"sounds\":[]}],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
}
void run(JitMode mode, int arithmeticIndex = 0) {
    setJitMode(mode);
    Runtime rt;
    // Runtime owns/roots targets and indices before any subsequent allocation.
    check(loadSb3(rt, project(43, arithmeticIndex), nullptr).ok, "load bounds fixture");
    if (rt.targets.size() != 2) { check(false, "loaded test target"); return; }
    Target* t = rt.targets[1];
    SList* indices = t->lookupVariableById("indices")->list;
    std::vector<Case> inputs;
    {
        // The temporary vector is not a GC root until its strings enter indices.
        Gc::DeferCollect defer;
        inputs = cases();
        for (auto c : inputs) indices->push(c.index);
    }
    check(inputs.size() == 43, "fixture iteration count matches cases");
    rt.greenFlag();
#ifdef XYO_JIT
    if (mode == JitMode::Eager)
        for (Thread* th : rt.threads) check(th->isCompiled, "eager test really compiled");
#endif
    rt.runUntilDone(100);
    check(rt.threads.empty(), "bounds script completed");
    SList* results = t->lookupVariableById("results")->list;
    check(results->len == inputs.size() * 5, "all result rows present");
    if (results->len != inputs.size() * 5) return;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const Value* row = results->items + i * 5;
        int expected = inputs[i].expected;
        if (arithmeticIndex) {
            const double value = cast::toNumber(inputs[i].index);
            double index;
            if (arithmeticIndex == 1) index = value + 0.0;
            else if (arithmeticIndex == 2) index = value * 0.0;
            else if (arithmeticIndex == 3) index = std::floor(value);
            else if (arithmeticIndex == 4) index = std::ceil(value);
            else {
                index = std::sqrt(value);
                if (arithmeticIndex == 6) index = (std::isnan(index) ? 0.0 : index) + 1.0;
            }
            expected = index >= 1.0 && index < 4.0 ? int(index) : -1;
        }
        if (expected < 0)
            check(row[0].isString() && cast::toString(row[0])->view().empty(), "invalid get returns empty string");
        else if (expected == 0) {
            double n = cast::toNumber(row[0]);
            check(n == 10 || n == 20 || n == 30, "random get returns an existing item");
        } else check(cast::toNumber(row[0]) == expected * 10, "get truncates valid fractional index");
        int mutations = 0;
        for (int j = 1; j <= 3; ++j) {
            double n = cast::toNumber(row[j]);
            double marker = 101 + double(i);
            if (expected == 0) {
                check(n == j * 10 || n == marker, "random replacement only writes marker");
                if (n == marker) ++mutations;
            } else check(n == (expected == j ? marker : j * 10), "replace has exact bounds/no-op semantics");
        }
        if (expected == 0) check(mutations == 1, "random replace modifies exactly one item");
        const double scalar = cast::toNumber(row[4]);
        if (scalar != double(i) + 2)
            std::printf("list bounds diagnostic mode=%d case=%zu scalar=%.17g expected=%.17g\n",
                        int(mode), i, scalar, double(i) + 2);
        check(scalar == double(i) + 2, "scalar alias write visible after list accesses");
    }
}
} // namespace

int test_list_codegen_bounds_main() {
    JitMode saved = jitMode();
    for (int arithmeticIndex = 0; arithmeticIndex < 7; ++arithmeticIndex) {
        run(JitMode::Off, arithmeticIndex);
        run(JitMode::Eager, arithmeticIndex);
    }
    setJitMode(saved);
    std::printf("list codegen bounds: %d checks, %d failures\n", checks, failures);
    return failures;
}
