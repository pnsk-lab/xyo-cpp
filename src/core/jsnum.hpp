// xyo-cpp: ECMAScript 準拠の数値↔文字列変換。
// V8/SpiderMonkey と同じ結果を出す必要がある（Scratch の toString/toNumber の基盤）。
#pragma once

#include <string>
#include <string_view>

namespace xyo::jsnum {

/// ES String(x): 最短往復表現を ES のフォーマット規則で出力。
/// 0→"0", -0→"0", NaN→"NaN", ±Inf→"±Infinity",
/// 指数表記は |指数|>=21 または n<=-6 のときのみ、他は小数表記。
std::string toString(double v);
/// UTF-16 版（SSStr 生成時に使う）
std::u16string toString16(double v);

/// ES Number(str): 前後 StrWhiteSpace 除去・符号・0x/0o/0b 接頭辞・
/// Infinity・10進指数リテラル。空は 0、解釈不能は NaN。
double toNumber(std::u16string_view s);
/// ASCII 高速パス用（char16_t 文字列を char に写した後）
double toNumberAscii(std::string_view s);

/// JS `+value` 相当（Number() と同じ）。Value 層から呼ばれる。
inline double toNumber(const char16_t* s, size_t len) {
    return toNumber(std::u16string_view(s, len));
}

/// 文字が ES StrWhiteSpaceChar か（WhiteSpace ∪ LineTerminator）
bool isStrWhiteSpace(char16_t c);

/// JS Math.round（.5 は +Infinity 方向、-0 を保持）。
double jsRound(double v);

/// JS Math.log（V8 と同じ fdlibm 実装。glibc の log とは 1 ULP 違い得る）。
double jsLog(double v);

} // namespace xyo::jsnum
