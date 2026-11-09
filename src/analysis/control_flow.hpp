// Control-flow graph for conservative Scratch scalar type analysis.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace xyo { class Blocks; }
namespace xyo::analysis {

enum class FlowNodeKind : uint8_t { Entry, Statement, LoopLatch, Exit, Barrier };

struct FlowNode {
    FlowNodeKind kind = FlowNodeKind::Statement;
    std::string blockId;
    std::string opcode;
    std::vector<size_t> successors;
    std::vector<size_t> predecessors;
};

struct FlowGraph {
    std::vector<FlowNode> nodes;
    size_t entry = 0;
    size_t exit = 1;
    bool truncated = false;
    // -1 = unreachable, entry dominates itself.
    std::vector<int> immediateDominators;
    std::unordered_map<std::string, std::vector<size_t>> blockNodes;

    bool dominates(size_t dominator, size_t node) const;
};

FlowGraph buildFlowGraph(const Blocks& blocks, const std::string& firstBlock,
                         size_t maxNodes = 16384);

} // namespace xyo::analysis
