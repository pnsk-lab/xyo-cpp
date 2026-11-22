// xyo-cpp: native WASM 拡張プロバイダ（vendored wasm3）。
//
// マニフェストの `provider: "wasm"` と `ext.wasm` を読み、拡張ブロックを
// ExtensionRegistry に登録する。ABI は docs/extensions.md §5.2。
#pragma once

#include <string>

#include "extension.hpp"

namespace xyo {

struct Runtime;

/// `wasmPath` のモジュールをロードし、`def` のブロックを登録する。
/// 失敗時は何も登録せず false を返す（呼び出し側がスタブへフォールバック可）。
bool registerWasmExtension(Runtime& rt, ExtensionRegistry& registry,
                           const ExtensionDef& def,
                           const std::string& wasmPath,
                           std::string* error = nullptr);

/// ビルドに wasm3 が含まれているか（Emscripten / XYO_WASM_EXT=OFF では false）。
bool wasmExtensionProviderAvailable();

} // namespace xyo
