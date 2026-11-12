// RT-001: direct LLVM23 O3 audit of live pointer SSA at capture helpers.
// A valid manifest + resolved pre-IR pointer count must NOT be mistaken
// for stack-map-backed GC roots. Malformed capture ABIs fail closed.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/rt_postopt_ssa_audit.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include <cassert>
#include <cstdio>

int main() {
    using namespace llvm;
    LLVMContext context;
    Module module("rt-postopt-ssa-audit-smoke", context);
    IRBuilder<> b(context);
    Type* ptr = b.getPtrTy();
    Type* i32 = b.getInt32Ty();
    Type* i64 = b.getInt64Ty();
    auto* helperType = FunctionType::get(
        b.getVoidTy(),
        {ptr, ptr, ptr, i32, i32, i32, i64, i64, i64, i64,
         i32, i32, ptr, i32, i32, ptr, ptr},
        false);
    auto* helper = Function::Create(helperType,
        GlobalValue::ExternalLinkage, "xyo_rt_capture_llvm_ssa", module);
    auto* sink = Function::Create(
        FunctionType::get(b.getVoidTy(), {ptr}, false),
        GlobalValue::ExternalLinkage, "escape_ptr", module);

    auto create = [&](const char* name, bool good) {
        auto* fn = Function::Create(
            FunctionType::get(b.getVoidTy(), {ptr}, false),
            GlobalValue::ExternalLinkage, name, module);
        BasicBlock* bb = BasicBlock::Create(context, "entry", fn);
        b.SetInsertPoint(bb);
        auto* ctx = fn->getArg(0);
        Value* loaded = b.CreateLoad(ptr, ctx, "unrooted.pointer");
        auto* captured = b.CreateAlloca(i64);
        auto* descriptors = b.CreateAlloca(i64);
        auto* origins = b.CreateAlloca(i32);
        auto* aliases = b.CreateAlloca(i32);
        auto* generated = b.CreateAlloca(i32);
        b.CreateCall(helper, {
            good ? static_cast<Value*>(ctx)
                 : static_cast<Value*>(ConstantPointerNull::get(b.getPtrTy())),
            captured, descriptors, b.getInt32(1), b.getInt32(1),
            b.getInt32(0), b.getInt64(6), b.getInt64(7),
            b.getInt64(8), b.getInt64(good ? 51 : 52),
            b.getInt32(1), b.getInt32(0), origins,
            b.getInt32(0), b.getInt32(0), aliases, generated});
        // This use is AFTER the runtime capture. A pointer can remain
        // live even though it was not included in the detached i64
        // descriptor stream. O3 must not erase the sink side effect.
        b.CreateCall(sink, {loaded});
        b.CreateRetVoid();
    };
    create("good_source", true);
    create("forged_source", false);
    assert(!verifyModule(module, &errs()));

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
    pb.buildPerModuleDefaultPipeline(OptimizationLevel::O3)
        .run(module, mam);
    assert(!verifyModule(module, &errs()));
    const auto sites = xyo::rt::auditPostoptSsaCaptures(module);
    assert(sites.size() == 2);
    bool goodFound = false, badRefused = false;
    for (const auto& site : sites) {
        if (site.siteId == 51) {
            goodFound = true;
            assert(site.reportValid && site.signatureValid);
            assert(site.capturedWords == 1 &&
                   site.declaredPointers == 1);
            assert(site.postoptLivePointers >= 1 &&
                   site.postoptUnknownPointers >= 1);
        } else if (site.siteId == 52) {
            badRefused = true;
            assert(!site.signatureValid && !site.reportValid);
        } else {
            assert(false && "foreign/omitted LLVM marker");
        }
    }
    assert(goodFound && badRefused);
    // The experimental StackMap may only instrument a VALID capture.
    // It includes the trusted JitCtx address and every unproven live
    // pointer, but is never an ownership proof for Scratch Value GC.
    const auto stackmaps =
        xyo::rt::emitExperimentalPostoptStackmaps(module);
    assert(stackmaps.size() == 2);
    unsigned emitted = 0;
    for (const auto& record : stackmaps) {
        if (record.sourceSite == 51) {
            assert(record.emitted);
            assert(record.unknownPointerLocations >= 1);
            ++emitted;
        } else {
            assert(record.sourceSite == 52 && !record.emitted);
        }
    }
    assert(emitted == 1);
    assert(!verifyModule(module, &errs()));
    std::puts("PASS RT-001 O3 postopt pointer: Unknown stackmap "
              "generated, forged capture refused; GC still vetoed");
}
