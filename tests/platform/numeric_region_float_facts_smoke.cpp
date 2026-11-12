// Standalone backend-neutral float-guard proof tests.
#include "ir/numeric_region_float_facts.hpp"
#include <cassert>
#include <iostream>
#include <limits>

using namespace xyo::ir;
namespace r = xyo::ir::region;
static ValueId emit(r::Graph& g, BlockId b, r::Op op, ValueId a=0, ValueId c=0,
                    double literal=0, uint32_t slot=0) {
    auto v = static_cast<ValueId>(g.ssa.definitions.size());
    const bool boolean = op == r::Op::Less;
    g.ssa.definitions.push_back({b,boolean ? Type::I1 : Type::F64,
        static_cast<uint32_t>(g.instructions[b].size()+1),false,0});
    g.instructions[b].push_back({v,op,a,c,literal,slot});
    return v;
}
static r::Graph binary(r::Op op) {
    r::Graph g;
    g.ssa.cfg.createBlock(); g.instructions.resize(1);
    auto a=emit(g,0,r::Op::Parameter,0,0,0,0);
    auto b=emit(g,0,r::Op::Parameter,0,0,0,1);
    auto out=emit(g,0,op,a,b);
    g.inputs={a,b}; g.outputs={out,b}; g.written={true,false};
    g.ssa.cfg.block(0).terminator={TerminatorKind::Return,0,0,0,out};
    return g;
}
static r::FloatFact unknownNumber() { r::FloatFact f; f.resultNonNaN=true; return f; }
static r::FloatFact interval(long double lo,long double hi,bool nonzero=false) {
    return {true,nonzero,true,lo,hi};
}
static r::FloatFact result(r::Op op,r::FloatFact a,r::FloatFact b) {
    auto g=binary(op);
    auto facts=r::analyzeFloatFacts(g,{a,b});
    assert(facts.size()==3);
    return facts[2];
}
static double evaluate(r::Op op,double a,double b) {
    switch(op) {
    case r::Op::Add: return a+b;
    case r::Op::Subtract: return a-b;
    case r::Op::Multiply: return a*b;
    case r::Op::Divide: return a/b;
    default: { auto rem=std::fmod(a,b); return rem/b<0 ? rem+b : rem; }
    }
}
int main() {
    const auto u=unknownNumber(), z=interval(0,0), nz=interval(1,2,true);
    for(auto op:{r::Op::Add,r::Op::Subtract}) {
        assert(!result(op,u,u).resultNonNaN); // Inf +/- Inf
        assert(result(op,u,z).resultNonNaN);
        assert(result(op,z,u).resultNonNaN);
        assert(result(op,nz,nz).finite);
    }
    assert(!result(r::Op::Multiply,u,z).resultNonNaN); // 0 * Inf
    assert(!result(r::Op::Multiply,z,u).resultNonNaN);
    assert(result(r::Op::Multiply,u,nz).resultNonNaN);
    assert(result(r::Op::Multiply,nz,u).resultNonNaN);
    assert(result(r::Op::Multiply,z,z).resultNonNaN);
    assert(!result(r::Op::Divide,u,u).resultNonNaN); // Inf/Inf or 0/0
    assert(!result(r::Op::Divide,z,z).resultNonNaN);
    assert(result(r::Op::Divide,nz,z).resultNonNaN); // finite nonzero / 0 => Inf
    assert(result(r::Op::Divide,z,nz).resultNonNaN);
    assert(result(r::Op::Divide,u,nz).resultNonNaN);
    assert(!result(r::Op::Modulo,u,nz).resultNonNaN);
    assert(!result(r::Op::Modulo,nz,u).resultNonNaN);
    assert(!result(r::Op::Modulo,nz,z).resultNonNaN);
    assert(result(r::Op::Modulo,nz,nz).resultNonNaN);
    assert(result(r::Op::Modulo,nz,nz).finite);

    const double max=std::numeric_limits<double>::max();
    const double tiny=std::numeric_limits<double>::denorm_min();
    auto overflow=result(r::Op::Multiply,interval(max,max,true),interval(2,2,true));
    assert(overflow.resultNonNaN && !overflow.finite);
    auto underflow=result(r::Op::Multiply,interval(tiny,tiny,true),interval(.5,.5,true));
    assert(underflow.finite && !underflow.nonzero && underflow.lo<=0);
    auto quotient=result(r::Op::Divide,interval(tiny,tiny,true),interval(2,2,true));
    assert(quotient.finite && !quotient.nonzero && quotient.lo<=0);
    auto regular=result(r::Op::Multiply,nz,nz);
    assert(regular.finite && regular.nonzero && regular.lo<=1 && regular.hi>=4);
    auto cancellation=result(r::Op::Subtract,nz,nz);
    assert(cancellation.finite && !cancellation.nonzero && cancellation.lo<=-1);

    // Sample IEEE edge values: any claimed producer proof must exclude NaN.
    const double inf=std::numeric_limits<double>::infinity();
    const double samples[]={-inf,-max,-2,-tiny,-0.0,0,tiny,2,max,inf};
    for(auto op:{r::Op::Add,r::Op::Subtract,r::Op::Multiply,r::Op::Divide,r::Op::Modulo})
        for(double a:samples) for(double b:samples) {
            auto af=std::isfinite(a) ? interval(a,a,a!=0) : u;
            auto bf=std::isfinite(b) ? interval(b,b,b!=0) : u;
            auto proof=result(op,af,bf);
            const auto actual=evaluate(op,a,b);
            if(proof.resultNonNaN) assert(!std::isnan(actual));
            if(proof.finite) {
                assert(std::isfinite(actual));
                assert(actual>=proof.lo && actual<=proof.hi);
            }
            if(proof.nonzero) assert(actual!=0);
        }

    // Literal NaN and infinities stay distinct; signed zero isn't nonzero.
    auto g=binary(r::Op::Add);
    for(double value:{-0.0,tiny,inf,std::numeric_limits<double>::quiet_NaN()}) {
        auto v=emit(g,0,r::Op::Constant,0,0,value);
        const auto facts=r::analyzeFloatFacts(g,{u,u});
        assert(facts[v].resultNonNaN==!std::isnan(value));
        assert(facts[v].finite==std::isfinite(value));
        assert(facts[v].nonzero==(!std::isnan(value) && value!=0));
    }
    // A guarded predecessor result is non-NaN at all subsequent uses.
    auto guarded=binary(r::Op::Multiply); // producer needs guard: unknown * unknown
    auto sum=emit(guarded,0,r::Op::Add,2,0);
    auto one=emit(guarded,0,r::Op::Constant,0,0,1);
    auto safe=emit(guarded,0,r::Op::Multiply,sum,one);
    auto facts=r::analyzeFloatFacts(guarded,{u,u});
    assert(!facts[2].resultNonNaN && !facts[sum].resultNonNaN);
    assert(facts[safe].resultNonNaN);

    // Generic Phi union: differing arms, one bounded and one guard-dependent.
    r::Graph join;
    for(int i=0;i<4;++i) join.ssa.cfg.createBlock();
    join.instructions.resize(4); join.exit=3;
    auto p=emit(join,0,r::Op::Parameter);
    auto c=emit(join,0,r::Op::Constant,0,0,1);
    auto cond=emit(join,0,r::Op::Less,p,c);
    join.ssa.cfg.block(0).terminator={TerminatorKind::Branch,1,2,cond};
    auto left=emit(join,1,r::Op::Add,p,c);
    auto right=emit(join,2,r::Op::Divide,p,p);
    join.ssa.cfg.block(1).terminator={TerminatorKind::Jump,3};
    join.ssa.cfg.block(2).terminator={TerminatorKind::Jump,3};
    auto phi=static_cast<ValueId>(join.ssa.definitions.size());
    join.ssa.definitions.push_back({3,Type::F64,0,true,0});
    join.ssa.cfg.block(3).phis.push_back({Type::F64,{{1,left},{2,right}}});
    join.ssa.cfg.block(3).terminator={TerminatorKind::Return,0,0,0,phi};
    join.inputs={p}; join.outputs={phi}; join.written={true};
    facts=r::analyzeFloatFacts(join,{u});
    assert(!facts[right].resultNonNaN && facts[phi].resultNonNaN);
    assert(!facts[phi].finite);
    facts=r::analyzeFloatFacts(join,{nz});
    assert(facts[phi].finite && facts[phi].nonzero && facts[phi].lo<=.5L && facts[phi].hi>=3);
    assert(r::analyzeFloatFacts(join,{}).empty());
    join.ssa.definitions.back().phiIndex=99;
    assert(r::analyzeFloatFacts(join,{u}).empty());
    std::cout<<"numeric region float facts smoke: passed\n";
}
