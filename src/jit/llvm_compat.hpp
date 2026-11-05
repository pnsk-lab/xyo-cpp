// xyo-cpp: LLVM のバージョン差を吸収する小さな互換ヘルパ。
//
// LLVM 19 で `TargetRegistry::lookupTarget` / `Target::createTargetMachine` /
// `Module::setTargetTriple` が StringRef から `const Triple&` を取る形へ変わり、
// `orc::ThreadSafeContext::withContextDo` が追加された。18 系（Ubuntu の
// llvm-18-dev は静的アーカイブを同梱するため静的リンクに使える）でも
// ビルドできるようにここで吸収する。
#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>

namespace xyo::llvmc {

/// lookupTarget / createTargetMachine / setTargetTriple へ渡す三つ組引数。
/// LLVM 19までは文字列。LLVM 20以降は Triple を受け取る。
#if LLVM_VERSION_MAJOR >= 20
using TripleArg = const llvm::Triple&;
inline const llvm::Triple& tripleArg(const llvm::Triple& t) { return t; }
#else
using TripleArg = llvm::StringRef;
inline llvm::StringRef tripleArg(const llvm::Triple& t) { return t.str(); }
#endif

/// ThreadSafeContext のロックを取って LLVMContext を渡す。
/// LLVM 20+ は withContextDo、19 以下は getLock()+getContext() を使う。
template <typename Fn>
inline void withContext(llvm::orc::ThreadSafeContext& tsc, Fn&& fn) {
#if LLVM_VERSION_MAJOR >= 20
    tsc.withContextDo([&](llvm::LLVMContext* lc) { fn(lc); });
#else
    auto lock = tsc.getLock();
    fn(tsc.getContext());
#endif
}

/// intrinsic の宣言を取得する（無ければモジュールへ追加）。
/// LLVM 23 で getDeclaration から getOrInsertDeclaration に改名された。
/// CMake が XYO_LLVM_GET_OR_INSERT を定義した場合だけ新しい名前を使う。
inline llvm::Function* intrinsicDeclaration(
        llvm::Module* m, llvm::Intrinsic::ID id,
        llvm::ArrayRef<llvm::Type*> overloads = {}) {
#if defined(XYO_LLVM_GET_OR_INSERT)
    return llvm::Intrinsic::getOrInsertDeclaration(m, id, overloads);
#else
    return llvm::Intrinsic::getDeclaration(m, id, overloads);
#endif
}

} // namespace xyo::llvmc
