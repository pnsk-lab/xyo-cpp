// Backend-neutral CFG cost hints. These affect representation, never legality.
#pragma once
#include "numeric_region.hpp"

namespace xyo::ir::region {
inline bool hasNestedConditional(const Graph& graph) {
    const auto order = topologicalOrder(graph);
    if (order.empty()) return true; // Invalid input must not drive narrowing.
    const size_t n = graph.ssa.cfg.blocks().size();
    std::vector<std::vector<uint8_t>> post(n, std::vector<uint8_t>(n, 1));
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const auto id = *it;
        const auto& term = graph.ssa.cfg.block(id).terminator;
        std::fill(post[id].begin(), post[id].end(), 0);
        if (term.kind == TerminatorKind::Jump || term.kind == TerminatorKind::Branch)
            post[id] = post[term.trueBlock];
        if (term.kind == TerminatorKind::Branch)
            for (size_t b = 0; b < n; ++b) post[id][b] &= post[term.falseBlock][b];
        post[id][id] = 1;
    }
    for (auto from : order) {
        if (graph.ssa.cfg.block(from).terminator.kind != TerminatorKind::Branch)
            continue;
        std::vector<uint8_t> seen(n, 0);
        std::vector<ir::BlockId> pending{from};
        seen[from] = 1;
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto id = pending[i];
            if (id != from && !post[from][id] &&
                graph.ssa.cfg.block(id).terminator.kind == TerminatorKind::Branch)
                return true;
            const auto& term = graph.ssa.cfg.block(id).terminator;
            const auto add = [&](ir::BlockId next) {
                if (!seen[next]) { seen[next] = 1; pending.push_back(next); }
            };
            if (term.kind == TerminatorKind::Jump || term.kind == TerminatorKind::Branch)
                add(term.trueBlock);
            if (term.kind == TerminatorKind::Branch) add(term.falseBlock);
        }
    }
    return false;
}
} // namespace xyo::ir::region
