// Standalone: c++ -std=c++20 -Isrc tests/platform/numeric_region_smoke.cpp
#include "ir/numeric_region.hpp"
#include <cassert>
#include <iostream>
#include <limits>

using namespace xyo::ir;
namespace r = xyo::ir::region;

static ValueId emit(r::Graph& g, BlockId b, r::Op op,
                    ValueId lhs = 0, ValueId rhs = 0, double literal = 0) {
    const auto id = static_cast<ValueId>(g.ssa.definitions.size());
    const bool boolean = op == r::Op::Less || op == r::Op::Greater ||
        op == r::Op::Equal || op == r::Op::And || op == r::Op::Or || op == r::Op::Not;
    g.ssa.definitions.push_back({b, boolean ? Type::I1 : Type::F64,
        static_cast<uint32_t>(g.instructions[b].size()+1), false, 0});
    g.instructions[b].push_back({id,op,lhs,rhs,literal,0});
    return id;
}
static r::Graph straight(r::Op op, double constant) {
    r::Graph g;
    g.ssa.cfg.createBlock();
    g.instructions.resize(1);
    const auto p = emit(g,0,r::Op::Parameter);
    const auto c = emit(g,0,r::Op::Constant,0,0,constant);
    const auto out = emit(g,0,op,p,c);
    g.inputs = {p}; g.outputs = {out}; g.written = {true};
    g.ssa.cfg.block(0).terminator = {TerminatorKind::Return,0,0,0,out};
    return g;
}
static r::Graph diamond() {
    r::Graph g;
    for (int i = 0; i < 4; ++i) g.ssa.cfg.createBlock();
    g.instructions.resize(4); g.exit = 3;
    const auto p = emit(g,0,r::Op::Parameter);
    const auto one = emit(g,0,r::Op::Constant,0,0,1);
    const auto condition = emit(g,0,r::Op::Less,p,one);
    g.ssa.cfg.block(0).terminator = {TerminatorKind::Branch,1,2,condition};
    const auto add = emit(g,1,r::Op::Add,p,one);
    g.ssa.cfg.block(1).terminator = {TerminatorKind::Jump,3};
    const auto sub = emit(g,2,r::Op::Subtract,p,one);
    g.ssa.cfg.block(2).terminator = {TerminatorKind::Jump,3};
    const auto phi = static_cast<ValueId>(g.ssa.definitions.size());
    g.ssa.cfg.block(3).phis.push_back({Type::F64,{{1,add},{2,sub}}});
    g.ssa.definitions.push_back({3,Type::F64,0,true,0});
    g.ssa.cfg.block(3).terminator = {TerminatorKind::Return,0,0,0,phi};
    g.inputs = {p}; g.outputs = {phi}; g.written = {true};
    return g;
}
int main() {
    auto g = diamond();
    assert(r::verify(g));
    assert(r::topologicalOrder(g).size() == 4);
    auto a = r::analyze(g);
    assert(a.integerSafe && a.iterations == 64);
    assert(a.values[g.outputs[0]].lo == -64);
    assert(a.values[g.outputs[0]].hi == 2097152.L+64);
    assert(a.values[0].lo == -63 && a.values[0].hi == 2097152.L+63);
    assert(a.values[1].lo == 1 && a.values[1].hi == 1);

    auto add = straight(r::Op::Add,1);
    a = r::analyze(add);
    assert(a.integerSafe && a.values[2].lo == 1 && a.values[2].hi == 2097216);
    auto mod = straight(r::Op::Modulo,17);
    a = r::analyze(mod);
    assert(a.integerSafe && a.values[2].lo == 0 && a.values[2].hi == 16);
    auto badMod = straight(r::Op::Modulo,-17);
    assert(r::verify(badMod) && !r::analyze(badMod).integerSafe);
    auto negativeLhs = straight(r::Op::Subtract,1);
    const auto seventeen = emit(negativeLhs,0,r::Op::Constant,0,0,17);
    const auto remainder = emit(negativeLhs,0,r::Op::Modulo,2,seventeen);
    negativeLhs.outputs[0] = remainder;
    negativeLhs.ssa.cfg.block(0).terminator.result = remainder;
    assert(r::verify(negativeLhs) && !r::analyze(negativeLhs).integerSafe);
    auto signedMod = straight(r::Op::Modulo,17);
    const auto neg = emit(signedMod,0,r::Op::Subtract,0,1);
    signedMod.instructions[0][2].lhs = neg;
    assert(!r::verify(signedMod)); // actual use rejected even with empty use metadata

    auto multiply = straight(r::Op::Multiply,-1);
    assert(r::verify(multiply) && !r::analyze(multiply).integerSafe); // possible -0
    auto positive = straight(r::Op::Multiply,2);
    assert(r::analyze(positive,20).integerSafe);
    assert(!r::analyze(positive,64).integerSafe); // widening, bounded work
    auto zero = straight(r::Op::Multiply,0);
    assert(r::analyze(zero).integerSafe);
    auto division = straight(r::Op::Divide,1);
    assert(r::verify(division) && !r::analyze(division).integerSafe);
    for (double c : {-0.0,0.5,std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
        auto floating = straight(r::Op::Add,c);
        assert(r::verify(floating) && !r::analyze(floating).integerSafe);
    }
    auto bad = g;
    bad.written[0] = false;
    assert(!r::verify(bad)); // readonly summary cannot change loop-carried input
    bad.outputs[0] = bad.inputs[0];
    assert(r::verify(bad));
    bad = g;
    bad.ssa.definitions.back().phiIndex = 1;
    assert(!r::verify(bad));
    bad = g;
    bad.ssa.cfg.block(3).phis[0].inputs[0].value = 3; // valid own predecessor
    bad.ssa.cfg.block(3).phis[0].inputs[1].value = 3; // sibling not dominated
    assert(!r::verify(bad));
    bad = g;
    bad.ssa.cfg.block(0).terminator.condition = 0; // F64 not I1
    assert(!r::verify(bad));
    bad = g;
    bad.ssa.cfg.block(2).terminator = {TerminatorKind::Jump,0};
    assert(r::topologicalOrder(bad).empty());
    bad = g;
    bad.ssa.cfg.createBlock(); bad.instructions.emplace_back();
    bad.ssa.cfg.block(4).terminator = {TerminatorKind::Return,0,0,0,0};
    assert(!r::verify(bad));
    assert(!r::analyze(g,0).integerSafe);
    assert(!r::analyze(g,64,-1).integerSafe);
    std::cout << "numeric region smoke: passed\n";
}
