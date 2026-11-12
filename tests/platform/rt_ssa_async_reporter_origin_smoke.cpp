// RT-001: reporter-return SSA values may contain boxed Scratch strings.
// ABI provenance is NOT GC membership, root ownership, or GC permission.
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
    Module module("native.reporter.origin", context);
    IRBuilder<> b(context);
    auto* fn = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getPtrTy(), b.getPtrTy()}, false),
        GlobalValue::ExternalLinkage, "probe", module);
    b.SetInsertPoint(BasicBlock::Create(context, "entry", fn));
    auto* trustedContext = fn->getArg(0);
    auto* foreignContext = fn->getArg(1);
    auto* string = ConstantPointerNull::get(b.getPtrTy());
    auto recognized = module.getOrInsertFunction(
        "xyo_rt_async_reporter",
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy(), b.getPtrTy()}, false));
    auto fake = module.getOrInsertFunction(
        "xyo_fake_reporter",
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy(), b.getPtrTy()}, false));
    auto* good = b.CreateCall(recognized, {trustedContext, string});
    auto* wrongCtx = b.CreateCall(recognized, {foreignContext, string});
    auto* wrongName = b.CreateCall(fake, {trustedContext, string});
    auto* wrongType = b.CreateAdd(good, b.getInt64(1));
    using xyo::rt::trustedAsyncReporterValueSource;
    assert(trustedAsyncReporterValueSource(good, trustedContext));
    assert(!trustedAsyncReporterValueSource(good, foreignContext));
    assert(!trustedAsyncReporterValueSource(wrongCtx, trustedContext));
    assert(!trustedAsyncReporterValueSource(wrongName, trustedContext));
    assert(!trustedAsyncReporterValueSource(wrongType, trustedContext));
    assert(!trustedAsyncReporterValueSource(nullptr, trustedContext));
    xyo::rt::LiveI64ProvenanceCensus census;
    census.observe(good, trustedContext);
    census.observe(wrongCtx, trustedContext);
    census.observe(wrongName, trustedContext);
    census.observe(wrongType, trustedContext);
    assert(census.total == 4 && census.asyncReporterValues == 1 &&
           census.unclassified == 3 && census.complete());
    b.CreateRetVoid();
    assert(!verifyFunction(*fn, &errs()));

    // Same symbol with forged ABI must reject, even with ptr context.
    Module broken("forged.reporter.signature", context);
    auto* brokenFn = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false),
        GlobalValue::ExternalLinkage, "broken", broken);
    b.SetInsertPoint(BasicBlock::Create(context, "entry", brokenFn));
    auto bad = broken.getOrInsertFunction(
        "xyo_rt_async_reporter",
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy()}, false));
    auto* malformed = b.CreateCall(bad, {brokenFn->getArg(0)});
    assert(!trustedAsyncReporterValueSource(malformed, brokenFn->getArg(0)));
    b.CreateRetVoid();
    assert(!verifyFunction(*brokenFn, &errs()));
    std::puts("PASS RT-001 async reporter SSA return: exact ABI recognized, "
              "forged name/signature/context/arithmetic rejected; gc_safe=0");
}
