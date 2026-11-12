// Backend-neutral acyclic numerical CFG and optional exact-integer proof.
#pragma once
#include "verifier.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace xyo::ir::region {
enum class Op { Parameter, Constant, Add, Subtract, Multiply, Divide, Modulo,
                Less, Greater, Equal, And, Or, Not, ListRead };
struct Instruction {
    ValueId result;
    Op op;
    ValueId lhs = 0, rhs = 0;
    double literal = 0;
    uint32_t slot = 0;
};
struct Graph {
    SsaGraph ssa;
    std::vector<std::vector<Instruction>> instructions;
    std::vector<ValueId> inputs, outputs;
    std::vector<bool> written;
    BlockId entry = 0, exit = 0;
    // Readonly list bindings are independent of scalar Parameter slots.
    uint32_t listSlots = 0;
    // Guarded-domain annotation, not a claim about storage contents. When set,
    // the backend must reject each nonfinite projected element before its uses.
    bool listElementsFinite = false;
};
namespace detail {
// Requires structurally validated target IDs.
inline std::vector<BlockId> order(const Graph& g) {
    const auto& blocks = g.ssa.cfg.blocks();
    std::vector<size_t> degree(blocks.size());
    for (const auto& b : blocks) {
        const auto& t = b.terminator;
        if (t.kind == TerminatorKind::Jump || t.kind == TerminatorKind::Branch)
            ++degree[t.trueBlock];
        if (t.kind == TerminatorKind::Branch && t.falseBlock != t.trueBlock)
            ++degree[t.falseBlock];
    }
    std::vector<BlockId> result;
    for (BlockId b = 0; b < blocks.size(); ++b)
        if (!degree[b]) result.push_back(b);
    for (size_t i = 0; i < result.size(); ++i) {
        const auto& t = blocks[result[i]].terminator;
        auto visit = [&](BlockId b) { if (--degree[b] == 0) result.push_back(b); };
        if (t.kind == TerminatorKind::Jump || t.kind == TerminatorKind::Branch)
            visit(t.trueBlock);
        if (t.kind == TerminatorKind::Branch && t.falseBlock != t.trueBlock)
            visit(t.falseBlock);
    }
    if (result.size() != blocks.size()) result.clear();
    return result;
}
} // namespace detail

