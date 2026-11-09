#include "control_flow.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include "../model/blocks.hpp"

namespace xyo::analysis {
namespace {

bool isLoop(std::string_view op) {
    return op == "control_repeat" || op == "control_repeat_until" ||
           op == "control_while" || op == "control_forever" ||
           op == "control_for_each";
}

struct Builder {
    const Blocks& blocks;
    FlowGraph graph;
    size_t maxNodes;

    size_t add(FlowNodeKind kind, const Block* block = nullptr) {
        const size_t index = graph.nodes.size();
        FlowNode node;
        node.kind = kind;
        if (block) {
            node.blockId = block->id;
            node.opcode = block->opcode;
            graph.blockNodes[block->id].push_back(index);
        }
        graph.nodes.push_back(std::move(node));
        return index;
    }

    void edge(size_t from, size_t to) {
        if (std::find(graph.nodes[from].successors.begin(),
                      graph.nodes[from].successors.end(), to) !=
            graph.nodes[from].successors.end()) return;
        graph.nodes[from].successors.push_back(to);
        graph.nodes[to].predecessors.push_back(from);
    }

    size_t barrier(size_t next) {
        graph.truncated = true;
        const size_t node = add(FlowNodeKind::Barrier);
        edge(node, next);
        return node;
    }

    size_t chain(const std::string& first, size_t successor, unsigned nesting) {
        if (first.empty()) return successor;
        if (nesting > 128 || graph.nodes.size() >= maxNodes)
            return barrier(successor);

        // Use an iterative traversal for linear stacks: a large .sb3 may
        // contain thousands of consecutive blocks without deep nesting.
        std::vector<const Block*> linear;
        std::unordered_set<std::string> seen;
        std::string id = first;
        while (!id.empty()) {
            if (linear.size() + graph.nodes.size() >= maxNodes) {
                graph.truncated = true;
                break;
            }
            if (!seen.insert(id).second) {
                graph.truncated = true;
                break;
            }
            const Block* block = blocks.getBlock(id);
            if (!block) {
                graph.truncated = true;
                break;
            }
            linear.push_back(block);
            id = block->next;
        }
        // If malformed or truncated, do not falsely claim knowledge of the
        // omitted instructions: the barrier invalidates all scalar facts.
        if (!id.empty()) successor = barrier(successor);

        for (auto it = linear.rbegin(); it != linear.rend(); ++it) {
            const Block& block = **it;
            if (graph.nodes.size() >= maxNodes) {
                successor = barrier(successor);
                continue;
            }
            size_t current = add(FlowNodeKind::Statement, &block);
            if (block.opcode == "control_if" ||
                block.opcode == "control_if_else") {
                const BlockInput* then = block.inputs.find("SUBSTACK");
                const BlockInput* other = block.inputs.find("SUBSTACK2");
                const size_t yes = then ? chain(then->block, successor, nesting + 1)
                                        : successor;
                const size_t no = block.opcode == "control_if_else" && other
                    ? chain(other->block, successor, nesting + 1)
                    : successor;
                edge(current, yes);
                edge(current, no);
            } else if (isLoop(block.opcode)) {
                // The interpreter may yield at every iteration, including
                // non-warp loops and scheduler deadlines in warp loops.
                // Treat every backedge as an external-mutation boundary.
                const size_t latch = add(FlowNodeKind::LoopLatch);
                edge(latch, current);
                const BlockInput* body = block.inputs.find("SUBSTACK");
                const size_t bodyEntry = body
                    ? chain(body->block, latch, nesting + 1) : latch;
                edge(current, bodyEntry);
                // Include zero-iteration exit as a safe over-approximation.
                edge(current, successor);
            } else {
                edge(current, successor);
            }
            successor = current;
        }
        return successor;
    }
};

void computeDominators(FlowGraph& graph) {
    const size_t n = graph.nodes.size();
    std::vector<uint8_t> visited(n);
    std::vector<size_t> postorder;
    std::vector<std::pair<size_t, size_t>> dfs;
    dfs.emplace_back(graph.entry, 0);
    visited[graph.entry] = 1;
    while (!dfs.empty()) {
        auto& [node, offset] = dfs.back();
        if (offset < graph.nodes[node].successors.size()) {
            const size_t next = graph.nodes[node].successors[offset++];
            if (!visited[next]) {
                visited[next] = 1;
                dfs.emplace_back(next, 0);
            }
        } else {
            postorder.push_back(node);
            dfs.pop_back();
        }
    }

    std::reverse(postorder.begin(), postorder.end());
    std::vector<int> rank(n, -1);
    for (size_t i = 0; i < postorder.size(); ++i)
        rank[postorder[i]] = static_cast<int>(i);

    auto& idom = graph.immediateDominators;
    idom.assign(n, -1);
    idom[graph.entry] = static_cast<int>(graph.entry);
    auto intersect = [&](size_t a, size_t b) {
        while (a != b) {
            while (rank[a] > rank[b]) a = static_cast<size_t>(idom[a]);
            while (rank[b] > rank[a]) b = static_cast<size_t>(idom[b]);
        }
        return a;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 1; i < postorder.size(); ++i) {
            const size_t node = postorder[i];
            int newIdom = -1;
            for (size_t pred : graph.nodes[node].predecessors) {
                if (idom[pred] < 0) continue;
                newIdom = newIdom < 0
                    ? static_cast<int>(pred)
                    : static_cast<int>(intersect(
                          static_cast<size_t>(newIdom), pred));
            }
            if (newIdom >= 0 && idom[node] != newIdom) {
                idom[node] = newIdom;
                changed = true;
            }
        }
    }
}

} // namespace

bool FlowGraph::dominates(size_t dominator, size_t node) const {
    if (dominator >= nodes.size() || node >= nodes.size() ||
        immediateDominators.size() != nodes.size() ||
        immediateDominators[dominator] < 0 ||
        immediateDominators[node] < 0) return false;
    // The entry's idom points to itself.
    for (size_t steps = 0; steps < nodes.size(); ++steps) {
        if (node == dominator) return true;
        int parent = immediateDominators[node];
        if (parent < 0 || parent == static_cast<int>(node)) return false;
        node = static_cast<size_t>(parent);
    }
    return false;
}

FlowGraph buildFlowGraph(const Blocks& blocks, const std::string& firstBlock,
                         size_t maxNodes) {
    Builder builder{blocks, {}, std::max<size_t>(maxNodes, 2)};
    builder.graph.entry = builder.add(FlowNodeKind::Entry);
    builder.graph.exit = builder.add(FlowNodeKind::Exit);
    const size_t start = builder.chain(firstBlock, builder.graph.exit, 0);
    builder.edge(builder.graph.entry, start);
    computeDominators(builder.graph);
    return std::move(builder.graph);
}

} // namespace xyo::analysis
