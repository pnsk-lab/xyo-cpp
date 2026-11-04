// JIT type feedback must never change Scratch's dynamically typed semantics.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

#ifdef XYO_JIT
extern "C" int32_t xyo_eq(uint64_t a, uint64_t b);
extern "C" int32_t xyo_lt(uint64_t a, uint64_t b);
extern "C" int32_t xyo_gt(uint64_t a, uint64_t b);
#endif

namespace {
using namespace xyo;

int failures = 0;
int checks = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL type feedback: %s\n", description);
    }
}

struct EnvGuard {
    std::string previous;
    bool existed = std::getenv("XYO_JIT_TYPE_PROFILE") != nullptr;
    EnvGuard() {
        if (existed) previous = std::getenv("XYO_JIT_TYPE_PROFILE");
    }
    void set(bool enabled) {
#ifdef _WIN32
        _putenv_s("XYO_JIT_TYPE_PROFILE", enabled ? "1" : "0");
#else
        setenv("XYO_JIT_TYPE_PROFILE", enabled ? "1" : "0", 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv_s("XYO_JIT_TYPE_PROFILE", existed ? previous.c_str() : "");
#else
        if (existed) setenv("XYO_JIT_TYPE_PROFILE", previous.c_str(), 1);
        else unsetenv("XYO_JIT_TYPE_PROFILE");
#endif
    }
};

constexpr const char* kProject = R"JSON({
 "targets": [{
  "isStage": true, "name": "Stage",
  "variables": {
   "x": ["x", 0], "raw": ["raw", 0],
   "sum": ["sum", 0], "less": ["less", 0],
   "difference": ["difference", 0], "product": ["product", 0],
   "quotient": ["quotient", 0], "zero_product": ["zero_product", 0],
   "zero_quotient": ["zero_quotient", 0], "mod_then_add": ["mod_then_add", 0],
   "mod_self": ["mod_self", 0], "mod_zero": ["mod_zero", 0],
   "rounded": ["rounded", 0],
   "round_mod": ["round_mod", 0],
   "round_plus_mod": ["round_plus_mod", 0],
   "math_abs": ["math_abs", 0], "math_floor": ["math_floor", 0],
   "math_ceil": ["math_ceil", 0], "math_atan": ["math_atan", 0],
   "math_exp": ["math_exp", 0], "math_pow10": ["math_pow10", 0],
   "truth_numeric": ["truth_numeric", 0],
   "truth_dynamic": ["truth_dynamic", 0],
   "eq_left_number": ["eq_left_number", 0],
   "eq_right_number": ["eq_right_number", 0],
   "eq_left_string": ["eq_left_string", 0],
   "eq_right_string": ["eq_right_string", 0],
   "eq_pair_dynamic": ["eq_pair_dynamic", 0]
  },
  "lists": {},
  "blocks": {
   "hat": {"opcode": "event_whenflagclicked", "next": "save",
           "topLevel": true, "inputs": {}, "fields": {}},
   "save": {"opcode": "data_setvariableto", "next": "compute",
            "inputs": {"VALUE": [2, "read_raw"]},
            "fields": {"VARIABLE": ["raw", "raw"]}},
   "read_raw": {"opcode": "data_variable", "next": null,
                "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "compute": {"opcode": "data_setvariableto", "next": "difference_compute",
               "inputs": {"VALUE": [2, "add"]},
               "fields": {"VARIABLE": ["sum", "sum"]}},
   "difference_compute": {"opcode": "data_setvariableto", "next": "product_compute",
               "inputs": {"VALUE": [2, "subtract"]},
               "fields": {"VARIABLE": ["difference", "difference"]}},
   "subtract": {"opcode": "operator_subtract", "next": null,
               "inputs": {"NUM1": [1, [4, "2"]], "NUM2": [2, "sub_read"]},
               "fields": {}},
   "sub_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "product_compute": {"opcode": "data_setvariableto", "next": "quotient_compute",
               "inputs": {"VALUE": [2, "multiply"]},
               "fields": {"VARIABLE": ["product", "product"]}},
   "multiply": {"opcode": "operator_multiply", "next": null,
               "inputs": {"NUM1": [2, "mul_read"], "NUM2": [1, [4, "2"]]},
               "fields": {}},
   "mul_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "quotient_compute": {"opcode": "data_setvariableto", "next": "zero_product_compute",
               "inputs": {"VALUE": [2, "divide"]},
               "fields": {"VARIABLE": ["quotient", "quotient"]}},
   "divide": {"opcode": "operator_divide", "next": null,
               "inputs": {"NUM1": [2, "div_read"], "NUM2": [1, [4, "2"]]},
               "fields": {}},
   "div_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "zero_product_compute": {"opcode": "data_setvariableto", "next": "zero_quotient_compute",
               "inputs": {"VALUE": [2, "zero_multiply"]},
               "fields": {"VARIABLE": ["zero_product", "zero_product"]}},
   "zero_multiply": {"opcode": "operator_multiply", "next": null,
               "inputs": {"NUM1": [2, "zero_mul_read"], "NUM2": [1, [4, "0"]]},
               "fields": {}},
   "zero_mul_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "zero_quotient_compute": {"opcode": "data_setvariableto", "next": "mod_add_compute",
               "inputs": {"VALUE": [2, "zero_divide"]},
               "fields": {"VARIABLE": ["zero_quotient", "zero_quotient"]}},
   "zero_divide": {"opcode": "operator_divide", "next": null,
               "inputs": {"NUM1": [2, "zero_div_read"], "NUM2": [1, [4, "0"]]},
               "fields": {}},
   "zero_div_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "mod_add_compute": {"opcode": "data_setvariableto", "next": "mod_self_compute",
               "inputs": {"VALUE": [2, "add_mod"]},
               "fields": {"VARIABLE": ["mod_then_add", "mod_then_add"]}},
   "mod_self_compute": {"opcode": "data_setvariableto", "next": "mod_zero_compute",
               "inputs": {"VALUE": [2, "mod_self_op"]},
               "fields": {"VARIABLE": ["mod_self", "mod_self"]}},
   "mod_self_op": {"opcode": "operator_mod", "next": null,
               "inputs": {"NUM1": [2, "mod_self_a"], "NUM2": [2, "mod_self_b"]},
               "fields": {}},
   "mod_self_a": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "mod_self_b": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "mod_zero_compute": {"opcode": "data_setvariableto", "next": "round_compute",
               "inputs": {"VALUE": [2, "mod_zero_op"]},
               "fields": {"VARIABLE": ["mod_zero", "mod_zero"]}},
   "mod_zero_op": {"opcode": "operator_mod", "next": null,
               "inputs": {"NUM1": [2, "mod_zero_a"], "NUM2": [1, [4, "0"]]},
               "fields": {}},
   "mod_zero_a": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "add_mod": {"opcode": "operator_add", "next": null,
               "inputs": {"NUM1": [2, "mod"], "NUM2": [1, [4, "2"]]},
               "fields": {}},
   "mod": {"opcode": "operator_mod", "next": null,
               "inputs": {"NUM1": [2, "mod_read"], "NUM2": [1, [4, "3"]]},
               "fields": {}},
   "mod_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "add": {"opcode": "operator_add", "next": null,
           "inputs": {"NUM1": [2, "read_sum"],
                      "NUM2": [1, [4, "2"]]}, "fields": {}},
   "read_sum": {"opcode": "data_variable", "next": null,
                "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "round_compute": {"opcode": "data_setvariableto", "next": "round_mod_compute",
               "inputs": {"VALUE": [2, "round_reporter"]},
               "fields": {"VARIABLE": ["rounded", "rounded"]}},
   "round_reporter": {"opcode": "operator_round", "next": null,
               "inputs": {"NUM": [2, "round_x"]}, "fields": {}},
   "round_x": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "round_mod_compute": {"opcode": "data_setvariableto", "next": "round_plus_mod_compute",
               "inputs": {"VALUE": [2, "round_mod_expression"]},
               "fields": {"VARIABLE": ["round_mod", "round_mod"]}},
   "round_mod_expression": {"opcode": "operator_mod", "next": null,
               "inputs": {"NUM1": [2, "round_mod_round"], "NUM2": [1, [4, "7"]]},
               "fields": {}},
   "round_mod_round": {"opcode": "operator_round", "next": null,
               "inputs": {"NUM": [2, "round_mod_x"]}, "fields": {}},
   "round_mod_x": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "round_plus_mod_compute": {"opcode": "data_setvariableto", "next": "math_abs_compute",
               "inputs": {"VALUE": [2, "round_plus_mod_expression"]},
               "fields": {"VARIABLE": ["round_plus_mod", "round_plus_mod"]}},
   "round_plus_mod_expression": {"opcode": "operator_mod", "next": null,
               "inputs": {"NUM1": [2, "round_plus_add"], "NUM2": [1, [4, "5"]]},
               "fields": {}},
   "round_plus_add": {"opcode": "operator_add", "next": null,
               "inputs": {"NUM1": [2, "round_plus_round"], "NUM2": [1, [4, "3"]]},
               "fields": {}},
   "round_plus_round": {"opcode": "operator_round", "next": null,
               "inputs": {"NUM": [2, "round_plus_x"]}, "fields": {}},
   "round_plus_x": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "math_abs_compute": {"opcode": "data_setvariableto", "next": "math_floor_compute",
               "inputs": {"VALUE": [2, "math_abs_op"]},
               "fields": {"VARIABLE": ["math_abs", "math_abs"]}},
   "math_abs_op": {"opcode": "operator_mathop", "next": null,
               "inputs": {"NUM": [2, "math_abs_read"]},
               "fields": {"OPERATOR": ["abs", null]}},
   "math_abs_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "math_floor_compute": {"opcode": "data_setvariableto", "next": "math_ceil_compute",
               "inputs": {"VALUE": [2, "math_floor_op"]},
               "fields": {"VARIABLE": ["math_floor", "math_floor"]}},
   "math_floor_op": {"opcode": "operator_mathop", "next": null,
               "inputs": {"NUM": [2, "math_floor_read"]},
               "fields": {"OPERATOR": ["floor", null]}},
   "math_floor_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "math_ceil_compute": {"opcode": "data_setvariableto", "next": "math_atan_compute",
               "inputs": {"VALUE": [2, "math_ceil_op"]},
               "fields": {"VARIABLE": ["math_ceil", "math_ceil"]}},
   "math_ceil_op": {"opcode": "operator_mathop", "next": null,
               "inputs": {"NUM": [2, "math_ceil_read"]},
               "fields": {"OPERATOR": ["ceiling", null]}},
   "math_ceil_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "math_atan_compute": {"opcode": "data_setvariableto", "next": "math_exp_compute",
               "inputs": {"VALUE": [2, "math_atan_op"]},
               "fields": {"VARIABLE": ["math_atan", "math_atan"]}},
   "math_atan_op": {"opcode": "operator_mathop", "next": null,
               "inputs": {"NUM": [2, "math_atan_read"]},
               "fields": {"OPERATOR": ["atan", null]}},
   "math_atan_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "math_exp_compute": {"opcode": "data_setvariableto", "next": "math_pow10_compute",
               "inputs": {"VALUE": [2, "math_exp_op"]},
               "fields": {"VARIABLE": ["math_exp", "math_exp"]}},
   "math_exp_op": {"opcode": "operator_mathop", "next": null,
               "inputs": {"NUM": [2, "math_exp_read"]},
               "fields": {"OPERATOR": ["e ^", null]}},
   "math_exp_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "math_pow10_compute": {"opcode": "data_setvariableto", "next": "compare",
               "inputs": {"VALUE": [2, "math_pow10_op"]},
               "fields": {"VARIABLE": ["math_pow10", "math_pow10"]}},
   "math_pow10_op": {"opcode": "operator_mathop", "next": null,
               "inputs": {"NUM": [2, "math_pow10_read"]},
               "fields": {"OPERATOR": ["10 ^", null]}},
   "math_pow10_read": {"opcode": "data_variable", "next": null,
               "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "compare": {"opcode": "data_setvariableto", "next": "change",
               "inputs": {"VALUE": [2, "less_than"]},
               "fields": {"VARIABLE": ["less", "less"]}},
   "less_than": {"opcode": "operator_lt", "next": null,
                 "inputs": {"OPERAND1": [2, "read_less"],
                            "OPERAND2": [1, [4, "2"]]}, "fields": {}},
   "read_less": {"opcode": "data_variable", "next": null,
                 "inputs": {}, "fields": {"VARIABLE": ["x", "x"]}},
   "change": {"opcode": "data_changevariableby", "next": "numeric_truth",
              "inputs": {"VALUE": [1, [4, "1"]]},
              "fields": {"VARIABLE": ["x", "x"]}},
   "numeric_truth": {"opcode": "control_if", "next": "dynamic_truth",
                     "inputs": {"CONDITION": [2, "truth_add"],
                                "SUBSTACK": [2, "set_truth_numeric"]},
                     "fields": {}},
   "truth_add": {"opcode": "operator_add", "next": null,
                 "inputs": {"NUM1": [2, "truth_read_raw"],
                            "NUM2": [1, [4, "0"]]}, "fields": {}},
   "truth_read_raw": {"opcode": "data_variable", "next": null,
                      "inputs": {}, "fields": {"VARIABLE": ["raw", "raw"]}},
   "set_truth_numeric": {"opcode": "data_setvariableto", "next": null,
                         "inputs": {"VALUE": [1, [4, "1"]]},
                         "fields": {"VARIABLE": ["truth_numeric", "truth_numeric"]}},
   "dynamic_truth": {"opcode": "control_if", "next": "eq_lnum_set",
                     "inputs": {"CONDITION": [2, "truth_read_dynamic"],
                                "SUBSTACK": [2, "set_truth_dynamic"]},
                     "fields": {}},
   "truth_read_dynamic": {"opcode": "data_variable", "next": null,
                          "inputs": {}, "fields": {"VARIABLE": ["raw", "raw"]}},
   "set_truth_dynamic": {"opcode": "data_setvariableto", "next": null,
                         "inputs": {"VALUE": [1, [4, "1"]]},
                         "fields": {"VARIABLE": ["truth_dynamic", "truth_dynamic"]}},
   "eq_lnum_set": {"opcode": "data_setvariableto", "next": "eq_rnum_set",
                   "inputs": {"VALUE": [2, "eq_lnum"]},
                   "fields": {"VARIABLE": ["eq_left_number", "eq_left_number"]}},
   "eq_lnum": {"opcode": "operator_equals", "next": null,
               "inputs": {"OPERAND1": [1, [4, "0"]],
                          "OPERAND2": [2, "eq_lnum_raw"]}, "fields": {}},
   "eq_lnum_raw": {"opcode": "data_variable", "next": null,
                   "fields": {"VARIABLE": ["raw", "raw"]}, "inputs": {}},
   "eq_rnum_set": {"opcode": "data_setvariableto", "next": "eq_lstr_set",
                   "inputs": {"VALUE": [2, "eq_rnum"]},
                   "fields": {"VARIABLE": ["eq_right_number", "eq_right_number"]}},
   "eq_rnum": {"opcode": "operator_equals", "next": null,
               "inputs": {"OPERAND1": [2, "eq_rnum_raw"],
                          "OPERAND2": [1, [4, "0"]]}, "fields": {}},
   "eq_rnum_raw": {"opcode": "data_variable", "next": null,
                   "fields": {"VARIABLE": ["raw", "raw"]}, "inputs": {}},
   "eq_lstr_set": {"opcode": "data_setvariableto", "next": "eq_rstr_set",
                   "inputs": {"VALUE": [2, "eq_lstr"]},
                   "fields": {"VARIABLE": ["eq_left_string", "eq_left_string"]}},
   "eq_lstr": {"opcode": "operator_equals", "next": null,
               "inputs": {"OPERAND1": [1, [10, "0"]],
                          "OPERAND2": [2, "eq_lstr_raw"]}, "fields": {}},
   "eq_lstr_raw": {"opcode": "data_variable", "next": null,
                   "fields": {"VARIABLE": ["raw", "raw"]}, "inputs": {}},
   "eq_rstr_set": {"opcode": "data_setvariableto", "next": "eq_pair_set",
                   "inputs": {"VALUE": [2, "eq_rstr"]},
                   "fields": {"VARIABLE": ["eq_right_string", "eq_right_string"]}},
   "eq_rstr": {"opcode": "operator_equals", "next": null,
               "inputs": {"OPERAND1": [2, "eq_rstr_raw"],
                          "OPERAND2": [1, [10, "0"]]}, "fields": {}},
   "eq_rstr_raw": {"opcode": "data_variable", "next": null,
                   "fields": {"VARIABLE": ["raw", "raw"]}, "inputs": {}},
   "eq_pair_set": {"opcode": "data_setvariableto", "next": null,
                   "inputs": {"VALUE": [2, "eq_pair"]},
                   "fields": {"VARIABLE": ["eq_pair_dynamic", "eq_pair_dynamic"]}},
   "eq_pair": {"opcode": "operator_equals", "next": null,
               "inputs": {"OPERAND1": [2, "eq_pair_raw"],
                          "OPERAND2": [2, "eq_pair_x"]}, "fields": {}},
   "eq_pair_raw": {"opcode": "data_variable", "next": null,
                   "fields": {"VARIABLE": ["raw", "raw"]}, "inputs": {}},
   "eq_pair_x": {"opcode": "data_variable", "next": null,
                   "fields": {"VARIABLE": ["x", "x"]}, "inputs": {}}
  },
  "costumes": [], "sounds": []
 }],
 "monitors": [], "extensions": [], "meta": {}
})JSON";

