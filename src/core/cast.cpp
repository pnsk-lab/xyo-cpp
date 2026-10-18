// xyo-cpp: util/cast.js の移植。
#include "cast.hpp"

#include <cmath>
#include <cstdlib>

#include "rng.hpp"
#include "slist.hpp"

namespace xyo::cast {

static bool strIsNotActuallyZero(const SStr* s) {
    if (!s) return true;                    // 空文字列
    for (uint32_t i = 0; i < s->len; ++i) {
        char16_t c = s->data()[i];
        if ((c >= u'0' && c <= u'9') || c == 9) return false;
    }
    return true;
}

/// jsnum::toNumber の Infinity リテラル受理条件と同型
/// （trim + 符号除去 + "Infinity" 一致）。
static bool isInfinityLiteral(std::u16string_view sv) {
    size_t lo = 0, hi = sv.size();
    while (lo < hi && jsnum::isStrWhiteSpace(sv[lo])) ++lo;
    while (hi > lo && jsnum::isStrWhiteSpace(sv[hi - 1])) --hi;
    sv = sv.substr(lo, hi - lo);
    if (!sv.empty() && (sv[0] == u'+' || sv[0] == u'-'))
        sv.remove_prefix(1);
    return sv == u"Infinity";
}

bool isNotActuallyZero(Value v) {
    if (!v.isString()) return false;
    return strIsNotActuallyZero(v.asStrPtr());
}

/// Number(v) 相当（NaN を潰さない内部版）
static double numberOrNaNImpl(Value v) {
    if (v.isNumber()) return v.asNumberUnchecked();
    if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
    if (v.isString()) {
        SStr* s = v.asStrPtr();
        if (!s) return 0.0;                 // "" → +0
        // 文字列は不変 — パース結果を SStr 内にメモして再パースを防ぐ。
        // numState: 1=数値（compare でも数値）, 2=NaN,
        // 3=toNumber→0 だが isNotActuallyZero により compare では NaN,
        // 4=Infinity リテラル（"infinity" のような case 違いは
        //   state2 になるが文字列比較では等しい — state ショートカット
        //   で誤って非等値にしないため別 state にする）
        if (s->numState == 0) {
            double d = jsnum::toNumber(s->view());
            s->numCache = d;
            if (std::isnan(d)) s->numState = 2;
            else if (d == 0 && strIsNotActuallyZero(s)) s->numState = 3;
            else if (std::isinf(d) && isInfinityLiteral(s->view()))
                s->numState = 4;
            else s->numState = 1;
        }
        return s->numState == 2
                   ? std::numeric_limits<double>::quiet_NaN()
                   : s->numCache;
    }
    if (v.isColor()) {
        // JS では [r,g,b] 配列 → "r,g,b" → Number → NaN
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double toNumberOrNaN(Value v) { return numberOrNaNImpl(v); }

double toNumber(Value v) {
    double n = numberOrNaNImpl(v);
    return std::isnan(n) ? 0.0 : n;
}

bool toBoolean(Value v) {
    if (v.isBool()) return v.asBool();
    if (v.isString()) {
        SStr* s = v.asStrPtr();
        if (!s || s->len == 0) return false;          // ''
        if (s->len == 1 && s->data()[0] == u'0') return false; // '0'
        // 'false'（大文字小文字無視）
        if (s->len == 5) {
            static const char16_t kFalse[] = u"false";
            for (int i = 0; i < 5; ++i) {
                char16_t c = s->data()[i];
                if (c >= u'A' && c <= u'Z') c += 32;
                if (c != kFalse[i]) return true;
            }
            return false;
        }
        return true;
    }
    if (v.isNumber()) {
        double d = v.asNumberUnchecked();
        return d != 0 && !std::isnan(d);    // 0/-0/NaN → false
    }
    if (v.isColor()) return true;           // JS: オブジェクトは truthy
    return false;
}

SStr* toString(Value v) {
    if (v.isString()) {
        SStr* s = v.asStrPtr();
        if (s) return s;
        return SStr::create(std::u16string_view{});
    }
    if (v.isNumber()) return SStr::create(jsnum::toString16(v.asNumberUnchecked()));
    if (v.isBool()) return sstrFromAscii(v.asBool() ? "true" : "false");
    if (v.isColor()) {
        char buf[24];
        uint64_t b = v.bits & 0xFFFFFF;
        int len = std::snprintf(buf, sizeof(buf), "%d,%d,%d",
                                int((b >> 16) & 0xFF), int((b >> 8) & 0xFF), int(b & 0xFF));
        return sstrFromAscii({buf, static_cast<size_t>(len)});
    }
    if (v.isUndefined()) return sstrFromAscii("undefined");
    return sstrFromAscii("");
}

Value toStringValue(Value v) {
    if (v.isString()) return v;
    if (v.isNumber()) {
        std::u16string s = jsnum::toString16(v.asNumberUnchecked());
        if (s.empty()) return Value::emptyStr();
        return Value::string(SStr::create(std::u16string_view(s)));
    }
    if (v.isBool()) return Value::string(sstrFromAscii(v.asBool() ? "true" : "false"));
    if (v.isColor()) {
        char buf[24];
        uint64_t b = v.bits & 0xFFFFFF;
        int len = std::snprintf(buf, sizeof(buf), "%d,%d,%d",
                                int((b >> 16) & 0xFF), int((b >> 8) & 0xFF), int(b & 0xFF));
        return Value::string(sstrFromAscii({buf, static_cast<size_t>(len)}));
    }
    if (v.isUndefined()) return Value::string(sstrFromAscii("undefined"));
    return Value::emptyStr();
}

double compare(Value v1, Value v2) {
    double n1 = numberOrNaNImpl(v1);
    double n2 = numberOrNaNImpl(v2);
    if (n1 == 0 && isNotActuallyZero(v1)) {
        n1 = std::numeric_limits<double>::quiet_NaN();
    } else if (n2 == 0 && isNotActuallyZero(v2)) {
        n2 = std::numeric_limits<double>::quiet_NaN();
    }
    if (std::isnan(n1) || std::isnan(n2)) {
        // 小文字化文字列比較
        SStr* s1 = toString(v1);
        SStr* s2 = toString(v2);
        return compareStringsCaseInsensitive(s1->view(), s2->view());
    }
    if ((n1 == std::numeric_limits<double>::infinity() &&
         n2 == std::numeric_limits<double>::infinity()) ||
        (n1 == -std::numeric_limits<double>::infinity() &&
         n2 == -std::numeric_limits<double>::infinity())) {
        return 0;
    }
    return n1 - n2;
}

bool isInt(Value v) {
    if (v.isNumber()) {
        double d = v.asNumberUnchecked();
        if (std::isnan(d)) return true;
        return d == std::floor(d);
    }
    if (v.isBool()) return true;
    if (v.isString()) {
        SStr* s = v.asStrPtr();
        if (!s) return true;
        for (uint32_t i = 0; i < s->len; ++i)
            if (s->data()[i] == u'.') return false;
        return true;
    }
    return false;
}

bool isWhiteSpace(Value v) {
    // JS: val === null || (typeof val === 'string' && val.trim().length === 0)
    // 非文字列・非 null は false
    if (!v.isString()) return false;
    SStr* s = v.asStrPtr();
    if (!s) return true;
    for (uint32_t i = 0; i < s->len; ++i)
        if (!jsnum::isStrWhiteSpace(s->data()[i])) return false;
    return true;
}

double toListIndexD(Value index, double length, bool acceptAll) {
    // util/cast.js toListIndex: 非数値は Number() 変換→NaN がそのまま返る
    // 経路を保持する（'all' で acceptAll=false 等）。
    double d = index.asNumberUnchecked();
    if (!index.isNumber()) {
        if (index.isString()) {
            SStr* s = index.asStrPtr();
            if (s) {
                auto sv = s->view();
                if (sv == u"all")
                    return acceptAll ? LIST_ALL : double(LIST_INVALID);
                if (sv == u"last")
                    return length > 0 ? length : double(LIST_INVALID);
                if (sv == u"random" || sv == u"any") {
                    if (length > 0)
                        return 1 + std::floor(
                            Rng::global().nextDouble() * length);
                    return LIST_INVALID;
                }
            }
        }
        d = numberOrNaNImpl(index);
    }
    d = std::floor(d);
    if (d < 1 || d > length) return LIST_INVALID;
    return d;
}

int32_t listIndex(Value index, uint32_t length) {
    if (!index.isNumber()) {
        if (index.isString()) {
            SStr* s = index.asStrPtr();
            if (s) {
                // fast path: 純粋な10進整数文字列（Scratch のリスト
                // インデックスに使われる文字列はほぼこの形）。特殊名
                // は数字を含まないので先に走査しても結果は変わらない。
                // jsnum::toNumber の全 JS 構文解析を避け、numCache も
                // 充填して JIT の numState fast path に乗せる。
                uint32_t n = s->len;
                if (n >= 1 && n <= 10) {
                    const char16_t* d = s->data();
                    uint64_t v = 0;   // 10 桁は u32 を超え得る
                    bool digits = true;
                    for (uint32_t k = 0; k < n; ++k) {
                        char16_t c = d[k];
                        if (c < u'0' || c > u'9') { digits = false; break; }
                        v = v * 10 + uint64_t(c - u'0');
                    }
                    if (digits) {
                        s->numCache = static_cast<double>(v);
                        s->numState = 1;
                        if (v < 1 || v > length) return -1;
                        return static_cast<int32_t>(v) - 1;
                    }
                }
                auto sv = s->view();
                if (sv == u"last") return static_cast<int32_t>(length) - 1;
                if (sv == u"random" || sv == u"any") {
                    if (length > 0)
                        return static_cast<int32_t>(
                            Rng::global().nextDouble() * length);
                    return -1;
                }
            }
        }
        // (+index || 0) | 0 — NaN/非数値は 0
        double d = numberOrNaNImpl(index);
        int32_t i = std::isnan(d) ? 0 : static_cast<int32_t>(d);
        if (i < 1 || i > static_cast<int32_t>(length)) return -1;
        return i - 1;
    }
    // number: index|0 (ToInt32、trunc)
    int32_t i = static_cast<int32_t>(index.asNumberUnchecked());
    if (i < 1 || i > static_cast<int32_t>(length)) return -1;
    return i - 1;
}

Rgba toRgbColorObject(Value v) {
    if (v.isString()) {
        SStr* s = v.asStrPtr();
        if (s) {
            // 文字列は不変 — "#RRGGBB" パース結果を SStr 内にメモ
            // （utf8 変換アロケートを再実行しない）。
            // rgbState: 1=rgbCache 有効, 2='#' 始まりでない
            if (s->rgbState == 1) {
                uint32_t c = s->rgbCache;
                return {int((c >> 16) & 0xFF), int((c >> 8) & 0xFF),
                        int(c & 0xFF), int((c >> 24) & 0xFF)};
            }
            if (s->rgbState == 0) {
                if (s->len >= 1 && s->data()[0] == u'#') {
                    std::string utf8 = SStr::toUtf8(s->view());
                    auto rgb = color::hexToRgb(utf8);
                    Rgba out = rgb ? Rgba{rgb->r, rgb->g, rgb->b, 255}
                                   : Rgba{0, 0, 0, 255};
                    s->rgbCache = uint32_t(out.a) << 24 |
                                  uint32_t(out.r) << 16 |
                                  uint32_t(out.g) << 8 |
                                  uint32_t(out.b);
                    s->rgbState = 1;
                    return out;
                }
                s->rgbState = 2;    // '#' 以外 → toNumber decimal 経路
            }
        }
    }
    double n = toNumber(v);
    return color::decimalToRgb(static_cast<int64_t>(n));
}

Rgb toRgbColorList(Value v) {
    Rgba c = toRgbColorObject(v);
    return {c.r, c.g, c.b};
}

} // namespace xyo::cast
