// xyo-cpp: AOT（プロジェクト全体のネイティブコード事前生成）。
//
// `xyo aot project.sb3 -o app` は、全スクリプトを 1 つの LLVM モジュールへ
// emit → O2 → オブジェクトファイルにし、実行中の `xyo` バイナリの末尾へ
// [sb3][object] を追記した単体実行ファイルを作る。
//
// 生成コードはホストの絶対アドレスを埋め込まない（aot_emit.hpp の
// slot 機構）。ロード時に JitEngine::loadAot が実アドレスを書き込み、
// スクリプトを通常の JIT キャッシュへ登録する — 以降は JIT と
// まったく同じ経路で動く。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xyo {
struct Runtime;
}

namespace xyo::aot {

struct BuildResult {
    std::string object;  // ターゲットのオブジェクトファイル
    std::string meta;   // ロード用メタデータ（JSON）
    int scripts = 0;
    int slots = 0;
};

/// プロジェクト全体を AOT コンパイルする。GPU 並列ループは
/// ホスト側プログラムを指す定数を埋め込むため対象外（CPU ループへ）。
bool compileProject(Runtime& rt, uint64_t projectHash, BuildResult& out,
                    std::string* why);

/// FNV-1a 64bit（sb3 とメタの対応検査用）。
uint64_t hashBytes(const void* p, size_t n);

/// 実行中バイナリの末尾へ [sb3][object][footer] を追記して
/// 生成オブジェクトだけをファイルへ書く（`xyo aot --obj`。
/// `xyo run/test --aot <file>` でロードして検証する用途）。
bool writeObjectFile(const std::string& path, const BuildResult& build,
                     std::string* why);

/// 単体実行ファイルを書き出す（Linux）。
bool writeExecutable(const std::string& outPath, const void* sb3,
                     size_t sb3Len, const BuildResult& build,
                     uint64_t projectHash, std::string* why);

/// 自分自身が AOT 実行ファイルなら埋め込みプロジェクトと
/// オブジェクトを取り出す。通常の `xyo` では false。
bool readSelf(std::vector<uint8_t>& sb3, std::vector<uint8_t>& object,
              uint64_t* projectHash);

} // namespace xyo::aot
