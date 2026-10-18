// xyo-cpp: LLVM のバージョン差を吸収する小さな互換ヘルパ。
//
// LLVM 19 で `TargetRegistry::lookupTarget` / `Target::createTargetMachine` /
// `Module::setTargetTriple` が StringRef から `const Triple&` を取る形へ変わり、
// `orc::ThreadSafeContext::withContextDo` が追加された。18 系（Ubuntu の
// llvm-18-dev は静的アーカイブを同梱するため静的リンクに使える）でも
// ビルドできるようにここで吸収する。
#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/TargetParser/Triple.h>

namespace xyo::llvmc {

/// lookupTarget / createTargetMachine / setTargetTriple へ渡す三つ組引数。
#if LLVM_VERSION_MAJOR >= 19
using TripleArg = const llvm::Triple&;
inline const llvm::Triple& tripleArg(const llvm::Triple& t) { return t; }
#else
using TripleArg = llvm::StringRef;
inline llvm::StringRef tripleArg(const llvm::Triple& t) { return t.str(); }
#endif

/// ThreadSafeContext のロックを取って LLVMContext を渡す。
/// LLVM 19+ は withContextDo、18 は getLock()+getContext() を使う。
template <typename Fn>
inline void withContext(llvm::orc::ThreadSafeContext& tsc, Fn&& fn) {
#if LLVM_VERSION_MAJOR >= 19
    tsc.withContextDo([&](llvm::LLVMContext* lc) { fn(lc); });
#else
    auto lock = tsc.getLock();
    fn(tsc.getContext());
#endif
}

} // namespace xyo::llvmc
