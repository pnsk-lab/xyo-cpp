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

namespace xyo {

class Blocks;

namespace mlir {

struct BuildOutcome {
    bool ok = false;
    std::string reason;   // フォールバック理由（診断用）
    bool hasLoop = false;
};

/// 文字列リテラルを GC ルートへ固定するコールバック（boxed bits を返す）。
/// SStr は GC 管理なので、生成コードへ埋め込む前に JitPinned へ登録する。
using StringPinner = std::function<uint64_t(const std::u16string&)>;

/// `topId` のハットから下を `module` の関数 `entryName` へ変換する。
BuildOutcome buildScriptFromBlocks(Module& module, Blocks& blocks,
                                   const std::string& topId,
                                   const std::string& entryName,
                                   const StringPinner& pin = {},
                                   std::map<std::string, std::string>* procNames = nullptr);

} // namespace mlir
} // namespace xyo
