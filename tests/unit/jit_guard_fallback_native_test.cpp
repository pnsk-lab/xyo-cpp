#include "../../src/jit/guard_fallback_instrument.hpp"
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <cassert>
#include <cstdint>

extern "C" void xyo_jit_guard_slow(uint32_t kind) noexcept {
    xyo::JitGuardSlowStats::get().noteHit(kind);
}

int main() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto maybeJit = llvm::orc::LLJITBuilder().create();
    assert(bool(maybeJit));
    auto jit = std::move(*maybeJit);
    auto& es = jit->getExecutionSession();
    llvm::orc::SymbolMap symbols;
    symbols[es.intern("xyo_jit_guard_slow")] =
        llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr::fromPtr(
                reinterpret_cast<void*>(&xyo_jit_guard_slow)),
            llvm::JITSymbolFlags::Exported);
    if (auto err = jit->getMainJITDylib().define(
            llvm::orc::absoluteSymbols(std::move(symbols)))) {
        llvm::consumeError(std::move(err));
        assert(false && "could not register runtime counter");
    }

    auto context = llvm::orc::ThreadSafeContext(
        std::make_unique<llvm::LLVMContext>());
    std::unique_ptr<llvm::Module> module;
    context.withContextDo([&](llvm::LLVMContext* ctx) {
        module = std::make_unique<llvm::Module>("slow-test", *ctx);
        module->setDataLayout(jit->getDataLayout());
        auto* i32 = llvm::Type::getInt32Ty(*ctx);
        auto* ft = llvm::FunctionType::get(i32, {i32}, false);
        auto* fn = llvm::Function::Create(
            ft, llvm::GlobalValue::ExternalLinkage, "slow_test", *module);
        auto* entry = llvm::BasicBlock::Create(*ctx, "entry", fn);
        auto* slow = llvm::BasicBlock::Create(*ctx, "profile.other", fn);
        auto* fast = llvm::BasicBlock::Create(*ctx, "fast", fn);
        llvm::IRBuilder<> b(entry);
        b.CreateCondBr(b.CreateICmpEQ(fn->getArg(0), b.getInt32(0)), slow, fast);
        b.SetInsertPoint(slow);
        b.CreateRet(b.getInt32(42));
        b.SetInsertPoint(fast);
        b.CreateRet(b.getInt32(7));
        const bool on = xyo::JitGuardSlowStats::enabled();
        assert(xyo::instrumentJitGuardSlowPaths(*module) == (on ? 1u : 0u));
        assert(!llvm::verifyModule(*module, &llvm::errs()));
    });

    if (auto err = jit->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
        llvm::consumeError(std::move(err));
        assert(false && "could not add module");
    }
    auto maybeEntry = jit->lookup("slow_test");
    assert(bool(maybeEntry));
    using Fn = int (*)(int);
    auto fn = reinterpret_cast<Fn>(maybeEntry->getValue());
    assert(fn(0) == 42);
    assert(fn(1) == 7);
    assert(fn(0) == 42);
    if (xyo::JitGuardSlowStats::enabled()) {
        auto& stats = xyo::JitGuardSlowStats::get();
        const size_t category = size_t(xyo::JitGuardSlowKind::NumericProfile);
        assert(stats.sites[category].load() == 1);
        assert(stats.hits[category].load() == 2);
    }
    return 0;
}
