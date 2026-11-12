// Audited attributes for external helpers whose implementation is not in the
// generated LLVM module. Keep this whitelist separate from opcode effects:
// a Scratch-pure reporter may still update caches/profiling or read volatile data.
#pragma once

#include <llvm/IR/Function.h>
#include <llvm/Support/ModRef.h>
#include <cstdlib>
#include <string_view>

namespace xyo::llvmc {

inline void applyHelperAttributes(llvm::FunctionCallee callee,
                                  std::string_view name) {
    // rt.cpp -> v8math.cpp: no caches, profiling, volatile memory, allocation,
    // callbacks or lazy initialization. sqrt is reached only with nonnegative
    // arguments, so it cannot set domain errno. Ordinary (non-strict) FP only:
    // this is not a claim to preserve observable floating-point exception flags.
    // Do NOT include atan/exp (volatile accesses), log/round (RTProf), or
    // to_num (string caches), even though they look like numeric functions.
    if (name != "xyo_asin" && name != "xyo_acos") return;
    static const bool enabled = [] {
      const char* value = std::getenv("XYO_JIT_PURE_MATH");
      return !value || value[0] != '0';
    }();
    if (!enabled) return;
    auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee());
    if (!f || !f->isDeclaration() || f->arg_size() != 1 ||
        !f->getReturnType()->isDoubleTy() ||
        !f->getFunctionType()->getParamType(0)->isDoubleTy() ||
        f->isVarArg()) return;
    f->setMemoryEffects(llvm::MemoryEffects::none());
    f->addFnAttr(llvm::Attribute::NoUnwind);
    f->addFnAttr(llvm::Attribute::WillReturn);
    // No speculatable/fast-math/noalias claims: those need separate proofs.
}

} // namespace xyo::llvmc
