// Short ASCII word equality: real 3D2 uses pixel "n" and raster "none" sentinels.
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include "engine/block_utility.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL equality literal: %s\n", what);
    }
}

std::string block(const char* opcode, const std::string& inputs,
                  const std::string& fields = "{}", const char* next = nullptr) {
    return std::string("{\"opcode\":\"") + opcode + "\",\"next\":" +
        (next ? "\"" + std::string(next) + "\"" : "null") +
        ",\"inputs\":" + inputs + ",\"fields\":" + fields + "}";
}

// Each project contains both literal positions. Inline SB3 text inputs become
// text shadows, so recognition exercises the AST rather than host pointer bits.
std::string project(const std::string& literal, bool probes) {
    const std::string lit = "[1,[10,\"" + literal + "\"]]";
    std::string blocks = "\"hat\":{\"opcode\":\"event_whenflagclicked\","
        "\"topLevel\":true,\"next\":\"set1\",\"inputs\":{},\"fields\":{}},";
    blocks += "\"set1\":" + block("data_setvariableto", "{\"VALUE\":[2,\"eq1\"]}",
        "{\"VARIABLE\":[\"out1\",\"out1\"]}", "set2") + ",";
    blocks += "\"set2\":" + block("data_setvariableto", "{\"VALUE\":[2,\"eq2\"]}",
        "{\"VARIABLE\":[\"out2\",\"out2\"]}") + ",";
    blocks += "\"eq1\":" + block("operator_equals",
        "{\"OPERAND1\":[2,\"dynamic1\"],\"OPERAND2\":" + lit + "}") + ",";
    blocks += "\"eq2\":" + block("operator_equals",
        "{\"OPERAND1\":" + lit + ",\"OPERAND2\":[2,\"dynamic2\"]}");
    for (const char* id : {"dynamic1", "dynamic2"}) {
        blocks += ",\"" + std::string(id) + "\":";
        if (!probes) {
            blocks += block("data_variable", "{}", "{\"VARIABLE\":[\"x\",\"x\"]}");
        } else {
            const std::string left = std::string(id) + "L";
            const std::string right = std::string(id) + "R";
            blocks += block("operator_join", "{\"STRING1\":[2,\"" + left +
                "\"],\"STRING2\":[2,\"" + right + "\"]}");
            blocks += ",\"" + left + "\":" + block("equality_probe", "{}",
                "{\"TAG\":[\"L\",null],\"VALUE\":[\"#123\",null]}");
            blocks += ",\"" + right + "\":" + block("equality_probe", "{}",
                "{\"TAG\":[\"R\",null],\"VALUE\":[\"456\",null]}");
        }
    }
    return "{\"targets\":[{\"isStage\":true,\"name\":\"Stage\","
        "\"variables\":{\"x\":[\"x\",null],\"out1\":[\"out1\",0],\"out2\":[\"out2\",0]},"
        "\"lists\":{},\"broadcasts\":{},\"blocks\":{" + blocks + "},"
        "\"costumes\":[],\"sounds\":[],\"currentCostume\":0}],"
        "\"monitors\":[],\"extensions\":[],\"meta\":{}}";
}

void checkCompiled(Runtime& rt, JitMode mode) {
#if defined(XYO_JIT)
    if (mode == JitMode::Eager) {
        check(rt.threads.size() == 1 && rt.threads[0]->isCompiled,
              "eager script actually compiled (no silent interpreter fallback)");
    }
#else
    (void)rt; (void)mode;
#endif
}

struct RootedValue {
    Value value = Value::undefined();
    RootedValue() {
        Gc::get().addRootProvider(this, [this](Gc& gc) { gc.markValue(value); });
    }
    ~RootedValue() { Gc::get().removeRootProvider(this); }
    RootedValue(const RootedValue&) = delete;
    RootedValue& operator=(const RootedValue&) = delete;
};

