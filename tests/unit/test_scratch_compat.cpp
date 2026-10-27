// Scratch 互換の境界ケース（数値文字列化・Math.round・Math.log・
// 非 BMP 小文字化・NaN リストインデックス）の回帰テスト。
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "core/cast.hpp"
#include "core/jsnum.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "core/v8math.hpp"
#include "rt/runtime_helpers.hpp"

namespace {
using namespace xyo;

int checks = 0, failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL scratch compat: %s\n", what);
    }
}

Value n(double d) { return Value::num(d); }
Value s(const char* utf8) { return Value::string(SStr::fromUtf8(utf8)); }
std::string ts(Value v) {
    SStr* p = cast::toString(v);
    return p ? SStr::toUtf8(p->view()) : std::string();
}
uint64_t bits(double d) { return std::bit_cast<uint64_t>(d); }

// JS Math.round の独立参照実装。
double jsRoundRef(double x) {
    if (!std::isfinite(x) || x == 0.0) return x;
    if (x > 0.0 && x < 0.5) return 0.0;
    if (x < 0.0 && x >= -0.5) return -0.0;
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    return std::floor(x + 0.5);
}

void test_number_string() {
    check(jsnum::toString(144115188075855872.0) == "144115188075855870",
          "2^57 shortest round-trip digits");
    check(jsnum::toString(72057594037927936.0) == "72057594037927940",
          "2^56 shortest round-trip digits");
    check(jsnum::toString(36028797018963968.0) == "36028797018963970",
          "2^55 shortest round-trip digits");
    check(jsnum::toString(-144115188075855872.0) == "-144115188075855870",
          "negative 2^57 shortest round-trip digits");
    check(jsnum::toString(9007199254740992.0) == "9007199254740992",
          "2^53 integer fast path unchanged");
    check(jsnum::toString(123456789012345678.0) == "123456789012345680",
          "large JSON integer uses shortest digits");
    check(jsnum::toString(1e21) == "1e+21", "1e21 exponent form");
    check(jsnum::toString(1e-7) == "1e-7", "1e-7 exponent form");
    check(jsnum::toString(5e-324) == "5e-324", "denormal form");
}

void test_js_round() {
    check(bits(jsnum::jsRound(-0.5)) == bits(-0.0), "round(-0.5) is -0");
    check(jsnum::jsRound(-0.5) == 0.0, "round(-0.5) compares to 0");
    check(jsnum::jsRound(0.5) == 1.0, "round(0.5) is 1");
    check(jsnum::jsRound(-1.5) == -1.0, "round(-1.5) is -1");
    check(jsnum::jsRound(-2.5) == -2.0, "round(-2.5) is -2");
    check(jsnum::jsRound(2.5) == 3.0, "round(2.5) is 3");
    check(jsnum::jsRound(0.49999999999999994) == 0.0,
          "round(0.49999999999999994) is 0");
    check(bits(jsnum::jsRound(-0.49999999999999994)) == bits(-0.0),
          "round(-0.49999999999999994) is -0");
    check(bits(jsnum::jsRound(-0.0)) == bits(-0.0), "round(-0) keeps -0");
    check(jsnum::jsRound(4503599627370495.0) == 4503599627370495.0,
          "round(2^52-1) is exact");
    check(jsnum::jsRound(9007199254740991.0) == 9007199254740991.0,
          "round(2^53-1) is exact");
    check(std::isnan(jsnum::jsRound(std::numeric_limits<double>::quiet_NaN())),
          "round(NaN) is NaN");
    check(std::isinf(jsnum::jsRound(std::numeric_limits<double>::infinity())),
          "round(+Inf) is +Inf");
    // 独立参照との一致（代表点）
    for (double x : {-3.5, -2.5, -1.5, -1.4, -0.6, -0.5, -0.4, -0.0,
                     0.0, 0.4, 0.5, 0.6, 1.5, 2.5, 3.5, 1e300, -1e300}) {
        check(bits(jsnum::jsRound(x)) == bits(jsRoundRef(x)),
              "jsRound matches JS Math.round reference");
    }
}

