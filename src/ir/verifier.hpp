// Validators for the currently supported typed scalar SSA and CFG schemas.
//
// These checks run in Release too. Do not use asserts to accept potentially
// malformed input. Cross-block SSA dominance cannot yet be checked: CFG
// blocks don't own typed value definitions or a def-use map at this stage.
#pragma once

#include "ssa.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xyo::ir {

enum class VerifyError : uint8_t {
    None,
    EmptyFunction,
    BadOperand,
    BadType,
    BadOpcode,
    BadCFG,
    RequiresScheduler,
    UnreachableBlock,
    MissingDefinition,
    DuplicateDefinition,
    BadPhiDefinition,
    NotDominated,
};

struct VerifyResult {
    VerifyError error = VerifyError::None;
    uint32_t index = 0;
    explicit operator bool() const { return error == VerifyError::None; }
};

inline VerifyResult verifyNumeric(const Function& function) {
    const auto& instructions = function.instructions();
    if (instructions.empty()) return {VerifyError::EmptyFunction};
    for (size_t i = 0; i < instructions.size(); ++i) {
        const auto& inst = instructions[i];
        if (inst.op == Op::Parameter) {
            if (inst.type != Type::F64)
                return {VerifyError::BadType, static_cast<uint32_t>(i)};
            continue;
        }
        if (inst.op != Op::Add && inst.op != Op::Sub &&
            inst.op != Op::Mul && inst.op != Op::Div &&
            inst.op != Op::LessThan && inst.op != Op::GreaterThan)
            return {VerifyError::BadOpcode, static_cast<uint32_t>(i)};
        if (inst.lhs >= i || inst.rhs >= i)
            return {VerifyError::BadOperand, static_cast<uint32_t>(i)};
        if (instructions[inst.lhs].type != Type::F64 ||
            instructions[inst.rhs].type != Type::F64)
            return {VerifyError::BadType, static_cast<uint32_t>(i)};
        const Type expected = inst.op == Op::LessThan ||
                                      inst.op == Op::GreaterThan
            ? Type::I1 : Type::F64;
        if (inst.type != expected)
            return {VerifyError::BadType, static_cast<uint32_t>(i)};
    }
    return {};
}

inline VerifyResult verifyControlFlow(const ControlFlow& graph) {
    if (!graph.validate()) return {VerifyError::BadCFG};
    const auto& blocks = graph.blocks();
    // A graph's entry is always block 0. Every retained block must be
    // reachable, so later LLVM/Binaryen lowering won't emit orphan PHIs.
    std::vector<uint8_t> reachable(blocks.size());
    std::vector<BlockId> queue{0};
    reachable[0] = 1;
    for (size_t offset = 0; offset < queue.size(); ++offset) {
        const auto& term = blocks[queue[offset]].terminator;
        auto push = [&](BlockId target) {
            if (!reachable[target]) {
                reachable[target] = 1;
                queue.push_back(target);
            }
        };
        if (term.kind == TerminatorKind::Jump ||
            term.kind == TerminatorKind::Branch)
            push(term.trueBlock);
        if (term.kind == TerminatorKind::Branch)
            push(term.falseBlock);
    }
    for (size_t i = 0; i < reachable.size(); ++i)
        if (!reachable[i])
            return {VerifyError::UnreachableBlock, static_cast<uint32_t>(i)};
    return {};
}

} // namespace xyo::ir

