#include "../../src/jit/guard_fallback_instrument.hpp"
#include <llvm/IR/Verifier.h>
#include <cassert>
#include <memory>

int main() {
    using namespace xyo;
    llvm::LLVMContext ctx;
    llvm::Module module("guard-probe", ctx);
    auto* type = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
    auto* function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, "test", module);
    for (const char* name : {
        "entry", "profile.other", "profile.other.1", "truthy.profile.other",
        "list.numeric.fallback", "eq.numeric.slow", "cmp.plain.scratch",
        "induction.fallback", "typed.ssa.convert", "profile.other.not_a_site"
    }) {
        auto* block = llvm::BasicBlock::Create(ctx, name, function);
        llvm::IRBuilder<> builder(block);
        builder.CreateRetVoid();
    }

    const bool on = JitGuardSlowStats::enabled();
    const auto sites = instrumentJitGuardSlowPaths(module);
    assert(sites == (on ? 8u : 0u));
    assert(!llvm::verifyModule(module, &llvm::errs()));
    unsigned calls = 0;
    for (auto& bb : *function) {
        for (auto& instruction : bb) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
            if (call && call->getCalledFunction() &&
                call->getCalledFunction()->getName() == "xyo_jit_guard_slow")
                ++calls;
        }
    }
    assert(calls == sites);
    if (on) {
        const auto& stats = JitGuardSlowStats::get();
        assert(stats.sites[size_t(JitGuardSlowKind::NumericProfile)] == 2);
        assert(stats.sites[size_t(JitGuardSlowKind::TypedSsaCacheMiss)] == 1);
    }
    return 0;
}
