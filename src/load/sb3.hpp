// xyo-cpp: serialization/sb3.js 移植 — project.json → Runtime。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xyo {

struct Runtime;
struct Target;
struct JsonValue;
class ZipReader;

struct Sb3LoadResult {
    bool ok = false;
    std::string error;
    std::vector<Target*> targets;
    /// 拡張opcode（core_* 以外の prefix_ 部分）
    std::vector<std::string> extensionIds;
};

/// project.json テキストからターゲットを構築し runtime にインストールする。
/// zip はアセット（コスチューム/サウンド）を持つ場合のみ指定（nullptr 可）。
/// isSingleSprite=true のとき json を単一ターゲット扱い。
Sb3LoadResult loadSb3(Runtime& runtime, const std::string& projectJson,
                      const ZipReader* zip, bool isSingleSprite = false);

/// .sb3/.sb2 ファイル（zip）を読み、フォーマットを判別してロードする。
/// Scratch 2.0 形式（objName+children）は loadSb2 へディスパッチ。
Sb3LoadResult loadSb3File(Runtime& runtime, const uint8_t* data, size_t size);

/// Scratch 2.0 project.json → Runtime（sb2.cpp）
Sb3LoadResult loadSb2(Runtime& runtime, const JsonValue& json,
                      const ZipReader* zip);

} // namespace xyo
