// Conditional-on-continuation floating-point facts; no backend dependencies.
#pragma once
#include "numeric_region.hpp"

namespace xyo::ir::region {
struct FloatFact {
    bool finite = false;
    bool nonzero = false;
    bool resultNonNaN = false;
    long double lo = -std::numeric_limits<long double>::infinity();
    long double hi = std::numeric_limits<long double>::infinity();
};

namespace float_fact_detail {
inline constexpr long double maxDouble = std::numeric_limits<double>::max();
inline constexpr long double minNormal = std::numeric_limits<double>::min();
inline FloatFact normalize(FloatFact f) {
    if (std::isnan(f.lo) || std::isnan(f.hi) || f.lo > f.hi) {
        // Malformed intervals must not supply range-derived proofs.
        f.lo = -std::numeric_limits<long double>::infinity();
        f.hi = std::numeric_limits<long double>::infinity();
    }
    if (f.finite) {
        f.lo = std::max(f.lo, -maxDouble);
        f.hi = std::min(f.hi, maxDouble);
        if (f.lo > f.hi) { f.lo = -maxDouble; f.hi = maxDouble; }
    }
    return f;
}
inline FloatFact unite(const FloatFact& a, const FloatFact& b) {
    // Incoming values have already passed their producer's ORD guard.
    return {a.finite && b.finite, a.nonzero && b.nonzero, true,
            std::min(a.lo,b.lo), std::max(a.hi,b.hi)};
}
inline void bounded(FloatFact& f, long double lo, long double hi) {
    // Include both long-double calculation error and binary64 rounding error.
    // The absolute margin handles subnormal rounding (including underflow to 0).
    const auto magnitude = std::max(std::fabs(lo),std::fabs(hi));
    if (!std::isfinite(magnitude) || magnitude > maxDouble/4 || lo > hi) return;
    const long double error = magnitude *
        (16.L * std::numeric_limits<double>::epsilon()) +
        16.L * std::numeric_limits<double>::denorm_min();
    lo = std::nextafter(lo-error,-std::numeric_limits<long double>::infinity());
    hi = std::nextafter(hi+error,std::numeric_limits<long double>::infinity());
    if (!std::isfinite(lo) || !std::isfinite(hi) ||
        lo < -maxDouble/2 || hi > maxDouble/2) return;
    f.finite = true;
    f.lo = lo; f.hi = hi;
    f.nonzero = lo >= 2*minNormal || hi <= -2*minNormal;
}
inline void arithmeticBounds(FloatFact& f, Op op,
                             const FloatFact& a, const FloatFact& b) {
    if (!a.finite || !b.finite) return;
    switch (op) {
    case Op::Add: bounded(f,a.lo+b.lo,a.hi+b.hi); break;
    case Op::Subtract: bounded(f,a.lo-b.hi,a.hi-b.lo); break;
    case Op::Multiply: {
        const long double ends[] = {a.lo*b.lo,a.lo*b.hi,a.hi*b.lo,a.hi*b.hi};
        bounded(f,*std::min_element(ends,ends+4),*std::max_element(ends,ends+4));
        break;
    }
    case Op::Divide: {
        if (b.lo <= 0 && b.hi >= 0) return;
        const long double ends[] = {a.lo/b.lo,a.lo/b.hi,a.hi/b.lo,a.hi/b.hi};
        bounded(f,*std::min_element(ends,ends+4),*std::max_element(ends,ends+4));
        break;
    }
    default: break;
    }
}
} // namespace float_fact_detail

// Input facts are explicit caller guarantees valid on EVERY invocation of the
// region (including loop-carried invocations), not hypothetical integer ranges.
// All Parameters must be Number/non-NaN guarded. Every earlier numeric producer
// whose resultNonNaN is false must retain its ORD guard and bail on NaN. Thus
// operands and Phi inputs are non-NaN conditional on reaching their uses, even
// when the producing instruction's resultNonNaN is false. This distinction is
// essential: resultNonNaN proves guard removal, not unconditional program facts.
// Returns empty for invalid graphs or a mismatched input-fact vector.
inline std::vector<FloatFact> analyzeFloatFacts(
    const Graph& g, const std::vector<FloatFact>& inputFacts) {
    if (inputFacts.size() != g.inputs.size()) return {};
    const auto order = topologicalOrder(g);
    if (order.empty()) return {};
    std::vector<FloatFact> facts(g.ssa.definitions.size());
    std::vector<std::vector<ValueId>> phis(g.instructions.size());
    for (ValueId v = 0; v < g.ssa.definitions.size(); ++v)
        if (g.ssa.definitions[v].phi) phis[g.ssa.definitions[v].block].push_back(v);
    for (BlockId block : order) {
        for (ValueId v : phis[block]) {
            const auto& def = g.ssa.definitions[v];
            const auto& phi = g.ssa.cfg.block(block).phis[def.phiIndex];
            auto f = facts[phi.inputs.front().value];
            f.resultNonNaN = true;
            for (size_t i = 1; i < phi.inputs.size(); ++i)
                f = float_fact_detail::unite(f,facts[phi.inputs[i].value]);
            facts[v] = f;
        }
        for (const auto& inst : g.instructions[block]) {
            FloatFact f;
            if (inst.op == Op::Parameter) {
                f = float_fact_detail::normalize(inputFacts[inst.slot]);
            } else if (inst.op == Op::ListRead) {
                // This producer always needs a runtime element guard. The
                // annotation upgrades that guard to reject infinities too; it
                // provides finite facts only conditional on continuation.
                // rhs is unused and intentionally not inspected.
                if (g.listElementsFinite) {
                    f.finite = true;
                    f.lo = -float_fact_detail::maxDouble;
                    f.hi = float_fact_detail::maxDouble;
                }
            } else if (inst.op == Op::Constant) {
                f.resultNonNaN = !std::isnan(inst.literal);
                f.finite = std::isfinite(inst.literal);
                f.nonzero = f.resultNonNaN && inst.literal != 0;
                if (f.resultNonNaN) f.lo = f.hi = inst.literal;
            } else if (g.ssa.definitions[inst.result].type == Type::I1) {
                f = {true,false,true,0,1};
            } else {
                const auto& a = facts[inst.lhs];
                const auto& b = facts[inst.rhs];
                switch (inst.op) {
                case Op::Add: case Op::Subtract:
                    f.resultNonNaN = a.finite || b.finite; break;
                case Op::Multiply:
                    f.resultNonNaN = (a.finite && b.finite) ||
                        (a.finite && a.nonzero) || (b.finite && b.nonzero);
                    break;
                case Op::Divide:
                    f.resultNonNaN = (b.finite && b.nonzero) ||
                                    (a.finite && a.nonzero);
                    break;
                case Op::Modulo:
                    f.resultNonNaN = a.finite && b.finite && b.nonzero;
                    if (f.resultNonNaN) {
                        // fmod and the sign-correction addition cannot overflow:
                        // that addition has operands of opposite sign. The ratio
                        // used to select correction may underflow, so use a signed
                        // symmetric bound rather than assuming a positive remainder.
                        const auto bound = std::max(std::fabs(b.lo),std::fabs(b.hi));
                        f.finite = true; f.lo = -bound; f.hi = bound;
                    }
                    break;
                default: break;
                }
                float_fact_detail::arithmeticBounds(f,inst.op,a,b);
            }
            facts[inst.result] = f;
        }
    }
    return facts;
}
} // namespace xyo::ir::region
