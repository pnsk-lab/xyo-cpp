// xyo-cpp: SStr — GC 管理 UTF-16 文字列。
// ペイロード先頭（alloc() の戻り値）がこの構造体。
// char16_t データは構造体直後に続く（len + 1 文字ぶん確保し NUL 終端）。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "gc.hpp"

namespace xyo {

struct SStr {
    uint32_t len;
    uint32_t hash;   // lazy; 0 = 未計算
    /// jsnum::toNumber 結果のメモ（文字列は不変なので一度だけパース）。
    /// numState: 0=未計算, 1=数値, 2=NaN, 3=比較では非数値の0,
    /// 4=Infinity リテラル（cast.cpp と codegen.cpp の判定と同期）。
    double numCache;
    uint32_t numState;
    /// "#RRGGBB" パース結果のメモ（toRgbColorObject 用）。
    /// rgbState: 0=未計算, 1=色（rgbCache=a<<24|r<<16|g<<8|b）,
    /// 2='#' 始まりでない（toNumber decimal 経路へ）。
    uint32_t rgbCache;
    uint32_t rgbState;
    /// 0 = flat（本体は構造体直後の char16_t data[len+1]）、
    /// 1 = cons（left/right の遅延連結。本体は初回アクセスで materialize）。
    uint32_t flags;
    struct SStr* left;    // cons のみ
    struct SStr* right;   // cons のみ
    /// cons の flatten 結果（未 materialize は null）。複数スレッドが同時に
    /// flatten しうるので publish は atomic（勝った側のバッファを使う）。
    char16_t* body;
    // char16_t data[len+1] がここに続く（flat のみ）
    static constexpr uint32_t kConsFlag = 1;
    /// kAsciiKnown が立っていれば kAsciiValue が ASCII 性を表す（遅延計算）。
    static constexpr uint32_t kAsciiKnown = 2;
    static constexpr uint32_t kAsciiValue = 4;
    /// 全コードユニットが 0x80 未満か。比較・検索の大小文字折り畳みや
    /// u16ToLower のコピーを省く判定に使う。一度だけ計算して flags に
    /// キャッシュする（cons は子の結果を合成する）。
    bool isAscii() const;

    // Keep the plain-field ABI used by JIT/AOT offsets, but access published
    // caches atomically: independent scripts can share the same immutable SStr.
    // Concurrent publishers compute identical payload/state from its contents.
    // Payloads must also be atomic (a late publisher can overlap a reader).
    uint32_t loadNumState() {
        return std::atomic_ref(numState).load(std::memory_order_acquire);
    }
    double loadNumCache() {
        return std::atomic_ref(numCache).load(std::memory_order_relaxed);
    }
    void publishNumCache(double value, uint32_t state) {
        std::atomic_ref(numCache).store(value, std::memory_order_relaxed);
        std::atomic_ref(numState).store(state, std::memory_order_release);
    }
    uint32_t loadRgbState() {
        return std::atomic_ref(rgbState).load(std::memory_order_acquire);
    }
    uint32_t loadRgbCache() {
        return std::atomic_ref(rgbCache).load(std::memory_order_relaxed);
    }
    void publishRgbCache(uint32_t value, uint32_t state) {
        std::atomic_ref(rgbCache).store(value, std::memory_order_relaxed);
        std::atomic_ref(rgbState).store(state, std::memory_order_release);
    }
    // Called only before publication. JIT list-index lowering may load the
    // payload speculatively even when state is zero, so initialize both.
    void initCaches() {
        numCache = 0;
        numState = 0;
        rgbCache = 0;
        rgbState = 0;
        flags = 0;
        left = nullptr;
        right = nullptr;
        body = nullptr;
    }
    static_assert(std::atomic_ref<double>::required_alignment <= alignof(double));
    static_assert(std::atomic_ref<uint32_t>::required_alignment <= alignof(uint32_t));

