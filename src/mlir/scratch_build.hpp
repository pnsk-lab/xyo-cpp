// xyo-cpp: Scratch ブロック木 → MLIR 風 IR（xyo dialect）のビルダー。
//
// 対応するのは「LLVM 前段で扱える」安全なサブセットだけ。1 つでも
// 未知の opcode・テキスト定数・手続き呼出し・比較の特殊系が現れたら
// ビルド全体を失敗させ、呼び出し側は既存 codegen へフォールバックする。
#pragma once

#include <functional>
#include <map>
#include <string>

#include "ir.hpp"
#include "core/value.hpp"

namespace xyo {

class Blocks;

namespace mlir {

struct BuildOutcome {
    bool ok = false;
    std::string reason;   // フォールバック理由（診断用）
    bool hasLoop = false;
    /// 内部で yield する helper（compat・glide・sayforsecs 等）を使った。
    /// これがあるとスタックレス・コルーチン化はできない。
    bool internalYield = false;
};

/// ビルド時のオプション。
struct BuildOptions {
    /// スタックレス・コルーチン化を前提とした IR を作る
    /// （ループ yield を予算判定なしの直接 yield にする等）。
    bool coroMode = false;
};

/// 文字列リテラルを GC ルートへ固定するコールバック（boxed bits を返す）。
/// SStr は GC 管理なので、生成コードへ埋め込む前に JitPinned へ登録する。
/// 既存の Value をそのまま返すことで、shadow が保持する SStr への
/// **ポインタ同一性**が保たれる（テストは stored.bits == shadow bits を
/// 要求する。新規 SStr に変換すると破れる）。
using StringPinner = std::function<uint64_t(const xyo::Value&)>;

/// `topId` のハットから下を `module` の関数 `entryName` へ変換する。
BuildOutcome buildScriptFromBlocks(Module& module, Blocks& blocks,
                                   const std::string& topId,
                                   const std::string& entryName,
                                   const StringPinner& pin = {},
                                   std::map<std::string, std::string>* procNames = nullptr,
                                   const BuildOptions* options = nullptr);

} // namespace mlir
} // namespace xyo
