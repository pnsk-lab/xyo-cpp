// RT-001: compile-time LLVM pointer address-region audit.
// This is intentionally NOT permission to GC a suspended native Fiber:
// loaded pointers and heap objects reachable through a host context or
// stack slot are still unproven. Assert the real fail-closed IR cases.
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
using xyo::rt::PointerRegion;

int main() {
    LLVMContext context;
    Module mod("rt001-pointer-region", context);
    IRBuilder<> b(context);
    auto* ptr = b.getPtrTy();
    auto* fn = Function::Create(
        FunctionType::get(b.getVoidTy(),
            {ptr, ptr, b.getInt1Ty(), b.getInt64Ty()}, false),
        GlobalValue::ExternalLinkage, "entry", mod);
    auto args = fn->arg_begin();
    auto* hostCtx = &*args++;
    auto* untrusted = &*args++;
    auto* choose = &*args++;
    auto* dynamicAddress = &*args;

    auto* global = new GlobalVariable(
        mod, b.getInt64Ty(), true, GlobalValue::PrivateLinkage,
        b.getInt64(17), "static-table");
    auto* entry = BasicBlock::Create(context, "entry", fn);
    auto* thenBlock = BasicBlock::Create(context, "then", fn);
    auto* elseBlock = BasicBlock::Create(context, "else", fn);
    auto* joinBlock = BasicBlock::Create(context, "join", fn);
    b.SetInsertPoint(entry);
    auto* stackA = b.CreateAlloca(ptr);
    auto* stackB = b.CreateAlloca(ptr);
    b.CreateStore(untrusted, stackA); // Content MAY hold heap pointers.
    auto* stackDerived = b.CreateGEP(b.getInt8Ty(), stackA,
                                     b.getInt32(1));
    auto* ctxDerived = b.CreateGEP(b.getInt8Ty(), hostCtx,
                                   b.getInt32(8));
    auto* ctxPointerIndex = b.CreateGEP(
        ptr, hostCtx, b.getInt32(1));
    auto* ctxDynamic = b.CreateGEP(
        b.getInt8Ty(), hostCtx, dynamicAddress);
    auto* ctxNegative = b.CreateGEP(
        b.getInt8Ty(), hostCtx, b.getInt32(-1));
    auto* ctxUnexpectedElement = b.CreateGEP(
        b.getInt32Ty(), hostCtx, b.getInt32(1));
    auto* globalDerived = b.CreateGEP(
        b.getInt8Ty(), global, b.getInt32(1));
    auto* unknownDerived = b.CreateGEP(
        b.getInt8Ty(), untrusted, b.getInt32(1));
    auto* loaded = b.CreateLoad(ptr, stackA);
    auto* loadedFromHost = b.CreateLoad(ptr, ctxDerived);
    auto* secondLoadFromHost = b.CreateLoad(ptr, loadedFromHost);
    auto* thirdLoadAddress = b.CreateGEP(
        b.getInt8Ty(), secondLoadFromHost, b.getInt32(4));
    auto* thirdLoadFromHost = b.CreateLoad(ptr, thirdLoadAddress);
    auto* loadedFromUnknown = b.CreateLoad(ptr, untrusted);
    auto* staticPointerSlot = new GlobalVariable(
        mod, ptr, false, GlobalValue::PrivateLinkage,
        ConstantPointerNull::get(cast<PointerType>(ptr)), "static-ptr");
    auto* loadedFromStatic = b.CreateLoad(ptr, staticPointerSlot);
    auto* derivedLoaded = b.CreateGEP(
        b.getInt8Ty(), loaded, b.getInt32(1));
    auto factory = mod.getOrInsertFunction(
        "pointer_factory", FunctionType::get(ptr, false));
    auto* returnedPointer = b.CreateCall(factory);
    auto* fabricated = b.CreateIntToPtr(
        b.CreateAdd(dynamicAddress, b.getInt64(12345)), ptr);
    auto* fromCtxOrUnknown =
        b.CreateSelect(choose, ctxDerived, unknownDerived);
    auto* fromCtxOrStatic =
        b.CreateSelect(choose, ctxDerived, global);
    auto* fromCtx =
        b.CreateSelect(choose, ctxDerived, hostCtx);
    auto* fromGlobal =
        b.CreateSelect(choose, global, globalDerived);
    b.CreateCondBr(choose, thenBlock, elseBlock);

    b.SetInsertPoint(thenBlock);
    auto* ctxBranch = b.CreateGEP(b.getInt8Ty(), hostCtx,
                                  b.getInt32(2));
    auto* stackBranch = b.CreateGEP(b.getInt8Ty(), stackA,
                                    b.getInt32(4));
    b.CreateBr(joinBlock);
    b.SetInsertPoint(elseBlock);
    auto* ctxOther = b.CreateGEP(b.getInt8Ty(), hostCtx,
                                 b.getInt32(4));
    auto* stackOther = b.CreateGEP(b.getInt8Ty(), stackB,
                                   b.getInt32(2));
    b.CreateBr(joinBlock);

    b.SetInsertPoint(joinBlock);
    auto* sameCtxPhi = b.CreatePHI(ptr, 2);
    sameCtxPhi->addIncoming(ctxBranch, thenBlock);
    sameCtxPhi->addIncoming(ctxOther, elseBlock);
    auto* mixedStackPhi = b.CreatePHI(ptr, 2);
    mixedStackPhi->addIncoming(stackBranch, thenBlock);
    mixedStackPhi->addIncoming(stackOther, elseBlock);
    auto* marker = b.CreateCall(mod.getOrInsertFunction(
        "checkpoint", FunctionType::get(b.getVoidTy(), false)));
    b.CreateCall(mod.getOrInsertFunction(
        "consume", FunctionType::get(b.getVoidTy(),
            {ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr,
             ptr, ptr, ptr, ptr}, false)),
        {hostCtx, untrusted, stackA, stackB, stackDerived,
         ctxDerived, globalDerived, unknownDerived, loaded,
         fabricated, fromCtxOrUnknown, fromCtxOrStatic,
         sameCtxPhi, mixedStackPhi});
    b.CreateCall(mod.getOrInsertFunction(
        "consume_extra", FunctionType::get(b.getVoidTy(),
            {ptr, ptr, ptr}, false)),
        {fromCtx, global, fromGlobal});
    b.CreateCall(mod.getOrInsertFunction(
        "consume_hazards", FunctionType::get(b.getVoidTy(),
            {ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr}, false)),
        {loadedFromHost, loadedFromUnknown, loadedFromStatic,
         derivedLoaded, returnedPointer, staticPointerSlot,
         secondLoadFromHost, thirdLoadFromHost});
    b.CreateRetVoid();
    assert(!verifyFunction(*fn, &errs()));

    using xyo::rt::classifyPointerRegion;
    assert(classifyPointerRegion(hostCtx) == PointerRegion::Unknown);
    assert(classifyPointerRegion(hostCtx, hostCtx) ==
           PointerRegion::HostContext);
    assert(classifyPointerRegion(ctxDerived, hostCtx) ==
           PointerRegion::HostContext);
    assert(classifyPointerRegion(global) == PointerRegion::StaticGlobal);
    // LLVM often constant-folds static GEP into ConstantExpr, which
    // remains static only through explicitly recognized operators.
    assert(classifyPointerRegion(globalDerived) ==
           PointerRegion::StaticGlobal);
    assert(classifyPointerRegion(fromGlobal, hostCtx) ==
           PointerRegion::StaticGlobal);
    assert(classifyPointerRegion(stackA, hostCtx) ==
           PointerRegion::NativeStack);
    assert(classifyPointerRegion(stackDerived, hostCtx) ==
           PointerRegion::NativeStack);
    assert(classifyPointerRegion(untrusted, hostCtx) ==
           PointerRegion::Unknown);
    assert(classifyPointerRegion(loaded, hostCtx) ==
           PointerRegion::Unknown);
    assert(classifyPointerRegion(fabricated, hostCtx) ==
           PointerRegion::Unknown);
    assert(classifyPointerRegion(fromCtx, hostCtx) ==
           PointerRegion::HostContext);
    assert(classifyPointerRegion(fromCtxOrUnknown, hostCtx) ==
           PointerRegion::Unknown);
    assert(classifyPointerRegion(fromCtxOrStatic, hostCtx) ==
           PointerRegion::Unknown);
    assert(classifyPointerRegion(sameCtxPhi, hostCtx) ==
           PointerRegion::HostContext);
    assert(classifyPointerRegion(mixedStackPhi, hostCtx) ==
           PointerRegion::Unknown);
    using xyo::rt::UnknownPointerCause;
    using xyo::rt::classifyUnknownPointerCause;
    assert(classifyUnknownPointerCause(loaded, hostCtx) ==
           UnknownPointerCause::LoadStack);
    assert(classifyUnknownPointerCause(derivedLoaded, hostCtx) ==
           UnknownPointerCause::LoadStack);
    assert(classifyUnknownPointerCause(loadedFromHost, hostCtx) ==
           UnknownPointerCause::LoadHost);
    assert(classifyUnknownPointerCause(loadedFromStatic, hostCtx) ==
           UnknownPointerCause::LoadStatic);
    assert(classifyUnknownPointerCause(loadedFromUnknown, hostCtx) ==
           UnknownPointerCause::LoadUnknown);
    assert(classifyUnknownPointerCause(returnedPointer, hostCtx) ==
           UnknownPointerCause::CallReturn);
    assert(classifyUnknownPointerCause(fabricated, hostCtx) ==
           UnknownPointerCause::IntOrAddressCast);
    assert(classifyUnknownPointerCause(untrusted, hostCtx) ==
           UnknownPointerCause::ForeignArgument);
    assert(classifyUnknownPointerCause(fromCtxOrUnknown, hostCtx) ==
           UnknownPointerCause::PhiSelect);
    assert(classifyPointerRegion(loadedFromHost, hostCtx) ==
           PointerRegion::Unknown);
    assert(classifyPointerRegion(loadedFromStatic, hostCtx) ==
           PointerRegion::Unknown);
    using xyo::rt::PointerLoadTerminal;
    using xyo::rt::tracePointerLoadChain;
    auto one = tracePointerLoadChain(loaded, hostCtx);
    assert(one.dereferences == 1);
    assert(one.terminal == PointerLoadTerminal::NativeStack);
    auto two = tracePointerLoadChain(secondLoadFromHost, hostCtx);
    assert(two.dereferences == 2 && two.addressDerivations >= 1);
    assert(two.terminal == PointerLoadTerminal::HostContext);
    auto three = tracePointerLoadChain(thirdLoadFromHost, hostCtx);
    assert(three.dereferences == 3 && three.addressDerivations >= 2);
    assert(three.terminal == PointerLoadTerminal::HostContext);
    assert(one.hostContextFieldOffset == std::nullopt);
    assert(two.hostContextFieldOffset == 8);
    assert(three.hostContextFieldOffset == 8);
    using xyo::rt::constantHostContextOffset;
    assert(constantHostContextOffset(ctxDerived, hostCtx) == 8);
    assert(constantHostContextOffset(ctxPointerIndex, hostCtx) ==
           sizeof(void*));
    assert(!constantHostContextOffset(ctxDynamic, hostCtx));
    assert(!constantHostContextOffset(ctxNegative, hostCtx));
    assert(!constantHostContextOffset(ctxUnexpectedElement, hostCtx));
    assert(!constantHostContextOffset(ctxDerived, untrusted));
    assert(classifyPointerRegion(secondLoadFromHost, hostCtx) ==
           PointerRegion::Unknown);
    assert(classifyPointerRegion(thirdLoadFromHost, hostCtx) ==
           PointerRegion::Unknown);
    auto foreign = tracePointerLoadChain(loadedFromUnknown, hostCtx);
    assert(foreign.dereferences == 1 &&
           foreign.terminal == PointerLoadTerminal::ForeignArgument);
    auto staticLoad = tracePointerLoadChain(loadedFromStatic, hostCtx);
    assert(staticLoad.dereferences == 1 &&
           staticLoad.terminal == PointerLoadTerminal::StaticGlobal);
    auto viaCall = tracePointerLoadChain(returnedPointer, hostCtx);
    assert(viaCall.dereferences == 0 &&
           viaCall.terminal == PointerLoadTerminal::CallReturn);
    auto viaInt = tracePointerLoadChain(fabricated, hostCtx);
    assert(viaInt.dereferences == 0 &&
           viaInt.terminal == PointerLoadTerminal::IntegerOrAddressCast);
    auto viaMerge = tracePointerLoadChain(mixedStackPhi, hostCtx);
    assert(viaMerge.terminal == PointerLoadTerminal::Merge);

    const auto report =
        xyo::rt::analyzeExtraLiveSsa(*fn, *marker, hostCtx);
    assert(report.valid);
    unsigned total = 0;
    for (unsigned count : report.pointerRegions) total += count;
    assert(total == report.unresolvedPointer);
    assert(report.pointerRegions[
        size_t(PointerRegion::NativeStack)] ==
        report.stackDerivedPointers);
    assert(report.pointerRegions[
        size_t(PointerRegion::HostContext)] >= 3);
    assert(report.pointerRegions[
        size_t(PointerRegion::StaticGlobal)] >= 1);
    assert(report.pointerRegions[
        size_t(PointerRegion::Unknown)] >= 5);

    unsigned unknownTotal = 0;
    for (unsigned n : report.unknownPointerCauses) unknownTotal += n;
    assert(unknownTotal == report.pointerRegions[
        size_t(PointerRegion::Unknown)]);
    for (auto cause : {UnknownPointerCause::LoadStack,
                       UnknownPointerCause::LoadHost,
                       UnknownPointerCause::LoadStatic,
                       UnknownPointerCause::LoadUnknown,
                       UnknownPointerCause::CallReturn,
                       UnknownPointerCause::IntOrAddressCast,
                       UnknownPointerCause::ForeignArgument,
                       UnknownPointerCause::PhiSelect})
        assert(report.unknownPointerCauses[size_t(cause)] != 0);
    assert(report.unknownPointerDetails.size() == unknownTotal);
    for (const auto& detail : report.unknownPointerDetails)
        assert(xyo::rt::unknownPointerCauseName(detail.cause)[0]);
    size_t countedLoadCauses = 0;
    for (unsigned i = 0; i <=
            size_t(UnknownPointerCause::LoadUnknown); ++i)
        countedLoadCauses += report.unknownPointerCauses[i];
    assert(report.unknownLoadChains.size() == countedLoadCauses);
    bool sawThreeLoads = false;
    for (const auto& detail : report.unknownLoadChains) {
        assert(detail.chain.dereferences >= 1);
        if (detail.chain.dereferences == 3 &&
            detail.chain.terminal == PointerLoadTerminal::HostContext)
            sawThreeLoads = true;
    }
    assert(sawThreeLoads);

    auto noTrust = xyo::rt::analyzeExtraLiveSsa(*fn, *marker);
    assert(noTrust.valid);
    assert(noTrust.pointerRegions[
        size_t(PointerRegion::HostContext)] == 0);
    assert(!xyo::rt::analyzeExtraLiveSsa(*fn, *marker, untrusted).valid);

    // A self-loop and an 80-deep context-derived GEP chain must not
    // elude cycle/depth guards to gain false host provenance.
    auto* loopFn = Function::Create(
        FunctionType::get(b.getVoidTy(), {ptr, b.getInt1Ty()}, false),
        GlobalValue::ExternalLinkage, "loop", mod);
    auto* loopEntry = BasicBlock::Create(context, "entry", loopFn);
    auto* loopBody = BasicBlock::Create(context, "body", loopFn);
    auto* loopEnd = BasicBlock::Create(context, "end", loopFn);
    b.SetInsertPoint(loopEntry);
    auto* root = &*loopFn->arg_begin();
    b.CreateBr(loopBody);
    b.SetInsertPoint(loopBody);
    auto* cycle = b.CreatePHI(ptr, 2);
    cycle->addIncoming(root, loopEntry);
    cycle->addIncoming(cycle, loopBody);
    b.CreateCondBr(loopFn->getArg(1), loopBody, loopEnd);
    b.SetInsertPoint(loopEnd);
    auto sink = mod.getOrInsertFunction("sink",
        FunctionType::get(b.getVoidTy(), {ptr}, false));
    b.CreateCall(sink, {cycle});
    b.CreateRetVoid();
    assert(!verifyFunction(*loopFn, &errs()));
    assert(classifyPointerRegion(cycle, root) == PointerRegion::Unknown);
    assert(tracePointerLoadChain(cycle, root).terminal ==
           PointerLoadTerminal::Merge);

    auto* deepFn = Function::Create(
        FunctionType::get(b.getVoidTy(), {ptr}, false),
        GlobalValue::ExternalLinkage, "deep", mod);
    auto* deepEntry = BasicBlock::Create(context, "entry", deepFn);
    b.SetInsertPoint(deepEntry);
    auto* deepRoot = deepFn->getArg(0);
    Value* deep = deepRoot;
    for (unsigned i = 0; i < 80; ++i)
        deep = b.CreateGEP(b.getInt8Ty(), deep, b.getInt32(1));
    b.CreateCall(sink, {deep});
    b.CreateRetVoid();
    assert(!verifyFunction(*deepFn, &errs()));
    assert(classifyPointerRegion(deep, deepRoot) ==
           PointerRegion::Unknown);
    const auto deepChain = tracePointerLoadChain(deep, deepRoot);
    assert(deepChain.terminal == PointerLoadTerminal::DepthLimit);
    assert(deepChain.dereferences == 0);

    std::printf(
        "PASS RT-001 pointer region: stack=%u host=%u static=%u "
        "unknown=%u total=%u; loaded/mixed/invalid/cycle/deep refused\n",
        report.pointerRegions[size_t(PointerRegion::NativeStack)],
        report.pointerRegions[size_t(PointerRegion::HostContext)],
        report.pointerRegions[size_t(PointerRegion::StaticGlobal)],
        report.pointerRegions[size_t(PointerRegion::Unknown)],
        report.unresolvedPointer);
}
