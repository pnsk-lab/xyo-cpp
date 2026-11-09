// xyo-cpp: ECMAScript 準拠の数値↔文字列変換。
#include "jsnum.hpp"

#if defined(XYO_NUM_TO_STR_DRAGONBOX)
#include "v8_decimal.hpp"
#endif

#include <bit>
#include <cstdint>
#include <charconv>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>  // Darwin declares strtod_l/newlocale here.
#endif

namespace xyo::jsnum {

// C ロケールで strtod（ユーザーロケールの '.' に左右されない）
static double strtodC(const char* s) {
#ifdef _WIN32
    // equivalente MSVCRT: _create_locale/_strtod_l
    static _locale_t loc = _create_locale(LC_ALL, "C");
    return _strtod_l(s, nullptr, loc);
#else
    static locale_t loc = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(0));
    return strtod_l(s, nullptr, loc);
#endif
}

// --- ES String(x) ---

#if defined(XYO_ASM_NUM_TO_STR)
// System V AMD64: writes signed decimal digits to buffer[32].
extern "C" size_t xyo_i64_to_ascii_asm(int64_t value, char* buffer);
#endif
#if defined(XYO_ASM_ECMA_FORMAT)
// Input is the shortest positive decimal significand and decimal point
// position. Writes ECMAScript's fixed/scientific representation to buffer.
extern "C" size_t xyo_ecma_format_asm(
    const char* digits, uint32_t length, int32_t point, char* buffer);
extern "C" size_t xyo_ecma_from_scientific_asm(
    const char* scientific, uint32_t length, char* buffer);
#endif

#if defined(XYO_NUM_TO_STR_DRAGONBOX)
size_t toChars(double v, char* output) {
    if (std::isnan(v)) {
        std::memcpy(output, "NaN", 3);
        return 3;
    }
    if (v == 0) {
        output[0] = '0';  // ECMAScript String(-0) == "0".
        return 1;
    }

    char* cursor = output;
    if (v < 0) {
        *cursor++ = '-';
        v = -v;
    }
    if (std::isinf(v)) {
        std::memcpy(cursor, "Infinity", 8);
        return static_cast<size_t>(cursor + 8 - output);
    }
    constexpr double kMaxExactInt = 9007199254740992.0;
    if (v <= kMaxExactInt) {
        const long long integer = static_cast<long long>(v);
        if (static_cast<double>(integer) == v) {
#if defined(XYO_ASM_NUM_TO_STR)
            if (integer >= 10 && integer <= 99) {
                return static_cast<size_t>(cursor - output) +
                       xyo_i64_to_ascii_asm(integer, cursor);
            }
#endif
            const auto result = std::to_chars(cursor, output + 64, integer);
            return static_cast<size_t>(result.ptr - output);
        }
    }
    return static_cast<size_t>(cursor - output) +
           v8_decimal::toCharsPositive(v, cursor);
}
#endif

std::string toString(double v) {
#if defined(XYO_NUM_TO_STR_DIRECT_BUFFER)
    char buffer[64];
    const size_t count = toChars(v, buffer);
    return std::string(buffer, count);
#endif
    if (std::isnan(v)) return "NaN";
    if (v == 0) return "0";                     // -0 もここ
    // 整数 fast path: |v| <= 2^53 の整数は正確に表現でき、その十進整数が
    // 最短 round-trip 表現になる。2^53 超では 2^57 の ...870 のように
    // 正確な整数値と最短表現がずれるため、最短探索へ落とす。
    constexpr double kMaxExactInt = 9007199254740992.0;  // 2^53
    if (v >= -kMaxExactInt && v <= kMaxExactInt) {
        long long iv = static_cast<long long>(v);
        if (static_cast<double>(iv) == v) {
            char buf[32];
#if defined(XYO_ASM_NUM_TO_STR)
            // Assembly is faster for 1-2 digit numbers, but our measured
            // system std::to_chars outperforms it on larger integers.
            // Keep the native implementation for those inputs.
            if ((iv >= 10 && iv <= 99) || (iv <= -10 && iv >= -99)) {
                const size_t len = xyo_i64_to_ascii_asm(iv, buf);
                return std::string(buf, len);
            }
#endif
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), iv);
            if (ec == std::errc()) return std::string(buf, ptr);
        }
    }
    if (v < 0) return "-" + toString(-v);
    if (std::isinf(v)) return "Infinity";

