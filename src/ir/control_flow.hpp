// Backend-neutral control-flow graph schema, independent of LLVM/Binaryen.
// This is the structural layer for a future whole-script SSA lowering.
// It deliberately does not encode Scratch deoptimization or yield semantics.
#pragma once

#include "numeric.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace xyo::ir {

using BlockId = uint32_t;

enum class TerminatorKind : uint8_t { None, Jump, Branch, Return };

struct Terminator {
    TerminatorKind kind = TerminatorKind::None;
    BlockId trueBlock = 0;
    BlockId falseBlock = 0;
    ValueId condition = 0;
    ValueId result = 0;
};

struct PhiInput {
    BlockId predecessor;
    ValueId value;
};

struct Phi {
    Type type = Type::F64;
    std::vector<PhiInput> inputs;
};

struct BasicBlock {
    std::vector<Phi> phis;
    Terminator terminator;
};

class ControlFlow {
public:
    BlockId createBlock() {
        const BlockId id = static_cast<BlockId>(blocks_.size());
        blocks_.emplace_back();
        return id;
    }

    BasicBlock& block(BlockId id) { return blocks_[id]; }
    const BasicBlock& block(BlockId id) const { return blocks_[id]; }

    const std::vector<BasicBlock>& blocks() const { return blocks_; }

    // Checks CFG shape and Phi predecessor edges. A separate SSA verifier
    // will be responsible for dominance, data types and side effects.
    bool validate() const {
        if (blocks_.empty()) return false;
        std::vector<std::vector<BlockId>> predecessors(blocks_.size());
        for (BlockId from = 0; from < blocks_.size(); ++from) {
            const auto& terminator = blocks_[from].terminator;
            if (terminator.kind == TerminatorKind::None) return false;
            if (terminator.kind == TerminatorKind::Jump ||
                terminator.kind == TerminatorKind::Branch) {
                if (terminator.trueBlock >= blocks_.size()) return false;
                predecessors[terminator.trueBlock].push_back(from);
            }
            if (terminator.kind == TerminatorKind::Branch) {
                if (terminator.falseBlock >= blocks_.size()) return false;
                if (terminator.falseBlock != terminator.trueBlock)
                    predecessors[terminator.falseBlock].push_back(from);
            }
        }
        for (BlockId id = 0; id < blocks_.size(); ++id) {
            auto& incoming = predecessors[id];
            std::sort(incoming.begin(), incoming.end());
            for (const auto& phi : blocks_[id].phis) {
                if (phi.inputs.size() != incoming.size()) return false;
                std::vector<BlockId> supplied;
                supplied.reserve(phi.inputs.size());
                for (auto edge : phi.inputs) supplied.push_back(edge.predecessor);
                std::sort(supplied.begin(), supplied.end());
                if (supplied != incoming) return false;
            }
        }
        return true;
    }
private:
    std::vector<BasicBlock> blocks_;
};

} // namespace xyo::ir
