// xyo-cpp: UTF-16 文字列操作 + SStr 生成。
#include "sstr.hpp"

#include <cstring>
#include <vector>

namespace xyo {

struct CasePropRange { uint32_t lo, hi; };
#include "case_props.inc"

// 生成テーブル（BMP 小写化・大写化）
#include "lower_table.inc"
#include "upper_table.inc"

static bool inRanges(const CasePropRange* r, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < r[mid].lo) hi = mid;
        else if (cp > r[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

static bool isCased(uint32_t cp) {
    return inRanges(kCasedRanges, std::size(kCasedRanges), cp);
}
static bool isCaseIgnorable(uint32_t cp) {
    return inRanges(kCaseIgnorableRanges, std::size(kCaseIgnorableRanges), cp);
}

SStr* sstrCreate(std::u16string_view sv) {
    return SStr::create(sv);
}

SStr* sstrFromAscii(std::string_view sv) {
    auto* s = static_cast<SStr*>(
        Gc::get().alloc(GcType::Str, sizeof(SStr) + (sv.size() + 1) * 2));
    s->len = static_cast<uint32_t>(sv.size());
    s->hash = 0;
    s->initCaches();
    for (size_t i = 0; i < sv.size(); ++i) s->data()[i] = static_cast<char16_t>(sv[i]);
    s->data()[sv.size()] = 0;
    return s;
}

SStr* sstrConcat(std::u16string_view a, std::u16string_view b) {
    auto* s = static_cast<SStr*>(
        Gc::get().alloc(GcType::Str,
                        sizeof(SStr) + (a.size() + b.size() + 1) * 2));
    s->len = static_cast<uint32_t>(a.size() + b.size());
    s->hash = 0;
    s->initCaches();
    std::memcpy(s->data(), a.data(), a.size() * 2);
    std::memcpy(s->data() + a.size(), b.data(), b.size() * 2);
    s->data()[s->len] = 0;
    return s;
}

SStr* sstrConcatAscii(std::u16string_view a, std::string_view b) {
    auto* s = static_cast<SStr*>(
        Gc::get().alloc(GcType::Str,
                        sizeof(SStr) + (a.size() + b.size() + 1) * 2));
    s->len = static_cast<uint32_t>(a.size() + b.size());
    s->hash = 0;
    s->initCaches();
    std::memcpy(s->data(), a.data(), a.size() * 2);
    for (size_t i = 0; i < b.size(); ++i)
        s->data()[a.size() + i] = static_cast<char16_t>(b[i]);
    s->data()[s->len] = 0;
    return s;
}

// --- 小写化 ---

/// c (BMP char16_t) の小写化。多くは 1 文字→1 文字。
/// U+0130 のみ 2 文字展開（'i' + U+0307）で *extra に第 2 文字を返す。
/// 戻り値: 第 1 文字。
char16_t lowerUnit(char16_t c, char16_t* extra) {
    *extra = 0;
    if (c >= u'A' && c <= u'Z') return c + 32;
    if (c < 0x80) return c;
    if (c == 0x0130) { *extra = 0x0307; return u'i'; }
    // レンジテーブル（二分探索するほど大きくないので線形でもよいが…29件なので線形）
    for (const auto& r : kLowerRanges) {
        if (c >= r.lo && c <= r.hi) return c + r.delta;
    }
    // 単独テーブル: 二分探索（ソート済みとは限らないので毎回線形…645件は重い）
    // ソート済み前提で二分探索
    {
        int lo = 0, hi = static_cast<int>(std::size(kLowerSingles)) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (kLowerSingles[mid].cp == c) return kLowerSingles[mid].lo;
            if (kLowerSingles[mid].cp < c) lo = mid + 1; else hi = mid - 1;
        }
    }
    return c;
}

char32_t lowerCp(uint32_t cp, char16_t* extra) {
    *extra = 0;
    if (cp <= 0xFFFF) return lowerUnit(static_cast<char16_t>(cp), extra);
    for (const auto& r : kLowerAstralRanges) {
        if (cp >= r.lo && cp <= r.hi) return cp + r.delta;
    }
    return cp;
}

static void appendCp(std::u16string& out, uint32_t cp) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<char16_t>(cp));
    } else {
        const uint32_t v = cp - 0x10000u;
        out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
    }
}

std::u16string u16ToLower(std::u16string_view sv) {
    // JS String.prototype.toLowerCase はコードポイント単位で畳み込み、
    // Greek capital sigma は語末だけ final sigma (U+03C2) になる
    // （Unicode Final_Sigma: 直前が Cased、直後が Cased でない）。
    bool ascii = true;
    for (char16_t c : sv) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return u16ToLowerAscii(sv);

    std::vector<uint32_t> cps;
    cps.reserve(sv.size());
    for (size_t i = 0; i < sv.size();) {
        const char16_t c = sv[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < sv.size()) {
            const char16_t d = sv[i + 1];
            if (d >= 0xDC00 && d <= 0xDFFF) {
                cps.push_back(0x10000u +
                    (static_cast<uint32_t>(c - 0xD800) << 10) +
                    static_cast<uint32_t>(d - 0xDC00));
                i += 2;
                continue;
            }
        }
        cps.push_back(c);
        ++i;
    }

    std::u16string out;
    out.reserve(sv.size());
    for (size_t i = 0; i < cps.size(); ++i) {
        const uint32_t cp = cps[i];
        char16_t extra = 0;
        uint32_t l = lowerCp(cp, &extra);
        if (cp == 0x03A3) {
            bool casedBefore = false, casedAfter = false;
            for (size_t j = i; j-- > 0;) {
                if (isCaseIgnorable(cps[j])) continue;
                casedBefore = isCased(cps[j]);
                break;
            }
            for (size_t j = i + 1; j < cps.size(); ++j) {
                if (isCaseIgnorable(cps[j])) continue;
                casedAfter = isCased(cps[j]);
                break;
            }
            if (casedBefore && !casedAfter) l = 0x03C2;  // ς
        }
        appendCp(out, l);
        if (extra) out.push_back(extra);
    }
    return out;
}

