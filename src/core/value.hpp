// xyo-cpp: NaN-boxed Scratch value.
//
// レイアウト (design.md §5.1):
//   (v>>48) < 0x7FF8                       → IEEE754 double（-NaN/-Inf もここ）
//   (v>>48) == 0x7FF8                      → タグ空間: payload 0=数値NaN, 1=false, 2=true, 3=空文字列
//   (v>>48) == 0x7FF9                      → ヒープ文字列ポインタ（下位48bit）
//   (v>>48) == 0x7FFA                      → 色 (下位24bit = RGB)
//
// NaN 正規化: 生のNaNを生み得る演算の後は必ず canonicalize() を通し kNan に揃える。
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace xyo {

struct SStr;   // GC管理 UTF-16 文字列
struct SList;  // GC管理リスト実体

namespace val {
inline constexpr uint64_t kTagBase    = 0x7FF8'0000'0000'0000ull; // payloadタグ
inline constexpr uint64_t kTagStr     = 0x7FF9'0000'0000'0000ull; // 文字列ポインタ
inline constexpr uint64_t kTagColor   = 0x7FFA'0000'0000'0000ull; // RGB内包
inline constexpr uint64_t kPtrMask    = 0x0000'FFFF'FFFF'FFFFull;
inline constexpr uint64_t kNan        = kTagBase | 0;             // 数値としてのNaN
inline constexpr uint64_t kFalse      = kTagBase | 1;
inline constexpr uint64_t kTrue       = kTagBase | 2;
inline constexpr uint64_t kEmptyStr   = kTagBase | 3;             // "" シングルトン
inline constexpr uint64_t kUndefined  = kTagBase | 4;             // JS undefined（非報告値）
} // namespace val

/// Scratch の第一級値。常に 8 バイト、コピーは trivial。
struct Value {
    uint64_t bits;

    // --- 構築 ---
    static Value num(double d) {
        Value v;
        std::memcpy(&v.bits, &d, 8);
        return v;
    }
    /// NaN になりうる計算結果を正規化して作る
    static Value numCanonical(double d) {
        return d != d ? nan() : num(d);
    }
    static Value nan()      { return Value{val::kNan}; }
    static Value boolean(bool b) { return Value{b ? val::kTrue : val::kFalse}; }
    static Value string(SStr* s) {
        return Value{val::kTagStr | (reinterpret_cast<uint64_t>(s) & val::kPtrMask)};
    }
    static Value color(uint8_t r, uint8_t g, uint8_t b) {
        return Value{val::kTagColor | (uint64_t(r) << 16) | (uint64_t(g) << 8) | b};
    }
    static Value emptyStr() { return Value{val::kEmptyStr}; }
    static Value undefined() { return Value{val::kUndefined}; }

    // --- 判定 ---
    bool isNumber() const {
        // タグ空間 (0x7FF8 系) 以外は全て数値。数値NaNは kNan のみ。
        return ((bits >> 48) & 0xFFF8) != 0x7FF8 || bits == val::kNan;
    }
    bool isString() const { return (bits >> 48) == 0x7FF9 || bits == val::kEmptyStr; }
    bool isBool()   const { return bits == val::kFalse || bits == val::kTrue; }
    bool isColor()  const { return (bits >> 48) == 0x7FFA; }
    bool isNaN()    const { return bits == val::kNan; }
    bool isUndefined() const { return bits == val::kUndefined; }

    // --- 取り出し（事前に isX で確認済みの前提）---
    double asNumberUnchecked() const {
        if (bits == val::kNan) {
            double d;
            uint64_t nb = 0x7FF8'0000'0000'0000ull;
            std::memcpy(&d, &nb, 8);
            return d;
        }
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    bool asBool() const { return bits == val::kTrue; }
    /// 文字列ポインタ。kEmptyStr（"" シングルトン）は nullptr を返す
    SStr* asStrPtr() const {
        if (bits == val::kEmptyStr) return nullptr;
        return reinterpret_cast<SStr*>(bits & val::kPtrMask);
    }
    /// 空文字列判定（kEmptyStr または len==0）
    bool isEmptyString() const { return bits == val::kEmptyStr; }

    bool operator==(const Value&) const = default;
};

static_assert(sizeof(Value) == 8);
static_assert(std::is_trivially_copyable_v<Value>);

} // namespace xyo
