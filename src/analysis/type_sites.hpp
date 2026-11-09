// Static candidates for changes in Scratch variable storage representation.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace xyo { struct Runtime; class Blocks; struct BlockInput; }
namespace xyo::analysis {
struct TypeWriteSite {
    std::string target, script, variable, blockId, opcode;
    uint32_t kinds = 0;
    bool inLoop = false;
    bool unresolved = false;
};
struct TypeSiteReport {
    std::vector<TypeWriteSite> sites;
    size_t polymorphicVariables = 0;
    size_t unknownSites = 0;
};
// Over-approximation of the Scratch storage kinds produced by an input.
// Unknown input/expression returns all bits; this is never a proof of numeric type.
uint32_t inferInputStorageKinds(const Blocks& blocks, const BlockInput* input);
TypeSiteReport analyzeTypeSites(Runtime& rt);
std::string formatTypeSites(const TypeSiteReport& report);
} // namespace xyo::analysis
