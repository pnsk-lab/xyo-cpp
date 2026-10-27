// xyo-cpp: cast/jsnum/string/list のユニットテスト。
// refs/scratch-vm/test/unit/util_cast.js の移植 + ES エッジケース。
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/jsnum.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "rt/runtime_helpers.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static Value s(std::string_view sv) {
    if (sv.empty()) return Value::emptyStr();
    return Value::string(SStr::fromUtf8(sv));
}
static Value n(double d) { return Value::num(d); }
static Value b(bool x) { return Value::boolean(x); }
static double tn(Value v) { return cast::toNumber(v); }
static std::string ts(Value v) { return SStr::toUtf8(cast::toString(v)); }
static bool tb(Value v) { return cast::toBoolean(v); }

static void test_toNumber() {
    CHECK_EQ(tn(n(0)), 0);
    CHECK_EQ(tn(n(1)), 1);
    CHECK_EQ(tn(n(3.14)), 3.14);
    CHECK_EQ(tn(n(std::nan(""))), 0);
    CHECK_EQ(tn(s("0")), 0);
    CHECK_EQ(tn(s("1")), 1);
    CHECK_EQ(tn(s("3.14")), 3.14);
    CHECK_EQ(tn(s("0.1e10")), 1000000000);
    CHECK_EQ(tn(s("foobar")), 0);
    CHECK_EQ(tn(b(true)), 1);
    CHECK_EQ(tn(b(false)), 0);
    CHECK_EQ(tn(s("true")), 0);
    CHECK_EQ(tn(s("false")), 0);
    CHECK_EQ(tn(Value::nan()), 0);
    // ES Number() の追加ケース
    CHECK_EQ(tn(s("0x10")), 16);
    CHECK_EQ(tn(s("0b11")), 3);
    CHECK_EQ(tn(s("0o17")), 15);
    CHECK_EQ(tn(s("-0x10")), 0);          // 符号は10進のみ → NaN → 0
    CHECK_EQ(tn(s("Infinity")), std::numeric_limits<double>::infinity());
    CHECK_EQ(tn(s("-Infinity")), -std::numeric_limits<double>::infinity());
    CHECK_EQ(tn(s("inf")), 0);           // 'inf' は NaN → 0
    CHECK_EQ(tn(s("nan")), 0);
    CHECK_EQ(tn(s("")), 0);
    CHECK_EQ(tn(s(" ")), 0);
    CHECK_EQ(tn(s(" \t\n ")), 0);
    CHECK_EQ(tn(s("  42  ")), 42);
    CHECK_EQ(tn(s(".5")), 0.5);
    CHECK_EQ(tn(s("5.")), 5);
    CHECK_EQ(tn(s("5.e3")), 5000);
    CHECK_EQ(tn(s(".e3")), 0);
    CHECK_EQ(tn(s("1e")), 0);
    CHECK_EQ(tn(s("+42")), 42);
    CHECK_EQ(tn(s("1e-324")), 0);         // V8 と同じく 0 に丸め
    CHECK_EQ(tn(s("4.9e-324")), 5e-324);  // 最小非正規数に丸め
    CHECK_EQ(tn(s("1e400")), std::numeric_limits<double>::infinity());
    CHECK_EQ(tn(s("9007199254740993")), 9007199254740992.0);
    CHECK_EQ(tn(s("0,5")), 0);            // カンマは小数点ではない
}

