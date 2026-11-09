// xyo-cpp: util/cast.js の移植。
#include "cast.hpp"
#include "../ir/value_abi.hpp"

#include <cmath>
#include <cstdlib>

#include "rng.hpp"
#include "slist.hpp"

namespace xyo::cast {

static bool strIsNotActuallyZero(const SStr* s) {
    if (!s) return true;                    // 空文字列
    const char16_t* data = s->data();
    for (uint32_t i = 0; i < s->len; ++i) {
        char16_t c = data[i];
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

// The state-dependent part of String -> Number is kept in one place for
// interpreter and JIT. Called only when the caller saw numState == 0.
// Concurrent publishers are safe because immutable SStr contents produce
// identical values; publishNumCache performs release/acquire synchronization.
static double parseNumberStringUncachedRaw(SStr* s) {
    // Numeric strings consisting of one ASCII digit need neither UTF-16
    // flattening nor the general ECMAScript parser. Publish the same state
    // (1 = genuinely numeric, including "0") as the generic path.
    if (s->len == 1 || s->len == 2) {
        // Flat strings store UTF-16 directly after the SStr header. Avoid
        // a non-inlined unitAt() call in the overwhelmingly common case;
        // composite strings still use unitAt() to avoid materialization.
        const bool flat = !s->isComposite() && !s->isLatin1();
        const char16_t* body = flat ? s->inlineBody() : nullptr;
        const char16_t first = flat ? body[0] : s->unitAt(0);
        if (s->len == 1) {
            if (first >= u'0' && first <= u'9') {
                const double d = static_cast<double>(first - u'0');
                s->publishNumCache(d, 1);
                return d;
            }
            // All other one-unit strings are either ECMAScript whitespace
            // (numeric zero) or invalid numeric literals. Keep Scratch's
            // historical numState distinction: TAB has numeric state 1,
            // other whitespace state 3, and invalid strings state 2.
            if (jsnum::isStrWhiteSpace(first)) {
                s->publishNumCache(0.0, first == u'\t' ? 1u : 3u);
                return 0.0;
            }
            const double invalid = std::numeric_limits<double>::quiet_NaN();
            s->publishNumCache(invalid, 2);
            return invalid;
        }
        if (s->len == 2 && (first == u'+' || first == u'-')) {
            const char16_t second = flat ? body[1] : s->unitAt(1);
            if (second >= u'0' && second <= u'9') {
                const double magnitude = static_cast<double>(second - u'0');
                const double result = first == u'-' ? -magnitude : magnitude;
                s->publishNumCache(result, 1);
                return result;
            }
        }
    }
    double d = jsnum::toNumber(s->view());
    uint32_t state;
    if (std::isnan(d)) state = 2;
    else if (d == 0 && strIsNotActuallyZero(s)) state = 3;
    else if (std::isinf(d) && isInfinityLiteral(s->view()))
        state = 4;
    else state = 1;
    s->publishNumCache(d, state);
    return state == 2 ? std::numeric_limits<double>::quiet_NaN() : d;
}

double parseNumberStringUncached(SStr* s) {
    double d = parseNumberStringUncachedRaw(s);
    return std::isnan(d) ? 0.0 : d;
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
        uint32_t state = s->loadNumState();
        if (state == 0) return parseNumberStringUncachedRaw(s);
        return state == 2
                   ? std::numeric_limits<double>::quiet_NaN()
                   : s->loadNumCache();
    }
    if (v.isColor()) {
        // JS では [r,g,b] 配列 → "r,g,b" → Number → NaN
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double toNumberOrNaN(Value v) { return numberOrNaNImpl(v); }

double toNumber(Value v) {
    // Fast immediate tags are shared with LLVM/WASM's scratch-value ABI.
    // Heap strings retain the existing UTF-16 numeric cache/parser.
    const auto immediate = ir::toNumberImmediate(v.bits);
    if (!immediate.needsStringParse) return immediate.value;
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
            const char16_t* data = s->data();
            for (int i = 0; i < 5; ++i) {
                char16_t c = data[i];
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

// Immutable SStr values commonly produced by string conversion. Return a
// rooted singleton instead of allocating on each interpreter/JIT call.
// The root provider is installed before any of these objects are allocated.
namespace {
enum class CommonString : uint8_t { Empty, True, False, Undefined };
SStr* g_commonStrings[4] = {};

SStr* commonString(CommonString value) {
    static const bool initialized = [] {
        Gc::get().addRootProvider(g_commonStrings, [](Gc& gc) {
            for (SStr* str : g_commonStrings) gc.markPtr(str);
        });
        g_commonStrings[0] = sstrFromAscii("");
        g_commonStrings[1] = sstrFromAscii("true");
        g_commonStrings[2] = sstrFromAscii("false");
        g_commonStrings[3] = sstrFromAscii("undefined");
        return true;
    }();
    (void)initialized;
    return g_commonStrings[static_cast<unsigned>(value)];
}
} // namespace

// Small integers repeat constantly in Scratch (`i ++ ""`, `join(i, ...)`).
// V8 keeps a number-string cache; return the same immutable SStr instead of
// formatting and allocating one per conversion. The entries are GC roots.
namespace {
constexpr int kSmallIntMin = -128;
constexpr int kSmallIntMax = 1023;
SStr* g_smallInts[kSmallIntMax - kSmallIntMin + 1] = {};
SStr* const* smallIntTable() {
    static SStr* const* table = [] {
        Gc::get().addRootProvider(g_smallInts, [](Gc& g) {
            for (SStr* s : g_smallInts) g.markPtr(s);
        });
        for (int i = kSmallIntMin; i <= kSmallIntMax; ++i) {
            SStr* str = sstrFromAscii(std::to_string(i));
            // A cached integer string has a known numeric meaning; avoid
            // running the cold parser the first time Scratch reads it back.
            str->publishNumCache(static_cast<double>(i), 1);
            g_smallInts[i - kSmallIntMin] = str;
        }
        return g_smallInts;
    }();
    return table;
}
} // namespace

SStr* smallIntString(double d) {
    // Check the range before conversion so NaN and huge values never reach an
    // out-of-range floating-to-integer conversion.
    if (!(d >= double(kSmallIntMin) && d <= double(kSmallIntMax))) return nullptr;
    const int i = static_cast<int>(d);
    if (double(i) != d) return nullptr;
    return smallIntTable()[i - kSmallIntMin];
}

// Canonical String(number) is ASCII and round-trips to the original number,
// except that String(-0) is "0" and reparses as positive zero. Seed the same
// cache states as numberOrNaNImpl without parsing the freshly formatted text.
static SStr* numberToString(double d) {
    if (SStr* cached = smallIntString(d)) return cached;
#if defined(XYO_NUM_TO_STR_DIRECT_BUFFER)
    // The number is formatted once into an on-stack buffer and copied
    // directly into the GC-owned UTF-16 string. Avoid an intermediate
    // std::string allocation for 16+ byte decimal representations.
    char buffer[64];
    const size_t length = jsnum::toChars(d, buffer);
    SStr* s = sstrFromAscii(std::string_view(buffer, length));
#else
    SStr* s = sstrFromAscii(jsnum::toString(d));
#endif
    const uint32_t state = std::isnan(d) ? 2 : (std::isinf(d) ? 4 : 1);
    const double parsed = std::isnan(d)
        ? std::numeric_limits<double>::quiet_NaN()
        : (d == 0 ? 0.0 : d);
    s->publishNumCache(parsed, state);
    return s;
}

SStr* toString(Value v) {
    if (v.isString()) {
        SStr* s = v.asStrPtr();
        if (s) return s;
        return commonString(CommonString::Empty);
    }
    if (v.isNumber()) return numberToString(v.asNumberUnchecked());
    if (v.isBool())
        return commonString(v.asBool() ? CommonString::True
                                      : CommonString::False);
    if (v.isColor()) {
        char buf[24];
        uint64_t b = v.bits & 0xFFFFFF;
        int len = std::snprintf(buf, sizeof(buf), "%d,%d,%d",
                                int((b >> 16) & 0xFF), int((b >> 8) & 0xFF), int(b & 0xFF));
        return sstrFromAscii({buf, static_cast<size_t>(len)});
    }
    if (v.isUndefined()) return commonString(CommonString::Undefined);
    return commonString(CommonString::Empty);
}

Value toStringValue(Value v) {
    if (v.isString()) return v;
    if (v.isNumber()) return Value::string(numberToString(v.asNumberUnchecked()));
    if (v.isBool())
        return Value::string(commonString(v.asBool() ? CommonString::True
                                                      : CommonString::False));
    if (v.isColor()) {
        char buf[24];
        uint64_t b = v.bits & 0xFFFFFF;
        int len = std::snprintf(buf, sizeof(buf), "%d,%d,%d",
                                int((b >> 16) & 0xFF), int((b >> 8) & 0xFF), int(b & 0xFF));
        return Value::string(sstrFromAscii({buf, static_cast<size_t>(len)}));
    }
    if (v.isUndefined())
        return Value::string(commonString(CommonString::Undefined));
    return Value::emptyStr();
}

// Scratch comparisons use numeric ordering only for valid numeric operands.
// numState already records whether a parsed string's zero is lexical
// (state 3), so checking it avoids an O(length) rescan on *every* compare.
// Keep this separate from toNumber: arithmetic deliberately treats states
// 2/3 as zero, whereas comparisons must fall back to case-folded text.
static bool numericCompareOperand(Value v, double& n) {
    if (v.isNumber()) {
        n = v.asNumberUnchecked();
        return !std::isnan(n);
    }
    if (v.isBool()) {
        n = v.asBool() ? 1.0 : 0.0;
        return true;
    }
    if (!v.isString()) return false;
    SStr* s = v.asStrPtr();
    if (!s) return false; // The empty string is a lexical zero, not numeric.
    uint32_t state = s->loadNumState();
    if (state == 0) {
        // Preserve the single shared parser and release/acquire num-cache.
        (void)parseNumberStringUncachedRaw(s);
        state = s->loadNumState();
    }
    if (state != 1 && state != 4) return false;
    n = s->loadNumCache();
    return true;
}

double compare(Value v1, Value v2) {
    // Identical boxed values, even canonical NaN, compare equal in Scratch:
    // NaN falls back to comparing the same "NaN" text.
    if (v1.bits == v2.bits) return 0.0;

    double n1 = 0.0, n2 = 0.0;
    const bool numeric1 = numericCompareOperand(v1, n1);
    const bool numeric2 = numericCompareOperand(v2, n2);
    if (numeric1 && numeric2) {
        // Subtracting two equal infinities would produce NaN; Scratch's
        // original Cast.compare explicitly treats those pairs as equal.
        if (n1 == n2 && std::isinf(n1)) return 0.0;
        return n1 - n2;
    }
    return compareStringsCaseInsensitive(toString(v1), toString(v2));
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
        const char16_t* data = s->data();
        for (uint32_t i = 0; i < s->len; ++i)
            if (data[i] == u'.') return false;
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
    const char16_t* data = s->data();
    for (uint32_t i = 0; i < s->len; ++i)
        if (!jsnum::isStrWhiteSpace(data[i])) return false;
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
    // Cast.toNumber() は NaN を 0 に潰すので、scratch-vm の toListIndex は
    // NaN を返さない（非数値は LIST_INVALID）。NaN を保持したまま返すと
    // listDelete 側の NaN 特別扱いが壊れたインデックスを作る。
    if (std::isnan(d)) return LIST_INVALID;
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
                        s->publishNumCache(static_cast<double>(v), 1);
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
            uint32_t state = s->loadRgbState();
            if (state == 1) {
                uint32_t c = s->loadRgbCache();
                return {int((c >> 16) & 0xFF), int((c >> 8) & 0xFF),
                        int(c & 0xFF), int((c >> 24) & 0xFF)};
            }
            if (state == 0) {
                if (s->len >= 1 && s->data()[0] == u'#') {
                    std::string utf8 = SStr::toUtf8(s->view());
                    auto rgb = color::hexToRgb(utf8);
                    Rgba out = rgb ? Rgba{rgb->r, rgb->g, rgb->b, 255}
                                   : Rgba{0, 0, 0, 255};
                    uint32_t packed = uint32_t(out.a) << 24 |
                                  uint32_t(out.r) << 16 |
                                  uint32_t(out.g) << 8 |
                                  uint32_t(out.b);
                    s->publishRgbCache(packed, 1);
                    return out;
                }
                s->publishRgbCache(0, 2);    // '#' 以外 → toNumber decimal 経路
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
