// xyo-cpp: jsexecute.js runtimeFunctions の移植。
#include "runtime_helpers.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace xyo::rt {

namespace {

/// `item # of` / `contains` の走査量（`XYO_LISTSTATS=1` で終了時に stderr）。
/// 「値→最初の index」索引を入れる価値があるかの判断材料。
struct ListScanStats {
    bool on = false;
    uint64_t calls = 0;
    uint64_t elems = 0;    // 実際に比較した要素数
    uint64_t found = 0;
    uint64_t posSum = 0;   // ヒット位置の合計（早期ヒット率の目安）

    ListScanStats() {
        const char* v = std::getenv("XYO_LISTSTATS");
        on = v && v[0] != '0';
    }
};

ListScanStats& stats() {
    static ListScanStats s;
    return s;
}

struct StatsPrinter {
    ~StatsPrinter() {
        ListScanStats& s = stats();
        if (!s.on || !s.calls) return;
        std::fprintf(stderr,
                     "liststats: calls=%llu scanned=%llu avg=%.1f found=%llu "
                     "avg_pos=%.1f\n",
                     (unsigned long long)s.calls,
                     (unsigned long long)s.elems,
                     double(s.elems) / double(s.calls),
                     (unsigned long long)s.found,
                     s.found ? double(s.posSum) / double(s.found) : 0.0);
    }
};
StatsPrinter g_printer;

inline void noteCall() {
    ListScanStats& s = stats();
    if (s.on) ++s.calls;
}

inline void noteScanned(uint32_t n) {
    ListScanStats& s = stats();
    if (s.on) s.elems += n;
}

inline void noteHit(uint32_t pos) {
    ListScanStats& s = stats();
    if (!s.on) return;
    ++s.found;
    s.posSum += pos;
}

}  // namespace

// --- 比較 ---

bool compareEqualSlow(Value v1, Value v2) {
    double n1 = cast::toNumberOrNaN(v1);
    if (std::isnan(n1) || (n1 == 0 && cast::isNotActuallyZero(v1))) {
        SStr* s1 = cast::toString(v1);
        SStr* s2 = cast::toString(v2);
        return compareStringsCaseInsensitive(s1->view(), s2->view()) == 0;
    }
    double n2 = cast::toNumberOrNaN(v2);
    if (std::isnan(n2) || (n2 == 0 && cast::isNotActuallyZero(v2))) {
        SStr* s1 = cast::toString(v1);
        SStr* s2 = cast::toString(v2);
        return compareStringsCaseInsensitive(s1->view(), s2->view()) == 0;
    }
    return n1 == n2;
}

/// JS `v1 === v2`: number は IEEE 比較（-0===0, NaN!==NaN）、それ以外はビット一致。
static bool strictEquals(Value v1, Value v2) {
    if (v1.isNumber() && v2.isNumber()) {
        return v1.asNumberUnchecked() == v2.asNumberUnchecked();
    }
    return v1 == v2;
}

bool compareEqual(Value v1, Value v2) {
    // (両方 number かつ非NaN) || v1===v2 → v1===v2
    if ((v1.isNumber() && v2.isNumber() &&
         !std::isnan(v1.asNumberUnchecked()) && !std::isnan(v2.asNumberUnchecked())) ||
        strictEquals(v1, v2)) {
        return strictEquals(v1, v2);
    }
    return compareEqualSlow(v1, v2);
}

bool compareGreaterThanSlow(Value v1, Value v2) {
    double n1 = cast::toNumberOrNaN(v1);
    double n2 = cast::toNumberOrNaN(v2);
    if (n1 == 0 && cast::isNotActuallyZero(v1)) {
        n1 = std::numeric_limits<double>::quiet_NaN();
    } else if (n2 == 0 && cast::isNotActuallyZero(v2)) {
        n2 = std::numeric_limits<double>::quiet_NaN();
    }
    if (std::isnan(n1) || std::isnan(n2)) {
        SStr* s1 = cast::toString(v1);
        SStr* s2 = cast::toString(v2);
        return compareStringsCaseInsensitive(s1->view(), s2->view()) > 0;
    }
    return n1 > n2;
}

bool compareGreaterThan(Value v1, Value v2) {
    // v1 が number で v1 非NaN → v1 > v2（v2 側の NaN は fcmp が false を返す）
    if (v1.isNumber() && v2.isNumber() && !std::isnan(v1.asNumberUnchecked())) {
        return v1.asNumberUnchecked() > v2.asNumberUnchecked();
    }
    return compareGreaterThanSlow(v1, v2);
}