static void test_toBoolean() {
    CHECK_EQ(tb(n(0)), false);
    CHECK_EQ(tb(n(1)), true);
    CHECK_EQ(tb(n(3.14)), true);
    CHECK_EQ(tb(n(-0.0)), false);
    CHECK_EQ(tb(n(std::nan(""))), false);
    CHECK_EQ(tb(Value::nan()), false);
    CHECK_EQ(tb(s("0")), false);
    CHECK_EQ(tb(s("1")), true);
    CHECK_EQ(tb(s("3.14")), true);
    CHECK_EQ(tb(s("0.1e10")), true);
    CHECK_EQ(tb(s("foobar")), true);
    CHECK_EQ(tb(s("false")), false);
    CHECK_EQ(tb(s("FALSE")), false);
    CHECK_EQ(tb(s("False")), false);
    CHECK_EQ(tb(s("falsE")), false);
    CHECK_EQ(tb(s("falsee")), true);
    CHECK_EQ(tb(s("")), false);
    CHECK_EQ(tb(b(true)), true);
    CHECK_EQ(tb(b(false)), false);
}

static void test_toString() {
    CHECK_EQ(ts(n(0)), "0");
    CHECK_EQ(ts(n(1)), "1");
    CHECK_EQ(ts(n(3.14)), "3.14");
    CHECK_EQ(ts(n(-0.0)), "0");
    CHECK_EQ(ts(n(std::nan(""))), "NaN");
    CHECK_EQ(ts(Value::nan()), "NaN");
    CHECK_EQ(ts(n(std::numeric_limits<double>::infinity())), "Infinity");
    CHECK_EQ(ts(n(-std::numeric_limits<double>::infinity())), "-Infinity");
    CHECK_EQ(ts(n(1e21)), "1e+21");
    CHECK_EQ(ts(n(1e20)), "100000000000000000000");
    CHECK_EQ(ts(n(1e-6)), "0.000001");
    CHECK_EQ(ts(n(1e-7)), "1e-7");
    CHECK_EQ(ts(n(5e-324)), "5e-324");
    CHECK_EQ(ts(n(123.456)), "123.456");
    CHECK_EQ(ts(n(0.3)), "0.3");
    CHECK_EQ(ts(n(0.30000000000000004)), "0.30000000000000004");
    CHECK_EQ(ts(n(123456789012345680.0)), "123456789012345680");
    CHECK_EQ(ts(n(1e15)), "1000000000000000");
    CHECK_EQ(ts(n(1e15 + 0.5)), "1000000000000000.5");
    CHECK_EQ(ts(n(1.0 / 3)), "0.3333333333333333");
    CHECK_EQ(ts(n(1.7976931348623157e308)), "1.7976931348623157e+308");
    CHECK_EQ(ts(s("foobar")), "foobar");
    CHECK_EQ(ts(s("0.1e10")), "0.1e10");
    CHECK_EQ(ts(b(true)), "true");
    CHECK_EQ(ts(b(false)), "false");
    CHECK_EQ(ts(Value::emptyStr()), "");
}

static void test_compare() {
    auto cmp = [](Value a, Value b) { return cast::compare(a, b); };
    CHECK_EQ(cmp(n(0), n(0)), 0);
    CHECK(cmp(n(1), n(0)) > 0);
    CHECK(cmp(n(0), n(1)) < 0);
    CHECK_EQ(cmp(n(1), n(1)), 0);
    CHECK_EQ(cmp(s("0"), s("0")), 0);
    CHECK_EQ(cmp(s("0.1e10"), s("1000000000")), 0);
    CHECK_EQ(cmp(s("foobar"), s("FOOBAR")), 0);
    CHECK(cmp(s("dog"), s("cat")) > 0);
    CHECK_EQ(cmp(b(true), b(true)), 0);
    CHECK(cmp(b(true), b(false)) > 0);
    CHECK(cmp(b(false), b(true)) < 0);
    // isNotActuallyZero 関連: "" / " " は 0 と比較しても「0 ではない」
    CHECK(cmp(s(""), n(0)) != 0);
    CHECK(cmp(s(" "), n(0)) != 0);
    CHECK_EQ(cmp(s("\t"), n(0)), 0);   // タブは 0 扱い（scratch-www 互換）
    CHECK_EQ(cmp(s(""), s("")), 0);
    CHECK(cmp(s(""), n(1)) != 0);
    // Infinity 同士
    CHECK_EQ(cmp(n(std::numeric_limits<double>::infinity()),
                 n(std::numeric_limits<double>::infinity())), 0);
    CHECK_EQ(cmp(n(-std::numeric_limits<double>::infinity()),
                 n(-std::numeric_limits<double>::infinity())), 0);
    CHECK(cmp(n(std::numeric_limits<double>::infinity()),
              n(-std::numeric_limits<double>::infinity())) > 0);
    // NaN vs NaN → 文字列比較 "nan"=="nan" → 0
    CHECK_EQ(cmp(Value::nan(), Value::nan()), 0);
    CHECK_EQ(cmp(Value::nan(), s("NaN")), 0);
    // "10" vs "9" → 数値比較 10>9
    CHECK(cmp(s("10"), s("9")) > 0);
    // "abc" vs "abd" → 文字列比較
    CHECK(cmp(s("abc"), s("abd")) < 0);
}

