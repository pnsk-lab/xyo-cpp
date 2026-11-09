// Forward, conservative scalar-storage-type dataflow for Scratch CFGs.
#pragma once
#include "control_flow.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace xyo { class Target; }
namespace xyo::analysis {

// A missing entry means ANY possible runtime kind; never interpret a missing
// value as the empty set or as a proven numeric value.
using TypeState = std::unordered_map<std::string, uint32_t>;

struct TypeFlowResult {
    FlowGraph cfg;
    std::vector<TypeState> before;
    std::vector<TypeState> after;
    std::vector<uint8_t> reachable;

    uint32_t kindBefore(size_t node, const std::string& variableId) const;
    uint32_t kindAfter(size_t node, const std::string& variableId) const;
};

TypeFlowResult analyzeTypeFlow(const Blocks& blocks,
                               const std::string& firstBlock,
                               size_t maxNodes = 16384,
                               Target* target = nullptr);

} // namespace xyo::analysis