bool compareLessThanSlow(Value v1, Value v2) {
    double n1 = cast::toNumberOrNaN(v1);
    double n2 = cast::toNumberOrNaN(v2);
    if (n1 == 0 && cast::isNotActuallyZero(v1)) {
        n1 = std::numeric_limits<double>::quiet_NaN();
    } else if (n2 == 0 && cast::isNotActuallyZero(v2)) {
        n2 = std::numeric_limits<double>::quiet_NaN();
    }
    if (std::isnan(n1) || std::isnan(n2)) {
        SStr* s1 = cast::toString(v1);
        SStr* s2 = cast::toString(v2);
        return compareStringsCaseInsensitive(s1->view(), s2->view()) < 0;
    }
    return n1 < n2;
}

bool compareLessThan(Value v1, Value v2) {
    // v2 側のみ NaN チェック（JS 版と同じ条件）
    if (v1.isNumber() && v2.isNumber() && !std::isnan(v2.asNumberUnchecked())) {
        return v1.asNumberUnchecked() < v2.asNumberUnchecked();
    }
    return compareLessThanSlow(v1, v2);
}

// --- 算術 ---

double mod(double n, double m) {
    double result = std::fmod(n, m);
    if (result / m < 0) result += m;
    return result;
}

double tanDeg(double angle) {
    angle = std::fmod(angle, 360.0);
    if (angle == -270.0 || angle == 90.0) return std::numeric_limits<double>::infinity();
    if (angle == -90.0 || angle == 270.0) return -std::numeric_limits<double>::infinity();
    return jsnum::jsRound(std::tan((M_PI * angle) / 180.0) * 1e10) / 1e10;
}

double limitPrecision(double v) {
    double rounded = jsnum::jsRound(v);
    double delta = v - rounded;
    return std::abs(delta) < 1e-9 ? rounded : v;
}

double randomInt(double low, double high) {
    return low + std::floor(Rng::global().nextDouble() * ((high + 1) - low));
}
double randomFloat(double low, double high) {
    return (Rng::global().nextDouble() * (high - low)) + low;
}
double randomIntWithout(double low, double high, double excluded) {
    double options = high - low;
    double r = low + std::floor(Rng::global().nextDouble() * options);
    return r >= excluded ? r + 1 : r;
}

// --- 文字列 ---

Value join(Value a, Value b) {
    // 最多ケース (str, num): 数値表記を直接書き込み中間 SStr を省く。
    // jsnum::toString の出力は ASCII のみ（NaN/Infinity/指数含む）。
    if (a.isString() && b.isNumber()) {
        std::string ns = jsnum::toString(b.asNumberUnchecked());
        SStr* s1 = a.asStrPtr();     // kEmptyStr → nullptr
        return Value::string(sstrConcatAscii(
            s1 ? s1->view() : std::u16string_view(), ns));
    }
    SStr* s1 = cast::toString(a);
    SStr* s2 = cast::toString(b);
    if (s1->len == 0 && s2->len == 0) return Value::emptyStr();
    return Value::string(sstrConcat(s1->view(), s2->view()));
}

/// letterOf の 1 文字結果キャッシュ（ASCII）。毎回の SStr::create を
/// 避けて GC 圧力を下げる。GC ルート登録済みで sweep されない。
/// プロバイダ登録を先に行う — 生成中の collect で未初期化を mark しうる。
/// JIT の inline fast path が直接参照するためファイルスコープに置く。
namespace {
SStr* g_asciiCache[128] = {};
bool g_asciiInit = [] {
    Gc::get().addRootProvider(g_asciiCache, [](Gc& g) {
        for (SStr* s : g_asciiCache) g.markPtr(s);
    });
    for (uint32_t i = 0; i < 128; ++i) {
        char16_t c = static_cast<char16_t>(i);
        g_asciiCache[i] = SStr::create(&c, 1);
    }
    return true;
}();
} // namespace

/// JIT fast path 用: 初期化を強制して配列先頭を返す（codegen 時に
/// 呼ばれる — 生成コード実行より必ず先に初期化が終わる）。
extern "C" SStr* const* xyo_ascii_cache() {
    (void)g_asciiInit;
    return g_asciiCache;
}

SStr* letterCached(uint32_t cp) {
    (void)g_asciiInit;
    return cp < 128 ? g_asciiCache[cp] : nullptr;
}

Value letterOf(Value letter, Value str) {
    // scratch3_operators.letterOf: index = toNumber(LETTER) - 1
    // charAt は ToIntegerOrInfinity(NaN→0, trunc) される
    double index = cast::toNumber(letter) - 1;
    if (std::isnan(index)) index = 0;      // JS: charAt(NaN) → charAt(0)
    SStr* s = cast::toString(str);
    if (index < 0 || index >= s->len) return Value::emptyStr();
    char16_t c = s->data()[static_cast<uint32_t>(index)];  // trunc
    if (SStr* cached = letterCached(c)) return Value::string(cached);
    return Value::string(SStr::create(&c, 1));
}

