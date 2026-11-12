// RT-001: ensure the yield SSA machine census really represents
// post-O3 live integer words and pointer-typed SSA locations.
// Compiler-only diagnostic; a boxed i64 is NOT an owned GC root.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/rt_yield_ssa_machine_stackmap.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include <cassert>
#include <cstdio>

static void verifyYieldI64Provenance() {
    using namespace llvm;
    LLVMContext ctx;
    Module m("rt001.i64.origin", ctx);
    IRBuilder<> b(ctx);
    auto* signature = FunctionType::get(
        b.getVoidTy(), {b.getPtrTy(), b.getInt64Ty(),
                         b.getInt64Ty()}, false);
    auto* f = Function::Create(
        signature, GlobalValue::ExternalLinkage,
        "xyo_script_provenance_p0", m);
    b.SetInsertPoint(BasicBlock::Create(ctx, "entry", f));
    auto* context = f->getArg(0);
    auto* alias = f->getArg(1);
    auto* other = f->getArg(2);
    const auto classify = [&](const llvm::Value* value,
                              uint64_t slot = 96,
                              uint64_t field = 104) {
        return llvm::StringRef(xyo::rt::classifyYieldI64Origin(
            value, context, slot, field));
    };
    assert(classify(alias) == "candidate_argument_alias");
    Value* binary = b.CreateMul(alias, b.getInt64(9));
    assert(classify(binary) == "unknown_bit_arithmetic");
    // Narrow zero-extension cannot encode NaN-box string tag 0x7FF9.
    // A wider zero-extension must remain unknown.
    auto* predicate = b.CreateICmpULT(alias, other);
    auto* shortNumber = b.CreateTrunc(alias, b.getInt32Ty());
    auto* largeNumber = b.CreateTrunc(alias, IntegerType::get(ctx, 48));
    assert(classify(b.CreateZExt(predicate, b.getInt64Ty())) ==
           "no_tagged_string_zext");
    assert(classify(b.CreateZExt(shortNumber, b.getInt64Ty())) ==
           "no_tagged_string_zext");
    assert(classify(b.CreateZExt(largeNumber, b.getInt64Ty())) ==
           "unknown_other");
    assert(classify(b.CreateSExt(shortNumber, b.getInt64Ty())) ==
           "unknown_other");
    auto* floatBits = b.CreateBitCast(
        b.CreateSIToFP(alias, b.getDoubleTy()), b.getInt64Ty());
    assert(classify(floatBits) == "unknown_float_bits");
    auto* loadPtr = b.CreateAlloca(b.getInt64Ty());
    b.CreateStore(other, loadPtr);
    auto* load = b.CreateLoad(b.getInt64Ty(), loadPtr);
    assert(classify(load) == "unknown_load_stack");
    auto* directField = b.CreateGEP(
        b.getInt8Ty(), context, b.getInt64(160));
    assert(classify(b.CreateLoad(b.getInt64Ty(), directField)) ==
           "unknown_load_context");
    assert(classify(b.CreateLoad(b.getInt64Ty(), context)) ==
           "unknown_load_context");
    auto* external = b.CreateIntToPtr(alias, b.getPtrTy());
    assert(classify(b.CreateLoad(b.getInt64Ty(), external)) ==
           "unknown_load_unresolved");
    assert(classify(b.CreateSelect(b.CreateICmpEQ(alias, other), alias, binary)) ==
           "unknown_merge");
    auto join = m.getOrInsertFunction(
        "xyo_join", FunctionType::get(
            b.getInt64Ty(), {b.getInt64Ty(), b.getInt64Ty()}, false));
    Value* joined = b.CreateCall(join, {alias, other});
    assert(classify(joined) == "candidate_generated_string");
    auto badJoin = m.getOrInsertFunction(
        "fake_join", FunctionType::get(
            b.getInt64Ty(), {b.getInt64Ty(), b.getInt64Ty()}, false));
    assert(classify(b.CreateCall(badJoin, {alias, other})) ==
           "unknown_call");
    auto reporter = m.getOrInsertFunction(
        "xyo_rt_async_reporter", FunctionType::get(
            b.getInt64Ty(), {b.getPtrTy(), b.getPtrTy()}, false));
    Value* retval = b.CreateCall(reporter, {context, context});
    assert(classify(retval) == "candidate_async_reporter");

    // Exact JitCtx::varSlotData -> Variable* -> Variable::value.
    Value* slotField = b.CreateGEP(
        b.getInt8Ty(), context, b.getInt64(96));
    Value* slots = b.CreateLoad(b.getPtrTy(), slotField);
    Value* slotAddr = b.CreateGEP(
        b.getPtrTy(), slots, b.getInt64(3));
    Value* variable = b.CreateLoad(b.getPtrTy(), slotAddr);
    Value* boxedField = b.CreateGEP(
        b.getInt8Ty(), variable, b.getInt64(104));
    Value* value = b.CreateLoad(b.getInt64Ty(), boxedField);
    assert(classify(value) == "candidate_variable_slot");
    assert(classify(value, 97, 104) == "unknown_load_unresolved");
    assert(classify(value, 96, 112) == "unknown_load_unresolved");

    auto resolver = m.getOrInsertFunction(
        "xyo_varslot_checked", FunctionType::get(
            b.getPtrTy(), {b.getPtrTy(), b.getInt64Ty(),
                             b.getPtrTy(), b.getPtrTy(),
                             b.getInt32Ty()}, false));
    auto* resolved = b.CreateCall(
        resolver, {context, b.getInt64(3), context, context, b.getInt32(0)});
    Value* resolvedValue = b.CreateLoad(
        b.getInt64Ty(), b.CreateGEP(
            b.getInt8Ty(), resolved, b.getInt64(104)));
    assert(classify(resolvedValue) == "candidate_checked_variable");
    assert(classify(resolvedValue, 96, 112) == "unknown_load_unresolved");
    assert(xyo::rt::isPotentialBoxedYieldOrigin(classify(value)));
    assert(!xyo::rt::isPotentialBoxedYieldOrigin(classify(binary)));
    b.CreateRetVoid();
    assert(!verifyFunction(*f, &errs()));
}

