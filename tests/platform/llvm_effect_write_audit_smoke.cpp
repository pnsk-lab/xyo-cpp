#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/llvm_effect_write_audit.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <cassert>
#include <iostream>

using namespace llvm;
using xyo::ir::auditLlvmEffectWrites;

int main() {
    LLVMContext c;
    Module m("ir001-postopt-effects", c);
    IRBuilder<> b(c);
    const auto voidFn = FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false);
    const auto source = m.getOrInsertFunction("xyo_ir001_broadcast_v_source", voidFn);
    const auto fence = m.getOrInsertFunction("xyo_ir001_vm_write", voidFn);
    const auto checkpoint = m.getOrInsertFunction("xyo_ir001_effect_checkpoint", voidFn);
    const auto opaque = m.getOrInsertFunction("unknown_runtime_write", voidFn);
    const auto registerTest = [&](const char* name) {
        auto* fn = Function::Create(
            FunctionType::get(b.getVoidTy(), {b.getPtrTy(), b.getInt1Ty()}, false),
            GlobalValue::ExternalLinkage, name, m);
        return fn;
    };
    const auto addStore = [&] (Value* dst, int64_t val) {
        return b.CreateStore(b.getInt64(val), dst);
    };
    const auto call = [&](FunctionCallee callee, Value* arg) {
        return b.CreateCall(callee, {arg});
    };

    // The store cannot execute before the external fence. Pre/post-O2 safe.
    auto* guarded = registerTest("guarded");
    auto* dst = &*guarded->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", guarded));
    call(source, dst);
    call(fence, dst);
    addStore(dst, 1);
    call(checkpoint, dst);
    b.CreateRetVoid();
    auto r = auditLlvmEffectWrites(*guarded);
    assert(r.tracedEffects == 1 && r.writeFences == 1);
    assert(r.memoryWrites == 1 && r.unguardedWrites == 0);
    assert(!r.executable());

    auto* missing = registerTest("missing_fence");
    dst = &*missing->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", missing));
    call(source, dst);
    addStore(dst, 2);
    call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*missing);
    assert(r.unguardedWrites == 1);

    // A fence in just one branch is insufficient at the join.
    auto* diamond = registerTest("branch_one_sided");
    auto iter = diamond->arg_begin();
    dst = &*iter++;
    auto* cond = &*iter;
    auto* entry = BasicBlock::Create(c, "entry", diamond);
    auto* thenBB = BasicBlock::Create(c, "fenced", diamond);
    auto* elseBB = BasicBlock::Create(c, "unfenced", diamond);
    auto* merge = BasicBlock::Create(c, "join", diamond);
    b.SetInsertPoint(entry);
    call(source, dst);
    b.CreateCondBr(cond, thenBB, elseBB);
    b.SetInsertPoint(thenBB);
    call(fence, dst);
    b.CreateBr(merge);
    b.SetInsertPoint(elseBB);
    b.CreateBr(merge);
    b.SetInsertPoint(merge);
    addStore(dst, 3);
    call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*diamond);
    assert(r.unguardedWrites == 1);

    // The backedge can re-enter a loop header carrying pending evidence.
    auto* loop = registerTest("loop_backedge");
    iter = loop->arg_begin();
    dst = &*iter++;
    cond = &*iter;
    entry = BasicBlock::Create(c, "entry", loop);
    auto* header = BasicBlock::Create(c, "loop.head", loop);
    auto* body = BasicBlock::Create(c, "loop.body", loop);
    auto* exitBB = BasicBlock::Create(c, "exit", loop);
    b.SetInsertPoint(entry);
    b.CreateBr(header);
    b.SetInsertPoint(header);
    addStore(dst, 4); // first iteration before source; later after source
    b.CreateCondBr(cond, body, exitBB);
    b.SetInsertPoint(body);
    call(source, dst);
    b.CreateBr(header);
    b.SetInsertPoint(exitBB);
    call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*loop);
    assert(r.unguardedWrites == 1);

    // An opaque helper can mutate state with no visible LLVM store.
    auto* unknown = registerTest("opaque_runtime_call");
    dst = &*unknown->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", unknown));
    call(source, dst);
    call(opaque, dst);
    call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*unknown);
    assert(r.unguardedOpaqueCalls == 1);

    // A foreign "readonly" call may still throw or perform other
    // observable actions. Do not confuse memory-readonly with pure.
    const auto readonlyExternal = m.getOrInsertFunction(
        "readonly_may_throw", voidFn);
    auto* readonlyCaller = registerTest("readonly_may_throw_caller");
    dst = &*readonlyCaller->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", readonlyCaller));
    call(source, dst);
    auto* readonlyInvocation = call(readonlyExternal, dst);
    readonlyInvocation->setOnlyReadsMemory();
    assert(!readonlyInvocation->mayWriteToMemory());
    assert(readonlyInvocation->mayHaveSideEffects());
    auto* readonlyCheckpoint = call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*readonlyCaller);
    assert(r.unguardedOpaqueCalls == 1);
    xyo::ir::LlvmLocallyPureCallees foreignCallees;
    const auto unsafeReadonly =
        xyo::ir::findUnprovenLlvmEffectCheckpoints(
            *readonlyCaller, &foreignCallees);
    assert(unsafeReadonly.size() == 1 &&
           unsafeReadonly[0] == readonlyCheckpoint);

    // Untrusted external declarations are not a source-level effect
    // proof, even if BOTH callsite and declaration claim readnone and
    // nounwind. Keep the returned value live across LLVM O2.
    auto readnoneExternal = m.getOrInsertFunction(
        "readnone_nounwind_external",
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy()}, false));
    auto* pureAttrs = cast<Function>(readnoneExternal.getCallee());
    pureAttrs->setDoesNotAccessMemory();
    pureAttrs->setDoesNotThrow();
    auto* attrsCaller = Function::Create(
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy()}, false),
        GlobalValue::ExternalLinkage,
        "unverified_readnone_nounwind_caller", m);
    dst = &*attrsCaller->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", attrsCaller));
    call(source, dst);
    auto* attrCall = b.CreateCall(readnoneExternal, {dst});
    attrCall->setDoesNotAccessMemory();
    attrCall->setDoesNotThrow();
    assert(!attrCall->mayWriteToMemory() && !attrCall->mayThrow());
    call(checkpoint, dst);
    b.CreateRet(attrCall);
    r = auditLlvmEffectWrites(*attrsCaller);
    assert(r.unguardedOpaqueCalls == 1);

    // A volatile or atomic LOAD and a standalone memory fence can have
    // externally observable synchronization / memory-mapped effects.
    // None is a StoreInst, but all must force diagnostic rejection.
    auto* volatileCaller = registerTest("volatile_load_side_effect");
    dst = &*volatileCaller->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", volatileCaller));
    call(source, dst);
    b.CreateLoad(b.getInt64Ty(), dst, true);
    call(checkpoint, dst);
    b.CreateRetVoid();
    assert(auditLlvmEffectWrites(*volatileCaller).unguardedWrites == 1);

    auto* atomicCaller = registerTest("atomic_load_side_effect");
    dst = &*atomicCaller->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", atomicCaller));
    call(source, dst);
    auto* atomicLoad = b.CreateLoad(b.getInt64Ty(), dst);
    atomicLoad->setAtomic(AtomicOrdering::Acquire);
    atomicLoad->setAlignment(Align(8));
    call(checkpoint, dst);
    b.CreateRetVoid();
    assert(auditLlvmEffectWrites(*atomicCaller).unguardedWrites == 1);

    auto* fenceCaller = registerTest("memory_fence_side_effect");
    dst = &*fenceCaller->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", fenceCaller));
    call(source, dst);
    b.CreateFence(AtomicOrdering::SequentiallyConsistent);
    call(checkpoint, dst);
    b.CreateRetVoid();
    assert(auditLlvmEffectWrites(*fenceCaller).unguardedWrites == 1);

    // A side-effect-free LLVM math intrinsic is compiler-controlled,
    // unlike an unverified foreign symbol with equivalent IR attrs.
    auto* mathCaller = Function::Create(
        FunctionType::get(b.getDoubleTy(), {b.getPtrTy(),
                                             b.getDoubleTy()}, false),
        GlobalValue::ExternalLinkage, "intrinsic_math_only", m);
    auto mathArgs = mathCaller->arg_begin();
    auto* mathContext = &*mathArgs++;
    auto* mathInput = &*mathArgs;
    b.SetInsertPoint(BasicBlock::Create(c, "entry", mathCaller));
    call(source, mathContext);
    auto* sqrtIntrinsic = Intrinsic::getOrInsertDeclaration(
        &m, Intrinsic::sqrt, {b.getDoubleTy()});
    auto* mathOutput = b.CreateCall(sqrtIntrinsic, {mathInput});
    auto* mathCheckpoint = call(checkpoint, mathContext);
    b.CreateRet(mathOutput);
    assert(auditLlvmEffectWrites(*mathCaller).unguardedOpaqueCalls == 0);
    assert(xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *mathCaller).empty());
    assert(mathCheckpoint->getCalledFunction());

    // Even a locally defined wrapper is unproven if its transitive
    // leaf is an external readnone/nounwind declaration.
    auto* attributedInternal = Function::Create(
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy()}, false),
        GlobalValue::InternalLinkage,
        "untrusted_external_wrapper", m);
    attributedInternal->addFnAttr(Attribute::NoInline);
    attributedInternal->setDoesNotThrow();
    auto* wrapperArg = &*attributedInternal->arg_begin();
    b.SetInsertPoint(
        BasicBlock::Create(c, "entry", attributedInternal));
    auto* foreignResult = b.CreateCall(
        readnoneExternal, {wrapperArg});
    foreignResult->setDoesNotAccessMemory();
    foreignResult->setDoesNotThrow();
    b.CreateRet(foreignResult);
    auto* wrappedCaller = Function::Create(
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy()}, false),
        GlobalValue::ExternalLinkage,
        "untrusted_external_wrapped_caller", m);
    dst = &*wrappedCaller->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", wrappedCaller));
    call(source, dst);
    auto* wrapperResult = b.CreateCall(attributedInternal, {dst});
    auto* wrappedCheckpoint = call(checkpoint, dst);
    b.CreateRet(wrapperResult);
    xyo::ir::LlvmLocallyPureCallees wrapperProof;
    assert(!wrapperProof.proven(attributedInternal));
    assert(xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *wrappedCaller, &wrapperProof).size() == 1);
    assert(wrappedCheckpoint->getCalledFunction());

    // A function-local store is counted conservatively; it can escape.
    auto* local = registerTest("local_store");
    dst = &*local->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", local));
    auto* slot = b.CreateAlloca(b.getInt64Ty());
    call(source, dst);
    b.CreateStore(b.getInt64(7), slot, true);
    call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*local);
    assert(r.unguardedWrites == 1 && r.localStackCandidates == 1);

    // A nonvolatile stack store into a provably non-escaping alloca
    // is not directly host-observable. Return the loaded scalar;
    // LLVM O2 is free to promote/eliminate the physical stack write.
    auto* isolated = Function::Create(
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy()}, false),
        GlobalValue::ExternalLinkage, "isolated_stack_store", m);
    dst = &*isolated->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", isolated));
    auto* owned = b.CreateAlloca(b.getInt64Ty());
    call(source, dst);
    b.CreateStore(b.getInt64(41), owned);
    auto* localValue = b.CreateLoad(b.getInt64Ty(), owned);
    call(checkpoint, dst);
    b.CreateRet(localValue);
    r = auditLlvmEffectWrites(*isolated);
    assert(r.isolatedStackWrites == 1);
    assert(r.unguardedIsolatedStackWrites == 1);
    assert(r.unguardedWrites == 0);

    // Passing that exact stack address to an opaque helper makes its
    // contents potentially visible. No longer classify as isolated.
    auto* escaped = registerTest("escaped_stack_store");
    dst = &*escaped->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", escaped));
    auto* leaked = b.CreateAlloca(b.getInt64Ty());
    call(source, dst);
    b.CreateStore(b.getInt64(42), leaked);
    call(opaque, leaked);
    call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*escaped);
    assert(r.isolatedStackWrites == 0 && r.unguardedWrites == 1);
    assert(r.unguardedOpaqueCalls == 1);

    // Derived GEP pointers cannot hide an escape. Check the *base*
    // allocation's complete capture graph, not just the store operand.
    auto* gepSafe = registerTest("local_gep_store");
    dst = &*gepSafe->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", gepSafe));
    auto* arrayTy = ArrayType::get(b.getInt64Ty(), 2);
    auto* safeArray = b.CreateAlloca(arrayTy);
    auto* safeGep = b.CreateInBoundsGEP(
        arrayTy, safeArray, {b.getInt32(0), b.getInt32(1)});
    call(source, dst);
    b.CreateStore(b.getInt64(43), safeGep);
    b.CreateLoad(b.getInt64Ty(), safeGep);
    call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*gepSafe);
    assert(r.isolatedStackWrites == 1 && r.unguardedWrites == 0);

    auto* gepEscaped = registerTest("escaped_gep_store");
    dst = &*gepEscaped->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", gepEscaped));
    auto* escapedArray = b.CreateAlloca(arrayTy);
    auto* escapedGep = b.CreateInBoundsGEP(
        arrayTy, escapedArray, {b.getInt32(0), b.getInt32(1)});
    call(source, dst);
    b.CreateStore(b.getInt64(44), escapedGep);
    call(opaque, escapedGep);
    call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*gepEscaped);
    assert(r.isolatedStackWrites == 0 && r.unguardedWrites == 1);

    // One function, TWO independent source intervals: only the FIRST
    // checkpoint lacks a fence. An unrelated uncovered store elsewhere
    // must not poison the SECOND, correctly guarded checkpoint.
    auto* mixed = registerTest("two_checkpoint_intervals");
    dst = &*mixed->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", mixed));
    call(source, dst);
    addStore(dst, 21);
    auto* firstMixedCheckpoint = call(checkpoint, dst);
    call(source, dst);
    call(fence, dst);
    addStore(dst, 22);
    auto* secondMixedCheckpoint = call(checkpoint, dst);
    b.CreateRetVoid();
    const auto mixedReport = auditLlvmEffectWrites(*mixed);
    assert(mixedReport.tracedEffects == 2 &&
           mixedReport.checkpoints == 2 &&
           mixedReport.unguardedWrites == 1);

    // Interprocedural analysis: an INTERNAL, nonescaping local-write
    // helper can be proven harmless, even if its callsite conservatively
    // advertises a memory write before LLVM's attribute inference.
    auto* pureLeaf = Function::Create(
        FunctionType::get(b.getInt64Ty(), false),
        GlobalValue::InternalLinkage, "pure_internal_leaf", m);
    pureLeaf->addFnAttr(Attribute::NoInline);
    pureLeaf->setDoesNotThrow();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", pureLeaf));
    auto* leafLocal = b.CreateAlloca(b.getInt64Ty());
    b.CreateStore(b.getInt64(123), leafLocal);
    auto* leafValue = b.CreateLoad(b.getInt64Ty(), leafLocal);
    b.CreateRet(leafValue);

    auto* pureIntermediate = Function::Create(
        FunctionType::get(b.getInt64Ty(), false),
        GlobalValue::InternalLinkage, "pure_internal_intermediate", m);
    pureIntermediate->addFnAttr(Attribute::NoInline);
    pureIntermediate->setDoesNotThrow();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", pureIntermediate));
    auto* childValue = b.CreateCall(pureLeaf);
    b.CreateRet(childValue);

    auto* safeCaller = registerTest("interprocedural_local_only");
    dst = &*safeCaller->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", safeCaller));
    call(source, dst);
    b.CreateCall(pureIntermediate);
    auto* safeCheckpoint = call(checkpoint, dst);
    b.CreateRetVoid();

    auto* unsafeLeaf = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false),
        GlobalValue::InternalLinkage, "impure_internal_leaf", m);
    unsafeLeaf->addFnAttr(Attribute::NoInline);
    b.SetInsertPoint(BasicBlock::Create(c, "entry", unsafeLeaf));
    auto* unsafeArgument = &*unsafeLeaf->arg_begin();
    addStore(unsafeArgument, 99);
    b.CreateRetVoid();

    auto* unsafeCaller = registerTest("interprocedural_external_write");
    dst = &*unsafeCaller->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", unsafeCaller));
    call(source, dst);
    b.CreateCall(unsafeLeaf, {dst});
    auto* unsafeCheckpoint = call(checkpoint, dst);
    b.CreateRetVoid();

    // A nested source effect is NEVER a pure callee. An in-module
    // effect wrapper hidden behind a direct internal call is also
    // rejected, even if a caller's memory attributes were misleading.
    auto* effectLeaf = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false),
        GlobalValue::InternalLinkage, "nested_source_effect", m);
    effectLeaf->addFnAttr(Attribute::NoInline);
    b.SetInsertPoint(BasicBlock::Create(c, "entry", effectLeaf));
    auto* effectArg = &*effectLeaf->arg_begin();
    call(source, effectArg);
    b.CreateRetVoid();
    auto* nestedCaller = registerTest("interprocedural_nested_effect");
    dst = &*nestedCaller->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", nestedCaller));
    call(source, dst);
    b.CreateCall(effectLeaf, {dst});
    auto* nestedCheckpoint = call(checkpoint, dst);
    b.CreateRetVoid();

    // A volatile READ inside an otherwise local-only helper is NOT
    // a pure function. The caller must remain fail-closed even though
    // the helper itself has no explicit store or host callback.
    auto* volatileLeaf = Function::Create(
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy()}, false),
        GlobalValue::InternalLinkage, "volatile_internal_leaf", m);
    volatileLeaf->addFnAttr(Attribute::NoInline);
    b.SetInsertPoint(BasicBlock::Create(c, "entry", volatileLeaf));
    auto* volatileArg = &*volatileLeaf->arg_begin();
    auto* volatileValue = b.CreateLoad(b.getInt64Ty(), volatileArg, true);
    b.CreateRet(volatileValue);
    auto* volatileInternalCaller = Function::Create(
        FunctionType::get(b.getInt64Ty(), {b.getPtrTy()}, false),
        GlobalValue::ExternalLinkage,
        "volatile_internal_caller", m);
    dst = &*volatileInternalCaller->arg_begin();
    b.SetInsertPoint(
        BasicBlock::Create(c, "entry", volatileInternalCaller));
    call(source, dst);
    auto* volatileReturn = b.CreateCall(volatileLeaf, {dst});
    auto* volatileInternalCheckpoint = call(checkpoint, dst);
    b.CreateRet(volatileReturn);

    // Recursion must not be accepted through a cyclic "pure" proof.
    auto* recursive = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getInt1Ty()}, false),
        GlobalValue::InternalLinkage, "recursive_internal", m);
    recursive->addFnAttr(Attribute::NoInline);
    auto* recursiveEntry = BasicBlock::Create(c, "entry", recursive);
    auto* recursiveCall = BasicBlock::Create(c, "recursive", recursive);
    auto* recursiveExit = BasicBlock::Create(c, "exit", recursive);
    b.SetInsertPoint(recursiveEntry);
    b.CreateCondBr(&*recursive->arg_begin(), recursiveCall, recursiveExit);
    b.SetInsertPoint(recursiveCall);
    b.CreateCall(recursive, {b.getFalse()});
    b.CreateBr(recursiveExit);
    b.SetInsertPoint(recursiveExit);
    b.CreateRetVoid();

    xyo::ir::LlvmLocallyPureCallees pureCallees;
    assert(pureCallees.proven(pureLeaf));
    assert(pureCallees.proven(pureIntermediate));
    assert(!pureCallees.proven(unsafeLeaf));
    assert(!pureCallees.proven(effectLeaf));
    assert(!pureCallees.proven(volatileLeaf));
    assert(!pureCallees.proven(recursive));

    // An indirect function pointer must not be resolved from a
    // seemingly harmless signature or a local callee's attributes.
    auto* indirectCaller = Function::Create(
        FunctionType::get(b.getVoidTy(),
                          {b.getPtrTy(), b.getPtrTy()}, false),
        GlobalValue::ExternalLinkage, "indirect_function_call", m);
    auto indirectArgs = indirectCaller->arg_begin();
    auto* indirectContext = &*indirectArgs++;
    auto* functionPointer = &*indirectArgs;
    b.SetInsertPoint(BasicBlock::Create(c, "entry", indirectCaller));
    call(source, indirectContext);
    b.CreateCall(voidFn, functionPointer, {indirectContext});
    auto* indirectCheckpoint = call(checkpoint, indirectContext);
    b.CreateRetVoid();

    auto* recursiveCaller = registerTest("recursive_internal_call");
    auto recArgs = recursiveCaller->arg_begin();
    auto* recursiveContext = &*recArgs++;
    auto* recursiveCondition = &*recArgs;
    b.SetInsertPoint(BasicBlock::Create(c, "entry", recursiveCaller));
    call(source, recursiveContext);
    b.CreateCall(recursive, {recursiveCondition});
    auto* recursiveCheckpoint = call(checkpoint, recursiveContext);
    b.CreateRetVoid();

    const auto indirectScan = xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *indirectCaller, &pureCallees);
    const auto recursiveScan = xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *recursiveCaller, &pureCallees);
    assert(indirectScan.size() == 1 &&
           indirectScan[0] == indirectCheckpoint);
    assert(recursiveScan.size() == 1 &&
           recursiveScan[0] == recursiveCheckpoint);

    const auto safeScan = xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *safeCaller, &pureCallees);
    const auto unsafeScan = xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *unsafeCaller, &pureCallees);
    const auto effectScan = xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *nestedCaller, &pureCallees);
    assert(safeScan.empty());
    assert(unsafeScan.size() == 1 && unsafeScan[0] == unsafeCheckpoint);
    assert(effectScan.size() == 1 && effectScan[0] == nestedCheckpoint);
    const auto volatileInternalScan =
        xyo::ir::findUnprovenLlvmEffectCheckpoints(
            *volatileInternalCaller, &pureCallees);
    assert(volatileInternalScan.size() == 1 &&
           volatileInternalScan[0] == volatileInternalCheckpoint);
    assert(auditLlvmEffectWrites(m).locallyProvenCalls >= 2);

    // If no source event is observed locally, we cannot infer anything
    // about a caller's active source frontier.
    auto* callee = registerTest("opaque_callee_checkpoint");
    dst = &*callee->arg_begin();
    b.SetInsertPoint(BasicBlock::Create(c, "entry", callee));
    addStore(dst, 5);
    call(checkpoint, dst);
    b.CreateRetVoid();
    r = auditLlvmEffectWrites(*callee);
    assert(r.functionsWithCheckpointsButNoEffects == 1);
    assert(r.unguardedWrites == 0);

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
    pb.buildPerModuleDefaultPipeline(OptimizationLevel::O2).run(m, mam);
    assert(!verifyModule(m, &errs()));
    // Verify the same sound conservative classification AFTER real O2,
    // including branch and loop predecessor propagation.
    assert(auditLlvmEffectWrites(*guarded).unguardedWrites == 0);
    assert(auditLlvmEffectWrites(*missing).unguardedWrites >= 1);
    assert(auditLlvmEffectWrites(*diamond).unguardedWrites >= 1);
    assert(auditLlvmEffectWrites(*loop).unguardedWrites >= 1);
    assert(auditLlvmEffectWrites(*unknown).unguardedOpaqueCalls >= 1);
    assert(auditLlvmEffectWrites(*readonlyCaller).unguardedOpaqueCalls >= 1);
    assert(auditLlvmEffectWrites(*attrsCaller).unguardedOpaqueCalls == 1);
    assert(auditLlvmEffectWrites(*mathCaller).unguardedOpaqueCalls == 0);
    assert(auditLlvmEffectWrites(*wrappedCaller).unguardedOpaqueCalls >= 1);
    assert(auditLlvmEffectWrites(*volatileCaller).unguardedWrites == 1);
    assert(auditLlvmEffectWrites(*atomicCaller).unguardedWrites == 1);
    assert(auditLlvmEffectWrites(*fenceCaller).unguardedWrites == 1);
    assert(auditLlvmEffectWrites(*local).unguardedWrites >= 1);
    assert(auditLlvmEffectWrites(*isolated).unguardedWrites == 0);
    // The stack write can disappear after mem2reg; do not require a
    // fixed count after optimization, only no host-visible write.
    assert(auditLlvmEffectWrites(*escaped).unguardedWrites >= 1);
    assert(auditLlvmEffectWrites(*gepEscaped).unguardedWrites >= 1);
    assert(auditLlvmEffectWrites(*gepSafe).unguardedWrites == 0);
    assert(auditLlvmEffectWrites(*mixed).unguardedWrites == 1);
    // LLVM O2 may eliminate a pure internal function entirely when
    // its returned scalar is unused. Re-resolve after IPO instead of
    // dereferencing a potentially erased Function*.
    if (auto* remaining = m.getFunction("pure_internal_intermediate"))
        assert(xyo::ir::LlvmLocallyPureCallees{}.proven(remaining));
    // The pure internal chain must still be classified as harmless
    // after the actual O2 IPO/inlining and memory attribute passes.
    xyo::ir::LlvmLocallyPureCallees optimizedCallees;
    assert(xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *safeCaller, &optimizedCallees).empty());
    assert(xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *unsafeCaller, &optimizedCallees).size() == 1);
    assert(xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *nestedCaller, &optimizedCallees).size() == 1);
    assert(xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *volatileInternalCaller, &optimizedCallees).size() == 1);
    assert(xyo::ir::findUnprovenLlvmEffectCheckpoints(
        *indirectCaller, &optimizedCallees).size() == 1);
    assert(xyo::ir::findUnprovenLlvmEffectCheckpoints(*mixed).size() == 1);
    assert(xyo::ir::findUnprovenLlvmEffectCheckpoints(*mixed)[0] ==
           firstMixedCheckpoint);
    assert(auditLlvmEffectWrites(m).functionsWithCheckpointsButNoEffects >= 1);

    auto countFenceCalls = [](const Function& f) {
        unsigned count = 0;
        for (const auto& block : f)
            for (const auto& instruction : block)
                if (const auto* cb = dyn_cast<CallBase>(&instruction))
                    if (cb->getCalledFunction() &&
                        cb->getCalledFunction()->getName() ==
                            "xyo_ir001_vm_write")
                        ++count;
        return count;
    };
    const auto guardedBefore = countFenceCalls(*guarded);
    const auto missingBefore = countFenceCalls(*missing);
    const auto diamondBefore = countFenceCalls(*diamond);
    const auto opaqueBefore = countFenceCalls(*unknown);
    const auto readonlyBefore = countFenceCalls(*readonlyCaller);
    const auto attrsBefore = countFenceCalls(*attrsCaller);
    const auto mathBefore = countFenceCalls(*mathCaller);
    const auto wrappedBefore = countFenceCalls(*wrappedCaller);
    const auto volatileBefore = countFenceCalls(*volatileCaller);
    const auto atomicBefore = countFenceCalls(*atomicCaller);
    const auto fenceBefore = countFenceCalls(*fenceCaller);
    const auto calleeBefore = countFenceCalls(*callee);
    const auto isolatedBefore = countFenceCalls(*isolated);
    const auto escapedBefore = countFenceCalls(*escaped);
    const auto gepSafeBefore = countFenceCalls(*gepSafe);
    const auto gepEscapedBefore = countFenceCalls(*gepEscaped);
    const auto mixedBefore = countFenceCalls(*mixed);
    const auto safeCallerBefore = countFenceCalls(*safeCaller);
    const auto unsafeCallerBefore = countFenceCalls(*unsafeCaller);
    const auto nestedCallerBefore = countFenceCalls(*nestedCaller);
    const auto volatileInternalBefore =
        countFenceCalls(*volatileInternalCaller);
    const auto indirectCallerBefore = countFenceCalls(*indirectCaller);
    const auto fenced = xyo::ir::fenceUnprovenLlvmEffectCheckpoints(m);
    assert(fenced >= 5);
    // A guarded source event retains its original marker count, but
    // every unproven function has a new barrier at its checkpoint.
    assert(countFenceCalls(*guarded) == guardedBefore);
    assert(countFenceCalls(*missing) == missingBefore + 1);
    assert(countFenceCalls(*diamond) == diamondBefore + 1);
    assert(countFenceCalls(*unknown) == opaqueBefore + 1);
    assert(countFenceCalls(*readonlyCaller) == readonlyBefore + 1);
    assert(countFenceCalls(*attrsCaller) == attrsBefore + 1);
    assert(countFenceCalls(*mathCaller) == mathBefore);
    assert(countFenceCalls(*wrappedCaller) == wrappedBefore + 1);
    assert(countFenceCalls(*volatileCaller) == volatileBefore + 1);
    assert(countFenceCalls(*atomicCaller) == atomicBefore + 1);
    assert(countFenceCalls(*fenceCaller) == fenceBefore + 1);
    assert(countFenceCalls(*callee) == calleeBefore + 1);
    assert(countFenceCalls(*isolated) == isolatedBefore);
    assert(countFenceCalls(*escaped) == escapedBefore + 1);
    assert(countFenceCalls(*gepSafe) == gepSafeBefore);
    assert(countFenceCalls(*gepEscaped) == gepEscapedBefore + 1);
    assert(countFenceCalls(*mixed) == mixedBefore + 1);
    assert(countFenceCalls(*safeCaller) == safeCallerBefore);
    assert(countFenceCalls(*unsafeCaller) == unsafeCallerBefore + 1);
    assert(countFenceCalls(*nestedCaller) == nestedCallerBefore + 1);
    assert(countFenceCalls(*volatileInternalCaller) ==
           volatileInternalBefore + 1);
    assert(countFenceCalls(*indirectCaller) ==
           indirectCallerBefore + 1);
    assert(safeCheckpoint->getCalledFunction());
    auto* injected = dyn_cast<CallBase>(firstMixedCheckpoint->getPrevNode());
    assert(injected && injected->getCalledFunction() &&
           injected->getCalledFunction()->getName() ==
               "xyo_ir001_vm_write");
    assert(secondMixedCheckpoint->getPrevNode() != injected);
    assert(xyo::ir::fenceUnprovenLlvmEffectCheckpoints(m) == 0);
    assert(!verifyModule(m, &errs()));

    std::cout << "PASS IR-001 post-opt LLVM write CFG audit + deny (O2)\n";
}