std::string encode(Value v) {
    if (v.isString()) return "s:" + SStr::toUtf8(cast::toString(v));
    if (v.isNumber() && std::isnan(v.asNumberUnchecked())) return "n:NaN";
    char result[32];
    std::snprintf(result, sizeof(result), "%016llx",
                  static_cast<unsigned long long>(v.bits));
    return result;
}

using State = std::vector<std::string>;
State execute(JitMode mode, Value value, bool feedbackEnabled,
              bool replaceAfterCompile) {
    EnvGuard env;
    env.set(feedbackEnabled);
    setJitMode(mode);

    Runtime rt;
    check(loadSb3(rt, kProject, nullptr).ok, "load type-feedback SB3");
    if (rt.targets.empty()) return {};
    Target* target = rt.targets[0];
    Variable* x = target->lookupVariableById("x");
    check(x != nullptr, "resolved input variable");
    if (!x) return {};

    // Compile against a numeric value and then deliberately violate the
    // feedback. The resulting native code must still execute correctly.
    if (!replaceAfterCompile) x->value = value;
    rt.greenFlag();
    check(rt.threads.size() == 1, "green flag spawns a script");
#ifdef XYO_JIT
    if (mode == JitMode::Eager && !rt.threads.empty())
        check(rt.threads[0]->isCompiled, "eager script was JIT compiled");
#endif
    if (replaceAfterCompile) x->value = value;
    rt.runUntilDone(40);
    check(rt.threads.empty(), "type feedback script terminates");

    State result;
    for (const char* id : {
             "x", "raw", "sum", "difference", "product", "quotient",
             "zero_product", "zero_quotient", "mod_then_add",
             "mod_self", "mod_zero", "rounded",
             "round_mod", "round_plus_mod", "math_abs", "math_floor",
             "math_ceil", "math_atan", "math_exp", "math_pow10", "less",
             "truth_numeric", "truth_dynamic", "eq_left_number",
             "eq_right_number", "eq_left_string", "eq_right_string",
             "eq_pair_dynamic"}) {
        Variable* v = target->lookupVariableById(id);
        check(v != nullptr, "output variable exists");
        if (!v) return {};
        result.push_back(encode(v->value));
    }
    return result;
}

