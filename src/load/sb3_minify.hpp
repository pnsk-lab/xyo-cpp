// xyo-cpp: sb3 の minify（可逆なサイズ削減）。
//
// Runtime を経由せず project.json の JSON と zip を直接書き換えるので、
// ローダが解釈しないフィールド（拡張の extensionStorage、customFonts 等）も
// そのまま保持する。既定は「意味を変えない」パスだけ:
//   - zip エントリの deflate 圧縮（scratch-vm と同じ対象拡張子）
//   - project.json の最短路 JSON 化 + ブロック/コメント ID の短縮
//     （TurboWarp の serialization/tw-compress-sb3.js 準拠）
//   - どのターゲットからも参照されていない zip アセットの除去
//   - 到達不能ブロックと、それに紐づくコメントの除去
//   - 未使用の変数/リストを削除する場合、一時 Runtime にロードして
//     engine/deps の analyzeVariableUsage（AOT/並列スケジューラと同じ
//     Variable* 解決）で実際の読み/書きを判定する（sensing_of 等の
//     動的参照がある場合のみ従来の文字列走査へフォールバック）。
// dropUnusedVariables を true にすると未使用の変数/リスト/ブロードキャストも
// 消す（既定 off — エディタの変数メニューから消えるため）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xyo {

struct Sb3MinifyOptions {
    /// zip エントリを deflate 圧縮する（level 0 で無効）。
    bool compress = true;
    /// zlib の圧縮レベル（0..9）。0 は無圧縮。
    int level = 9;
    /// ブロック/コメント ID を短い ID へ振り直す（TW compress 準拠）。
    bool compactIds = true;
    /// project.json から参照されていない zip アセットを捨てる。
    bool dropUnusedAssets = true;
    /// topLevel から到達できないブロック（と付随コメント）を捨てる。
    bool dropOrphanBlocks = true;
    /// 未使用の変数/リスト/ブロードキャストを捨てる（非可逆）。
    bool dropUnusedVariables = false;
    /// コメントを全て捨てる（非可逆）。
    bool stripComments = false;
};

struct Sb3MinifyStats {
    size_t inputBytes = 0;
    size_t outputBytes = 0;
    size_t projectJsonIn = 0;
    size_t projectJsonOut = 0;

    size_t blocksBefore = 0;
    size_t blocksAfter = 0;
    size_t commentsBefore = 0;
    size_t commentsAfter = 0;
    size_t variablesBefore = 0;   // variables + lists + broadcasts
    size_t variablesAfter = 0;

    size_t assetsBefore = 0;      // project.json 以外の zip エントリ
    size_t assetsAfter = 0;
    size_t assetsDropped = 0;
    size_t assetsDeduped = 0;

    size_t zipCompressed = 0;
    size_t zipStored = 0;
};

struct Sb3MinifyResult {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> data;
    Sb3MinifyStats stats;
};

/// sb3 の zip バイト列を minify する。
Sb3MinifyResult minifySb3(const uint8_t* data, size_t size,
                          const Sb3MinifyOptions& options = {});

/// ファイルを読み、minify して outPath へ書く（UTF-8 パス）。
Sb3MinifyResult minifySb3File(const char* inPath, const char* outPath,
                              const Sb3MinifyOptions& options = {});

} // namespace xyo
