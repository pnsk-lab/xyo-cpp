#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/llvm_ssa_liveness.hpp"
#include "../../src/ir/llvm_ssa_spill.hpp"
#include "../../src/ir/source_ssa_snapshot.hpp"
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/Support/TargetSelect.h>
#include <memory>
#include <string>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <cassert>
#include <iostream>

int main() {
    llvm::LLVMContext ctx;
    llvm::Module mod("ssa-liveness", ctx);
    llvm::IRBuilder<> b(ctx);
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(b.getVoidTy(), {b.getInt32Ty(), b.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage, "run", mod);
    auto it = fn->arg_begin();
    llvm::Value* input = &*it++;
    llvm::Value* ptr = &*it;
    auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    auto* exit = llvm::BasicBlock::Create(ctx, "exit", fn);
    const auto markerFn = mod.getOrInsertFunction(
        "checkpoint", llvm::FunctionType::get(b.getVoidTy(), false));
    const auto sink = mod.getOrInsertFunction(
        "sink", llvm::FunctionType::get(b.getVoidTy(),
            {b.getInt32Ty(), b.getPtrTy()}, false));

    b.SetInsertPoint(entry);
    auto* live = llvm::cast<llvm::Instruction>(
        b.CreateAdd(input, b.getInt32(1), "live"));
    b.CreateAdd(input, b.getInt32(2), "dead");
    auto* pointer = llvm::cast<llvm::Instruction>(
        b.CreateGEP(b.getInt8Ty(), ptr, live, "pointer"));
    auto* marker = b.CreateCall(markerFn);
    b.CreateBr(exit);
    b.SetInsertPoint(exit);
    b.CreateCall(sink, {live, pointer});
    b.CreateCall(sink, {input, pointer});
    b.CreateRetVoid();

    const auto result = xyo::ir::analyzeLlvmCheckpointLiveness(*fn, *marker);
    assert(result.validCheckpoint);
    assert(!result.numericComplete());
    assert(result.unsupported == 2); // pointer instruction and live argument
    assert(result.numeric.size() == 1);
    assert(result.numeric[0].definition == live);
    assert(result.numeric[0].width == 32);

    auto* other = llvm::Function::Create(
        llvm::FunctionType::get(b.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "other", mod);
    assert(!xyo::ir::analyzeLlvmCheckpointLiveness(
        *other, *marker).validCheckpoint);
    // A marker with no live instruction or argument is a valid empty
    // numeric liveness set (still not an effect-continuity certificate).
    auto* empty = llvm::Function::Create(
        llvm::FunctionType::get(b.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "empty", mod);
    auto* emptyBlock = llvm::BasicBlock::Create(ctx, "entry", empty);
    b.SetInsertPoint(emptyBlock);
    auto* emptyMarker = b.CreateCall(markerFn);
    b.CreateRetVoid();
    const auto emptyLive = xyo::ir::analyzeLlvmCheckpointLiveness(
        *empty, *emptyMarker);
    assert(emptyLive.numericComplete() && emptyLive.numeric.empty());
    // A compiler-only spill across the real checkpoint for i1/i32/i64.
    auto* sfn = llvm::Function::Create(
        llvm::FunctionType::get(b.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "spill", mod);
    auto* sb = llvm::BasicBlock::Create(ctx, "entry", sfn);
    b.SetInsertPoint(sb);
    const auto yieldBool = mod.getOrInsertFunction(
        "yield_bool", llvm::FunctionType::get(b.getInt1Ty(), false));
    const auto yieldInt = mod.getOrInsertFunction(
        "yield_int", llvm::FunctionType::get(b.getInt32Ty(), false));
    const auto yieldLong = mod.getOrInsertFunction(
        "yield_long", llvm::FunctionType::get(b.getInt64Ty(), false));
    llvm::Value* one = b.CreateCall(yieldBool);
    llvm::Value* two = b.CreateCall(yieldInt);
    llvm::Value* three = b.CreateCall(yieldLong);
    auto* savePoint = b.CreateCall(markerFn);
    auto* afterMarkerValue = b.CreateCall(yieldInt);
    const auto consume = mod.getOrInsertFunction(
        "consume", llvm::FunctionType::get(b.getVoidTy(),
            {b.getInt1Ty(), b.getInt32Ty(), b.getInt64Ty()}, false));
    b.CreateCall(consume, {one, two, three});
    b.CreateRetVoid();
    const auto spillLive =
        xyo::ir::analyzeLlvmCheckpointLiveness(*sfn, *savePoint);
    assert(spillLive.numericComplete() && spillLive.numeric.size() == 3);
    auto futureLive = spillLive;
    futureLive.numeric.push_back({afterMarkerValue, spillLive.numeric.back().id + 1, 32});
    const auto originalInstructionCount = sb->size();
    assert(!xyo::ir::emitLlvmNumericSsaSpill(*savePoint, futureLive));
    assert(sb->size() == originalInstructionCount);
    auto duplicateId = spillLive;
    duplicateId.numeric[1].id = duplicateId.numeric[0].id;
    assert(!xyo::ir::emitLlvmNumericSsaSpill(*savePoint, duplicateId));
    assert(sb->size() == originalInstructionCount);
    const auto spill =
        xyo::ir::emitLlvmNumericSsaSpill(*savePoint, spillLive);
    assert(spill && spill->complete && spill->storage);
    assert(spill->ids.size() == 3 && spill->widths.size() == 3);
    assert(spill->widths[0] == 1 && spill->widths[1] == 32 &&
           spill->widths[2] == 64);
    unsigned stores = 0, extensions = 0;
    for (const llvm::Instruction& inst : *sb) {
        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
            assert(store->isVolatile());
            ++stores;
        }
        if (llvm::isa<llvm::ZExtInst>(inst)) ++extensions;
    }
    assert(stores == 3 && extensions == 2);
    assert(!llvm::verifyFunction(*sfn, &llvm::errs()));
    const auto partial = xyo::ir::emitLlvmNumericSsaSpill(*marker, result);
    assert(partial && !partial->complete && partial->ids.size() == 1);
    assert(!llvm::verifyFunction(*fn, &llvm::errs()));
    // Execute an actual LLVM JIT function. An i64 value is produced
    // before a checkpoint, read after it, and also transferred from a
    // native alloca into a caller-owned array while the function is live.
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::LLVMContext execContext;
    auto executable = std::make_unique<llvm::Module>(
        "ir001-executable-transfer", execContext);
    llvm::IRBuilder<> exec(execContext);
    auto* transferFn = llvm::Function::Create(
        llvm::FunctionType::get(exec.getVoidTy(),
            {exec.getPtrTy(), exec.getInt64Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, "ir001_test_transfer",
        *executable);
    auto transferArgs = transferFn->arg_begin();
    llvm::Value* output = &*transferArgs++;
    llvm::Value* inputValue = &*transferArgs;
    auto* transferBlock = llvm::BasicBlock::Create(
        execContext, "entry", transferFn);
    auto* transferMarkerFn = llvm::Function::Create(
        llvm::FunctionType::get(exec.getVoidTy(), false),
        llvm::GlobalValue::InternalLinkage, "ir001_marker", *executable);
    auto* markerBlock = llvm::BasicBlock::Create(
        execContext, "entry", transferMarkerFn);
    exec.SetInsertPoint(markerBlock);
    exec.CreateRetVoid();
    exec.SetInsertPoint(transferBlock);
    llvm::Value* calculated = exec.CreateAdd(
        inputValue, exec.getInt64(7), "live.calculated");
    auto* transferMarker = exec.CreateCall(transferMarkerFn);
    // This pointer exists in the same function, but is defined AFTER the
    // checkpoint and therefore cannot legally be used for a pre-marker copy.
    auto* lateDestination = llvm::cast<llvm::Instruction>(
        exec.CreateGEP(exec.getInt64Ty(), output, exec.getInt32(0),
                       "late.destination"));
    exec.CreateStore(calculated,
        exec.CreateConstGEP1_32(exec.getInt64Ty(), output, 1));
    exec.CreateRetVoid();
    const auto transferLive = xyo::ir::analyzeLlvmCheckpointLiveness(
        *transferFn, *transferMarker);
    assert(transferLive.validCheckpoint &&
           transferLive.numeric.size() == 1);
    auto saved = xyo::ir::emitLlvmNumericSsaSpill(
        *transferMarker, transferLive);
    assert(saved && saved->ids.size() == 1);
    assert(!xyo::ir::emitLlvmNumericSsaTransfer(
        *transferMarker, *saved, output, 0));
    assert(!xyo::ir::emitLlvmNumericSsaTransfer(
        *transferMarker, *saved, ptr, 1)); // foreign LLVM function
    assert(!xyo::ir::emitLlvmNumericSsaTransfer(
        *transferMarker, *saved, lateDestination, 1));
    assert(xyo::ir::emitLlvmNumericSsaTransfer(
        *transferMarker, *saved, output, 1));
    assert(!llvm::verifyModule(*executable, &llvm::errs()));
    std::string jitError;
    std::unique_ptr<llvm::ExecutionEngine> jit(
        llvm::EngineBuilder(std::move(executable))
            .setEngineKind(llvm::EngineKind::JIT)
            .setErrorStr(&jitError)
            .create());
    assert(jit && jitError.empty());
    using TransferFunction = void (*)(uint64_t*, uint64_t);
    auto fnAddress = jit->getFunctionAddress("ir001_test_transfer");
    assert(fnAddress);
    auto generated = reinterpret_cast<TransferFunction>(fnAddress);
    uint64_t transferred[2] = {};
    generated(transferred, 123456789ULL);
    assert(transferred[0] == 123456796ULL);
    assert(transferred[1] == 123456796ULL);
    // The caller-owned array may be discarded after making the detached
    // IR snapshot. Exact bits and stable IDs survive native stack teardown.
    xyo::ir::SourceSsaManifest manifest{
        20261109, 13, 17, 2, 19,
        {{saved->ids.front(), saved->widths.front()}}};
    auto detached = xyo::ir::SourceSsaSnapshot::capture(
        manifest, std::span<const uint64_t>(transferred, 1));
    assert(detached && detached->matches(manifest));
    transferred[0] = 0;
    assert(detached->values.front() == 123456796ULL);
    auto wrongManifest = manifest;
    ++wrongManifest.resumePc;
    assert(!detached->matches(wrongManifest));
    std::cout << "PASS LLVM SSA checkpoint liveness\n";
}
