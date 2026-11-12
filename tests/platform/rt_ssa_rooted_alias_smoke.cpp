// RT-001: exact bit-preserving LLVM i64 SSA ancestry, never GC pointer guessing.
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
    LLVMContext ctx;
    Module mod("rt001.same.rooted.argument", ctx);
    IRBuilder<> b(ctx);
    auto* fn = Function::Create(
        FunctionType::get(b.getVoidTy(),
            {b.getPtrTy(), b.getInt64Ty(), b.getInt64Ty(),
             b.getInt1Ty()}, false),
        GlobalValue::ExternalLinkage, "probe", mod);
    auto arg = fn->arg_begin();
    const Value* context = &*arg++;
    Value* a = &*arg++;
    Value* different = &*arg++;
    Value* condition = &*arg++;
    auto* entry = BasicBlock::Create(ctx, "entry", fn);
    auto* yes = BasicBlock::Create(ctx, "yes", fn);
    auto* no = BasicBlock::Create(ctx, "no", fn);
    auto* merge = BasicBlock::Create(ctx, "merge", fn);
    b.SetInsertPoint(entry);
    auto* sameSelect = b.Insert(SelectInst::Create(
        condition, a, a));
    auto* differentSelect = b.CreateSelect(condition, a, different);
    auto* math = b.CreateAdd(a, b.getInt64(1));
    auto* freeze = b.CreateFreeze(a);
    auto* slot = b.CreateAlloca(b.getInt64Ty());
    b.CreateStore(a, slot);
    auto* loaded = b.CreateLoad(b.getInt64Ty(), slot);
    b.CreateCondBr(condition, yes, no);
    b.SetInsertPoint(yes); b.CreateBr(merge);
    b.SetInsertPoint(no); b.CreateBr(merge);
    b.SetInsertPoint(merge);
    auto* samePhi = b.CreatePHI(b.getInt64Ty(), 2);
    samePhi->addIncoming(a, yes);
    samePhi->addIncoming(a, no);
    auto* mixedPhi = b.CreatePHI(b.getInt64Ty(), 2);
    mixedPhi->addIncoming(a, yes);
    mixedPhi->addIncoming(different, no);
    auto* nested = b.CreateSelect(condition, samePhi, sameSelect);
    auto* mixedNested = b.CreateSelect(condition, samePhi, mixedPhi);
    auto sink = mod.getOrInsertFunction("consume",
        FunctionType::get(b.getVoidTy(),
            {b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty(),
             b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty(),
             b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty()},
            false));
    b.CreateCall(sink,
        {sameSelect, differentSelect, math, freeze, loaded,
         samePhi, mixedPhi, nested, mixedNested});
    b.CreateRetVoid();
    assert(!verifyFunction(*fn, &errs()));

    using xyo::rt::sameBoxedArgumentRoot;
    assert(sameBoxedArgumentRoot(context) == 0);
    assert(sameBoxedArgumentRoot(a) == 1);
    assert(sameBoxedArgumentRoot(different) == 2);
    assert(sameBoxedArgumentRoot(condition) == 0);
    assert(sameBoxedArgumentRoot(sameSelect) == 1);
    assert(sameBoxedArgumentRoot(differentSelect) == 0);
    assert(sameBoxedArgumentRoot(math) == 0);
    assert(sameBoxedArgumentRoot(freeze) == 0);
    assert(sameBoxedArgumentRoot(loaded) == 0);
    assert(sameBoxedArgumentRoot(samePhi) == 1);
    assert(sameBoxedArgumentRoot(mixedPhi) == 0);
    assert(sameBoxedArgumentRoot(nested) == 1);
    assert(sameBoxedArgumentRoot(mixedNested) == 0);

    auto* cyclic = Function::Create(
        FunctionType::get(b.getVoidTy(), {b.getInt64Ty(),
                                         b.getInt1Ty()}, false),
        GlobalValue::ExternalLinkage, "cycle", mod);
    auto* start = BasicBlock::Create(ctx, "start", cyclic);
    auto* loop = BasicBlock::Create(ctx, "loop", cyclic);
    auto* end = BasicBlock::Create(ctx, "end", cyclic);
    b.SetInsertPoint(start);
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    auto* phi = b.CreatePHI(b.getInt64Ty(), 2);
    phi->addIncoming(&*cyclic->arg_begin(), start);
    phi->addIncoming(phi, loop);
    auto cyclicArgs = cyclic->arg_begin();
    ++cyclicArgs;
    b.CreateCondBr(&*cyclicArgs, loop, end);
    b.SetInsertPoint(end);
    auto consumeOne = mod.getOrInsertFunction("consume_one",
        FunctionType::get(b.getVoidTy(), {b.getInt64Ty()}, false));
    b.CreateCall(consumeOne, {phi});
    b.CreateRetVoid();
    assert(!verifyFunction(*cyclic, &errs()));
    assert(sameBoxedArgumentRoot(phi) == 0);

    // Arbitrary nested selects must fail bounded recursion, even if all
    // branches repeat the same argument and LLVM IR remains well-typed.
    auto* deepFn = Function::Create(
        FunctionType::get(b.getVoidTy(),
            {b.getInt64Ty(), b.getInt1Ty()}, false),
        GlobalValue::ExternalLinkage, "deep", mod);
    auto* deepEntry = BasicBlock::Create(ctx, "entry", deepFn);
    b.SetInsertPoint(deepEntry);
    auto deepArgs = deepFn->arg_begin();
    Value* deep = &*deepArgs++;
    Value* cond = &*deepArgs;
    for (int i = 0; i < 70; ++i)
        deep = b.Insert(SelectInst::Create(cond, deep, deep));
    b.CreateCall(consumeOne, {deep});
    b.CreateRetVoid();
    assert(!verifyFunction(*deepFn, &errs()));
    assert(sameBoxedArgumentRoot(deep) == 0);
    // Independently GC-created string SSA is a different provenance
    // proof from original boxed procedure argument ancestry. The
    // trusted host join returns an i64 Scratch string; two separate
    // join sites can be merged into a live LLVM i64 PHI safely ONLY
    // when every incoming edge has the same trusted producer class.
    auto* creator = Function::Create(
        FunctionType::get(b.getVoidTy(),
            {b.getInt64Ty(), b.getInt1Ty()}, false),
        GlobalValue::ExternalLinkage, "string_creator", mod);
    auto* stringStart = BasicBlock::Create(ctx, "begin", creator);
    auto* stringYes = BasicBlock::Create(ctx, "left", creator);
    auto* stringNo = BasicBlock::Create(ctx, "right", creator);
    auto* stringMerge = BasicBlock::Create(ctx, "merge", creator);
    auto args = creator->arg_begin();
    auto* source = &*args++;
    auto* stringCondition = &*args;
    auto joinFn = mod.getOrInsertFunction(
        "xyo_join", FunctionType::get(b.getInt64Ty(),
            {b.getInt64Ty(), b.getInt64Ty()}, false));
    auto joinSuffixFn = mod.getOrInsertFunction(
        "xyo_join_suffix", FunctionType::get(b.getInt64Ty(),
            {b.getInt64Ty(), b.getInt64Ty()}, false));
    auto untrustedFn = mod.getOrInsertFunction(
        "xyo_random_untrusted", FunctionType::get(
            b.getInt64Ty(), {b.getInt64Ty()}, false));
    b.SetInsertPoint(stringStart);
    b.CreateCondBr(stringCondition, stringYes, stringNo);
    b.SetInsertPoint(stringYes);
    auto* left = b.CreateCall(joinFn, {source, source});
    b.CreateBr(stringMerge);
    b.SetInsertPoint(stringNo);
    auto* right = b.CreateCall(joinSuffixFn, {source, source});
    auto* untrusted = b.CreateCall(untrustedFn, {source});
    b.CreateBr(stringMerge);
    b.SetInsertPoint(stringMerge);
    auto* phiGood = b.CreatePHI(b.getInt64Ty(), 2);
    phiGood->addIncoming(left, stringYes);
    phiGood->addIncoming(right, stringNo);
    auto* phiBad = b.CreatePHI(b.getInt64Ty(), 2);
    phiBad->addIncoming(left, stringYes);
    phiBad->addIncoming(untrusted, stringNo);
    // Both select inputs must dominate the merge block; retain an
    // explicit instruction rather than folding identical values away.
    auto* selectGood = b.Insert(SelectInst::Create(
        stringCondition, phiGood, phiGood));
    auto* selectBad = b.CreateSelect(stringCondition, phiGood, source);
    auto* newArithmetic = b.CreateAdd(phiGood, b.getInt64(1));
    b.CreateCall(consumeOne, {phiGood});
    b.CreateCall(consumeOne, {phiBad});
    b.CreateCall(consumeOne, {selectGood});
    b.CreateCall(consumeOne, {selectBad});
    b.CreateCall(consumeOne, {newArithmetic});
    b.CreateRetVoid();
    assert(!verifyFunction(*creator, &errs()));
    using xyo::rt::trustedGeneratedStringSource;
    assert(!trustedGeneratedStringSource(source));
    assert(trustedGeneratedStringSource(left));
    assert(trustedGeneratedStringSource(right));
    assert(!trustedGeneratedStringSource(untrusted));
    assert(trustedGeneratedStringSource(phiGood));
    assert(!trustedGeneratedStringSource(phiBad));
    assert(trustedGeneratedStringSource(selectGood));
    assert(!trustedGeneratedStringSource(selectBad));
    assert(!trustedGeneratedStringSource(newArithmetic));
    assert(!trustedGeneratedStringSource(loaded));
    assert(!trustedGeneratedStringSource(freeze));
    assert(!trustedGeneratedStringSource(phi));
    assert(!trustedGeneratedStringSource(deep));

    // Unknown 64-bit words remain hazardous even when pointer SSA
    // disappears. Numeric width alone must never authorize a GC.
    xyo::rt::LiveI64ProvenanceCensus census;
    census.observe(a);
    census.observe(nested);
    census.observe(left);
    census.observe(phiGood);
    census.observe(math);
    census.observe(mixedPhi);
    census.observe(loaded);
    census.observe(condition); // i1 is not a 64-bit candidate
    assert(census.total == 7);
    assert(census.argumentAliases == 2);
    assert(census.generatedStrings == 2);
    assert(census.unclassified == 3);
    assert(census.complete());

    std::puts("PASS RT-001: exact original-arg ancestry plus "
              "trusted generated join/PHI/SELECT, mixed/cycle/"
              "arithmetic/load provenance rejected");
}
