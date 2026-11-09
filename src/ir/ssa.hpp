// Block-scoped SSA metadata used by the verifier before LLVM/WASM lowering.
// No backend dependencies. Value IDs refer to definitions in this table, not
// to the scalar expression-local IDs in numeric.hpp.
#pragma once

#include "control_flow.hpp"

#include <cstdint>
#include <vector>

namespace xyo::ir {

struct SsaDefinition {
    BlockId block = 0;
    Type type = Type::F64;
    // Phi values are available at block entry. Non-Phi definitions are
    // ordered by position (1,2,...); a use at position p sees defs < p.
    uint32_t position = 0;
    bool phi = false;
    uint32_t phiIndex = 0;
};

struct SsaUse {
    BlockId block = 0;
    ValueId value = 0;
    Type expectedType = Type::F64;
    uint32_t position = 0;
};

struct SsaGraph {
    ControlFlow cfg;
    std::vector<SsaDefinition> definitions;
    std::vector<SsaUse> uses;
};

} // namespace xyo::ir
