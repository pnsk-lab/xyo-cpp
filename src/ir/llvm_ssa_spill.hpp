// IR-001: opt-in numeric LLVM SSA spill emission for an exact checkpoint.
// The emitted alloca belongs to the native activation and is NOT a
// detached, scheduler-owned continuation. No executable deopt authorization.
#pragma once

#include "llvm_ssa_liveness.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace xyo::ir {

struct LlvmSsaSpill {
    llvm::AllocaInst* storage = nullptr;
    std::vector<uint32_t> ids;
    std::vector<uint8_t> widths;
    bool complete = false;
};

// Capture representable instruction results even if other values remain
// unsupported. The result is explicitly incomplete in that case, never
// an executable state or a certificate of complete SSA coverage.
// Pre-optimization SSA IDs must not be persisted after IR rewriting.
// A caller may insert inspection loads after marker, while the original
// marker remains in place. Storage is NOT a GC root or a frame image.
inline std::optional<LlvmSsaSpill> emitLlvmNumericSsaSpill(
    llvm::CallBase& marker, const LlvmCheckpointLiveness& live) {
    if (!live.validCheckpoint || live.numeric.empty() ||
        live.numeric.size() > 4096 || !marker.getFunction())
        return std::nullopt;
    auto& function = *marker.getFunction();
    // Validate the entire descriptor before mutating the LLVM IR.
    // A stale analysis result can name a value defined AFTER the marker;
    // emitting a store for that value would produce invalid LLVM IR.
    llvm::DominatorTree dominators(function);
    bool first = true;
    uint32_t previousId = 0;
    for (const auto& slot : live.numeric) {
        if (!slot.definition || slot.definition->getFunction() != &function ||
            (slot.width != 1 && slot.width != 32 && slot.width != 64) ||
            !slot.definition->getType()->isIntegerTy(slot.width) ||
            !dominators.dominates(slot.definition, &marker) ||
            (!first && slot.id <= previousId))
            return std::nullopt;
        previousId = slot.id;
        first = false;
    }
    auto& context = function.getContext();
    llvm::IRBuilder<> entry(&*function.getEntryBlock().getFirstInsertionPt());
    llvm::Type* wordTy = llvm::Type::getInt64Ty(context);
    llvm::Value* count = entry.getInt32(uint32_t(live.numeric.size()));
    auto* storage = entry.CreateAlloca(wordTy, count, "ir001.ssa.spill");
    llvm::IRBuilder<> before(&marker);
    LlvmSsaSpill output;
    output.storage = storage;
    for (size_t i = 0; i < live.numeric.size(); ++i) {
        const auto& slot = live.numeric[i];
        llvm::Value* word = const_cast<llvm::Instruction*>(slot.definition);
        if (slot.width != 64)
            word = before.CreateZExt(word, wordTy, "ir001.ssa.extend");
        auto* ptr = before.CreateConstGEP1_32(
            wordTy, storage, uint32_t(i), "ir001.ssa.slot");
        before.CreateStore(word, ptr)->setVolatile(true);
        output.ids.push_back(slot.id);
        output.widths.push_back(slot.width);
    }
    output.complete = live.numericComplete(); // NOT effect/GC coverage
    return output;
}

// Transfer spill words into caller-owned storage synchronously, while the
// native activation is alive. The caller MUST supply at least ids.size()
// writable uint64_t slots and publish them to rooted storage before the
// native frame is destroyed. No pointer escapes into a detached image.
// This emits code; it does not execute or validate the host destination.
inline bool emitLlvmNumericSsaTransfer(
    llvm::CallBase& marker, const LlvmSsaSpill& spill,
    llvm::Value* destination, uint32_t destinationCapacity) {
    if (!marker.getFunction() || !spill.storage || !destination ||
        !destination->getType()->isPointerTy() ||
        !spill.storage->getFunction() ||
        spill.storage->getFunction() != marker.getFunction() ||
        spill.ids.empty() || spill.ids.size() != spill.widths.size() ||
        spill.ids.size() > destinationCapacity)
        return false;
    // Avoid references to instructions/arguments owned by other functions.
    if (const auto* arg = llvm::dyn_cast<llvm::Argument>(destination)) {
        if (arg->getParent() != marker.getFunction()) return false;
    } else if (const auto* inst =
                   llvm::dyn_cast<llvm::Instruction>(destination)) {
        if (inst->getFunction() != marker.getFunction()) return false;
        // Being owned by the function does not imply availability at the
        // checkpoint (e.g. a GEP in a successor or a later instruction).
        llvm::DominatorTree dominators(*marker.getFunction());
        if (!dominators.dominates(inst, &marker)) return false;
    } else {
        return false;
    }
    llvm::IRBuilder<> before(&marker);
    llvm::Type* wordTy = before.getInt64Ty();
    for (uint32_t i = 0; i < spill.ids.size(); ++i) {
        auto* source = before.CreateConstGEP1_32(wordTy, spill.storage, i);
        auto* value = before.CreateLoad(wordTy, source, "ir001.ssa.read");
        value->setVolatile(true);
        auto* target = before.CreateConstGEP1_32(wordTy, destination, i);
        before.CreateStore(value, target)->setVolatile(true);
    }
    return true;
}

} // namespace xyo::ir
