// xyo-cpp: sb3 シリアライザ（ビルトインエディタの保存用）。
// Runtime（targets/blocks/variables/costumes/sounds/monitors）を
// Scratch 3.0 project.json + zip コンテナへ直列化する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xyo {

struct Runtime;
class Blocks;
class Target;
struct JsonValue;

struct Sb3SaveResult {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> data;   // zip バイト列（project.json + アセット）
};

/// Runtime を sb3 の zip バイト列へ直列化する。
Sb3SaveResult saveSb3(Runtime& runtime);

/// sb3 ファイルへ書き込む（UTF-8 パス）。成功時 true。
bool saveSb3File(Runtime& runtime, const char* path);

/// 単一スプライトを .sprite3（sprite.json + アセット zip）へ直列化する
/// （scratch-vm exportSprite 準拠）。成功時 true。
bool saveSprite3File(Runtime& runtime, Target& target, const char* path);

/// バックパック用: headId から到達可能なブロック（next/inputs/shadow）を
/// `{"head": id, "blocks": {...}}` の JSON 断片にする。
JsonValue saveBlocksFragment(Blocks& src, const std::string& headId);

} // namespace xyo
