// Executable, typed subset of the portable CFG IR.
// Integer CFG plus numeric Scratch Value operations (f64, raw NaN-boxed
// scalar slots, canonical NaN, and transactional external slot binding).
// External slots are caller-owned snapshots, NEVER live GC pointers. A
// host-side guard and commit is required before/after executing such a CFG.
#pragma once

#include "verifier.hpp"
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace xyo::ir {

inline constexpr ValueId kNoValue = UINT32_MAX;
inline constexpr uint64_t kMaxScheduledNumericRepeat = 1000000;

enum class ExecOp : uint8_t {
    ConstI64, AddI64, SubI64, LessI64,
    LoadVar, StoreVar, ChangeVar,
    ConstF64, AddF64, SubF64, MulF64, DivF64, AbsF64, FloorF64, CeilF64, SqrtF64, RoundF64,
    LessF64, GreaterF64, EqualF64,
    AndI1, OrI1, NotI1,
    LoadNumber, StoreNumber, ChangeNumber, EncodeNumber,
    RepeatCount
};
struct ExecInstruction {
    ValueId result;
    ExecOp op;
    ValueId lhs = 0;
    ValueId rhs = 0;
    int64_t literal = 0;
    uint32_t slot = 0; // Valid only for slot access operations
};

struct LoopYieldBoundary {
    BlockId latch = 0;
    BlockId header = 0;
    // A stable Scratch block ID for deopt/resume; no Block* lifetime leak.
    std::string scratchBlockId;
    // Metadata used to reconstruct a top-level repeat after deopt.
    // counterPhi == kNoValue disables interpreter reconstruction.
    ValueId counterPhi = kNoValue;
    uint64_t repeatCount = 0;
    // Non-kNoValue means repeatCount is dynamic. limitValue contains the
    // rounded trip count, evaluated once at loop entry and saved in frame.
    ValueId limitValue = kNoValue;
};

struct Executable {
    SsaGraph ssa;
    std::vector<std::vector<ExecInstruction>> instructions;
    // Slots are 64-bit cells. By default these are zero-initialized
    // invocation locals; externalValueSlots uses raw host-staged Values.
    uint32_t variableSlots = 0;
    // When true, main takes an LLVM ptr / WASM i32 parameter pointing
    // to caller-staged 64-bit Scratch Values. No direct Variable* access.
    bool externalValueSlots = false;
    // Resumable ABI: generated run(slots*, frame*) returns 0 on completion,
    // 1 at a scheduler checkpoint. frame = {pc, result, SSA values...}.
    // The frame and slots are both staged and published atomically by host.
    bool resumable = false;
    // A scheduler must be consulted at EVERY listed loop backedge before
    // executing the next iteration. Non-resumable backends reject this
    // metadata by default; resumable backends publish a continuation frame.
    std::vector<LoopYieldBoundary> yieldBoundaries;
};

inline VerifyResult verifyExecutable(const Executable& program) {
    const auto& graph = program.ssa.cfg;
    if (program.instructions.size() != graph.blocks().size())
        return {VerifyError::BadCFG};
    SsaGraph ssa = program.ssa;
    std::vector<uint8_t> emitted(ssa.definitions.size());
    for (BlockId b = 0; b < program.instructions.size(); ++b) {
        for (size_t i = 0; i < program.instructions[b].size(); ++i) {
            const auto& inst = program.instructions[b][i];
            const bool store = inst.op == ExecOp::StoreVar ||
                               inst.op == ExecOp::StoreNumber;
            const bool memoryOp = inst.op == ExecOp::LoadVar ||
                inst.op == ExecOp::StoreVar || inst.op == ExecOp::ChangeVar ||
                inst.op == ExecOp::LoadNumber ||
                inst.op == ExecOp::StoreNumber ||
                inst.op == ExecOp::ChangeNumber;
            const bool binaryF64 = inst.op == ExecOp::AddF64 ||
                inst.op == ExecOp::SubF64 || inst.op == ExecOp::MulF64 ||
                inst.op == ExecOp::DivF64;
            const bool unaryF64 = inst.op == ExecOp::AbsF64 ||
                inst.op == ExecOp::RoundF64 ||
                inst.op == ExecOp::FloorF64 || inst.op == ExecOp::CeilF64 ||
                inst.op == ExecOp::SqrtF64;
            const bool compareF64 = inst.op == ExecOp::LessF64 ||
                inst.op == ExecOp::GreaterF64 ||
                inst.op == ExecOp::EqualF64;
            const bool binaryBool = inst.op == ExecOp::AndI1 ||
                inst.op == ExecOp::OrI1;
            const bool unaryBool = inst.op == ExecOp::NotI1;
            const bool binaryI64 = inst.op == ExecOp::AddI64 ||
                inst.op == ExecOp::SubI64 || inst.op == ExecOp::LessI64;
            const bool constOp = inst.op == ExecOp::ConstI64 ||
                                 inst.op == ExecOp::ConstF64;
            const bool loadOp = inst.op == ExecOp::LoadVar ||
                                inst.op == ExecOp::LoadNumber;
            const bool encode = inst.op == ExecOp::EncodeNumber;
            const bool repeatCount = inst.op == ExecOp::RepeatCount;
            // RepeatCount.literal is its checked, execution-mode-specific
            // trip cap. Never let a backend silently choose a larger cap.
            if (repeatCount && (inst.literal <= 0 ||
                uint64_t(inst.literal) > kMaxScheduledNumericRepeat ||
                (!program.resumable && inst.literal > 1024)))
                return {VerifyError::BadCFG};
            if (!memoryOp && !binaryF64 && !binaryI64 &&
                !compareF64 && !unaryF64 && !binaryBool && !unaryBool &&
                !constOp && !encode && !repeatCount)
                return {VerifyError::BadOpcode, static_cast<uint32_t>(i)};
            if (memoryOp && inst.slot >= program.variableSlots)
                return {VerifyError::BadOperand, static_cast<uint32_t>(i)};
            if (store) {
                if (inst.result != kNoValue)
                    return {VerifyError::BadOperand, static_cast<uint32_t>(i)};
                const Type srcType = inst.op == ExecOp::StoreNumber
                    ? Type::F64 : Type::I64;
                ssa.uses.push_back(
                    {b, inst.lhs, srcType, uint32_t(i + 1)});
                continue;
            }
            if (inst.result >= emitted.size())
                return {VerifyError::MissingDefinition, inst.result};
            if (emitted[inst.result]++)
                return {VerifyError::DuplicateDefinition, inst.result};
            const auto& def = ssa.definitions[inst.result];
            if (def.phi || def.block != b || def.position != i + 1)
                return {VerifyError::BadOperand, inst.result};
            const Type resultType =
                (inst.op == ExecOp::LessI64 || compareF64 ||
                 binaryBool || unaryBool)
                ? Type::I1 : (binaryF64 || unaryF64 || inst.op == ExecOp::ConstF64 ||
                    inst.op == ExecOp::LoadNumber ||
                    inst.op == ExecOp::ChangeNumber
                    ? Type::F64 : Type::I64);
            if (def.type != resultType)
                return {VerifyError::BadType, inst.result};
            if (inst.op == ExecOp::ChangeVar ||
                inst.op == ExecOp::ChangeNumber || unaryF64 || unaryBool ||
                encode || repeatCount) {
                ssa.uses.push_back({b, inst.lhs,
                    inst.op == ExecOp::ChangeVar ? Type::I64 :
                    unaryBool ? Type::I1 : Type::F64,
                    uint32_t(i + 1)});
            } else if (!constOp && !loadOp) {
                const Type operand =
                    binaryBool ? Type::I1 :
                    (binaryF64 || compareF64) ? Type::F64 : Type::I64;
                ssa.uses.push_back({b, inst.lhs, operand,
                                    uint32_t(i + 1)});
                ssa.uses.push_back({b, inst.rhs, operand,
                                    uint32_t(i + 1)});
            }
        }
        const auto& term = graph.block(b).terminator;
        if (term.kind == TerminatorKind::Branch)
            ssa.uses.push_back({b, term.condition, Type::I1, UINT32_MAX});
        if (term.kind == TerminatorKind::Return)
            ssa.uses.push_back({b, term.result, Type::I64, UINT32_MAX});
    }
    if (program.resumable && !program.externalValueSlots)
        return {VerifyError::BadCFG};
    for (const auto& yield : program.yieldBoundaries) {
        if (yield.latch >= graph.blocks().size() ||
            yield.header >= graph.blocks().size() ||
            yield.scratchBlockId.empty())
            return {VerifyError::BadCFG};
        const auto& term = graph.block(yield.latch).terminator;
        if (term.kind != TerminatorKind::Jump ||
            term.trueBlock != yield.header)
            return {VerifyError::BadCFG};
        if (yield.counterPhi != kNoValue &&
            (yield.counterPhi >= program.ssa.definitions.size() ||
             !program.ssa.definitions[yield.counterPhi].phi ||
             program.ssa.definitions[yield.counterPhi].block != yield.header ||
             program.ssa.definitions[yield.counterPhi].type != Type::I64))
            return {VerifyError::BadCFG};
        if (yield.limitValue != kNoValue &&
            (yield.limitValue >= program.ssa.definitions.size() ||
             program.ssa.definitions[yield.limitValue].type != Type::I64 ||
             program.ssa.definitions[yield.limitValue].phi))
            return {VerifyError::BadCFG};
    }
    for (ValueId id = 0; id < emitted.size(); ++id)
        if (!ssa.definitions[id].phi && !emitted[id])
            return {VerifyError::MissingDefinition, id};
    return verifySSA(ssa);
}

// The two backends implement the same logical instruction/CFG contract.
// Phi nodes are declared before instructions; incoming values are linked
// only after all blocks are lowered (necessary for cyclic loop-carried Phi).
template <typename Backend>
VerifyResult lowerExecutable(
    const Executable& program, Backend& backend,
    bool allowUnscheduledLoopsForTests = false) {
    const auto validation = verifyExecutable(program);
    if (!validation) return validation;
    // This is a test-only escape hatch. A production JIT must implement
    // checkpoints, transactional commit and exact deopt continuation.
    if (!allowUnscheduledLoopsForTests && !program.resumable &&
        !program.yieldBoundaries.empty())
        return {VerifyError::RequiresScheduler};
    const auto& cfg = program.ssa.cfg;
    backend.start(program);
    for (BlockId b = 0; b < cfg.blocks().size(); ++b)
        for (uint32_t i = 0; i < cfg.block(b).phis.size(); ++i)
            backend.declarePhi(b, i, cfg.block(b).phis[i]);
    for (BlockId b = 0; b < cfg.blocks().size(); ++b) {
        backend.beginBlock(b);
        for (const auto& instruction : program.instructions[b])
            backend.emit(instruction);
        backend.terminate(b, cfg.block(b).terminator);
    }
    for (BlockId b = 0; b < cfg.blocks().size(); ++b)
        for (uint32_t i = 0; i < cfg.block(b).phis.size(); ++i)
            backend.wirePhi(b, i, cfg.block(b).phis[i]);
    backend.finish();
    return {};
}

} // namespace xyo::ir
