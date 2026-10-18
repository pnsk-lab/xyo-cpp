// xyo-cpp: util/cast.js の bit-for-bit 移植（design.md §5.3）。
#pragma once

#include <cstdint>

#include "color.hpp"
#include "jsnum.hpp"
#include "sstr.hpp"
#include "value.hpp"

namespace xyo::cast {

/// 比較の `n===0 && isNotActuallyZero(v)` 判定に使う内部関数。
/// 文字列に数字・タブが一切含まれなければ true（""や空白のみは 0 扱いしない）。
bool isNotActuallyZero(Value v);

double toNumber(Value v);        // NaN → 0
double toNumberOrNaN(Value v);   // NaN 保持（+v 相当）
bool   toBoolean(Value v);
SStr*  toString(Value v);        // 常に非 null。空文字列は len=0 SStr
Value  toStringValue(Value v);   // 空文字列は kEmptyStr シングルトン
double compare(Value v1, Value v2);
bool   isInt(Value v);
bool   isWhiteSpace(Value v);

/// Scratch リストインデックス結果（jsexecute/toListIndex 共通）
/// >= 1: 有効な 1-based インデックス
enum : int32_t {
    LIST_ALL     = -1,
    LIST_INVALID = -2,
};
/// toListIndex — 1-based Scratch インデックス。JS では NaN がそのまま
/// 返る経路がある（'all' で acceptAll=false や非数値文字列）ため double。
/// 呼び出し側で `idx == LIST_ALL` / `idx == LIST_INVALID` / isnan を判定。
double toListIndexD(Value index, double length, bool acceptAll);

/// jsexecute の listIndex（0-based、'all' は扱わない、失敗 -1）
int32_t listIndex(Value index, uint32_t length);

Rgba toRgbColorObject(Value v);
Rgb  toRgbColorList(Value v);

} // namespace xyo::cast
