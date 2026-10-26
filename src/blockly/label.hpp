// xyo-cpp: Blockly のラベル文字列（モデル → 表示テキスト）。
// 幾何・描画は layout.hpp / shape.cpp が担当する。
#pragma once

#include <string>

#include "catalog.hpp"

namespace xyo {

class Blocks;
struct Block;

namespace blockly {

/// ブロックの fields[name] を UTF-8 文字列で返す（無ければ空）。
std::string fieldStr(Block* b, const std::string& name);
/// shadow ブロックの表示値（math_number の NUM / text の TEXT /
/// menu のドロップダウンラベル等）。
std::string shadowValue(Blocks* blocks, Block* shadow);
/// %n を □ へ置き換えた短いラベル（バックパックのチップ等）。
std::string shortBlockLabel(const edit::BlockDef* d);
/// %n を「フィールドの表示値」へ置換した 1 本の文字列。
std::string blockLabelText(Blocks* blocks, Block* b);
/// 1 つの引数の表示文字列（shadow/dropdown/variable/icon を解決）。
std::string argDisplay(Blocks* blocks, Block* b, const edit::ArgDef& a);
/// shadow ブロックが白い入力フィールド（数値/文字列）として表示されるか。
/// その場合のフィールド名を out へ。false なら menu/reporter 表示。
bool shadowIsField(Block* shadow, std::string* fieldName = nullptr,
                   bool* isNum = nullptr);

} // namespace blockly
} // namespace xyo