static void test_isInt() {
    CHECK_EQ(cast::isInt(n(0)), true);
    CHECK_EQ(cast::isInt(n(1)), true);
    CHECK_EQ(cast::isInt(n(0.0)), true);
    CHECK_EQ(cast::isInt(n(3.14)), false);
    CHECK_EQ(cast::isInt(n(std::nan(""))), true);
    CHECK_EQ(cast::isInt(s("0")), true);
    CHECK_EQ(cast::isInt(s("1")), true);
    CHECK_EQ(cast::isInt(s("0.0")), false);
    CHECK_EQ(cast::isInt(s("0.1e10")), false);
    CHECK_EQ(cast::isInt(s("3.14")), false);
    CHECK_EQ(cast::isInt(b(true)), true);
    CHECK_EQ(cast::isInt(b(false)), true);
}

static void test_toListIndex() {
    using cast::LIST_ALL;
    using cast::LIST_INVALID;
    CHECK_EQ(cast::toListIndexD(n(1), 6, false), 1);
    CHECK_EQ(cast::toListIndexD(n(6), 6, false), 6);
    CHECK_EQ(cast::toListIndexD(n(-1), 6, false), LIST_INVALID);
    CHECK_EQ(cast::toListIndexD(n(0.1), 6, false), LIST_INVALID);
    CHECK_EQ(cast::toListIndexD(n(0), 6, false), LIST_INVALID);
    CHECK_EQ(cast::toListIndexD(n(7), 6, false), LIST_INVALID);
    CHECK_EQ(cast::toListIndexD(s("all"), 6, true), LIST_ALL);
    CHECK_EQ(cast::toListIndexD(s("all"), 6, false), LIST_INVALID);
    CHECK_EQ(cast::toListIndexD(s("last"), 6, false), 6);
    CHECK_EQ(cast::toListIndexD(s("last"), 0, false), LIST_INVALID);
    int r = cast::toListIndexD(s("random"), 6, false);
    CHECK(r >= 1 && r <= 6);
    CHECK_EQ(cast::toListIndexD(s("random"), 0, false), LIST_INVALID);
    r = cast::toListIndexD(s("any"), 6, false);
    CHECK(r >= 1 && r <= 6);
    CHECK_EQ(cast::toListIndexD(s("any"), 0, false), LIST_INVALID);
    // 数字文字列・特殊値
    CHECK_EQ(cast::toListIndexD(s("3"), 6, false), 3);
    CHECK_EQ(cast::toListIndexD(s("3.9"), 6, false), 3);   // floor
    CHECK_EQ(cast::toListIndexD(b(true), 6, false), 1);    // true→1
    // 0-based listIndex
    CHECK_EQ(cast::listIndex(n(1), 6), 0);
    CHECK_EQ(cast::listIndex(n(6), 6), 5);
    CHECK_EQ(cast::listIndex(n(0), 6), -1);
    CHECK_EQ(cast::listIndex(n(7), 6), -1);
    CHECK_EQ(cast::listIndex(n(1.9), 6), 0);              // trunc
    CHECK_EQ(cast::listIndex(s("last"), 6), 5);
    CHECK_EQ(cast::listIndex(s("abc"), 6), -1);
}