static void verifyYieldSchedulerTickOrigin() {
    // Independent of retained LLVM value names: recognize the actual
    // post-O3 loop recurrence and the trusted JitCtx ABI field, never
    // a user Scratch variable that happens to be an i64 increment.
    using namespace llvm;
    LLVMContext c;
    Module m("rt001-tick-origin", c);
    IRBuilder<> b(c);
    auto* fn = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false),
        GlobalValue::ExternalLinkage, "xyo_script_tick", m);
    auto* ctx = fn->getArg(0);
    auto* entry = BasicBlock::Create(c, "entry", fn);
    auto* loop = BasicBlock::Create(c, "loop", fn);
    auto* exit = BasicBlock::Create(c, "exit", fn);
    b.SetInsertPoint(entry);
    Value* source = b.CreateGEP(b.getInt8Ty(), ctx, b.getInt64(104));
    Value* initial = b.CreateLoad(b.getInt64Ty(), source);
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    auto* phi = b.CreatePHI(b.getInt64Ty(), 2, "ycnt.test");
    Value* tick = b.CreateAdd(phi, b.getInt64(1), "tick.next");
    Value* other = b.CreateAdd(phi, b.getInt64(2));
    b.CreateCondBr(b.CreateICmpULT(tick, b.getInt64(24000)),
                   loop, exit);
    phi->addIncoming(initial, entry);
    phi->addIncoming(tick, loop);
    b.SetInsertPoint(exit);
    b.CreateRetVoid();
    assert(!verifyModule(m, &errs()));
    assert(xyo::rt::isNativeYieldTickIncrement(tick, ctx, 104));
    assert(!xyo::rt::isNativeYieldTickIncrement(tick, ctx, 96));
    assert(!xyo::rt::isNativeYieldTickIncrement(tick, nullptr, 104));
    assert(!xyo::rt::isNativeYieldTickIncrement(
        tick, ctx, UINT32_MAX));
    assert(!xyo::rt::isNativeYieldTickIncrement(other, ctx, 104));
    assert(!xyo::rt::isNativeYieldTickIncrement(initial, ctx, 104));
    // A foreign input pointer must not be treated as JitCtx.
    auto* foreign = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false),
        GlobalValue::ExternalLinkage, "foreign", m);
    assert(!xyo::rt::isNativeYieldTickIncrement(
        tick, foreign->getArg(0), 104));
}

