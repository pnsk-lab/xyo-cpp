// xyo-cpp: MLIR 風 IR → LLVM IR の lowering。
#pragma once

#if defined(XYO_JIT) && !defined(__EMSCRIPTEN__)

#include <string>

#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "ir.hpp"

namespace xyo::mlir {

struct LoweredFunction {
    bool ok = false;
    llvm::Function* fn = nullptr;
    bool mayYield = false;
    /// mayYield のうち「自発的に譲る（毎反復・待機）」もの。false なら
    /// 予算 watchdog だけ（CompiledScript::cooperativeOnly 相当）。
    bool voluntaryYield = false;
    /// compat 層/clone 系ヘルパを emit した — 呼出し側スレッドの
    /// affinity（既存 codegen の CompiledScript::mainThread と同じ）。
    bool mainThread = false;
    std::string error;
};

/// `source` の全関数を `dest` へ lower し、`entryName` の `void(ptr)` を返す。
/// `coroMode` を立てると、スタックレス・コルーチン化（stackless::lower）が
/// 受理できる IR 形状で emit する — ループ yield は予算判定なしの
/// `xyo_yield_fast`、待機系は begin/poll 形、内部で yield する helper は
/// 使わない。
LoweredFunction lowerToLLVM(Module& source, llvm::Module& dest,
                            llvm::LLVMContext& lc,
                            const std::string& entryName,
                            bool coroMode = false);

} // namespace xyo::mlir

#endif // XYO_JIT && !__EMSCRIPTEN__