static void test_rt_math() {
    // mod: floored
    CHECK_EQ(rt::mod(5, 3), 2);
    CHECK_EQ(rt::mod(-5, 3), 1);
    CHECK_EQ(rt::mod(5, -3), -1);
    CHECK_EQ(rt::mod(-5, -3), -2);
    CHECK_EQ(rt::mod(0, 5), 0);
    CHECK(std::isnan(rt::mod(5, 0)));
    // tan: 特殊角
    CHECK_EQ(rt::tanDeg(90), std::numeric_limits<double>::infinity());
    CHECK_EQ(rt::tanDeg(270), -std::numeric_limits<double>::infinity());
    CHECK_EQ(rt::tanDeg(-90), -std::numeric_limits<double>::infinity());
    CHECK_EQ(rt::tanDeg(-270), std::numeric_limits<double>::infinity());
    CHECK_EQ(rt::tanDeg(0), 0);
    CHECK_EQ(rt::tanDeg(45), 1);
    CHECK_EQ(rt::tanDeg(450), std::numeric_limits<double>::infinity()); // 450%360=90
    // limitPrecision
    CHECK_EQ(rt::limitPrecision(3.0000000004), 3);
    CHECK_EQ(rt::limitPrecision(3.5), 3.5);
    CHECK_EQ(rt::limitPrecision(3.00000001), 3.00000001);
}

static void test_rt_compare() {
    using rt::compareEqual;
    using rt::compareLessThan;
    using rt::compareGreaterThan;
    CHECK(compareEqual(n(1), n(1)));
    CHECK(compareEqual(n(0), n(-0.0)));
    CHECK(compareEqual(s("abc"), s("ABC")));
    CHECK(compareEqual(Value::nan(), s("NaN")));       // NaN==NaN は文字列一致
    CHECK(!compareEqual(n(1), n(2)));
    CHECK(compareEqual(b(true), n(1)));
    CHECK(compareGreaterThan(n(2), n(1)));
    CHECK(compareLessThan(n(1), n(2)));
    CHECK(!compareGreaterThan(n(1), n(1)));
    CHECK(compareGreaterThan(s("b"), s("A")));         // "b">"a"
    CHECK(!compareLessThan(Value::nan(), n(0)));       // NaN: 文字列 "nan" vs "0"
    CHECK(compareGreaterThan(Value::nan(), n(0)));     // "nan" > "0"
}

static void test_rt_string() {
    CHECK_EQ(ts(rt::join(s("hello"), s(" world"))), "hello world");
    CHECK_EQ(ts(rt::join(n(1), n(2))), "12");
    CHECK_EQ(ts(rt::letterOf(n(1), s("abc"))), "a");
    CHECK_EQ(ts(rt::letterOf(n(3), s("abc"))), "c");
    CHECK_EQ(rt::letterOf(n(0), s("abc")).bits, Value::emptyStr().bits);
    CHECK_EQ(rt::letterOf(n(4), s("abc")).bits, Value::emptyStr().bits);
    CHECK_EQ(ts(rt::letterOf(n(1.9), s("abc"))), "a"); // charAt trunc
    CHECK_EQ(rt::strLength(s("hello")), 5);
    CHECK_EQ(rt::strLength(n(12345)), 5);
    CHECK(rt::strContains(s("hello world"), s("WORLD")));
    CHECK(rt::strContains(s("hello"), s("")));
    CHECK(!rt::strContains(s("hello"), s("xyz")));
    // 絵文字（サロゲートペア）: length は UTF-16 コードユニット数
    CHECK_EQ(rt::strLength(s("😀")), 2);   // U+1F600 → 2 units
    {
        Value ch = rt::letterOf(n(1), s("😀"));
        SStr* cs = ch.asStrPtr();
        CHECK(cs && cs->len == 1 && cs->data()[0] == 0xD83D);   // 上位サロゲートが返る
    }
}