void test_js_log() {
    // V8 の Math.log と同じ bit（glibc の std::log とは 1 ULP 違い得る）。
    check(bits(jsnum::jsLog(3.0)) == bits(1.0986122886681096),
          "log(3) matches V8 bits");
    check(bits(jsnum::jsLog(10.0)) == bits(2.302585092994046),
          "log(10) matches V8 bits");
    check(bits(jsnum::jsLog(0.5)) == bits(-0.6931471805599453),
          "log(0.5) matches V8 bits");
    check(bits(jsnum::jsLog(1e-7)) == bits(-16.11809565095832),
          "log(1e-7) matches V8 bits");
    check(bits(jsnum::jsLog(1e21)) == bits(48.35428695287496),
          "log(1e21) matches V8 bits");
    check(std::isinf(jsnum::jsLog(0.0)) && jsnum::jsLog(0.0) < 0,
          "log(0) is -Infinity");
    check(std::isnan(jsnum::jsLog(-1.0)), "log(-1) is NaN");
    check(std::isinf(jsnum::jsLog(std::numeric_limits<double>::infinity())),
          "log(+Inf) is +Inf");
}

void test_astral_casefold() {
    // U+10400 (Deseret capital) / U+10428 (small)
    Value a = s("\xF0\x90\x90\x80");
    Value b = s("\xF0\x90\x90\xA8");
    check(cast::compare(a, b) == 0, "Deseret case-insensitive compare");
    check(rt::strContains(a, b), "Deseret case-insensitive contains");
    check(cast::compare(s("\xC3\x84"), s("\xC3\xA4")) == 0, "BMP Ä/ä still equal");
    check(cast::compare(s("\xE2\x84\xAA"), s("k")) == 0, "Kelvin sign/k still equal");
    check(cast::compare(s("\xEA\x9F\x8E"), s("\xEA\x9F\x8F")) == 0,
          "Unicode 17 U+A7CE lowercases to U+A7CF");
    check(cast::compare(s("\xF0\x96\xBA\xA0"), s("\xF0\x96\xBA\xBB")) == 0,
          "Unicode 17 U+16EA0 lowercases to U+16EBB");
    std::u16string lower = u16ToLower(a.asStrPtr()->view());
    check(lower.size() == 2 && lower[0] == 0xD801 && lower[1] == 0xDC28,
          "astral toLowerCase yields U+10428");
}

// V8 (fdlibm) の Math.asin/acos/atan/exp とビット一致すること。
// 期待値は Node 26 (V8 14.6) の出力から取得。
void test_v8math() {
    check(bits(v8math::asin(0.7137281021284307)) ==
              bits(0.794806522356647), "asin bit-exact");
    check(bits(v8math::acos(-0.5467314076748808)) ==
              bits(2.149251875262327), "acos bit-exact");
    check(bits(v8math::atan(0.04135948024180158)) ==
              bits(0.0413359211505751), "atan bit-exact");
    check(bits(v8math::exp(0.33089555746090094)) ==
              bits(1.392214378309982), "exp bit-exact");
    check(bits(v8math::exp(-0.8499533886188995)) ==
              bits(0.42743485481332033), "exp negative bit-exact");
    check(std::isinf(v8math::exp(1e300)) && v8math::exp(1e300) > 0,
          "exp overflow is +Infinity");
    check(v8math::exp(-INFINITY) == 0.0, "exp(-Infinity) is 0");
    check(std::isnan(v8math::asin(2.0)), "asin(2) is NaN");
    check(std::isnan(v8math::acos(2.0)), "acos(2) is NaN");
    check(bits(v8math::atan(INFINITY)) ==
              bits(1.5707963267948966), "atan(Infinity) is pi/2");
    check(bits(v8math::atan2(145.56588329463045, -45.079255250489865)) ==
              bits(1.8711126121147605), "atan2 bit-exact");
    check(bits(v8math::atan2(116.55084054025008, -17.98930690750825)) ==
              bits(1.7239351718561136), "atan2 bit-exact 2");
    check(std::isnan(v8math::atan2(0.0, 0.0)) == false,
          "atan2(0,0) is finite");
    check(bits(v8math::sin(1.0)) == bits(0.8414709848078965),
          "sin bit-exact");
    check(bits(v8math::cos(1.0)) == bits(0.5403023058681398),
          "cos bit-exact");
    check(bits(v8math::sin(1e300)) == bits(-0.8178819121159085),
          "sin large argument bit-exact");
    check(bits(v8math::cos(1e300)) == bits(-0.5753861119575491),
          "cos large argument bit-exact");
}