void compareModes(Value value, bool changeAfterCompile) {
    const State reference = execute(JitMode::Off, value, true, changeAfterCompile);
    const State specialized = execute(JitMode::Eager, value, true, changeAfterCompile);
    const State generic = execute(JitMode::Eager, value, false, changeAfterCompile);
    if (reference != specialized) {
        std::printf("type feedback mismatch: input=%s, after=%d\n",
                    encode(value).c_str(), int(changeAfterCompile));
        for (size_t i = 0; i < reference.size() && i < specialized.size(); ++i)
            std::printf("  %zu: interpreter=%s compiled=%s\n", i,
                        reference[i].c_str(), specialized[i].c_str());
    }
    check(!reference.empty() && reference == specialized,
          "profiled compiled execution matches interpreter");
    check(!reference.empty() && reference == generic,
          "profiling opt-out matches interpreter");
}
} // namespace

int test_type_profile_main() {
    const JitMode saved = jitMode();
    Gc::DeferCollect keepStrings;
    static_assert(std::is_copy_constructible_v<Variable>);
    {
        Variable v;
        check(v.typeProfile() == 0, "unseen variable has no type feedback");
        v.observeValueType(Value::num(1.0));
        check(v.observedOnlyNumber(), "first numeric sample is monomorphic");
        Variable clone = v;
        check(clone.observedOnlyNumber(), "feedback survives variable copy");
        v.observeValueType(Value::emptyStr());
        check(v.typeProfile() == (Variable::kObservedNumber |
                                  Variable::kObservedString),
              "mixed types are recorded monotonically");
        check(!v.observedOnlyNumber(), "mixed types are not numeric monomorphic");
    }
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<Value> cases{
        Value::num(5), Value::num(0.5), Value::num(-3.75),
        Value::num(std::nextafter(0.5, 0.0)),
        Value::num(std::nextafter(0.5, 1.0)),
        Value::num(-0.5), Value::num(std::nextafter(-0.5, -1.0)),
        Value::num(std::nextafter(-0.5, 0.0)),
        Value::num(4503599627370496.0),
        Value::num(std::nextafter(4503599627370496.0, 0.0)),
        Value::num(2147483647.0), Value::num(2147483648.0),
        Value::num(std::numeric_limits<double>::max()),
        Value::num(std::numeric_limits<double>::denorm_min()),
        Value::num(-0.0), Value::num(inf),
        Value::num(-inf), Value::nan(),
        Value::boolean(true), Value::boolean(false),
        Value::emptyStr(), Value::string(SStr::fromUtf8("123")),
        Value::string(SStr::fromUtf8(" 3.5 ")),
        Value::string(SStr::fromUtf8(" ")),
        Value::string(SStr::fromUtf8("hello")),
        Value::string(SStr::fromUtf8("0")),
        Value::string(SStr::fromUtf8("00")),
        Value::string(SStr::fromUtf8("-0")),
        Value::string(SStr::fromUtf8("false")),
        Value::string(SStr::fromUtf8("FALSE")),
        Value::string(SStr::fromUtf8("0.0")),
        Value::string(SStr::fromUtf8(" 0 ")),
    };
    for (Value input : cases) {
        compareModes(input, false); // type observed before compilation
        compareModes(input, true);  // type changed after compilation
    }
#ifdef XYO_JIT
    // JIT runtime-helper regression: the second call exercises the cached
    // nonnumeric-string comparison path without changing Scratch semantics.
    auto checkPair = [&](const char* left, const char* right,
                         bool expected) {
        SStr* l = SStr::fromUtf8(left);
        SStr* r = SStr::fromUtf8(right);
        Value a = Value::string(l), b = Value::string(r);
        check(bool(xyo_eq(a.bits, b.bits)) == expected,
              "JIT string equality before number-cache population");
        check(bool(xyo_eq(a.bits, b.bits)) == expected,
              "JIT string equality after number-cache population");
    };
    checkPair("Hello World", "hello world", true);
    checkPair("Hello World", "hello world!", false);
    checkPair("already", "ALREADY", true);
    checkPair("Infinity", "infinity", true);
    checkPair("0", "00", true);
    checkPair(" ", "0", false);
    checkPair("Ä", "ä", true);
    // < and > must preserve Scratch's switch between numeric ordering and
    // case-insensitive lexical ordering, including numeric-string caches.
    auto checkOrder = [&](const char* left, const char* right) {
        Value a = Value::string(SStr::fromUtf8(left));
        Value b = Value::string(SStr::fromUtf8(right));
        Value referenceA = Value::string(SStr::fromUtf8(left));
        Value referenceB = Value::string(SStr::fromUtf8(right));
        const double cmp = cast::compare(referenceA, referenceB);
        const bool lt = cmp < 0, gt = cmp > 0;
        check(bool(xyo_lt(a.bits, b.bits)) == lt &&
              bool(xyo_gt(a.bits, b.bits)) == gt,
              "JIT string order before number-cache population");
        (void)cast::toNumberOrNaN(a);
        (void)cast::toNumberOrNaN(b);
        check(bool(xyo_lt(a.bits, b.bits)) == lt &&
              bool(xyo_gt(a.bits, b.bits)) == gt,
              "JIT string order after number-cache population");
    };
    checkOrder("abc", "ABD");
    checkOrder("Hello World", "hello world");
    checkOrder("123", "23");
    checkOrder("-10", "2");
    checkOrder("0", "00");
    checkOrder(" ", "0");
    checkOrder("Infinity", "infinity");
    checkOrder("-Infinity", "1000");
    checkOrder("1e309", "Infinity");
    checkOrder("Ä", "z");
    checkOrder("Z", "ä");
    checkOrder("00012", "12");
#endif
    setJitMode(saved);
    std::printf("type feedback: %d checks, %d failures\n", checks, failures);
    return failures;
}
