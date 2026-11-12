// IR-001 compiler-only: bind frozen Scratch numeric source-slot labels
// to LLVM instruction identity at an exact live checkpoint.
// This is not a general Scratch variable-local provenance oracle.
// No runtime roots, effect receipts, SSA mutation or executable deopt.
#pragma once

#include "source_numeric_resume_coverage.hpp"
#include "llvm_ssa_liveness.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xyo::ir {

struct LlvmSourceNumericRegistration {
    SourceResumeSlot source; // from the frozen source lowering
    const llvm::Instruction* definition = nullptr; // actual LLVM identity
};

// A typed LLVM i64 is not automatically a Scratch numeric value: host
// helpers and pointer conversions can return boxed GC references in
// integer registers. Permit only locally explained RAW numeric bit
// transformations. In particular a call returning i64, a load,
// ptrtoint and an LLVM Argument are not self-certifying.
// LLVM icmp itself produces a genuine one-bit integer regardless of
// the operands' original representation.
inline bool provenLlvmRawNumeric(
    const llvm::Value* value,
    std::unordered_set<const llvm::Value*>& active,
    uint32_t& budget, uint32_t depth = 0) {
    if (!value || !value->getType()->isIntegerTy() ||
        depth >= 64 || budget == 0 || !active.insert(value).second)
        return false;
    --budget;
    bool proven = false;
    if (llvm::isa<llvm::ConstantInt>(value) ||
        llvm::isa<llvm::ICmpInst>(value)) {
        proven = true;
    } else if (const auto* cast =
                   llvm::dyn_cast<llvm::CastInst>(value)) {
        if (llvm::isa<llvm::ZExtInst, llvm::SExtInst,
                      llvm::TruncInst>(cast))
            proven = provenLlvmRawNumeric(
                cast->getOperand(0), active, budget, depth + 1);
    } else if (const auto* binary =
                   llvm::dyn_cast<llvm::BinaryOperator>(value)) {
        switch (binary->getOpcode()) {
        case llvm::Instruction::Add:
        case llvm::Instruction::Sub:
        case llvm::Instruction::Mul:
        case llvm::Instruction::UDiv:
        case llvm::Instruction::SDiv:
        case llvm::Instruction::URem:
        case llvm::Instruction::SRem:
        case llvm::Instruction::And:
        case llvm::Instruction::Or:
        case llvm::Instruction::Xor:
        case llvm::Instruction::Shl:
        case llvm::Instruction::LShr:
        case llvm::Instruction::AShr:
            proven = provenLlvmRawNumeric(
                binary->getOperand(0), active, budget, depth + 1) &&
                provenLlvmRawNumeric(
                    binary->getOperand(1), active, budget, depth + 1);
            break;
        default:
            break;
        }
    } else if (const auto* select =
                   llvm::dyn_cast<llvm::SelectInst>(value)) {
        proven = provenLlvmRawNumeric(
            select->getTrueValue(), active, budget, depth + 1) &&
            provenLlvmRawNumeric(
                select->getFalseValue(), active, budget, depth + 1);
    } else if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
        proven = phi->getNumIncomingValues() != 0;
        for (unsigned i = 0; proven && i < phi->getNumIncomingValues(); ++i)
            proven = provenLlvmRawNumeric(
                phi->getIncomingValue(i), active, budget, depth + 1);
    }
    active.erase(value);
    return proven;
}

inline bool provenLlvmRawNumeric(const llvm::Value* value) {
    std::unordered_set<const llvm::Value*> active;
    uint32_t budget = 512;
    return provenLlvmRawNumeric(value, active, budget);
}

// A binding is only admitted if the EXACT LLVM instruction is live at
// the checkpoint; a coincidentally identical SSA numeric ID is never
// sufficient. Uncaptured arguments, pointers, floating registers,
// unrooted Scratch boxed Values and later definitions all fail closed.
// Extra numeric SSA coverage does not prove complete source restore.
inline std::optional<std::vector<SourceNumericSsaBinding>>
bindLlvmSourceNumericSlots(
    llvm::Function& function, llvm::CallBase& checkpoint,
    const SourceResumeRequirements& required,
    std::vector<LlvmSourceNumericRegistration> registrations) {
    if (!required.valid() || checkpoint.getFunction() != &function ||
        function.isDeclaration() ||
        registrations.size() > required.required.size())
        return std::nullopt;
    const auto live =
        analyzeLlvmCheckpointLiveness(function, checkpoint);
    if (!live.validCheckpoint)
        return std::nullopt;

    std::sort(registrations.begin(), registrations.end(),
        [](const auto& a, const auto& b) {
            if (a.source.statementPc != b.source.statementPc)
                return a.source.statementPc < b.source.statementPc;
            return a.source.sourceSlot < b.source.sourceSlot;
        });
    std::vector<SourceNumericSsaBinding> output;
    output.reserve(registrations.size());
    for (size_t i = 0; i < registrations.size(); ++i) {
        const auto& registration = registrations[i];
        if (!registration.definition ||
            registration.definition->getFunction() != &function ||
            !validSourceResumeSlot(registration.source) ||
            (registration.source.kind !=
                 SourceResumeSlotKind::NumericExpression &&
             registration.source.kind !=
                 SourceResumeSlotKind::NumericLoopTemporary) ||
            (i && registrations[i - 1].source.statementPc ==
                      registration.source.statementPc &&
                  registrations[i - 1].source.sourceSlot ==
                      registration.source.sourceSlot))
            return std::nullopt;

        const auto requirement = std::lower_bound(
            required.required.begin(), required.required.end(),
            registration.source,
            [](const SourceResumeSlot& a, const SourceResumeSlot& b) {
                return a.statementPc < b.statementPc ||
                       (a.statementPc == b.statementPc &&
                        a.sourceSlot < b.sourceSlot);
            });
        if (requirement == required.required.end() ||
            *requirement != registration.source)
            return std::nullopt;
        auto found = std::find_if(live.numeric.begin(), live.numeric.end(),
            [&](const LlvmLiveValue& value) {
                return value.definition == registration.definition;
            });
        if (found == live.numeric.end() ||
            found->width != registration.source.width ||
            !provenLlvmRawNumeric(registration.definition))
            return std::nullopt;
        const uint64_t descriptor =
            uint64_t(found->id) |
            (uint64_t(found->width) << 32);
        // Only descriptors actually included in the compiler-owned
        // typed checkpoint are acceptable. It is not enough that the
        // LLVM instruction is live in some other snapshot.
        if (std::find(required.site.descriptors.begin(),
                      required.site.descriptors.end(), descriptor) ==
            required.site.descriptors.end())
            return std::nullopt;
        for (const auto& previous : output)
            if (previous.descriptor == descriptor)
                return std::nullopt;
        output.push_back({registration.source, descriptor});
    }
    return output;
}

// Stronger entry point for an authentic compiled Scratch source:
// verify each original executable statement name and procedure owner
// against the frozen RT-001 source dictionary BEFORE looking at LLVM.
template<class FrozenSymbols>
std::optional<std::vector<SourceNumericSsaBinding>>
bindLlvmSourceNumericSlotsFromFrozenSource(
    llvm::Function& function, llvm::CallBase& checkpoint,
    const SourceResumeRequirements& required,
    const FrozenSymbols& frozen,
    std::vector<LlvmSourceNumericRegistration> registrations) {
    if (!verifyFrozenSourceResumeSlots(required, frozen))
        return std::nullopt;
    return bindLlvmSourceNumericSlots(
        function, checkpoint, required, std::move(registrations));
}

} // namespace xyo::ir