inline bool verify(const Graph& g) {
    const auto& blocks = g.ssa.cfg.blocks();
    const auto& defs = g.ssa.definitions;
    if (g.entry != 0 || g.exit >= blocks.size() ||
        g.instructions.size() != blocks.size() ||
        g.inputs.size() != g.outputs.size() || g.inputs.size() != g.written.size())
        return false;
    if (!verifyControlFlow(g.ssa.cfg) || detail::order(g).empty()) return false;
    // Derive actual uses: caller-supplied use metadata need not be complete.
    SsaGraph checked = g.ssa;
    std::vector<bool> defined(defs.size()), parameters(g.inputs.size());
    for (ValueId v = 0; v < defs.size(); ++v) {
        if (defs[v].type != Type::F64 && defs[v].type != Type::I1) return false;
        if (defs[v].phi) defined[v] = true;
    }
    for (BlockId b = 0; b < blocks.size(); ++b) {
        // In an acyclic entry block an empty Phi cannot define a value.
        for (const auto& phi : blocks[b].phis)
            if (phi.inputs.empty()) return false;
        const auto& instructions = g.instructions[b];
        if (instructions.size() >= UINT32_MAX - 1) return false;
        for (size_t i = 0; i < instructions.size(); ++i) {
            const auto& inst = instructions[i];
            if (inst.result >= defs.size() || defined[inst.result]) return false;
            defined[inst.result] = true;
            const auto& def = defs[inst.result];
            const uint32_t pos = static_cast<uint32_t>(i + 1);
            if (def.phi || def.block != b || def.position != pos) return false;
            Type type = Type::F64;
            auto use = [&](ValueId v, Type t) { checked.uses.push_back({b,v,t,pos}); };
            switch (inst.op) {
            case Op::Parameter:
                if (b != g.entry || inst.slot >= g.inputs.size() ||
                    g.inputs[inst.slot] != inst.result || parameters[inst.slot]) return false;
                parameters[inst.slot] = true;
                break;
            case Op::Constant: break;
            case Op::ListRead:
                if (inst.slot >= g.listSlots) return false;
                use(inst.lhs, Type::F64); // rhs is unused and need not name a value.
                break;
            case Op::Add: case Op::Subtract: case Op::Multiply:
            case Op::Divide: case Op::Modulo:
                use(inst.lhs, Type::F64); use(inst.rhs, Type::F64); break;
            case Op::Less: case Op::Greater: case Op::Equal:
                type = Type::I1;
                use(inst.lhs, Type::F64); use(inst.rhs, Type::F64); break;
            case Op::And: case Op::Or:
                type = Type::I1;
                use(inst.lhs, Type::I1); use(inst.rhs, Type::I1); break;
            case Op::Not:
                type = Type::I1; use(inst.lhs, Type::I1); break;
            default: return false;
            }
            if (def.type != type) return false;
        }
        const auto& t = blocks[b].terminator;
        const auto end = static_cast<uint32_t>(instructions.size() + 1);
        switch (t.kind) {
        case TerminatorKind::Jump: break;
        case TerminatorKind::Branch:
            checked.uses.push_back({b,t.condition,Type::I1,end}); break;
        case TerminatorKind::Return:
            if (b != g.exit) return false;
            checked.uses.push_back({b,t.result,Type::F64,end}); break;
        default: return false;
        }
    }
    if (blocks[g.exit].terminator.kind != TerminatorKind::Return) return false;
    for (bool found : defined) if (!found) return false;
    for (bool found : parameters) if (!found) return false;
    const auto end = static_cast<uint32_t>(g.instructions[g.exit].size() + 1);
    for (size_t slot = 0; slot < g.outputs.size(); ++slot) {
        if (!g.written[slot] && g.outputs[slot] != g.inputs[slot]) return false;
        checked.uses.push_back({g.exit,g.outputs[slot],Type::F64,end});
    }
    return static_cast<bool>(verifySSA(checked));
}
// Empty for any invalid graph, including unreachable blocks and cycles.
inline std::vector<BlockId> topologicalOrder(const Graph& g) {
    return verify(g) ? detail::order(g) : std::vector<BlockId>{};
}
struct Facts {
    bool finite = false, integral = false;
    long double lo = 0, hi = 0;
};
struct Analysis {
    bool integerSafe = false;
    std::vector<Facts> values;
    unsigned iterations = 0;
};
namespace detail {
inline constexpr long double exactLimit = 281474976710656.L; // 2^48
inline Facts unknown() {
    const auto inf = std::numeric_limits<long double>::infinity();
    return {false,false,-inf,inf};
}
inline Facts unite(const Facts& a, const Facts& b) {
    return {a.finite && b.finite, a.integral && b.integral,
            std::min(a.lo,b.lo), std::max(a.hi,b.hi)};
}
inline bool safe(const Facts& f) {
    return f.finite && f.integral && std::isfinite(f.lo) && std::isfinite(f.hi) &&
           f.lo <= f.hi && f.lo >= -exactLimit && f.hi <= exactLimit;
}
inline Facts bounded(long double lo, long double hi) {
    Facts f{true,true,lo,hi};
    return safe(f) ? f : unknown();
}
} // namespace detail