#if defined(XYO_NUM_TO_STR_DRAGONBOX)
    // V8 Dragonbox gives decimal significand and exponent directly.
    // V8's SignificandToChars emits digits without scientific text parsing.
    return v8_decimal::toStringPositive(v);
#endif

    // 最短往復表現。**std::to_chars(scientific) は Ryu で最短桁を返す**
    // ので、それを桁の生成に使う（ビット一致は 40 万件のサンプル +
    // 境界値で確認済み）。桁と指数だけ取り出して、以下で ES の表記
    // ルール（固定/指数の切替は 1e21 と 1e-6）へ整形する。
    //
    // 以前は p=1..17 の "%.*e" + strtod の往復で最短 p を探していたが、
    // 非整数 1 回の変換に最大 17 回の snprintf+strtod がかかり、
    // `x/4 ++ ""` のようなループで 1.2µs/回 になっていた
    // （TurboWarp は 50ns、str_cast が 30 倍差の主因）。
    char buf[64];
    std::string_view sv;
    {
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v,
                                       std::chars_format::scientific);
        if (ec != std::errc()) {
            // 保険（通常は到達しない）: 従来の往復探索
            int p = 0;
            for (p = 1; p <= 17; ++p) {
                std::snprintf(buf, sizeof(buf), "%.*e", p - 1, v);
                if (strtodC(buf) == v) break;
            }
            sv = std::string_view(buf);
        } else {
            sv = std::string_view(buf, size_t(ptr - buf));
        }
    }

#if defined(XYO_ASM_ECMA_FORMAT)
    // Shortest-roundtrip digits are computed by std::to_chars; hand-written
    // ASM parses the scientific decimal and lays it out as ECMAScript text.
    char text[64];
    const size_t length = xyo_ecma_from_scientific_asm(
        sv.data(), static_cast<uint32_t>(sv.size()), text);
    return std::string(text, length);
#else
    int e = 0;
    size_t epos = sv.find('e');
    if (epos == std::string_view::npos) epos = sv.find('E');
    std::string_view mant = sv;
    if (epos != std::string_view::npos) {
        mant = sv.substr(0, epos);
        // 指数は "+21"/"-07" 形式 — from_chars は符号を受理しないので手動で
        std::string_view es = sv.substr(epos + 1);
        bool eneg = false;
        if (!es.empty() && (es[0] == '+' || es[0] == '-')) {
            eneg = es[0] == '-';
            es.remove_prefix(1);
        }
        std::from_chars(es.data(), es.data() + es.size(), e);
        if (eneg) e = -e;
    }

    size_t dot = mant.find('.');
    int fracLen = dot == std::string_view::npos ? 0 : static_cast<int>(mant.size() - dot - 1);

    // digits: 先頭の 0 は除く（"0.001" → "1"）
    char digits[32];
    int k = 0;
    for (char c : mant) {
        if (c == '.') continue;
        if (k == 0 && c == '0') continue;  // 先頭ゼロ除去
        digits[k++] = c;
    }
    digits[k] = '\0';
    int n = e - fracLen + k;

    // Legacy C++ formatter: reference path and non-ASM platforms.
    std::string out;
    if (k <= n && n <= 21) {
        // s の後に n-k 個の 0
        out.append(digits, k).append(n - k, '0');
    } else if (0 < n && n <= 21) {
        out.append(digits, n).push_back('.');
        out.append(digits + n, k - n);
    } else if (-6 < n && n <= 0) {
        out = "0.";
        out.append(-n, '0').append(digits, k);
    } else {
        // 指数表記: e は常に符号付き・ゼロ詰めなし
        out.push_back(digits[0]);
        if (k > 1) {
            out.push_back('.');
            out.append(digits + 1, k - 1);
        }
        out.push_back('e');
        int ex = n - 1;
        out.push_back(ex < 0 ? '-' : '+');
        char eb[16];
        auto [ep, eec] = std::to_chars(eb, eb + sizeof(eb), ex < 0 ? -ex : ex);
        out.append(eb, ep);
    }
    return out;
#endif
}

std::u16string toString16(double v) {
    std::string s = toString(v);
    return {s.begin(), s.end()};
}

// --- ES Number(str) ---

