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
    // char16_t data[len+1] がここに続く

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
    }
    static_assert(std::atomic_ref<double>::required_alignment <= alignof(double));
    static_assert(std::atomic_ref<uint32_t>::required_alignment <= alignof(uint32_t));

    char16_t* data() { return reinterpret_cast<char16_t*>(this + 1); }
    const char16_t* data() const { return reinterpret_cast<const char16_t*>(this + 1); }
    std::u16string_view view() const { return {data(), len}; }

    static SStr* create(std::u16string_view sv) {
        auto* s = static_cast<SStr*>(
            Gc::get().alloc(GcType::Str, sizeof(SStr) + (sv.size() + 1) * 2));
        s->len = static_cast<uint32_t>(sv.size());
        s->hash = 0;
        s->initCaches();
        std::memcpy(s->data(), sv.data(), sv.size() * 2);
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

/// JS 的 UTF-16 code unit 比較（toLowerCase なし）
int compareStrings(std::u16string_view a, std::u16string_view b);

/// UTF-16 の char index（letter of 用、1-based）
bool sstrLetterAt(std::u16string_view s, int64_t idx1, char16_t& out);

/// contains: 大文字小文字を区別しない部分一致（OP_CONTAINS 用）
bool sstrContains(std::u16string_view hay, std::u16string_view needle);

} // namespace xyo