// integral also excludes possible -0. Entry guards must exclude -0 and
// non-Number/noninteger inputs. Both branch arms are conservatively merged.
// Public facts are unions across all requested repetitions, not just the last.
inline Analysis analyze(const Graph& g, unsigned iterations = 64,
                        long double entryLimit = 2097152.L) {
    Analysis result;
    result.values.assign(g.ssa.definitions.size(), detail::unknown());
    const auto order = topologicalOrder(g);
    if (order.empty() || iterations == 0 || !std::isfinite(entryLimit) ||
        entryLimit < 0 || entryLimit > detail::exactLimit) return result;
    std::vector<Facts> inputs(g.inputs.size(),
                             Facts{true,true,0,std::floor(entryLimit)});
    std::vector<const Instruction*> producers(g.ssa.definitions.size(), nullptr);
    for (const auto& block : g.instructions)
        for (const auto& inst : block) producers[inst.result] = &inst;
    std::vector<std::vector<ValueId>> phis(g.instructions.size());
    for (ValueId v = 0; v < g.ssa.definitions.size(); ++v)
        if (g.ssa.definitions[v].phi) phis[g.ssa.definitions[v].block].push_back(v);
    result.integerSafe = true;
    for (unsigned repeat = 0; repeat < iterations; ++repeat) {
        std::vector<Facts> values(g.ssa.definitions.size(), detail::unknown());
        for (BlockId b : order) {
            for (ValueId v : phis[b]) {
                const auto& def = g.ssa.definitions[v];
                const auto& phi = g.ssa.cfg.block(b).phis[def.phiIndex];
                Facts f = detail::unknown();
                bool first = true;
                for (const auto& in : phi.inputs) {
                    f = first ? values[in.value] : detail::unite(f,values[in.value]);
                    first = false;
                }
                values[v] = f;
            }
            for (const auto& inst : g.instructions[b]) {
                Facts f = detail::unknown();
                if (inst.op == Op::Parameter) f = inputs[inst.slot];
                else if (inst.op == Op::ListRead) {
                    // An element's type/range is unknown, independent of its index.
                    // Do not inspect the unused rhs field.
                } else if (inst.op == Op::Constant) {
                    if (std::isfinite(inst.literal)) {
                        const long double n = inst.literal;
                        f = {true, std::trunc(n) == n &&
                             !(n == 0 && std::signbit(inst.literal)), n, n};
                    }
                } else if (inst.op == Op::Less || inst.op == Op::Greater ||
                           inst.op == Op::Equal || inst.op == Op::And ||
                           inst.op == Op::Or || inst.op == Op::Not) {
                    f = {true,true,0,1};
                } else {
                    const auto& a = values[inst.lhs];
                    const auto& c = values[inst.rhs];
                    if (detail::safe(a) && detail::safe(c)) {
                        switch (inst.op) {
                        case Op::Add:
                            f = detail::bounded(a.lo+c.lo,a.hi+c.hi); break;
                        case Op::Subtract:
                            f = detail::bounded(a.lo-c.hi,a.hi-c.lo); break;
                        case Op::Multiply: {
                            const bool az = a.lo <= 0 && a.hi >= 0;
                            const bool cz = c.lo <= 0 && c.hi >= 0;
                            if ((az && c.lo < 0) || (cz && a.lo < 0)) break;
                            const long double ends[] = {
                                a.lo*c.lo,a.lo*c.hi,a.hi*c.lo,a.hi*c.hi};
                            f = detail::bounded(*std::min_element(ends,ends+4),
                                                *std::max_element(ends,ends+4));
                            break;
                        }
                        case Op::Modulo:
                            // Only a literal positive integer denominator is proven.
                            if (a.lo >= 0 && producers[inst.rhs] &&
                                producers[inst.rhs]->op == Op::Constant &&
                                c.lo == c.hi && c.lo > 0)
                                f = detail::bounded(0,std::min(a.hi,c.hi-1));
                            break;
                        // General division does not prove exact integer semantics.
                        default: break;
                        }
                    }
                }
                values[inst.result] = f;
            }
        }
        for (ValueId v = 0; v < values.size(); ++v) {
            if (g.ssa.definitions[v].type == Type::F64 && !detail::safe(values[v]))
                result.integerSafe = false;
            result.values[v] = repeat == 0 ? values[v]
                : detail::unite(result.values[v],values[v]);
        }
        // Simultaneous feedback, never the aggregate facts from earlier repeats.
        for (size_t slot = 0; slot < inputs.size(); ++slot)
            inputs[slot] = values[g.outputs[slot]];
        ++result.iterations;
    }
    return result;
}
} // namespace xyo::ir::region
