// RT-001: LLVM23 compile-time pointer-origin census. No GC addresses
// or interpreter frames are constructed; this is diagnostic only.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "../../src/jit/rt_ssa_extra_capture.hpp"
#include "../../src/ir/llvm_ssa_liveness.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <cstdio>
int main() {
    llvm::LLVMContext ctx;
    llvm::Module mod("rt001.pointer.origins", ctx);
    llvm::IRBuilder<> b(ctx);
    auto* ptr = b.getPtrTy();
    auto* i64 = b.getInt64Ty();
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(b.getVoidTy(),
            {ptr, b.getInt1Ty(), i64}, false),
        llvm::GlobalValue::ExternalLinkage, "probe", mod);
    auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    b.SetInsertPoint(entry);
    auto arg = fn->arg_begin();
    auto* ptrArg = &*arg++;
    auto* cond = &*arg++;
    auto* intArg = &*arg++;
    auto* stack = b.CreateAlloca(ptr);
    b.CreateStore(ptrArg, stack);
    auto* derived = b.CreateGEP(b.getInt8Ty(), ptrArg, b.getInt32(1));
    auto get = mod.getOrInsertFunction("get",
        llvm::FunctionType::get(ptr, false));
    auto* callResult = b.CreateCall(get);
    auto* loaded = b.CreateLoad(ptr, stack);
    auto* selected = b.CreateSelect(cond, derived, callResult);
    auto* opaque = b.CreateIntToPtr(
        b.CreateAdd(intArg, b.getInt64(1)), ptr);
    auto checkpoint = mod.getOrInsertFunction("checkpoint",
        llvm::FunctionType::get(b.getVoidTy(), false));
    auto* marker = b.CreateCall(checkpoint);
    auto consume = mod.getOrInsertFunction("consume",
        llvm::FunctionType::get(b.getVoidTy(),
            {ptr, ptr, ptr, ptr, ptr, ptr, ptr}, false));
    b.CreateCall(consume,
        {ptrArg, stack, derived, callResult, loaded, selected, opaque});
    b.CreateRetVoid();
    if (llvm::verifyFunction(*fn, &llvm::errs())) return 2;
    auto live = xyo::ir::analyzeLlvmCheckpointLiveness(*fn, *marker);
    auto extra = xyo::rt::analyzeExtraLiveSsa(*fn, *marker);
    std::printf("orig_unsupported=%u unresolved_ptr=%u ptr_origins=",
        live.unsupported, extra.unresolvedPointer);
    for (size_t i=0;i<extra.pointerOrigins.size();++i)
        std::printf("%s%u",i?",":"",extra.pointerOrigins[i]);
    std::printf(" other=%u valid=%d\n",
        extra.unresolvedOther, int(extra.valid));
    assert(extra.valid && live.validCheckpoint);
    assert(live.numeric.empty() && live.unsupported == 7);
    assert(extra.unresolvedPointer == 7 && extra.unresolvedOther == 0);
    for (auto count : extra.pointerOrigins) assert(count == 1);

    // Cross-function markers are forbidden; no source provenance may
    // accidentally be borrowed from another LLVM function.
    auto* foreign = llvm::Function::Create(
        llvm::FunctionType::get(b.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "foreign", mod);
    assert(!xyo::rt::analyzeExtraLiveSsa(*foreign, *marker).valid);
    return 0;
}