double strLength(Value s) {
    SStr* str = cast::toString(s);
    return static_cast<double>(str->len);
}

bool strContains(Value hay, Value needle) {
    SStr* h = cast::toString(hay);
    SStr* n = cast::toString(needle);
    return sstrContains(h->view(), n->view());
}

SStr* toLowerSStr(Value v) {
    return SStr::create(u16ToLower(cast::toString(v)->view()));
}

// --- リスト ---

Value listGet(SList* list, Value idx) {
    int32_t i = cast::listIndex(idx, list->len);
    if (i < 0) return Value::emptyStr();
    return list->items[i];
}

void listReplace(SList* list, Value idx, Value v) {
    int32_t i = cast::listIndex(idx, list->len);
    if (i < 0) return;
    list->items[i] = v;
    list->monitorDirty = true;
}

void listInsert(SList* list, Value idx, Value v) {
    int32_t i = cast::listIndex(idx, list->len + 1);
    if (i < 0) return;
    list->insert0(static_cast<uint32_t>(i), v);
    list->monitorDirty = true;
}

void listDelete(SList* list, Value idx) {
    // data_deleteoflist — toListIndex(acceptAll=true)。
    double i = cast::toListIndexD(idx, list->len, true);
    if (i == cast::LIST_ALL) { list->clear(); return; }
    if (i == cast::LIST_INVALID || i < 1) return;
    list->erase0(static_cast<uint32_t>(i - 1));
    list->monitorDirty = true;
}

void listDeleteAll(SList* list) { list->clear(); }

/// JS Array#indexOf — SameValueZero（NaN===NaN, 文字列は内容比較）
static bool sameValueZero(Value a, Value b) {
    if (a.isNumber() && b.isNumber()) {
        double x = a.asNumberUnchecked(), y = b.asNumberUnchecked();
        return x == y || (std::isnan(x) && std::isnan(y));
    }
    if (a.isString() && b.isString()) {
        SStr* sa = a.asStrPtr();
        SStr* sb = b.asStrPtr();
        if (!sa || !sb) return sa == sb;
        return sa->view() == sb->view();
    }
    return a == b;
}

bool listContains(SList* list, Value item) {
    noteCall();
    // indexOf(SameValueZero) fast path → Cast.compare===0 走査（JS 版と同じ順序）
    for (uint32_t i = 0; i < list->len; ++i) {
        if (sameValueZero(list->items[i], item)) {
            noteScanned(i + 1);
            noteHit(i + 1);
            return true;
        }
    }
    for (uint32_t i = 0; i < list->len; ++i) {
        if (compareEqual(list->items[i], item)) {
            noteScanned(list->len + i + 1);
            noteHit(i + 1);
            return true;
        }
    }
    noteScanned(2 * list->len);
    return false;
}

double listIndexOf(SList* list, Value item) {
    noteCall();
    for (uint32_t i = 0; i < list->len; ++i) {
        if (compareEqual(list->items[i], item)) {
            noteScanned(i + 1);
            noteHit(i + 1);
            return i + 1;
        }
    }
    noteScanned(list->len);
    return 0;
}

Value listContents(SList* list) {
    // scratch-vm getListContents: 全要素が「文字列かつ長さ 1」のときだけ
    // join('')、さもなければ join(' ')。数値 1 は typeof が string ではない
    // ので区切りが入る（TurboWarp コンパイラの (item + '').length 判定とは
    // ここで異なる。公式 Scratch に合わせる）。
    bool allSingleLetters = true;
    for (uint32_t i = 0; i < list->len; ++i) {
        const Value& it = list->items[i];
        SStr* s = it.isString() ? it.asStrPtr() : nullptr;
        if (!s || s->len != 1) { allSingleLetters = false; break; }
    }
    std::u16string out;
    for (uint32_t j = 0; j < list->len; ++j) {
        if (!allSingleLetters && j) out.push_back(u' ');
        SStr* e = cast::toString(list->items[j]);
        out.append(e->view());
    }
    return out.empty() ? Value::emptyStr()
                       : Value::string(SStr::create(std::u16string_view(out)));
}

Rgb colorToList(Value v) { return cast::toRgbColorList(v); }

double daysSince2000() {
    // Scratch と同じ UTC 起点・ms 精度（scratch-vm の local/DST 補正は
    // 代数的に打ち消し合い Date.now()-Date.UTC(2000,0,1) に帰着する）。
    // std::time は秒精度で fps.time のような差分計測を整数秒へ量子化
    // してしまうため chrono を使う。
    const double nowMs = double(std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return (nowMs - 946684800000.0) / (24 * 60 * 60 * 1000);
}

} // namespace xyo::rt
