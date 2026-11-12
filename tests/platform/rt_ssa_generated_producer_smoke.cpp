// RT-001: production native LLVM string-call provenance, pure IR smoke.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/jit/rt_ssa_extra_capture.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <cassert>
#include <cstdio>

int main() {
    llvm::LLVMContext context;
    llvm::Module module("rt001.generated.string.provenance", context);
    llvm::IRBuilder<> b(context);
    auto* i64 = b.getInt64Ty();
    auto* i1 = b.getInt1Ty();

    auto* function = llvm::Function::Create(
        llvm::FunctionType::get(
            b.getVoidTy(), {i64, i64, i1}, false),
        llvm::GlobalValue::ExternalLinkage, "probe", module);
    auto args = function->arg_begin();
    llvm::Value* arg1 = &*args++;
    llvm::Value* arg2 = &*args++;
    llvm::Value* condition = &*args;

    const auto signature = llvm::FunctionType::get(i64, {i64, i64}, false);
    const auto trusted = module.getOrInsertFunction(
        "xyo_join_suffix", signature);
    const auto trusted2 = module.getOrInsertFunction(
        "xyo_join", signature);
    const auto untrusted = module.getOrInsertFunction(
        "untrusted_join_suffix", signature);
    const auto wrongAbi = module.getOrInsertFunction(
        "xyo_join_strings",
        llvm::FunctionType::get(i64, {i64}, false));

    auto* entry = llvm::BasicBlock::Create(
        context, "entry", function);
    auto* yes = llvm::BasicBlock::Create(
        context, "yes", function);
    auto* no = llvm::BasicBlock::Create(
        context, "no", function);
    auto* join = llvm::BasicBlock::Create(
        context, "join", function);

    b.SetInsertPoint(entry);
    b.CreateCondBr(condition, yes, no);
    b.SetInsertPoint(yes);
    auto* joinedTrue = b.CreateCall(trusted, {arg1, arg2});
    auto* unknownTrue = b.CreateCall(untrusted, {arg1, arg2});
    b.CreateBr(join);
    b.SetInsertPoint(no);
    auto* joinedFalse = b.CreateCall(trusted2, {arg2, arg1});
    auto* mismatched = b.CreateCall(wrongAbi, {arg1});
    b.CreateBr(join);
    b.SetInsertPoint(join);
    auto* merged = b.CreatePHI(i64, 2, "gc.string.join");
    merged->addIncoming(joinedTrue, yes);
    merged->addIncoming(joinedFalse, no);
    auto* mixed = b.CreatePHI(i64, 2, "unknown.mix");
    mixed->addIncoming(unknownTrue, yes);
    mixed->addIncoming(joinedFalse, no);
    auto* picked = b.Insert(llvm::SelectInst::Create(
        condition, merged, merged));
    auto* mixedPick = b.Insert(llvm::SelectInst::Create(
        condition, merged, mixed));
    auto* math = b.CreateAdd(merged, b.getInt64(1));
    // A depth-70 chain must fail closed even if both SELECT arms
    // originate from the same trusted host result.
    llvm::Value* tooDeep = merged;
    for (unsigned i = 0; i < 70; ++i)
        tooDeep = b.Insert(llvm::SelectInst::Create(
            condition, tooDeep, tooDeep));
    const auto sink = module.getOrInsertFunction(
        "consume", llvm::FunctionType::get(
            b.getVoidTy(), {i64, i64, i64, i64}, false));
    // Only merged PHI/select results dominate this block. Raw
    // per-arm helper call values DO NOT: testing them as direct
    // sink arguments here would create invalid LLVM IR.
    b.CreateCall(sink, {merged, mixed, picked, mixedPick});
    b.CreateRetVoid();
    assert(!llvm::verifyFunction(*function, &llvm::errs()));

    using xyo::rt::trustedGeneratedStringSource;
    assert(!trustedGeneratedStringSource(arg1));
    assert(!trustedGeneratedStringSource(arg2));
    assert(!trustedGeneratedStringSource(condition));
    assert(trustedGeneratedStringSource(joinedTrue));
    assert(trustedGeneratedStringSource(joinedFalse));
    assert(trustedGeneratedStringSource(merged));
    assert(trustedGeneratedStringSource(picked));
    assert(!trustedGeneratedStringSource(unknownTrue));
    assert(!trustedGeneratedStringSource(mismatched));
    assert(!trustedGeneratedStringSource(mixed));
    assert(!trustedGeneratedStringSource(mixedPick));
    assert(!trustedGeneratedStringSource(math));
    assert(xyo::rt::sameBoxedArgumentRoot(merged) == 0);

    assert(!trustedGeneratedStringSource(tooDeep));

    std::puts("PASS RT-001: native-created i64 join Call/PHI/"
              "SELECT certificates; wrong ABI, mixed producer, "
              "arithmetic and 70-depth fail closed");
}
