// Context-sensitive numeric text literals: facts must not unbox stored strings.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/json.hpp"
#include "core/rng.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "engine/block_utility.hpp"
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
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL literal context: %s\n", message); }
}
struct EnvGuard {
    std::string name, previous;
    bool existed;
    EnvGuard(const char* key, const char* value) : name(key), existed(std::getenv(key) != nullptr) {
        if (existed) previous = std::getenv(key);
        put(value);
    }
    void put(const char* value) {
#ifdef _WIN32
        _putenv_s(name.c_str(), value ? value : "");
#else
        if (value) setenv(name.c_str(), value, 1); else unsetenv(name.c_str());
#endif
    }
    ~EnvGuard() { put(existed ? previous.c_str() : nullptr); }
};
const std::vector<std::string>& literals() {
    static const std::vector<std::string> values = {
        "", " ", "\n", " \r\n ", "\t", " \t\n ", "\u00a0", "\u2003", "\ufeff",
        "\u00a02\ufeff", "\u20282\u2029", "0", "+0", "-0", "-0.0", "1e-999", "-1e-999",
        "1", "01", " 2 ", "2.9", "3.9", ".5", "-0.5", "-2", "4", "0x2", "0b11", "0o3",
        "Infinity", "+Infinity", "-Infinity", "infinity", "INFINITY", "inf", "Inf",
        "1e999", "-1e999", "1e30", "4294967296", "NaN", "nan", "not-a-number", "2x",
        "last", "LAST", "random", "Random", "any", "ANY", "all", "All"
    };
    return values;
}
Value text(const std::string& s) { return s.empty() ? Value::emptyStr() : Value::string(SStr::fromUtf8(s)); }
std::string quoted(const std::string& s) { return jsonStringify(JsonValue(s)); }
std::string lit(const std::string& s) { return "[1,[10," + quoted(s) + "]]"; }
std::string number(int n) { return "[1,[4," + std::to_string(n) + "]]"; }
std::string blockRef(const std::string& s) { return "[2," + quoted(s) + "]"; }
std::string field(const char* kind, const std::string& s) {
    return "{\"" + std::string(kind) + "\":[" + quoted(s) + "," + quoted(s) + "]}";
}
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
struct Graph {
    std::string blocks;
    int serial = 0, statement = 0;
    std::string fresh() { return "r" + std::to_string(serial++); }
    void add(const std::string& id, const std::string& op, const std::string& next,
             const std::string& inputs = "{}", const std::string& fields = "{}", const std::string& extra = "") {
        if (!blocks.empty()) blocks += ",";
        blocks += quoted(id) + ":{\"opcode\":" + quoted(op) + ",\"next\":" +
            (next.empty() ? "null" : quoted(next)) + ",\"inputs\":" + inputs + ",\"fields\":" + fields + extra + "}";
    }
    void stack(const std::string& op, const std::string& inputs, const std::string& fields) {
        std::string id = "s" + std::to_string(statement++);
        add(id, op, "s" + std::to_string(statement), inputs, fields);
    }
    std::string reporter(const std::string& op, const std::string& inputs, const std::string& fields = "{}") {
        std::string id = fresh(); add(id, op, "", inputs, fields); return blockRef(id);
    }
    std::string readX() { return reporter("data_variable", "{}", field("VARIABLE", "x")); }
    std::string item(const std::string& list, const std::string& index) {
        return reporter("data_itemoflist", "{\"INDEX\":" + index + "}", field("LIST", list));
    }
    void append(const std::string& list, const std::string& item) {
        stack("data_addtolist", "{\"ITEM\":" + item + "}", field("LIST", list));
    }
    std::string probe(const std::string& tag, const std::string& value) {
        return reporter("literal_probe", "{}", "{\"TAG\":[" + quoted(tag) + ",null],\"VALUE\":[" + quoted(value) + ",null]}");
    }
    std::string dynamicItem() {
        std::string a = probe("A", "A"), b = probe("B", "B");
        return reporter("operator_join", "{\"STRING1\":" + a + ",\"STRING2\":" + b + "}");
    }
};
std::string project() {
    Graph g;
    g.add("hat", "event_whenflagclicked", "s0", "{}", "{}", ",\"topLevel\":true");
    std::string lists = "\"cmp\":[\"cmp\",[]],\"idx\":[\"idx\",[]],\"kept\":[\"kept\",[]]";
    for (size_t i = 0; i < literals().size(); ++i) {
        const auto& s = literals()[i];
        for (const char* op : {"operator_equals", "operator_lt", "operator_gt"}) {
            for (int shape = 0; shape < 4; ++shape) {
                std::string a = shape == 0 ? g.readX() : lit(s);
                std::string b = shape == 1 ? g.readX() : shape == 0 ? lit(s) : shape == 2 ? lit("0") : lit(lower(s));
                g.append("cmp", g.reporter(op, "{\"OPERAND1\":" + a + ",\"OPERAND2\":" + b + "}"));
            }
        }
        g.append("kept", lit(s));
        for (bool empty : {false, true}) {
            const std::string name = "l" + std::to_string(i) + (empty ? "e" : "n");
            lists += "," + quoted(name) + ":[" + quoted(name) + (empty ? ",[]]" : ",[10,20,30]]");
            g.append("idx", g.item(name, lit(s)));
            g.stack("data_replaceitemoflist", "{\"INDEX\":" + lit(s) + ",\"ITEM\":" + g.dynamicItem() + "}", field("LIST", name));
            for (int j = 1; j <= 3; ++j) g.append("idx", g.item(name, number(j)));
            g.append("idx", g.reporter("data_lengthoflist", "{}", field("LIST", name)));
        }
    }
    // Additional dynamic INDEX checks INDEX-before-ITEM and nested ITEM order.
    std::string index = g.probe("I", "2"), item = g.dynamicItem();
    g.stack("data_replaceitemoflist", "{\"INDEX\":" + index + ",\"ITEM\":" + item + "}", field("LIST", "order"));
    lists += ",\"order\":[\"order\",[10,20,30]]";
    g.append("idx", g.item("order", number(2)));
    for (const auto& pair : {std::pair<std::string, std::string>{"Infinity", "infinity"}, {" Infinity ", "infinity"}, {"\t", "0"}})
        g.append("cmp", g.reporter("operator_equals", "{\"OPERAND1\":" + lit(pair.first) + ",\"OPERAND2\":" + lit(pair.second) + "}"));
    g.add("s" + std::to_string(g.statement), "data_setvariableto", "", "{\"VALUE\":" + number(1) + "}", field("VARIABLE", "done"));
    return "{\"targets\":[{\"isStage\":true,\"name\":\"Stage\",\"variables\":{\"x\":[\"x\",0],\"done\":[\"done\",0]},\"lists\":{" + lists +
        "},\"blocks\":{" + g.blocks + "},\"costumes\":[],\"sounds\":[]}],\"monitors\":[],\"extensions\":[],\"meta\":{}}";
}
std::string* probeOrder = nullptr;
Value probe(const ArgValues& args, BlockUtility&) {
    if (probeOrder) *probeOrder += SStr::toUtf8(cast::toString(args.get("TAG")));
    return args.get("VALUE");
}
// Stable typed representation: pointers differ across fresh Runtimes, but the
// boxed string tag and original spelling must agree, not just numeric coercion.
std::string encode(Value v) {
    if (v.isString()) return "s:" + SStr::toUtf8(cast::toString(v));
    char bits[32]; std::snprintf(bits, sizeof(bits), "b:%016llx", static_cast<unsigned long long>(v.bits));
    return bits;
}
using Rows = std::vector<std::vector<std::string>>;
struct Result { Rows cmp, idx; std::vector<std::vector<uint32_t>> rng; };
Result run(JitMode mode, bool useAot = false) {
    EnvGuard parallel("XYO_PAR", "0");
    setJitMode(mode);
    const std::string json = project();
#ifdef XYO_JIT
    constexpr uint64_t hash = 0x4c4954434f4e5445ull;
    aot::BuildResult build;
    std::string why;
    if (useAot) {
        Runtime source;
        source.registerPrimitive("literal_probe", probe);
        bool ok = loadSb3(source, json, nullptr).ok;
        check(ok, "load AOT source");
        if (!ok) return {};
        ok = aot::compileProject(source, hash, build, &why);
        if (!ok) std::printf("literal AOT compile: %s\n", why.c_str());
        check(ok && build.scripts > 0, "compile combined AOT project");
        if (!ok || !build.scripts) return {};
    } // Source Runtime destroyed before object binding.
#else
    (void)useAot;
#endif
    Runtime rt;
    rt.registerPrimitive("literal_probe", probe);
    bool loaded = loadSb3(rt, json, nullptr).ok;
    check(loaded && rt.targets.size() == 1, "load combined literal context project");
    if (!loaded || rt.targets.size() != 1) return {};
#ifdef XYO_JIT
    if (useAot) {
        bool ok = rt.jit && rt.jit->loadAot(rt, build.object.data(), build.object.size(), hash, &why);
        if (!ok) std::printf("literal AOT load: %s\n", why.c_str());
        check(ok, "load AOT in fresh Runtime");
        if (!ok) return {};
    }
#endif
    Target* t = rt.targets[0];
    auto list = [t](const std::string& name) { return t->lookupVariableById(name)->list; };
    // Compare exact boxed bits against this Runtime's loaded shadow payload,
    // rather than comparing pointer bits between separately loaded projects.
    std::vector<uint64_t> literalBits;
    const Block* cursor = t->blocks->getBlock("hat");
    while (cursor) {
        auto f = cursor->fields.find("LIST");
        if (cursor->opcode == "data_addtolist" && f != cursor->fields.end() && f->second.id == "kept") {
            const BlockInput* input = cursor->inputs.find("ITEM");
            const Block* shadow = input ? t->blocks->getBlock(input->block.empty() ? input->shadow : input->block) : nullptr;
            check(shadow && shadow->opcode == "text" && !shadow->fields.empty(), "stored literals originate in actual text shadow inputs");
            literalBits.push_back(shadow && !shadow->fields.empty() ? shadow->fields.begin()->second.value.bits : 0);
        }
        cursor = cursor->next.empty() ? nullptr : t->blocks->getBlock(cursor->next);
    }
    check(literalBits.size() == literals().size(), "all storage shadow payload bits located");
    std::vector<Value> inputs;
    Gc::get().addRootProvider(&inputs, [&inputs](Gc& gc) { for (Value v : inputs) gc.markValue(v); });
    {
        Gc::DeferCollect defer;
        for (const auto& s : literals()) inputs.push_back(text(s));
        for (double n : {0.0, -0.0, 1.0, 2.9, -2.0, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
            inputs.push_back(Value::num(n));
        inputs.push_back(Value::nan()); inputs.push_back(Value::boolean(false)); inputs.push_back(Value::boolean(true));
        inputs.push_back(Value::undefined());
    }
    Result result;
    for (size_t row = 0; row < inputs.size(); ++row) {
        t->lookupVariableById("x")->value = inputs[row];
        t->lookupVariableById("done")->value = Value::num(0);
        for (const char* name : {"cmp", "idx", "kept"}) list(name)->clear();
        for (size_t i = 0; i < literals().size(); ++i) for (bool empty : {false, true}) {
            SList* l = list("l" + std::to_string(i) + (empty ? "e" : "n"));
            l->clear();
            if (!empty) for (int n : {10, 20, 30}) l->push(Value::num(n));
        }
        list("order")->clear(); for (int n : {10, 20, 30}) list("order")->push(Value::num(n));
        std::string order;
        probeOrder = &order;
        Rng::global().seed(0x12345678 + row);
        rt.greenFlag();
#ifdef XYO_JIT
        if (mode == JitMode::Eager) check(rt.threads.size() == 1 && rt.threads[0]->isCompiled, "eager actually compiled (no interpreter fallback)");
        if (useAot) {
            JitEngine::ScriptStats stats;
            check(rt.jit->scriptStats(t->blocks->getBlock("hat"), &stats) && stats.compiled && stats.aot, "script actually uses loaded AOT");
        }
#endif
        rt.runUntilDone(100);
        probeOrder = nullptr;
        check(rt.threads.empty() && cast::toNumber(t->lookupVariableById("done")->value) == 1, "script completes");
        std::string wantedOrder;
        for (size_t i = 0; i < literals().size() * 2; ++i) wantedOrder += "AB";
        check(order == wantedOrder + "IAB", "ITEM side effects execute even for invalid/empty indices; INDEX then ITEM left-to-right");
        result.rng.emplace_back();
        for (int j = 0; j < 8; ++j) result.rng.back().push_back(Rng::global().next());
        result.cmp.emplace_back(); result.idx.emplace_back();
        for (uint32_t j = 0; j < list("cmp")->len; ++j) result.cmp.back().push_back(encode(list("cmp")->items[j]));
        for (uint32_t j = 0; j < list("idx")->len; ++j) result.idx.back().push_back(encode(list("idx")->items[j]));
        check(list("kept")->len == literals().size(), "all original literals stored");
        for (size_t j = 0; j < literals().size() && j < list("kept")->len; ++j) {
            const Value stored = list("kept")->items[j];
            check(stored.isString() && encode(stored) == "s:" + literals()[j],
                  "literal storage preserves boxed string type and original spelling");
            // AOT binds freshly allocated StrLit slots, not the loader's shadow
            // pointer; identity is only a contract for interpreter/eager JIT.
            if (!useAot)
                check(j < literalBits.size() && stored.bits == literalBits[j],
                      "interpreter/eager literal storage preserves exact boxed bits");
        }
        size_t at = 0;
        Gc::DeferCollect defer;
        for (const auto& s : literals()) for (int op = 0; op < 3; ++op) for (int shape = 0; shape < 4; ++shape) {
            Value a = shape == 0 ? inputs[row] : text(s);
            Value b = shape == 1 ? inputs[row] : shape == 0 ? text(s) : text(shape == 2 ? "0" : lower(s));
            double c = cast::compare(a, b);
            bool want = op == 0 ? c == 0 : op == 1 ? c < 0 : c > 0;
            bool ok = at < list("cmp")->len && list("cmp")->items[at].isBool() && list("cmp")->items[at].asBool() == want;
            if (!ok) std::printf("literal compare diagnostic mode=%d input=%zu literal=%s op=%d shape=%d output=%zu\n", int(mode), row, quoted(s).c_str(), op, shape, at);
            check(ok, "all compare ops and both operand orders match public cast");
            ++at;
        }
        check(list("cmp")->len == at + 3, "all comparison outputs present");
        if (list("cmp")->len == at + 3) {
            check(list("cmp")->items[at].asBool(), "Infinity vs infinity falls back using original string");
            check(!list("cmp")->items[at + 1].asBool(), "padded Infinity vs infinity is not equal");
            check(list("cmp")->items[at + 2].asBool(), "TAB is actually zero for comparison");
        }
        check(list("idx")->len == literals().size() * 10 + 1, "all list read/replace output rows present");
        for (size_t i = 0; i < literals().size(); ++i) for (int empty = 0; empty < 2; ++empty) {
            const size_t base = i * 10 + empty * 5;
            if (base + 4 >= list("idx")->len) continue;
            check(cast::toNumber(list("idx")->items[base + 4]) == (empty ? 0 : 3), "special/invalid replacement preserves list length");
            const double n = cast::toNumberOrNaN(text(literals()[i]));
            // Ordered bounds before conversion: never cast huge/Inf to an int.
            // Interpreter huge integer conversion is not a portable oracle.
            if (!std::isfinite(n) || std::fabs(n) > 2147483647.0) continue;
            int index = !empty && n >= 1 && n < 4 ? int(n) : 0;
            check(encode(list("idx")->items[base]) == (index ? encode(Value::num(index * 10)) : "s:"), "numeric text read uses safe ordered bounds and truncation");
            for (int j = 1; j <= 3; ++j)
                check(encode(list("idx")->items[base + j]) == (empty ? "s:" : index == j ? "s:AB" : encode(Value::num(j * 10))), "numeric text replacement has exact bounds/no-op behavior");
        }
    }
    Gc::get().removeRootProvider(&inputs);
    return result;
}
} // namespace

int test_literal_context_main() {
    checks = failures = 0;
    const JitMode saved = jitMode();
    const Result interp = run(JitMode::Off);
    const Result optimized = run(JitMode::Eager);
    check(optimized.cmp == interp.cmp, "actual eager/interpreter comparisons agree");
    check(optimized.rng == interp.rng, "special indices consume exactly the interpreter RNG sequence");
    check(optimized.idx.size() == interp.idx.size(), "same number of interpreter/JIT output runs");
    for (size_t row = 0; row < optimized.idx.size() && row < interp.idx.size(); ++row) {
        for (size_t i = 0; i < literals().size(); ++i) {
            Gc::DeferCollect defer;
            const double n = cast::toNumberOrNaN(text(literals()[i]));
            const bool special = literals()[i] == "last" || literals()[i] == "random" || literals()[i] == "any";
            if (!special && (!std::isfinite(n) || std::fabs(n) > 2147483647.0)) continue;
            for (size_t j = 0; j < 10; ++j) {
                size_t at = i * 10 + j;
                check(at < optimized.idx[row].size() && at < interp.idx[row].size() && optimized.idx[row][at] == interp.idx[row][at], "safe numeric text list outputs agree with interpreter");
            }
        }
    }
#ifdef XYO_JIT
    const Result aot = run(JitMode::Eager, true);
    check(aot.cmp == optimized.cmp && aot.idx == optimized.idx && aot.rng == optimized.rng, "fresh Runtime AOT matches eager outputs and subsequent RNG");
#endif
    setJitMode(saved);
    std::printf("literal context: %d checks, %d failures\n", checks, failures);
    return failures;
}