int main() {
    verifyYieldI64Provenance();
    verifyYieldSchedulerTickOrigin();
    using namespace llvm;
    LLVMContext ctx;
    Module m("rt-yield-ssa-snapshot", ctx);
    IRBuilder<> b(ctx);
    auto* p = b.getPtrTy();
    auto* yield = Function::Create(
        FunctionType::get(b.getVoidTy(), {p}, false),
        GlobalValue::ExternalLinkage, "xyo_yield", m);
    auto* sink = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getInt64Ty(), p}, false),
        GlobalValue::ExternalLinkage, "xyo_test_yield_live_sink", m);
    auto* fn = Function::Create(
        FunctionType::get(b.getVoidTy(), {p, b.getInt64Ty()}, false),
        GlobalValue::ExternalLinkage,
        "xyo_script_live_ssa_p0", m);
    b.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
    // LLVM must retain the nonconstant boxed word across yield
    // because an externally defined helper consumes it afterward.
    Value* live = b.CreateMul(fn->getArg(1), b.getInt64(7));
    Value* addr = b.CreateGEP(b.getInt8Ty(),
                              fn->getArg(0), b.getInt64(24));
    b.CreateCall(yield, {fn->getArg(0)});
    b.CreateCall(sink, {live, addr});
    b.CreateRetVoid();

    assert(!verifyModule(m, &errs()));

    PassBuilder pb;
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cam;
    ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cam, mam);
    pb.buildPerModuleDefaultPipeline(OptimizationLevel::O3).run(m, mam);

    // Calling the snapshot BEFORE the separate direct-edge geometry
    // StackMap must refuse it, not silently claim a native safepoint.
    const auto refused = xyo::rt::emitExperimentalYieldSsaSnapshots(m);
    assert(refused.size() == 1 && !refused[0].emitted);

    const auto edge = xyo::rt::emitExperimentalYieldEdgeStackmaps(m);
    assert(edge.size() == 1 && edge[0].emitted);
    const auto recorded = xyo::rt::emitExperimentalYieldSsaSnapshots(m);
    assert(recorded.size() == 1 && recorded[0].emitted &&
           recorded[0].livenessValid &&
           recorded[0].liveI64 >= 1 &&
           recorded[0].expectedLocations >= 2 &&
           recorded[0].sources.size() ==
               recorded[0].expectedLocations &&
           recorded[0].sources[0].location == 1 &&
           recorded[0].sources[0].family == "helper_arg" &&
           recorded[0].stackmapId != edge[0].id);
    bool foundLiveMul = false;
    for (size_t i = 0; i < recorded[0].sources.size(); ++i) {
        const auto& source = recorded[0].sources[i];
        assert(source.location == i + 1);
        if (source.family == "i64_instruction" &&
            source.opcode == "mul")
            foundLiveMul = true;
    }
    assert(foundLiveMul);
    assert(!verifyModule(m, &errs()));
    unsigned stackmapCalls = 0;
    for (const BasicBlock& bb : *fn) {
        for (const Instruction& inst : bb) {
            const auto* call = dyn_cast<CallInst>(&inst);
            if (!call || !call->getCalledFunction()) continue;
            if (call->getCalledFunction()->getIntrinsicID() ==
                Intrinsic::experimental_stackmap) {
                ++stackmapCalls;
                assert(call->arg_size() == (call->getArgOperand(0) ==
                    ConstantInt::get(b.getInt64Ty(), recorded[0].stackmapId)
                    ? recorded[0].expectedLocations + 2 : 3));
            }
        }
    }
    assert(stackmapCalls == 2);
    // No arbitrary second instrumentation when the first pass has
    // already emitted its map. Such a duplicate would perturb
    // locations and break the machine identity contract.
    auto repeated = xyo::rt::emitExperimentalYieldSsaSnapshots(m);
    assert(repeated.size() == 1 && !repeated[0].emitted);
    std::puts("PASS RT-001 yield-live-ssa post-O3: i64 location emitted, "
              "prerequisite and duplicate fail-closed, verifier PASS; "
              "ownership_proven=0 gc_safe=0");
}
