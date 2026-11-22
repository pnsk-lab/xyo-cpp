// xyo-cpp: 拡張マニフェスト（manifest.json）の読み込み（Phase 1）。
//
// マニフェストは「ブロック定義」とプロバイダのメタデータを持つ。Phase 1 では
// ブロック定義だけを読み、実装は ManifestStubExtension（no-op）として登録する。
// 実際の動作は WASM / JS / native プロバイダが後から差し込む。
#pragma once

#include <string>
#include <vector>

#include "extension.hpp"

namespace xyo {

struct ExtensionLoadReport {
    std::vector<ExtensionDef> loaded;   // 登録に成功した定義
    std::vector<std::string> errors;    // パース / 登録の失敗
};

/// JSON テキストを 1 つのマニフェストとしてパースする。
bool parseExtensionManifest(const std::string& json, ExtensionDef* out,
                            std::string* error = nullptr);

/// ファイルを読んでパースする。
bool loadExtensionManifestFile(const std::string& path, ExtensionDef* out,
                               std::string* error = nullptr);

/// `pathList` は OS のパス区切り（POSIX は ':'、Windows は ';'）で区切られた
/// ディレクトリ / `.json` の列。ディレクトリは直下の `manifest.json` と、
/// 1 階層下の `<name>/manifest.json` を読む。
ExtensionLoadReport loadExtensionManifests(const std::string& pathList);

/// マニフェスト定義を no-op のスタブ実装として Runtime へ登録する。
/// ブロックは実行時に undefined を返すが、JIT の compat 経路には乗る。
bool registerManifestExtension(Runtime& rt, ExtensionRegistry& registry,
                               const ExtensionDef& def,
                               std::string* error = nullptr);

/// 環境変数 `XYO_EXTENSION_PATH` を読み、見つかったマニフェストを登録する。
/// 未設定なら何もしない（既定の動作を変えない）。
ExtensionLoadReport bootstrapExtensionsFromEnvironment(
    Runtime& rt, ExtensionRegistry& registry);

} // namespace xyo