std::u16string u16ToLowerAscii(std::u16string_view sv) {
    std::u16string out;
    out.reserve(sv.size());
    for (char16_t c : sv) {
        out.push_back((c >= u'A' && c <= u'Z') ? c + 32 : c);
    }
    return out;
}

static bool isAsciiOnly(std::u16string_view s) {
    for (char16_t c : s) if (c >= 0x80) return false;
    return true;
}

int compareStrings(std::u16string_view a, std::u16string_view b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

int compareStringsCaseInsensitive(std::u16string_view a, std::u16string_view b) {
    // ASCII 高速パス
    if (isAsciiOnly(a) && isAsciiOnly(b)) {
        size_t n = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < n; ++i) {
            char16_t ca = a[i], cb = b[i];
            if (ca >= u'A' && ca <= u'Z') ca += 32;
            if (cb >= u'A' && cb <= u'Z') cb += 32;
            if (ca != cb) return ca < cb ? -1 : 1;
        }
        if (a.size() == b.size()) return 0;
        return a.size() < b.size() ? -1 : 1;
    }
    std::u16string la = u16ToLower(a), lb = u16ToLower(b);
    return compareStrings(la, lb);
}

bool sstrLetterAt(std::u16string_view s, int64_t idx1, char16_t& out) {
    if (idx1 < 1 || static_cast<uint64_t>(idx1) > s.size()) return false;
    out = s[static_cast<size_t>(idx1 - 1)];
    return true;
}

bool sstrContains(std::u16string_view hay, std::u16string_view needle) {
    std::u16string h = u16ToLower(hay);
    std::u16string n = u16ToLower(needle);
    if (n.empty()) return true;
    return h.find(n) != std::u16string::npos;
}

SStr* sstrLower(std::u16string_view sv) {
    return SStr::create(u16ToLower(sv));
}

// --- 大写化（JS toUpperCase 相当、多:1/1:多マッピング対応）---

std::u16string u16ToUpper(std::u16string_view sv) {
    std::u16string out;
    out.reserve(sv.size());
    for (char16_t c : sv) {
        if (c >= u'a' && c <= u'z') { out.push_back(c - 32); continue; }
        if (c < 0x80) { out.push_back(c); continue; }
        int lo = 0, hi = int(std::size(kUpperMap)) - 1;
        bool found = false;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (kUpperMap[mid].cp == c) {
                for (uint8_t i = 0; i < kUpperMap[mid].len; ++i)
                    out.push_back(char16_t(kUpperMap[mid].u[i]));
                found = true;
                break;
            }
            if (kUpperMap[mid].cp < c) lo = mid + 1; else hi = mid - 1;
        }
        if (!found) out.push_back(c);
    }
    return out;
}

// --- UTF-8 相互変換（I/O 境界用。内部は UTF-16）---

static void encodeUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string SStr::toUtf8(std::u16string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (size_t i = 0; i < sv.size(); ++i) {
        char16_t c = sv[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < sv.size()) {
            char16_t lo = sv[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                encodeUtf8(out, 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00));
                ++i;
                continue;
            }
        }
        // 孤立サロゲートは WTF-8 的にそのままエンコード（JS 的に正しい挙動）
        encodeUtf8(out, c);
    }
    return out;
}

SStr* SStr::fromUtf8(std::string_view sv) {
    // UTF-8 → UTF-16（サロゲートペア対応、不正シーケンスは U+FFFD）
    std::u16string tmp;
    tmp.reserve(sv.size());
    for (size_t i = 0; i < sv.size();) {
        unsigned char c = sv[i];
        char32_t cp;
        size_t adv;
        if (c < 0x80) { cp = c; adv = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; adv = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; adv = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; adv = 4; }
        else { cp = 0xFFFD; adv = 1; }
        if (i + adv > sv.size()) { cp = 0xFFFD; adv = 1; }
        if (adv > 1) {
            bool ok = true;
            for (size_t j = 1; j < adv; ++j)
                if ((sv[i + j] & 0xC0) != 0x80) { ok = false; break; }
            if (!ok) { cp = 0xFFFD; adv = 1; }
            else {
                for (size_t j = 1; j < adv; ++j)
                    cp = (cp << 6) | (sv[i + j] & 0x3F);
            }
        }
        i += adv;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            tmp.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            tmp.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            tmp.push_back(static_cast<char16_t>(cp));
        }
    }
    return create(tmp);
}

} // namespace xyo
