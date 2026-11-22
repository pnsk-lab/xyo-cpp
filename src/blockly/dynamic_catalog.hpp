// xyo-cpp: 実行時に登録された拡張ブロックのエディタカタログ。
//
// `catalog.hpp` の static な kBlockDefs / kExtensionDefs を変えず、動的定義を
// 別の deque に保持する。BlockDef* を保持する呼び出し側があるため、vector の
// 再確保でポインタを無効化しない。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "../extensions/extension.hpp"
#include "catalog.hpp"

namespace xyo::edit {

/// マニフェストのブロック定義をカタログへ追加する（同じ id/opcode は無視）。
void registerExtensionDef(const ExtensionDef& def);

/// 動的登録されたブロック。無ければ nullptr。
const BlockDef* findDynamicBlock(std::string_view opcode);
bool isDynamicBlock(std::string_view opcode);

/// 動的拡張のカテゴリ（インストール順）。
std::vector<CategoryDef> dynamicExtensionCategories();

/// テスト用。全動的定義を消す。
void clearDynamicExtensions();

} // namespace xyo::edit