static void test_rt_list() {
    auto* list = SList::create();
    list->push(n(10));
    list->push(s("abc"));
    list->push(b(true));
    CHECK_EQ(ts(rt::listGet(list, n(1))), "10");
    CHECK_EQ(ts(rt::listGet(list, n(2))), "abc");
    CHECK_EQ(rt::listGet(list, n(4)).bits, Value::emptyStr().bits);
    CHECK_EQ(ts(rt::listGet(list, s("last"))), "true");
    rt::listReplace(list, n(2), n(99));
    CHECK_EQ(ts(rt::listGet(list, n(2))), "99");
    rt::listInsert(list, n(2), s("ins"));
    CHECK_EQ(list->len, 4);
    CHECK_EQ(ts(rt::listGet(list, n(2))), "ins");
    rt::listDelete(list, n(2));
    CHECK_EQ(list->len, 3);
    CHECK(rt::listContains(list, n(99)));
    CHECK(rt::listContains(list, s("99")));        // 数値==文字列で一致
    CHECK(!rt::listContains(list, n(12345)));
    CHECK_EQ(rt::listIndexOf(list, n(99)), 2);
    CHECK_EQ(rt::listIndexOf(list, n(12345)), 0);
    CHECK_EQ(ts(rt::listContents(list)), "10 99 true");
    rt::listDelete(list, s("all"));
    CHECK_EQ(list->len, 0);
    // 1 文字要素のみ → join('')
    list->push(s("a"));
    list->push(s("b"));
    list->push(s("c"));
    CHECK_EQ(ts(rt::listContents(list)), "abc");
    // 数値 1 は typeof string ではない → join(' ') に切り替わる
    list->push(n(1));
    CHECK_EQ(ts(rt::listContents(list)), "a b c 1");
    // 2 文字要素が混ざる → join(' ')
    list->push(s("xy"));
    CHECK_EQ(ts(rt::listContents(list)), "a b c 1 xy");
}

static void test_rgb() {
    auto c1 = cast::toRgbColorList(s("#ff8000"));
    CHECK_EQ(c1.r, 255); CHECK_EQ(c1.g, 128); CHECK_EQ(c1.b, 0);
    auto c2 = cast::toRgbColorList(s("#fff"));
    CHECK_EQ(c2.r, 255); CHECK_EQ(c2.g, 255); CHECK_EQ(c2.b, 255);
    auto c3 = cast::toRgbColorList(n(16777215));
    CHECK_EQ(c3.r, 255); CHECK_EQ(c3.g, 255); CHECK_EQ(c3.b, 255);
    auto c4 = cast::toRgbColorList(s("foobar"));
    CHECK_EQ(c4.r, 0); CHECK_EQ(c4.g, 0); CHECK_EQ(c4.b, 0);
    auto c5 = cast::toRgbColorList(s("#nothex"));
    CHECK_EQ(c5.r, 0); CHECK_EQ(c5.g, 0); CHECK_EQ(c5.b, 0);
    auto c6 = cast::toRgbColorList(n(0));
    CHECK_EQ(c6.r, 0); CHECK_EQ(c6.g, 0); CHECK_EQ(c6.b, 0);
    // 0x80010203 (a=128)
    auto c7 = cast::toRgbColorObject(s("0x80010203"));
    CHECK_EQ(c7.a, 128); CHECK_EQ(c7.r, 1); CHECK_EQ(c7.g, 2); CHECK_EQ(c7.b, 3);
}

