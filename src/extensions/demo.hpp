// xyo-cpp: 拡張レジストリの参照実装（デモ）。
#pragma once

#include <string>

#include "extension.hpp"

namespace xyo {

/// `demo_double` (reporter) / `demo_next` (reporter) / `demo_set` (stack)。
/// Phase 0 の配線確認用。既定の Runtime には登録されない。
bool registerDemoExtension(Runtime& rt, ExtensionRegistry& registry,
                           std::string* error = nullptr);

} // namespace xyo
