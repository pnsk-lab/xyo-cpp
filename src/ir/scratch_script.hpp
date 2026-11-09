// Lossless, backend-neutral Scratch block graph snapshot.
//
// This is the source IR for whole scripts, including operations that a given
// JIT backend cannot yet execute. An opaque opcode is preserved verbatim;
// unsupported code must fall back, not be elided. Unlike the numeric-only
// SSA subset this representation preserves Scratch's ordered inputs, shadow
// fallback, mutation, nested substacks, and continuation (next) edges.
//
// The snapshot is compiler-local: BlockField holds GC-managed Value bits,
// and this type is NOT a GC root or a persistent asynchronous compilation
// artifact. A worker-safe/long-lived form will require owned literals.
#pragma once

#include "../model/blocks.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xyo::ir {

enum class ScratchIrError : uint8_t {
    None, MissingTop, MissingBlock, DuplicateId, InvalidEdge,
    CyclicGraph, TooManyBlocks
};

struct ScratchScript {
    std::string topId;
    std::vector<Block> nodes;
    std::unordered_map<std::string, size_t> byId;
    ScratchIrError error = ScratchIrError::None;
    std::string errorBlock;
    static constexpr size_t kMaxNodes = 100000;

    const Block* getBlock(const std::string& id) const {
        auto it = byId.find(id);
        return it == byId.end() ? nullptr : &nodes[it->second];
    }

    bool valid() const { return error == ScratchIrError::None; }

    // Copy a script, including every nested reporter, shadow and substack.
    // Dependency edges are visited in Scratch's input insertion order.
    // Every Block is deeply copied (including its Mutation and ordered
    // InputSet), independent of source Blocks identity after return.
    static ScratchScript capture(const Blocks& source, const Block* top) {
        ScratchScript result;
        if (!top || top->id.empty()) {
            result.error = ScratchIrError::MissingTop;
            return result;
        }
        result.topId = top->id;
        struct Frame {
            std::string id;
            size_t edge = 0;
        };
        // Iterative DFS: deeply nested Scratch inputs cannot exhaust the
        // C++ call stack. Color=1 means currently active, =2 finished.
        std::unordered_map<std::string, uint8_t> color;
        std::vector<Frame> stack;
        stack.push_back({top->id, 0});
        auto fail = [&](ScratchIrError err, const std::string& id) {
            result.error = err;
            result.errorBlock = id;
            result.nodes.clear();
            result.byId.clear();
        };
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto* block = source.getBlock(frame.id);
            if (!block) {
                fail(ScratchIrError::MissingBlock, frame.id);
                return result;
            }
            if (color[frame.id] == 0) {
                if (result.nodes.size() >= kMaxNodes) {
                    fail(ScratchIrError::TooManyBlocks, frame.id);
                    return result;
                }
                color[frame.id] = 1;
                result.byId.emplace(frame.id, result.nodes.size());
                result.nodes.push_back(*block);
            }
            // Each ordered input contributes two edges: active reporter
            // and shadow fallback. The successor edge is visited last.
            const size_t count = block->inputs.items.size() * 2;
            if (frame.edge > count) {
                color[frame.id] = 2;
                stack.pop_back();
                continue;
            }
            std::string child;
            if (frame.edge < count) {
                const size_t edge = frame.edge++;
                const auto& input = block->inputs.items[edge / 2].second;
                child = edge % 2 == 0 ? input.block : input.shadow;
            } else {
                ++frame.edge;
                child = block->next;
            }
            if (child.empty()) continue;
            if (color[child] == 1) {
                fail(ScratchIrError::CyclicGraph, child);
                return result;
            }
            if (color[child] == 0) stack.push_back({std::move(child), 0});
        }
        return result;
    }
};

// A whole-project snapshot also preserves orphaned scripts, procedure
// definitions/prototypes and extension blocks, even if none is reachable
// from a selected event hat. This is an immutable *structural* snapshot,
// not yet a lowerable/effectful SSA function.
struct ScratchProject {
    std::vector<Block> nodes;
    std::vector<std::string> scriptIds;
    std::unordered_map<std::string, size_t> byId;
    ScratchIrError error = ScratchIrError::None;

    const Block* getBlock(const std::string& id) const {
        auto it = byId.find(id);
        return it == byId.end() ? nullptr : &nodes[it->second];
    }
    static ScratchProject capture(Blocks& source) {
        ScratchProject result;
        result.scriptIds = source.getScripts();
        const auto& all = source.allBlocks();
        if (all.size() > ScratchScript::kMaxNodes) {
            result.error = ScratchIrError::TooManyBlocks;
            return result;
        }
        std::vector<const Block*> sorted;
        sorted.reserve(all.size());
        for (const auto& [id, block] : all) sorted.push_back(&block);
        std::sort(sorted.begin(), sorted.end(), [](const Block* a,
                                                   const Block* b) {
            return a->seq == b->seq ? a->id < b->id : a->seq < b->seq;
        });
        result.nodes.reserve(sorted.size());
        result.byId.reserve(sorted.size());
        for (const Block* block : sorted) {
            if (block->id.empty() ||
                !result.byId.emplace(block->id, result.nodes.size()).second) {
                result.error = ScratchIrError::DuplicateId;
                return result;
            }
            result.nodes.push_back(*block);
        }
        for (const auto& id : result.scriptIds) {
            if (!result.getBlock(id)) {
                result.error = ScratchIrError::MissingTop;
                return result;
            }
        }
        return result;
    }
};

} // namespace xyo::ir
