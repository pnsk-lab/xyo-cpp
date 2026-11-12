#pragma once

// PROF-004. Instruments a fixed, audited set of LLVM slow basic blocks.
// Instrumentation is opt-in, must run *before* LLVM optimization, and
// is never enabled in ordinary production modules or AOT modules.
#include "guard_fallback_telemetry.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

namespace xyo {

inline int classifyJitGuardSlowBlock(llvm::StringRef name) noexcept {
    // LLVM can add .1, .2... when a function contains repeated sites.
    const auto matches = [name](llvm::StringRef prefix) {
        if (name == prefix) return true;
        if (!name.starts_with(prefix)) return false;
        auto suffix = name.drop_front(prefix.size());
        if (!suffix.consume_front(".") || suffix.empty()) return false;
        for (char c : suffix)
            if (c < '0' || c > '9') return false;
        return true;
    };
    if (matches("profile.other"))
        return int(JitGuardSlowKind::NumericProfile);
    if (matches("truthy.profile.other"))
        return int(JitGuardSlowKind::TruthyProfile);
    if (matches("list.numeric.fallback"))
        return int(JitGuardSlowKind::NumericList);
    if (matches("eq.numeric.slow"))
        return int(JitGuardSlowKind::NumericEquality);
    if (matches("cmp.plain.scratch"))
        return int(JitGuardSlowKind::PlainComparison);
    if (matches("induction.fallback"))
        return int(JitGuardSlowKind::Induction);
    if (matches("typed.ssa.convert"))
        return int(JitGuardSlowKind::TypedSsaCacheMiss);
    return -1;
}

// Counter function is defined in backend_llvm.cpp and registered as an ORC
// absolute symbol. No generated code may reference it with stats disabled.
inline size_t instrumentJitGuardSlowPaths(llvm::Module& module) {
    if (!JitGuardSlowStats::enabled()) return 0;
    auto* voidTy = llvm::Type::getVoidTy(module.getContext());
    auto* i32Ty = llvm::Type::getInt32Ty(module.getContext());
    llvm::FunctionCallee counter = module.getOrInsertFunction(
        "xyo_jit_guard_slow", llvm::FunctionType::get(
            voidTy, {i32Ty}, false));
    size_t total = 0;
    for (llvm::Function& function : module) {
        if (function.isDeclaration()) continue;
        for (llvm::BasicBlock& block : function) {
            const int kind = classifyJitGuardSlowBlock(block.getName());
            if (kind < 0 || block.empty()) continue;
            llvm::IRBuilder<> builder(&block, block.getFirstInsertionPt());
            builder.CreateCall(counter, {builder.getInt32(unsigned(kind))});
            JitGuardSlowStats::get().noteSite(JitGuardSlowKind(kind));
            ++total;
        }
    }
    return total;
}
} // namespace xyo