// Verify def-use relationships, including Phi values on predecessor edges.
// Unreachable blocks have already been rejected by verifyControlFlow.
namespace xyo::ir {
inline VerifyResult verifySSA(const SsaGraph& graph) {
    const auto basic = verifyControlFlow(graph.cfg);
    if (!basic) return basic;
    const auto& blocks = graph.cfg.blocks();
    const size_t n = blocks.size();
    const size_t values = graph.definitions.size();

    // Reachable dominators: entry dominates only itself, other blocks start
    // at the universal set, then converge by predecessor intersection.
    std::vector<std::vector<BlockId>> preds(n);
    for (BlockId i = 0; i < n; ++i) {
        const auto& t = blocks[i].terminator;
        if (t.kind == TerminatorKind::Jump ||
            t.kind == TerminatorKind::Branch)
            preds[t.trueBlock].push_back(i);
        if (t.kind == TerminatorKind::Branch &&
            t.falseBlock != t.trueBlock)
            preds[t.falseBlock].push_back(i);
    }
    std::vector<std::vector<uint8_t>> dom(
        n, std::vector<uint8_t>(n, 1));
    std::fill(dom[0].begin(), dom[0].end(), 0);
    dom[0][0] = 1;
    bool changed;
    do {
        changed = false;
        for (BlockId b = 1; b < n; ++b) {
            std::vector<uint8_t> next(n, 1);
            for (BlockId p : preds[b])
                for (size_t d = 0; d < n; ++d)
                    next[d] &= dom[p][d];
            next[b] = 1;
            if (next != dom[b]) {
                dom[b] = std::move(next);
                changed = true;
            }
        }
    } while (changed);

    std::vector<std::vector<uint8_t>> declared(n);
    for (size_t b = 0; b < n; ++b)
        declared[b].resize(blocks[b].phis.size());
    for (ValueId i = 0; i < values; ++i) {
        const auto& def = graph.definitions[i];
        if (def.block >= n) return {VerifyError::MissingDefinition, i};
        if (def.phi) {
            if (def.position != 0 ||
                def.phiIndex >= blocks[def.block].phis.size())
                return {VerifyError::BadPhiDefinition, i};
            if (declared[def.block][def.phiIndex]++)
                return {VerifyError::DuplicateDefinition, i};
            if (def.type != blocks[def.block].phis[def.phiIndex].type)
                return {VerifyError::BadType, i};
        } else if (def.position == 0) {
            return {VerifyError::BadOperand, i};
        }
    }
    std::vector<std::vector<uint32_t>> positions(n);
    for (size_t b = 0; b < n; ++b) {
        for (uint8_t marked : declared[b])
            if (!marked) return {VerifyError::BadPhiDefinition,
                                  static_cast<uint32_t>(b)};
        // A duplicate instruction position means two SSA definitions use
        // the same program point, which makes ordering ambiguous.
    }
    for (ValueId i = 0; i < values; ++i) {
        const auto& def = graph.definitions[i];
        if (def.phi) continue;
        auto& pos = positions[def.block];
        if (std::find(pos.begin(), pos.end(), def.position) != pos.end())
            return {VerifyError::DuplicateDefinition, i};
        pos.push_back(def.position);
    }

    auto visible = [&](ValueId id, BlockId block, uint32_t pos,
                       Type expected) -> VerifyError {
        if (id >= values) return VerifyError::MissingDefinition;
        const auto& def = graph.definitions[id];
        if (def.type != expected) return VerifyError::BadType;
        if (!dom[block][def.block]) return VerifyError::NotDominated;
        if (block == def.block && !def.phi && def.position >= pos)
            return VerifyError::NotDominated;
        return VerifyError::None;
    };

    for (size_t i = 0; i < graph.uses.size(); ++i) {
        const auto& use = graph.uses[i];
        if (use.block >= n || use.position == 0)
            return {VerifyError::BadOperand, static_cast<uint32_t>(i)};
        const auto err = visible(use.value, use.block, use.position,
                                 use.expectedType);
        if (err != VerifyError::None)
            return {err, static_cast<uint32_t>(i)};
    }
    for (BlockId b = 0; b < n; ++b) {
        for (const auto& phi : blocks[b].phis)
            for (const auto& input : phi.inputs) {
                // A Phi reads on the incoming edge, *not* in the merge
                // block. This also permits a loop-carried self-edge value.
                const auto err = visible(input.value, input.predecessor,
                                         UINT32_MAX, phi.type);
                if (err != VerifyError::None)
                    return {err, b};
            }
    }
    return {};
}
} // namespace xyo::ir