static void test_case_insensitive() {
    // compareStringsCaseInsensitive
    CHECK_EQ(compareStringsCaseInsensitive(u"Hello", u"HELLO"), 0);
    CHECK(compareStringsCaseInsensitive(u"abc", u"ABD") < 0);
    CHECK(compareStringsCaseInsensitive(u"abd", u"ABC") > 0);
    // 非 ASCII: Ä/ä
    CHECK_EQ(compareStringsCaseInsensitive(u"ÄPFEL", u"äpfel"), 0);
    // トルコ İ → i + U+0307
    std::u16string lo = u16ToLower(u"İ");
    CHECK_EQ(lo.size(), 2);
    CHECK_EQ(lo[0], u'i');
    CHECK_EQ(lo[1], 0x0307);
}

static void test_gc() {
    // GC: ルート登録なしでは全て回収される
    size_t before = Gc::get().objectCount();
    {
        auto* s1 = SStr::create(u"temp_string");
        auto* s2 = SStr::create(u"another");
        (void)s1; (void)s2;
    }
    Gc::get().collect();
    // ルートがないため新規 2 つは回収済み
    CHECK(Gc::get().objectCount() <= before + 0);
}

extern int test_e2e_main();
extern int test_pen();
extern int test_aot_main();
extern int test_wasm_emit_main();
extern int test_wasm_compile_main();
extern int test_minify_main();
extern int test_deps_main();
extern int test_analysis_main();
extern int test_parfor_main();
extern int test_pen_hoist_main();
extern int test_rewrite_main();
extern int test_parallel_strings_main();
extern int test_scheduler_main();
extern int test_equality_literal_main();
extern int test_color_fast_main();
extern int test_motion_numeric_main();
extern int test_slot_hoist_main();
extern int test_list_codegen_bounds_main();
extern int test_nan_codegen_main();
extern int test_literal_context_main();
extern int test_danger_main();
extern int test_list_string_intern_main();
extern int test_gl_chunk_plan_main();
extern int test_scratch_compat_main();
int main() {
    test_toNumber();
    test_toBoolean();
    test_toString();
    test_compare();
    test_isInt();
    test_toListIndex();
    test_rt_math();
    test_rt_compare();
    test_rt_string();
    test_rt_list();
    test_rgb();
    test_case_insensitive();
    test_gc();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    int e2eFail = test_e2e_main();
    int penFail = test_pen();
    int aotFail = test_aot_main();
    int wasmEmitFail = test_wasm_emit_main();
    int wasmCompileFail = test_wasm_compile_main();
    int minifyFail = test_minify_main();
    int depsFail = test_deps_main();
    int analysisFail = test_analysis_main();
    int parforFail = test_parfor_main();
    int penHoistFail = test_pen_hoist_main();
    int rewriteFail = test_rewrite_main();
    int parallelStringsFail = test_parallel_strings_main();
    int schedulerFail = test_scheduler_main();
    int equalityLiteralFail = test_equality_literal_main();
    int colorFastFail = test_color_fast_main();
    int motionNumericFail = test_motion_numeric_main();
    int slotHoistFail = test_slot_hoist_main();
    int listCodegenBoundsFail = test_list_codegen_bounds_main();
    int nanCodegenFail = test_nan_codegen_main();
    int literalContextFail = test_literal_context_main();
    int dangerFail = test_danger_main();
    int listStringInternFail = test_list_string_intern_main();
    int glChunkPlanFail = test_gl_chunk_plan_main();
    int scratchCompatFail = test_scratch_compat_main();
    return (g_fail + e2eFail + penFail + aotFail + wasmEmitFail +
            wasmCompileFail + minifyFail + depsFail + analysisFail + parforFail + penHoistFail + rewriteFail +
            parallelStringsFail + schedulerFail +
            equalityLiteralFail + colorFastFail + motionNumericFail + slotHoistFail +
            listCodegenBoundsFail + nanCodegenFail + literalContextFail + dangerFail + listStringInternFail +
            glChunkPlanFail + scratchCompatFail)
               ? 1
               : 0;
}