void testValues(JitMode mode, const std::string& literal) {
    setJitMode(mode);
    Runtime rt;
    check(loadSb3(rt, project(literal, false), nullptr).ok, "load value project");
    if (rt.targets.empty()) return;
    Target* t = rt.targets[0];
    Variable* x = t->lookupVariableById("x");
    Variable* out1 = t->lookupVariableById("out1");
    Variable* out2 = t->lookupVariableById("out2");
    check(x && out1 && out2, "resolve test variables");
    if (!x || !out1 || !out2) return;
    const std::vector<std::u16string> strings = {
        u"n", u"N", u"k", u"K", u"s", u"S", u"i", u"I", u"z", u"Z",
        u"#123456", u"nonsense", u"N\u0307", u"n\u212a", u"", u" ",
        u"0", u"1", u"NaN", u"Infinity", u"false", u"undefined",
        u"none", u"NoNe", u"NONE", u"non", u"nonex", u"nona", u"true", u"TRUE",
        u"FALSE", u"nan", u"infinity", u"1e999", u"+Infinity", u"-Infinity",
        u"some", u"\u017fome", u"skip", u"s\u212aip", u"SkIp", u"s\u212aXX",
        u"abcdefgh", u"ABCDEFGH", u"abcdefghi", u"a\u212acdefgh",
        u"\u212a", // Kelvin sign lowercases to ASCII k: must use helper.
        u"\u017f", // Long s is NOT JS lowercased to ASCII s.
        u"\u0130", // Dotted capital I expands to i + combining dot.
        u"\u00d1", u"\u00f1", u"\U00010400", u"\U00010428",
        std::u16string(1, char16_t(0xd800)), // unpaired surrogate
        std::u16string(1, char16_t(0xdc00))
    };
    RootedValue lit;
    lit.value = Value::string(SStr::fromUtf8(literal));
    auto run = [&](Value value) {
        // Root BOTH operands before reference comparison can allocate (notably
        // numeric NaN vs literal "n", which must compare "NaN" against "n").
        x->value = value;
        const bool expected = cast::compare(x->value, lit.value) == 0;
        rt.greenFlag();
        checkCompiled(rt, mode);
        rt.runUntilDone(100);
        check(rt.threads.empty(), "script completes");
        check(out1->value.isBool() && out1->value.asBool() == expected,
              "dynamic == literal matches original comparison");
        check(out2->value.isBool() && out2->value.asBool() == expected,
              "literal == dynamic matches original comparison");
    };
    run(x->value); // Include the loader's JSON null representation.
    run(Value::string(SStr::fromUtf8(literal)));
    std::string uppercase = literal;
    for (char& c : uppercase)
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    run(Value::string(SStr::fromUtf8(uppercase)));
    for (const auto& text : strings)
        run(text.empty() ? Value::emptyStr() : Value::string(SStr::create(text)));
    for (Value value : {Value::num(0), Value::num(-0.0), Value::num(1),
                       Value::num(-1), Value::nan(),
                       Value::num(std::numeric_limits<double>::denorm_min()),
                       Value::num(-std::numeric_limits<double>::denorm_min()),
                       Value::num(std::numeric_limits<double>::min()),
                       Value::num(std::numeric_limits<double>::max()),
                       Value::num(-std::numeric_limits<double>::max()),
                       Value::num(1e-7), Value::num(1e21), Value::num(0.125),
                       Value::num(16777215),
                       Value::num(std::numeric_limits<double>::infinity()),
                       Value::num(-std::numeric_limits<double>::infinity()),
                       Value::boolean(false), Value::boolean(true),
                       Value::undefined(), Value::color(0, 1, 2)})
        run(value);
    // Scratch's Value ABI has no separate null tag; JSON null loads undefined.
}

std::string* probeOrder = nullptr;
Value probe(const ArgValues& args, BlockUtility&) {
    if (probeOrder) *probeOrder += SStr::toUtf8(args.get("TAG").asStrPtr()->view());
    return args.get("VALUE");
}

void testReporterEffects(JitMode mode, const std::string& literal) {
    setJitMode(mode);
    Runtime rt;
    rt.registerPrimitive("equality_probe", probe);
    check(loadSb3(rt, project(literal, true), nullptr).ok, "load side-effect project");
    if (rt.targets.empty()) return;
    std::string order;
    probeOrder = &order;
    rt.greenFlag();
    checkCompiled(rt, mode);
    rt.runUntilDone(100);
    probeOrder = nullptr;
    check(rt.threads.empty(), "side-effect script completes");
    check(order == "LRLR", "both reporters execute left-to-right for both literal positions");
    for (const char* id : {"out1", "out2"}) {
        Variable* out = rt.targets[0]->lookupVariableById(id);
        check(out && out->value.isBool() && !out->value.asBool(),
              "side-effect reporter result takes ASCII-prefix rejection path");
    }
}
} // namespace

int test_equality_literal_main() {
    const JitMode saved = jitMode();
    for (JitMode mode : {JitMode::Off, JitMode::Eager}) {
        // Infinity (numeric), numeric text, and >8-letter words must retain
        // generic comparison; "NaN"/true/false must handle non-heap spellings.
        for (char letter = 'a'; letter <= 'z'; ++letter)
            testValues(mode, std::string(1, letter));
        for (const char* literal : {"N", "none", "NoNe", "true", "false", "NaN",
                                    "Infinity", "infinity", "some", "skip", "1",
                                    "abcdefgh", "abcdefghi"})
            testValues(mode, literal);
        testReporterEffects(mode, "n");
        testReporterEffects(mode, "none");
    }
    setJitMode(saved);
    std::printf("equality literal: %d checks, %d failures\n", checks, failures);
    return failures;
}