void test_list_contents() {
    auto* l = SList::create();
    l->push(n(1)); l->push(n(2)); l->push(n(3));
    check(ts(rt::listContents(l)) == "1 2 3", "numeric list uses spaces");
    l->clear(); l->push(s("a")); l->push(n(1));
    check(ts(rt::listContents(l)) == "a 1", "mixed list uses spaces");
    l->clear(); l->push(s("a")); l->push(s("b"));
    check(ts(rt::listContents(l)) == "ab", "single-char strings join");
    l->clear(); l->push(s("a")); l->push(s("bc"));
    check(ts(rt::listContents(l)) == "a bc", "multi-char strings use spaces");
    l->clear();
    check(ts(rt::listContents(l)) == "", "empty list is empty");
}

void test_final_sigma() {
    auto low = [](const char* utf8) {
        std::u16string in(SStr::fromUtf8(utf8)->view());
        return u16ToLower(in);
    };
    check(low("ΟΣ") == std::u16string(u"ος"), "final sigma at word end");
    check(low("Σ") == std::u16string(u"σ"), "lone sigma is medial");
    check(low("ΣΣ") == std::u16string(u"σς"), "last of two sigmas final");
    check(low("ΟΣΑ") == std::u16string(u"οσα"), "sigma before letter medial");
    check(low("Ο\xCC\x81Σ") == std::u16string(u"ο\u0301ς"),
          "combining mark before sigma");
    check(low("ΟΣ\xCC\x81") == std::u16string(u"ος\u0301"),
          "combining mark after sigma");
    check(cast::compare(s("ΟΣ"), s("ος")) == 0, "compare final sigma");
    check(cast::compare(s("ΟΣ"), s("οσ")) != 0, "medial sigma not equal");
}

void test_nan_list_index() {
    check(cast::toListIndexD(Value::nan(), 3.0, true) == cast::LIST_INVALID,
          "NaN toListIndex is LIST_INVALID");
    check(cast::toListIndexD(s("abc"), 3.0, true) == cast::LIST_INVALID,
          "non-numeric toListIndex is LIST_INVALID");

    auto* l = SList::create();
    l->push(s("a")); l->push(s("b")); l->push(s("c"));
    rt::listDelete(l, Value::nan());
    check(l->len == 3 && ts(l->items[0]) == "a", "delete NaN is a no-op");
    rt::listDelete(l, s("abc"));
    check(l->len == 3, "delete non-numeric is a no-op");
    rt::listInsert(l, Value::nan(), s("X"));
    check(l->len == 3 && ts(l->items[0]) == "a", "insert NaN is a no-op");
    rt::listInsert(l, s("abc"), s("X"));
    check(l->len == 3, "insert non-numeric is a no-op");
}

} // namespace

int test_scratch_compat_main() {
    checks = failures = 0;
    test_number_string();
    test_js_round();
    test_js_log();
    test_v8math();
    test_list_contents();
    test_final_sigma();
    test_astral_casefold();
    test_nan_list_index();
    std::printf("scratch compat: %d checks, %d failures\n", checks, failures);
    return failures;
}