    bool isCons() const { return flags & kConsFlag; }
    /// flat の本体（構造体直後）。cons では left/right がここに入る。
    char16_t* inlineBody() { return reinterpret_cast<char16_t*>(this + 1); }
    const char16_t* inlineBody() const {
        return reinterpret_cast<const char16_t*>(this + 1);
    }
    char16_t* data() {
        return isCons() ? const_cast<char16_t*>(consBody()) : inlineBody();
    }
    const char16_t* data() const { return isCons() ? consBody() : inlineBody(); }
    std::u16string_view view() const { return {data(), len}; }
    /// cons を 1 回だけ materialize して本体を返す（string_util.cpp）。
    const char16_t* consBody() const;
    /// Read one UTF-16 unit without flattening shallow cons paths. The caller
    /// must check index < len. Deep paths fall back to materialization so random
    /// access over a skewed rope cannot repeatedly walk its entire depth.
    char16_t unitAt(uint32_t index) const;

    static SStr* create(std::u16string_view sv) {
        auto* s = static_cast<SStr*>(
            Gc::get().alloc(GcType::Str, sizeof(SStr) + (sv.size() + 1) * 2));
        s->len = static_cast<uint32_t>(sv.size());
        s->hash = 0;
        s->initCaches();
        if (!sv.empty()) std::memcpy(s->data(), sv.data(), sv.size() * 2);
        s->data()[sv.size()] = 0;
        return s;
    }
    static SStr* create(const char16_t* p, uint32_t len) {
        return create(std::u16string_view(p, len));
    }
    /// ASCII/UTF-8 リテラルから（実装コード・テスト用）
    static SStr* fromUtf8(std::string_view sv);
    /// UTF-16 → UTF-8
    static std::string toUtf8(std::u16string_view sv);
    static std::string toUtf8(const SStr* s) { return s ? toUtf8(s->view()) : std::string{}; }
};

// --- UTF-16 文字列操作（string-util/jsexecute 系で使う）---

/// SStr 生成（alloc は GC 管理、空なら nullptr を返さず len=0 のオブジェクト）
SStr* sstrCreate(std::u16string_view sv);
SStr* sstrFromAscii(std::string_view sv);
SStr* sstrConcat(std::u16string_view a, std::u16string_view b);
/// 連結ノード（V8 の cons string 相当）: コピーせず left/right を保持し、
/// 本文は view()/data() が初めて必要になったときに 1 回だけ作る。
SStr* sstrCons(SStr* a, SStr* b);
/// 後段が ASCII（数値表記など）の結合 — 中間 SStr を作らない
SStr* sstrConcatAscii(std::u16string_view a, std::string_view b);
SStr* sstrLower(std::u16string_view sv);

/// Unicode 小写化（BMP テーブル + U+0130 特殊展開）
char32_t lowerCp(uint32_t cp, char16_t* extra);
/// UTF-16 単位の toLowerCase（JS String.prototype.toLowerCase 相当）
std::u16string u16ToLower(std::u16string_view sv);
/// ASCII のみ高速 toLowerCase（compare のホットパス）
std::u16string u16ToLowerAscii(std::u16string_view sv);
/// JS toUpperCase 相当（BMP テーブル、1:多マッピング対応）
std::u16string u16ToUpper(std::u16string_view sv);

/// JS 的小文字化文字列比較: a<b → -1, a==b → 0, a>b → 1
int compareStringsCaseInsensitive(std::u16string_view a, std::u16string_view b);
/// SStr 版: 事前計算済みの ASCII 判定を使い、毎回の全走査を省く。
int compareStringsCaseInsensitive(const SStr* a, const SStr* b);

/// JS 的 UTF-16 code unit 比較（toLowerCase なし）
int compareStrings(std::u16string_view a, std::u16string_view b);

/// UTF-16 の char index（letter of 用、1-based）
bool sstrLetterAt(std::u16string_view s, int64_t idx1, char16_t& out);

/// contains: 大文字小文字を区別しない部分一致（OP_CONTAINS 用）
bool sstrContains(std::u16string_view hay, std::u16string_view needle);
/// SStr 版: ASCII 判定をキャッシュから引き、毎回の全走査を省く。
bool sstrContains(const SStr* hay, const SStr* needle);

} // namespace xyo