bool isStrWhiteSpace(char16_t c) {
    switch (c) {
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
    case 0x0020: case 0x00A0: case 0x1680:
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
    case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
    case 0x3000: case 0xFEFF:
        return true;
    default:
        return false;
    }
}

static bool isDigit(char16_t c) { return c >= u'0' && c <= u'9'; }
static bool isHexDigit(char16_t c) {
    return isDigit(c) || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
}

double toNumber(std::u16string_view sv) {
    // 前後 StrWhiteSpace 除去
    size_t lo = 0, hi = sv.size();
    while (lo < hi && isStrWhiteSpace(sv[lo])) ++lo;
    while (hi > lo && isStrWhiteSpace(sv[hi - 1])) --hi;
    if (lo == hi) return 0;                     // 空 or 空白のみ → +0
    sv = sv.substr(lo, hi - lo);

    // One-character numeric strings ("0".."9") are common after
    // joins and list access. They do not need sign/radix detection,
    // integer overflow checks, or the general decimal parser.
    if (sv.size() == 1) {
        // Whitespace was already stripped, and no other single UTF-16
        // code unit is a valid ECMAScript NumericLiteral.
        return isDigit(sv[0])
            ? static_cast<double>(sv[0] - u'0')
            : std::numeric_limits<double>::quiet_NaN();
    }

    // 0x / 0o / 0b 接頭辞は符号を取らない（ES: 符号は StrDecimalLiteral のみ）
    if (sv.size() >= 2 && sv[0] == u'0') {
        char16_t p = sv[1];
        if (p == u'x' || p == u'X' || p == u'o' || p == u'O' || p == u'b' || p == u'B') {
            int base = (p == u'x' || p == u'X') ? 16 : (p == u'o' || p == u'O') ? 8 : 2;
            auto rest = sv.substr(2);
            if (rest.empty()) return std::numeric_limits<double>::quiet_NaN();
            // Short binary/octal/hexadecimal values fit exactly in the
            // IEEE-754 significand. Select the integer decoder ONCE based on
            // the digit count, not per digit. Larger inputs retain the
            // original FP recurrence (and therefore the original rounding).
            const unsigned maxExactDigits =
                base == 16 ? 13u : base == 8 ? 17u : 53u;
            if (rest.size() <= maxExactDigits) {
                uint64_t exact = 0;
                const unsigned shift = base == 16 ? 4u :
                                       base == 8 ? 3u : 1u;
                for (char16_t c : rest) {
                    int d;
                    if (isDigit(c)) d = c - u'0';
                    else if (c >= u'a' && c <= u'f') d = c - u'a' + 10;
                    else if (c >= u'A' && c <= u'F') d = c - u'A' + 10;
                    else return std::numeric_limits<double>::quiet_NaN();
                    if (d >= base) return std::numeric_limits<double>::quiet_NaN();
                    exact = (exact << shift) | unsigned(d);
                }
                return static_cast<double>(exact);
            }
            double v = 0;
            for (char16_t c : rest) {
                int d;
                if (isDigit(c)) d = c - u'0';
                else if (c >= u'a' && c <= u'f') d = c - u'a' + 10;
                else if (c >= u'A' && c <= u'F') d = c - u'A' + 10;
                else return std::numeric_limits<double>::quiet_NaN();
                if (d >= base) return std::numeric_limits<double>::quiet_NaN();
                v = v * base + d;
            }
            return v;
        }
    }

    bool neg = false;
    if (sv[0] == u'+' || sv[0] == u'-') {
        neg = sv[0] == u'-';
        sv.remove_prefix(1);
        if (sv.empty()) return std::numeric_limits<double>::quiet_NaN();
        // A sign followed by exactly one digit is an exact conversion.
        // Unary minus on zero must retain the IEEE-754 negative-zero bit.
        if (sv.size() == 1 && isDigit(sv[0])) {
            const double d = static_cast<double>(sv[0] - u'0');
            return neg ? -d : d;
        }
    }

    // Infinity
    if (sv == u"Infinity") {
        return neg ? -std::numeric_limits<double>::infinity()
                   : std::numeric_limits<double>::infinity();
    }

    // Most first-use Scratch numeric strings are small decimal integers.
    // Parse them without locale setup, a temporary ASCII buffer, or strtod.
    // Only return directly while the integer is exactly representable as a
    // double; larger values must use the correctly-rounded decimal parser.
    constexpr uint64_t kMaxExactInteger = 9007199254740992ull; // 2^53
    if (sv.size() <= 18 && isDigit(sv[0])) {
        uint64_t integer = 0;
        bool integral = true;
        for (char16_t c : sv) {
            if (!isDigit(c)) {
                integral = false;
                break;
            }
            unsigned digit = static_cast<unsigned>(c - u'0');
            if (integer > (kMaxExactInteger - digit) / 10) {
                integral = false;
                break;
            }
            integer = integer * 10 + digit;
        }
        if (integral) {
            double value = static_cast<double>(integer);
            return neg ? -value : value; // Also preserves -0.
        }
    }

#if !defined(__APPLE__)
    // Apple libc++ (including Xcode 16.4) does not provide floating-point
    // std::from_chars. On macOS, use the existing validated, correctly
    // rounded C-locale strtod_l path below instead.
    // Most decimal strings in Scratch are short. From_chars is a full
    // consumer of the decimal grammar once the leading sign is removed;
    // require complete consumption and an ASCII numeric starting character
    // to reject extensions such as "nan", "inf", or trailing junk. This
    // avoids the separate UTF-16 grammar scan for typical decimals.
    // Overflow/underflow are delegated to the existing C-locale fallback.
    if (sv.size() < 64 && (isDigit(sv[0]) || sv[0] == u'.')) {
        char ascii[64];
        const size_t length = sv.size();
        for (size_t j = 0; j < length; ++j) {
            if (sv[j] > 0x7f)
                return std::numeric_limits<double>::quiet_NaN();
            ascii[j] = static_cast<char>(sv[j]);
        }
        double result = 0;
        auto parsed = std::from_chars(
            ascii, ascii + length, result, std::chars_format::general);
        if (parsed.ptr == ascii + length) {
            if (parsed.ec == std::errc())
                return neg ? -result : result;
            if (parsed.ec == std::errc::result_out_of_range) {
                ascii[length] = '\0';
                result = strtodC(ascii);
                return neg ? -result : result;
            }
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
#endif

    // Long decimal literals use the existing full grammar validation.
    // 10 進リテラルの形状検証: [0-9]* (. [0-9]*)? ([eE][+-]?[0-9]+)? で少なくとも1桁
    size_t i = 0, n = sv.size();
    bool anyDigit = false;
    while (i < n && isDigit(sv[i])) { ++i; anyDigit = true; }
    if (i < n && sv[i] == u'.') {
        ++i;
        while (i < n && isDigit(sv[i])) { ++i; anyDigit = true; }
    }
    if (!anyDigit) return std::numeric_limits<double>::quiet_NaN();
    if (i < n && (sv[i] == u'e' || sv[i] == u'E')) {
        ++i;
        if (i < n && (sv[i] == u'+' || sv[i] == u'-')) ++i;
        if (i >= n || !isDigit(sv[i])) return std::numeric_limits<double>::quiet_NaN();
        while (i < n && isDigit(sv[i])) ++i;
    }
    if (i != n) return std::numeric_limits<double>::quiet_NaN();

    // 検証済み ASCII → strtod_l(C locale)（全ケースで正しく丸められる）
    char small[64];
    std::string big;
    char* dst = small;
    if (n + 1 >= sizeof(small)) {
        big.resize(n + 1);
        dst = big.data();
    }
    for (size_t j = 0; j < n; ++j) dst[j] = static_cast<char>(sv[j]);
    dst[n] = '\0';
    // from_chars is locale-independent and ordinarily uses a fast
    // correctly-rounded decimal converter. We have already validated the
    // ECMAScript grammar. Preserve strtod_l for out-of-range values,
    // including overflow to Infinity and underflow to signed zero.
    double v;
#if defined(__APPLE__)
    v = strtodC(dst);
#else
    auto parsed = std::from_chars(dst, dst + n, v,
                                  std::chars_format::general);
    if (parsed.ec != std::errc() || parsed.ptr != dst + n)
        v = strtodC(dst);
#endif
    return neg ? -v : v;
}

double toNumberAscii(std::string_view s) {
    // char → char16_t に写して共通パスへ
    char16_t buf[512];
    std::u16string tmp;
    std::u16string_view sv;
    if (s.size() <= sizeof(buf) / sizeof(buf[0])) {
        for (size_t i = 0; i < s.size(); ++i) buf[i] = static_cast<char16_t>(s[i]);
        sv = {buf, s.size()};
    } else {
        tmp.assign(s.begin(), s.end());
        sv = tmp;
    }
    return toNumber(sv);
}

double jsRound(double x) {
    // ES Math.round: 有限なら floor(x + 0.5)。ただし
    //   -0.5 <= x < 0     -> -0
    //    0   <= x < 0.5   -> +0
    //   |x| >= 2^52       -> x（端数が表現できない。x+0.5 の丸めで誤らない）
    // とする。単純な floor(x+0.5) は 0.49999999999999994 を 1 にしてしまう。
    if (!std::isfinite(x) || x == 0.0) return x;
    if (x > 0.0 && x < 0.5) return 0.0;
    if (x < 0.0 && x >= -0.5) return -0.0;
    constexpr double kNoFraction = 4503599627370496.0;  // 2^52
    if (x >= kNoFraction || x <= -kNoFraction) return x;
    return std::floor(x + 0.5);
}

// fdlibm e_log.c の移植。元の著作権表示（permissive）を保持する:
//
// ====================================================
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//
// Developed at SunSoft, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice
// is preserved.
// ====================================================
//
// V8 の Math.log と同じ結果をプラットフォーム非依存で返すため、
// glibc の std::log は使わない（1 ULP 違うことがある）。
double jsLog(double x) {
    constexpr double ln2_hi = 6.93147180369123816490e-01;
    constexpr double ln2_lo = 1.90821492927058770002e-10;
    constexpr double two54 = 1.80143985094819840000e+16;
    constexpr double Lg1 = 6.666666666666735130e-01;
    constexpr double Lg2 = 3.999999999940941908e-01;
    constexpr double Lg3 = 2.857142874366239149e-01;
    constexpr double Lg4 = 2.222219843214978396e-01;
    constexpr double Lg5 = 1.818357216161805012e-01;
    constexpr double Lg6 = 1.531383769920937332e-01;
    constexpr double Lg7 = 1.479819860511658591e-01;

    uint64_t bits = std::bit_cast<uint64_t>(x);
    int32_t hx = static_cast<int32_t>(bits >> 32);
    uint32_t lx = static_cast<uint32_t>(bits);
    int32_t k = 0;
    if (hx < 0x00100000) {                      // x < 2^-1022
        if ((static_cast<uint32_t>(hx & 0x7fffffff) | lx) == 0)
            return -std::numeric_limits<double>::infinity();  // log(±0)
        if (hx < 0) return std::numeric_limits<double>::quiet_NaN();
        k -= 54;                                // subnormal: scale up
        x *= two54;
        bits = std::bit_cast<uint64_t>(x);
        hx = static_cast<int32_t>(bits >> 32);
    }
    if (hx >= 0x7ff00000) return x + x;         // +Inf / NaN
    k += (hx >> 20) - 1023;
    hx &= 0x000fffff;
    int32_t i = (hx + 0x95f64) & 0x100000;
    bits = (bits & 0xffffffffull) |
           (static_cast<uint64_t>(hx | (i ^ 0x3ff00000)) << 32);
    x = std::bit_cast<double>(bits);            // normalize x or x/2
    k += (i >> 20);
    double f = x - 1.0;
    if ((0x000fffff & (2 + hx)) < 3) {          // -2^-20 <= f < 2^-20
        if (f == 0.0) {
            if (k == 0) return 0.0;
            const double dk = static_cast<double>(k);
            return dk * ln2_hi + dk * ln2_lo;
        }
        double R = f * f * (0.5 - 0.33333333333333333 * f);
        if (k == 0) return f - R;
        const double dk = static_cast<double>(k);
        return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
    double s = f / (2.0 + f);
    const double dk = static_cast<double>(k);
    double z = s * s;
    i = hx - 0x6147a;
    double w = z * z;
    int32_t j = 0x6b851 - hx;
    double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    i |= j;
    double R = t2 + t1;
    if (i > 0) {
        const double hfsq = 0.5 * f * f;
        if (k == 0) return f - (hfsq - s * (hfsq + R));
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    }
    if (k == 0) return f - s * (f - R);
    return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

} // namespace xyo::jsnum
