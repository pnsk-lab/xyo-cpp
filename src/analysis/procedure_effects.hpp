// Conservative interprocedural Scratch procedure effect analysis.
#pragma once
#include <cstddef>
#include <string>
#include <unordered_set>

namespace xyo {
class Blocks;
class Target;
}

namespace xyo::analysis {

struct ProcedureEffects {
    std::unordered_set<std::string> readScalarIds;
    std::unordered_set<std::string> writtenScalarIds;
    bool unknownScalarWrites = false;
    bool unknownScalarReads = false;
    bool mayMutateLists = false;
    bool mayLaunchScripts = false;
    bool bodyMayYield = false;
    // A procedure call can yield on the warp deadline even when its body
    // contains no yielding block. This flag is intentionally conservative.
    bool callMayYield = true;
    bool recursive = false;
    bool truncated = false;
    size_t visitedBlocks = 0;
};

// Uses the same procedure resolution as the interpreter. The result is a
// summary of possible effects, NOT permission to skip a runtime type guard.
ProcedureEffects summarizeProcedureEffects(
    Blocks& blocks, Target* target, const std::string& proccode,
    size_t maxBlocks = 8192);

} // namespace xyo::analysis
