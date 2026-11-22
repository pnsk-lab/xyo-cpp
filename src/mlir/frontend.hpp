// xyo-cpp: MLIR 風 IR フロントエンドの入口（LLVM 前段への接続）。
#pragma once

#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace xyo {

struct Runtime;
struct Target;
struct Blocks;
struct CompiledScript;
struct JitPinned;

namespace mlir {

/// **既定で有効**。`XYO_MLIR=0`（または `XYO_JIT_MLIR=0`）で従来の
/// codegen に戻せる。テスト用にプロセス内で強制できる（-1 で環境変数へ
/// 戻す）。未対応・失敗時は `tryEmitScript` が false を返し、従来経路が
/// そのまま走る（フォールバックは常に安全側）。
void setForceEnabled(int value);
bool isEnabled();

/// このプロセスで MLIR 経路が生成したスクリプト数（診断/テスト用）。
uint64_t compiledScripts();
/// サブセット外で既存 codegen へフォールバックした回数。
uint64_t fallbackScripts();

/// `jitEmitScript` の先頭から呼ばれる。成功したら true（`out` と
/// `pendingFnNames` を埋め、`mod` に `entryName` の関数を追加済み）。
/// false のとき `mod` は一切変更していない。
bool tryEmitScript(llvm::Module& mod, llvm::LLVMContext& lc,
                   Runtime& runtime, Target* target, Blocks* blocks,
                   const std::string& topBlockId,
                   const std::string& entryName, CompiledScript& out,
                   JitPinned& pinned,
                   std::vector<std::string>& pendingFnNames,
                   std::map<std::string, std::string>& procNameMap);

/// テスト用: 直近に生成した MLIR テキストを返す。
std::string lastDump();

} // namespace mlir
} // namespace xyo

#endif // XYO_JIT && !__EMSCRIPTEN__
