// xyo-cpp: jsexecute.js runtimeFunctions + プリミティブ共通ヘルパ。
// インタプリタ・JIT 双方から呼ばれる「Scratch 意味論の正本」。
#pragma once

#include "../core/cast.hpp"
#include "../core/color.hpp"
#include "../core/rng.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../core/value.hpp"

namespace xyo::rt {

// --- 比較（jsexecute の fast/slow 2 段構成）---
bool compareEqualSlow(Value v1, Value v2);
bool compareEqual(Value v1, Value v2);
bool compareGreaterThanSlow(Value v1, Value v2);
bool compareGreaterThan(Value v1, Value v2);
bool compareLessThanSlow(Value v1, Value v2);
bool compareLessThan(Value v1, Value v2);

// --- 算術 ---
double mod(double n, double m);                  // floored
double tanDeg(double angle);                     // ±90/270 → ±Inf
double limitPrecision(double v);
double randomInt(double low, double high);
double randomFloat(double low, double high);
double randomIntWithout(double low, double high, double excluded); // inclusiveRandIntWithout

// --- 文字列演算（scratch3_operators + jsgen の OP_*）---
Value join(Value a, Value b);
Value letterOf(Value letter, Value str);
double strLength(Value s);                        // toString(s).length
bool  strContains(Value hay, Value needle);
SStr* toLowerSStr(Value v);

// --- リスト（jsexecute listGet/Insert/Replace/Delete/Contains/IndexOf/Contents）---
Value  listGet(SList* list, Value idx);
void   listReplace(SList* list, Value idx, Value v);
void   listInsert(SList* list, Value idx, Value v);
void   listDelete(SList* list, Value idx);
void   listDeleteAll(SList* list);
bool   listContains(SList* list, Value item);
double listIndexOf(SList* list, Value item);
Value  listContents(SList* list);

// --- 色 ---
Rgb colorToList(Value v);

// --- その他 jsexecute 補助 ---
double daysSince2000();

} // namespace xyo::rt
