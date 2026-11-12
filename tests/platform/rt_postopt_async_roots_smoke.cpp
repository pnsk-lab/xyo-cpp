// RT-001: real LLVM O3 root buffer and live SSA coverage, GC still vetoed.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/rt_postopt_ssa_audit.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <cassert>
#include <cstdio>
using namespace llvm;

int main() {
    LLVMContext ctx;
    Module m("rt-postopt-async-root-smoke", ctx);
    m.setDataLayout("e-m:e-p:64:64-i64:64-n8:16:32:64-S128");
    IRBuilder<> b(ctx);
    auto* ptr = b.getPtrTy();
    auto* i64 = b.getInt64Ty();
    auto native = m.getOrInsertFunction("xyo_rt_async_reporter_rooted",
        FunctionType::get(i64, {ptr, ptr, ptr, b.getInt32Ty()}, false));
    auto unrooted = m.getOrInsertFunction("xyo_rt_async_reporter",
        FunctionType::get(i64, {ptr, ptr}, false));
    auto sink = m.getOrInsertFunction("sink_i64",
        FunctionType::get(b.getVoidTy(), {i64}, false));
    auto mutate = m.getOrInsertFunction("mutate_buffer",
        FunctionType::get(b.getVoidTy(), {ptr}, false));
    const auto make = [&](const char* name, unsigned stores,
                          unsigned count, bool contextValid,
                          bool mayClobber = false,
                          bool captureLive = true) {
        auto* fn = Function::Create(
            FunctionType::get(b.getVoidTy(), {ptr, ptr}, false),
            GlobalValue::ExternalLinkage, name, m);
        b.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
        auto* trusted = fn->getArg(0);
        auto* foreign = fn->getArg(1);
        Value* base = b.CreateAlloca(i64, b.getInt32(2));
        auto* prior = b.CreateCall(unrooted, {
            trusted, ConstantPointerNull::get(ptr)});
        if (stores >= 1)
            b.CreateStore(captureLive ? static_cast<Value*>(prior) :
                                      static_cast<Value*>(b.getInt64(1)),
                          base);
        if (stores >= 2)
            b.CreateStore(b.getInt64(42),
                b.CreateGEP(i64, base, b.getInt32(1)));
        if (mayClobber) b.CreateCall(mutate, {base});
        b.CreateCall(native, {
            contextValid ? trusted : foreign,
            ConstantPointerNull::get(ptr), base, b.getInt32(count)});
        b.CreateCall(sink, {prior});
        b.CreateRetVoid();
    };
    make("valid_stores", 2, 2, true);
    make("missing_store", 1, 2, true);
    make("forged_context", 2, 2, false);
    make("invalid_count", 2, 65, true);
    make("intervening_clobber", 2, 2, true, true);
    make("missing_live_i64", 2, 2, true, false, false);
    // The original instruction liveness omits function argument registers.
    // Keep an i64 native parameter live AFTER the call without putting it
    // in the root buffer; optimized audit must refuse this site.
    auto* argFn = Function::Create(
        FunctionType::get(b.getVoidTy(), {ptr, ptr, i64}, false),
        GlobalValue::ExternalLinkage, "missing_live_argument", m);
    b.SetInsertPoint(BasicBlock::Create(ctx, "entry", argFn));
    Value* argBuffer = b.CreateAlloca(i64, b.getInt32(2));
    b.CreateStore(b.getInt64(15), argBuffer);
    b.CreateStore(b.getInt64(24),
        b.CreateGEP(i64, argBuffer, b.getInt32(1)));
    b.CreateCall(native, {argFn->getArg(0),
                          ConstantPointerNull::get(ptr),
                          argBuffer, b.getInt32(2)});
    b.CreateCall(sink, {argFn->getArg(2)});
    b.CreateRetVoid();

    // The shape classifier must recognize only the exact two-load
    // JitCtx::varSlotData[constant] chain. It must NOT promote the
    // returned Variable* to a GC-safe or deoptimizable pointer.
    auto* slotFn = Function::Create(
        FunctionType::get(b.getVoidTy(), {ptr, ptr, i64}, false),
        GlobalValue::ExternalLinkage, "varslot_pointer_shape", m);
    b.SetInsertPoint(BasicBlock::Create(ctx, "entry", slotFn));
    auto* ctxPtr = slotFn->getArg(0);
    Value* arrayField = b.CreateConstGEP1_32(
        b.getInt8Ty(), ctxPtr, 96);
    Value* slotArray = b.CreateLoad(ptr, arrayField);
    Value* slotAddress = b.CreateConstGEP1_32(
        b.getInt8Ty(), slotArray, 8);
    Value* slotValue = b.CreateLoad(ptr, slotAddress);
    const auto exact = xyo::rt::asyncBorrowedVarSlotIndex(
        slotValue, ctxPtr, m.getDataLayout(), 96);
    assert(exact && *exact == 1);
    assert(xyo::rt::classifyPointerRegion(slotValue, ctxPtr) ==
           xyo::rt::PointerRegion::Unknown);
    assert(!xyo::rt::asyncBorrowedVarSlotIndex(
        slotValue, ctxPtr, m.getDataLayout(), 104));
    assert(!xyo::rt::asyncBorrowedVarSlotIndex(
        slotValue, slotFn->getArg(1), m.getDataLayout(), 96));
    Value* otherField = b.CreateConstGEP1_32(
        b.getInt8Ty(), ctxPtr, 104);
    Value* otherArray = b.CreateLoad(ptr, otherField);
    Value* wrongField = b.CreateLoad(ptr, otherArray);
    assert(!xyo::rt::asyncBorrowedVarSlotIndex(
        wrongField, ctxPtr, m.getDataLayout(), 96));
    Value* dynamicAddress = b.CreateGEP(
        b.getInt8Ty(), slotArray, slotFn->getArg(2));
    Value* dynamicSlot = b.CreateLoad(ptr, dynamicAddress);
    assert(!xyo::rt::asyncBorrowedVarSlotIndex(
        dynamicSlot, ctxPtr, m.getDataLayout(), 96));
    auto* volatileSlot = b.CreateLoad(ptr, slotAddress);
    volatileSlot->setVolatile(true);
    assert(!xyo::rt::asyncBorrowedVarSlotIndex(
        volatileSlot, ctxPtr, m.getDataLayout(), 96));
    b.CreateRetVoid();

    assert(!verifyModule(m, &errs()));
    PassBuilder pb;
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    pb.buildPerModuleDefaultPipeline(OptimizationLevel::O3).run(m, mam);
    assert(!verifyModule(m, &errs()));
    const auto sites = xyo::rt::auditPostoptAsyncRoots(m);
    assert(sites.size() == 7);
    unsigned good = 0, rejected = 0;
    for (const auto& site : sites) {
        if (site.functionName == "valid_stores") {
            assert(site.signatureValid && site.bufferValid &&
                   site.completeStores && site.declaredWords == 2 &&
                   site.initializedWords == 2 && site.liveI64 >= 1 &&
                   site.coveredLiveI64 >= 1 && site.coveredAllLiveI64);
            ++good;
        } else if (site.functionName == "missing_store") {
            assert(site.signatureValid && site.bufferValid &&
                   !site.completeStores && site.initializedWords == 1);
            ++rejected;
        } else if (site.functionName == "forged_context") {
            assert(!site.signatureValid && !site.completeStores);
            ++rejected;
        } else if (site.functionName == "invalid_count") {
            assert(!site.signatureValid && !site.completeStores);
            ++rejected;
        } else if (site.functionName == "intervening_clobber") {
            assert(site.signatureValid && site.bufferValid &&
                   !site.completeStores);
            ++rejected;
        } else if (site.functionName == "missing_live_i64") {
            assert(site.signatureValid && site.bufferValid &&
                   site.completeStores && site.liveI64 >= 1 &&
                   site.coveredLiveI64 == 0 &&
                   !site.coveredAllLiveI64);
            ++rejected;
        } else if (site.functionName == "missing_live_argument") {
            assert(site.signatureValid && site.bufferValid &&
                   site.completeStores && site.liveArgumentI64 >= 1 &&
                   site.coveredLiveI64 == 0 &&
                   !site.coveredAllLiveI64);
            ++rejected;
        } else assert(false);
    }
    assert(good == 1 && rejected == 6);
    // The actual StackMap builder uses the SAME fail-closed post-O3
    // contract. A malformed helper, missing live SSA value or clobbered
    // buffer must NOT be allowed to mint machine location records.
    const auto maps = xyo::rt::emitExperimentalAsyncStackmaps(m);
    assert(maps.size() == 7);
    unsigned emitted = 0, denied = 0;
    for (const auto& site : maps) {
        uint32_t regions = 0;
        for (auto count : site.pointerRegions) regions += count;
        assert(regions == site.livePointers);
        assert(site.stackmapId != 0);
        if (site.functionName == "valid_stores") {
            assert(site.emitted && site.liveI64 >= 1);
            ++emitted;
        } else {
            assert(!site.emitted);
            ++denied;
        }
    }
    assert(emitted == 1 && denied == 6);
    assert(!verifyModule(m, &errs()));
    std::puts("PASS RT-001 O3 async roots: SSA covered, invalid ABI/"
              "missing stores rejected; machine_roots_proven=0 gc_safe=0");
}
