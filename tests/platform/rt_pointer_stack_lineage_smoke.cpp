// RT-001: native stack pointer lineage is a diagnostic hazard classification.
// It is NOT a GC root, and is NEVER a restorable pointer across Fiber release.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/jit/rt_ssa_extra_capture.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <cassert>
#include <cstdio>

using namespace llvm;

int main() {
    LLVMContext context;
    Module module("rt001-stack-lineage", context);
    IRBuilder<> b(context);
    auto* ptrType = b.getPtrTy();
    auto* fn = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getInt1Ty(), ptrType}, false),
        GlobalValue::ExternalLinkage, "lineage", module);
    auto arg = fn->arg_begin();
    Value* condition = &*arg++;
    Value* externalPtr = &*arg;
    auto* entry = BasicBlock::Create(context, "entry", fn);
    auto* thenBlock = BasicBlock::Create(context, "then", fn);
    auto* elseBlock = BasicBlock::Create(context, "else", fn);
    auto* merge = BasicBlock::Create(context, "merge", fn);
    b.SetInsertPoint(entry);
    auto* stackA = b.CreateAlloca(b.getInt8Ty());
    auto* stackB = b.CreateAlloca(b.getInt8Ty());
    auto* derivedA = b.CreateGEP(b.getInt8Ty(), stackA, b.getInt32(1));
    auto* derivedExternal =
        b.CreateGEP(b.getInt8Ty(), externalPtr, b.getInt32(1));
    auto* loaded = b.CreateLoad(ptrType,
        b.CreateAlloca(ptrType));
    auto* intToPtr = b.CreateIntToPtr(b.getInt64(42), ptrType);
    b.CreateCondBr(condition, thenBlock, elseBlock);
    b.SetInsertPoint(thenBlock);
    auto* thenA = b.CreateGEP(b.getInt8Ty(), stackA, b.getInt32(2));
    b.CreateBr(merge);
    b.SetInsertPoint(elseBlock);
    auto* elseA = b.CreateGEP(b.getInt8Ty(), stackA, b.getInt32(3));
    auto* elseB = b.CreateGEP(b.getInt8Ty(), stackB, b.getInt32(4));
    b.CreateBr(merge);
    b.SetInsertPoint(merge);
    auto* phiSame = b.CreatePHI(ptrType, 2);
    phiSame->addIncoming(thenA, thenBlock);
    phiSame->addIncoming(elseA, elseBlock);
    auto* phiMixed = b.CreatePHI(ptrType, 2);
    phiMixed->addIncoming(thenA, thenBlock);
    phiMixed->addIncoming(elseB, elseBlock);
    auto* selectSame = b.CreateSelect(condition, derivedA, phiSame);
    auto* selectMixed = b.CreateSelect(condition, derivedA, derivedExternal);
    auto checkpoint = module.getOrInsertFunction(
        "checkpoint", FunctionType::get(b.getVoidTy(), false));
    auto* marker = b.CreateCall(checkpoint);
    auto consume = module.getOrInsertFunction("consume",
        FunctionType::get(b.getVoidTy(),
            {ptrType, ptrType, ptrType, ptrType, ptrType,
             ptrType, ptrType, ptrType, ptrType, ptrType},
            false));
    b.CreateCall(consume,
        {stackA, stackB, derivedA, derivedExternal, loaded,
         intToPtr, phiSame, phiMixed, selectSame, selectMixed});
    b.CreateRetVoid();

    assert(!verifyFunction(*fn, &errs()));
    const auto* a = xyo::rt::nativeStackPointerRoot(stackA);
    assert(a == stackA);
    assert(xyo::rt::nativeStackPointerRoot(derivedA) == a);
    assert(xyo::rt::nativeStackPointerRoot(phiSame) == a);
    assert(xyo::rt::nativeStackPointerRoot(selectSame) == a);
    assert(xyo::rt::nativeStackPointerRoot(stackB) == stackB);
    assert(xyo::rt::nativeStackPointerRoot(derivedExternal) == nullptr);
    assert(xyo::rt::nativeStackPointerRoot(externalPtr) == nullptr);
    assert(xyo::rt::nativeStackPointerRoot(loaded) == nullptr);
    assert(xyo::rt::nativeStackPointerRoot(intToPtr) == nullptr);
    assert(xyo::rt::nativeStackPointerRoot(phiMixed) == nullptr);
    assert(xyo::rt::nativeStackPointerRoot(selectMixed) == nullptr);

    auto report = xyo::rt::analyzeExtraLiveSsa(*fn, *marker);
    assert(report.valid);
    assert(report.stackDerivedPointers >= 5);
    assert(report.distinctStackAllocas == 2);
    assert(report.unresolvedPointer >= report.stackDerivedPointers);

    // A well-formed LLVM pointer PHI that carries itself around a loop
    // has no finite alloca-only proof; recursion MUST reject the cycle.
    auto* cycleFn = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getInt1Ty()}, false),
        GlobalValue::ExternalLinkage, "cycle", module);
    auto* cycleEntry = BasicBlock::Create(context, "entry", cycleFn);
    auto* loop = BasicBlock::Create(context, "loop", cycleFn);
    auto* end = BasicBlock::Create(context, "end", cycleFn);
    b.SetInsertPoint(cycleEntry);
    auto* root = b.CreateAlloca(b.getInt8Ty());
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    auto* cyclePhi = b.CreatePHI(ptrType, 2);
    cyclePhi->addIncoming(root, cycleEntry);
    cyclePhi->addIncoming(cyclePhi, loop);
    b.CreateCondBr(&*cycleFn->arg_begin(), loop, end);
    b.SetInsertPoint(end);
    auto sink = module.getOrInsertFunction("consume_one",
        FunctionType::get(b.getVoidTy(), {ptrType}, false));
    b.CreateCall(sink, {cyclePhi});
    b.CreateRetVoid();
    assert(!verifyFunction(*cycleFn, &errs()));
    assert(xyo::rt::nativeStackPointerRoot(cyclePhi) == nullptr);

    // Pathological long pointer chains should fail closed, not recurse
    // unboundedly. LLVM itself still has valid IR.
    auto* deepFn = Function::Create(
        FunctionType::get(b.getVoidTy(), false),
        GlobalValue::ExternalLinkage, "deep", module);
    auto* deepEntry = BasicBlock::Create(context, "entry", deepFn);
    b.SetInsertPoint(deepEntry);
    Value* deep = b.CreateAlloca(b.getInt8Ty());
    for (int i = 0; i < 80; ++i)
        deep = b.CreateGEP(b.getInt8Ty(), deep, b.getInt32(1));
    b.CreateCall(sink, {deep});
    b.CreateRetVoid();
    assert(!verifyFunction(*deepFn, &errs()));
    assert(xyo::rt::nativeStackPointerRoot(deep) == nullptr);

    std::printf("PASS stack lineage: exact-root PHI/SELECT, mixed/unknown, "
                "self-cycle, 80-deep chain, live slots=%u roots=%u\n",
                report.stackDerivedPointers, report.distinctStackAllocas);
    return 0;
}
