// Deterministic content fingerprint of the source graph captured for a JIT
// compilation. Runtime deopt requires BOTH the Blocks revision and the
// original source content: a direct Block edit may omit resetCache().
//
// The dual independent 64-bit accumulators avoid accidental collisions in
// ordinary editor state. This is NOT a cryptographic signature and must
// not be treated as authentication of untrusted external source files.
#pragma once

#include "scratch_pipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace xyo::ir {

struct ScratchSourceDigest {
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool operator==(const ScratchSourceDigest&) const = default;
    explicit operator bool() const { return lo != 0 && hi != 0; }
};

class ScratchSourceHasher {
public:
    void word(uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            byte(uint8_t(value >> shift));
    }

    void string(std::string_view value) {
        word(value.size());
        for (unsigned char c : value) byte(c);
    }

    ScratchSourceDigest finish() const { return {a_, b_}; }

private:
    void byte(uint8_t b) {
        a_ = (a_ ^ b) * 1099511628211ULL;
        b_ = (b_ ^ uint8_t(b + 0x9D)) * 14029467366897019727ULL;
        b_ ^= b_ >> 29;
    }

    uint64_t a_ = 14695981039346656037ULL;
    uint64_t b_ = 7809847782465536322ULL;
};

inline ScratchSourceDigest scratchSourceDigest(
    const ScratchCompilationUnit& unit) {
    if (!unit.valid()) return {};
    ScratchSourceHasher h;
    h.string("xyo-scratch-source-v1");
    h.string(unit.script.topId);

    // Captures append reachable procedure definitions in resolution order.
    // Hash in sorted block-ID order to avoid DFS traversal/map order drift.
    std::vector<const Block*> nodes;
    nodes.reserve(unit.script.nodes.size());
    for (const Block& block : unit.script.nodes) nodes.push_back(&block);
    std::sort(nodes.begin(), nodes.end(),
        [](const Block* a, const Block* b) { return a->id < b->id; });
    h.word(nodes.size());
    for (const Block* b : nodes) {
        h.string(b->id);
        h.string(b->opcode);
        h.string(b->next);
        h.string(b->parent);
        h.string(b->targetId);
        h.word(b->seq); // procedure definition resolution order
        h.word(b->topLevel);
        h.word(b->shadow);
        h.word(b->inputs.items.size());
        for (const auto& [name, input] : b->inputs.items) {
            h.string(name);
            h.string(input.block);
            h.string(input.shadow);
        }
        h.word(b->fields.size());
        for (const auto& [name, field] : b->fields) {
            h.string(name);
            h.string(field.id);
            // Source field Values are copied from the live Blocks. In
            // particular string literals keep their original tagged bits;
            // the fingerprint is computed immediately, not kept as a root.
            h.word(field.value.bits);
        }
        h.word(bool(b->mutation));
        if (b->mutation) {
            h.string(b->mutation->tagName);
            h.word(b->mutation->attrs.size());
            for (const auto& [name, value] : b->mutation->attrs) {
                h.string(name);
                h.string(value);
            }
            // Deliberately omit Mutation::parsed/argumentIds: those
            // are mutable caches derived from attrs, not source content.
        }
    }

    std::vector<std::pair<std::string_view, std::string_view>> definitions;
    definitions.reserve(unit.procedureDefinitions.size());
    for (const auto& [code, id] : unit.procedureDefinitions)
        definitions.emplace_back(code, id);
    std::sort(definitions.begin(), definitions.end());
    h.word(definitions.size());
    for (const auto& [code, id] : definitions) {
        h.string(code);
        h.string(id);
    }
    return h.finish();
}

} // namespace xyo::ir
