// xyo-cpp: ECMAScript 準拠の数値↔文字列変換。
#include "jsnum.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>
#include <locale.h>

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

std::string toString(double v) {
    if (std::isnan(v)) return "NaN";
    if (v == 0) return "0";                     // -0 もここ
    // 整数 fast path: i64 に往復できる整数は ES 表記が十進整数
    // （|v|<1e21 なら指数表記にならず 9.2e18<1e21<2^63）。往復探索を省略。
    if (v > -9.2e18 && v < 9.2e18) {
        long long iv = static_cast<long long>(v);
        if (static_cast<double>(iv) == v) {
            char buf[24];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), iv);
            if (ec == std::errc()) return std::string(buf, ptr);
        }
    }
    if (v < 0) return "-" + toString(-v);
    if (std::isinf(v)) return "Infinity";

    // 最短往復表現: 精度 p=1..17 の "%.*e" で往復する最小 p を探す。
    // %.*e は正しく丸められる（最近接・同距離は偶数）ので ES の digits 選択と一致する。
    // （libstdc++ の std::to_chars は一部ケースで最短を返さないことが判明）
    char buf[32];
    int p = 0;
    for (p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof(buf), "%.*e", p - 1, v);
        double back = strtodC(buf);
        if (back == v) break;
    }
    std::string_view sv(buf);

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

    std::string out;
    out.reserve(32);
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

    // 0x / 0o / 0b 接頭辞は符号を取らない（ES: 符号は StrDecimalLiteral のみ）
    if (sv.size() >= 2 && sv[0] == u'0') {
        char16_t p = sv[1];
        if (p == u'x' || p == u'X' || p == u'o' || p == u'O' || p == u'b' || p == u'B') {
            int base = (p == u'x' || p == u'X') ? 16 : (p == u'o' || p == u'O') ? 8 : 2;
            auto rest = sv.substr(2);
            if (rest.empty()) return std::numeric_limits<double>::quiet_NaN();
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
    }

    // Infinity
    if (sv == u"Infinity") {
        return neg ? -std::numeric_limits<double>::infinity()
                   : std::numeric_limits<double>::infinity();
    }

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
    double v = strtodC(dst);
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

} // namespace xyo::jsnum
