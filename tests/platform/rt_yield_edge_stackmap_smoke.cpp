// RT-001: postoptimizer-yield call-edge StackMaps are diagnostics only.
// No implication that a native Fiber is stopped at the StackMap PC.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/rt_yield_edge_stackmap.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <cassert>
#include <cstdio>
#include <set>

int main() {
    using namespace llvm;
    LLVMContext ctx;
    Module module("rt-yield-edge", ctx);
    IRBuilder<> b(ctx);
    Type* p = b.getPtrTy();
    auto* yield = Function::Create(
        FunctionType::get(b.getVoidTy(), {p}, false),
        GlobalValue::ExternalLinkage, "xyo_yield", module);
    auto* fast = Function::Create(
        FunctionType::get(b.getVoidTy(), {p}, false),
        GlobalValue::ExternalLinkage, "xyo_yield_fast", module);
    auto* waiting = Function::Create(
        FunctionType::get(b.getVoidTy(), {p}, false),
        GlobalValue::ExternalLinkage, "xyo_wait_yield", module);
    auto* nested = Function::Create(
        FunctionType::get(b.getVoidTy(), {p}, false),
        GlobalValue::ExternalLinkage, "xyo_script_nested_p1", module);
    auto* wrong = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getInt64Ty()}, false),
        GlobalValue::ExternalLinkage, "xyo_yield_wrong", module);

    auto create = [&](const char* name, int kind) {
        auto* fn = Function::Create(
            FunctionType::get(b.getVoidTy(), {p}, false),
            GlobalValue::ExternalLinkage, name, module);
        b.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
        Value* arg = fn->getArg(0);
        if (kind == 1) b.CreateCall(yield, {arg});
        else if (kind == 2) b.CreateCall(fast, {arg});
        else if (kind == 3) b.CreateCall(yield, {
            ConstantPointerNull::get(b.getPtrTy())});
        else if (kind == 4) b.CreateCall(waiting, {arg});
        else if (kind == 5) b.CreateCall(
            FunctionType::get(b.getVoidTy(), false), arg, {});
        else if (kind == 6) b.CreateCall(nested, {arg});
        else b.CreateCall(wrong, {b.getInt64(9)});
        b.CreateRetVoid();
    };
    create("xyo_script_good_p0", 1);
    create("xyo_script_fast_p0", 2);
    create("xyo_script_forged_p0", 3);
    create("xyo_script_wait_p0", 4);
    create("xyo_script_indirect_p0", 5);
    create("xyo_script_nestedcaller_p0", 6);
    create("xyo_script_wrong_p0", 7);
    create("other_runtime_function", 1);
    assert(!verifyModule(module, &errs()));

    PassBuilder pass;
    LoopAnalysisManager loop;
    FunctionAnalysisManager functions;
    CGSCCAnalysisManager cgs;
    ModuleAnalysisManager modules;
    pass.registerModuleAnalyses(modules);
    pass.registerCGSCCAnalyses(cgs);
    pass.registerFunctionAnalyses(functions);
    pass.registerLoopAnalyses(loop);
    pass.crossRegisterProxies(loop, functions, cgs, modules);
    pass.buildPerModuleDefaultPipeline(OptimizationLevel::O3)
        .run(module, modules);

    const auto records = xyo::rt::emitExperimentalYieldEdgeStackmaps(module);
    std::set<uint64_t> ids;
    unsigned emitted = 0, refused = 0;
    for (const auto& site : records) {
        assert(ids.insert(site.id).second);
        if (site.emitted) {
            assert(site.locationCount == 1);
            assert(site.ordinal == 1);
            assert(site.functionName == "xyo_script_good_p0" ||
                   site.functionName == "xyo_script_fast_p0");
            ++emitted;
        } else {
            assert(site.functionName == "xyo_script_forged_p0");
            ++refused;
        }
    }
    assert(emitted == 2 && refused == 1 &&
           ids.size() == 3 && records.size() == 3);
    assert(!verifyModule(module, &errs()));
    // There are four other (unmapped) suspension possibilities:
    // forged JitCtx, a nested wait helper, an indirect call and a
    // nested JIT procedure. None may be treated as a certified
    // yield-edge StackMap merely because two direct calls matched.
    const auto unmapped =
        xyo::rt::auditUnmappedYieldEdges(module);
    bool forged = false, wait = false, indirect = false, child = false;
    for (const auto& site : unmapped) {
        if (site.functionName == "xyo_script_forged_p0") {
            assert(site.reason == "unmapped_direct_yield");
            forged = true;
        } else if (site.functionName == "xyo_script_wait_p0") {
            assert(site.helperName == "xyo_wait_yield" &&
                   site.reason == "nested_helper");
            wait = true;
        } else if (site.functionName == "xyo_script_indirect_p0") {
            assert(site.reason == "indirect_call" &&
                   site.helperName == "<indirect>");
            indirect = true;
        } else if (site.functionName == "xyo_script_nestedcaller_p0") {
            assert(site.reason == "nested_jit_call");
            child = true;
        } else {
            assert(false && "unexpected unmapped native yield edge");
        }
    }
    assert(forged && wait && indirect && child &&
           unmapped.size() == 4);

    // The emitted pseudo-instruction must be immediately before the
    // corresponding native yield CALL in the optimized IR; it may not
    // be silently moved after it or applied to malformed yield helpers.
    for (Function& fn : module) {
        if (fn.isDeclaration() ||
            !fn.getName().starts_with("xyo_script_")) continue;
        for (BasicBlock& bb : fn) {
            for (Instruction& inst : bb) {
                auto* call = dyn_cast<CallInst>(&inst);
                if (!call || !call->getCalledFunction()) continue;
                StringRef helper = call->getCalledFunction()->getName();
                if (helper != "xyo_yield" &&
                    helper != "xyo_yield_fast") continue;
                bool valid = fn.getName() != "xyo_script_forged_p0";
                auto* previous = dyn_cast_or_null<CallInst>(
                    call->getPrevNode());
                if (valid) {
                    assert(previous && previous->getCalledFunction() &&
                           previous->getCalledFunction()
                               ->isIntrinsic() &&
                           previous->getCalledFunction()
                               ->getIntrinsicID() ==
                               Intrinsic::experimental_stackmap);
                } else if (previous && previous->getCalledFunction()) {
                    assert(!previous->getCalledFunction()->isIntrinsic() ||
                           previous->getCalledFunction()->getIntrinsicID() !=
                               Intrinsic::experimental_stackmap);
                }
            }
        }
    }
    std::puts("PASS RT-001 yield-edge post-O3: valid immediate call "
              "probes, forged call rejected, valid module, GC veto");
}
