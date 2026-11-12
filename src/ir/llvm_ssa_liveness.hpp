// IR-001: conservative LLVM SSA values live across a source checkpoint.
// Diagnostic/compiler input only. No side-effect, GC or deopt certification.
#pragma once

#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace xyo::ir {

struct LlvmLiveValue {
    const llvm::Instruction* definition = nullptr;
    uint32_t id = 0;    // function-local IR traversal order before optimization
    uint8_t width = 0;  // i1/i32/i64 raw bit width
};

struct LlvmCheckpointLiveness {
    std::vector<LlvmLiveValue> numeric;
    uint32_t unsupported = 0;
    bool validCheckpoint = false;
    // Only complete if every potentially live value is representable.
    // Does NOT mean all effects/GC roots are safely recoverable.
    bool numericComplete() const {
        return validCheckpoint && unsupported == 0;
    }
};

// The marker must be a real call instruction inside a defined function.
// Dominance identifies definitions available at the marker. Reachability
// deliberately over-approximates future uses, including loop backedges.
// A value that cannot be encoded in a pointer-free numeric frame makes the
// checkpoint incomplete, rather than silently discarding the live value.
inline LlvmCheckpointLiveness analyzeLlvmCheckpointLiveness(
    llvm::Function& function, llvm::CallBase& marker) {
    LlvmCheckpointLiveness result;
    if (marker.getFunction() != &function || function.isDeclaration())
        return result;
    result.validCheckpoint = true;

    std::unordered_set<const llvm::BasicBlock*> reachable;
    std::vector<const llvm::BasicBlock*> work{marker.getParent()};
    while (!work.empty()) {
        const auto* block = work.back();
        work.pop_back();
        if (!reachable.insert(block).second) continue;
        for (const llvm::BasicBlock* next : llvm::successors(block))
            work.push_back(next);
    }
    llvm::DominatorTree dominators(function);
    const auto* markerBlock = marker.getParent();

    auto usedLater = [&](const llvm::Instruction& definition) {
        for (const llvm::Use& use : definition.uses()) {
            const auto* user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
            if (!user || !reachable.contains(user->getParent())) continue;
            if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(user)) {
                const auto incoming =
                    phi->getIncomingBlock(use.getOperandNo());
                if (reachable.contains(incoming)) return true;
                continue;
            }
            if (user->getParent() != markerBlock ||
                marker.comesBefore(user)) return true;
            // A loop can revisit this block: including a previously
            // executed use is conservative (false positives fail closed).
            for (const auto* pred : llvm::predecessors(markerBlock))
                if (reachable.contains(pred)) return true;
        }
        return false;
    };

    // Unmapped live function arguments also prevent complete SSA recovery.
    for (const auto& argument : function.args()) {
        for (const auto& use : argument.uses()) {
            const auto* user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
            if (!user || !reachable.contains(user->getParent())) continue;
            if (user->getParent() != markerBlock || marker.comesBefore(user)) {
                ++result.unsupported;
                break;
            }
        }
    }
    uint32_t nextId = 0;
    for (const auto& block : function) {
        for (const auto& instruction : block) {
            const uint32_t id = nextId++;
            if (instruction.getType()->isVoidTy() ||
                !dominators.dominates(&instruction, &marker) ||
                !usedLater(instruction))
                continue;
            const auto* type = instruction.getType();
            uint8_t width = 0;
            if (type->isIntegerTy(1)) width = 1;
            else if (type->isIntegerTy(32)) width = 32;
            else if (type->isIntegerTy(64)) width = 64;
            if (!width) {
                ++result.unsupported;
                continue;
            }
            result.numeric.push_back({&instruction, id, width});
        }
    }
    return result;
}

} // namespace xyo::ir
