// Standalone readonly-list schema and analysis tests; no runtime/list dependency.
#include "ir/numeric_region_float_facts.hpp"
#include <cassert>
#include <iostream>
#include <limits>
using namespace xyo::ir;
namespace r = xyo::ir::region;
static ValueId emit(r::Graph& g, r::Op op, ValueId lhs=0, ValueId rhs=0,
                    double literal=0, uint32_t slot=0) {
    auto id=static_cast<ValueId>(g.ssa.definitions.size());
    const bool boolean=op==r::Op::Less;
    g.ssa.definitions.push_back({0,boolean ? Type::I1 : Type::F64,
        static_cast<uint32_t>(g.instructions[0].size()+1),false,0});
    g.instructions[0].push_back({id,op,lhs,rhs,literal,slot});
    return id;
}
int main() {
    static_assert(static_cast<int>(r::Op::Not)==12);
    static_assert(static_cast<int>(r::Op::ListRead)==13);
    r::Graph g;
    assert(g.listSlots==0 && !g.listElementsFinite);
    g.ssa.cfg.createBlock(); g.instructions.resize(1); g.listSlots=1;
    auto counter=emit(g,r::Op::Parameter,0,0,0,0);
    auto acc=emit(g,r::Op::Parameter,0,0,0,1);
    auto one=emit(g,r::Op::Constant,0,0,1);
    auto next=emit(g,r::Op::Add,counter,one);
    const auto unused=std::numeric_limits<ValueId>::max();
    auto element=emit(g,r::Op::ListRead,next,unused,0,0);
    auto sum=emit(g,r::Op::Add,acc,element);
    g.inputs={counter,acc}; g.outputs={next,sum}; g.written={true,true};
    g.ssa.cfg.block(0).terminator={TerminatorKind::Return,0,0,0,sum};
    assert(r::verify(g));
    assert(r::topologicalOrder(g).size()==1);
    auto core=r::analyze(g);
    assert(!core.integerSafe && core.iterations==64);
    assert(!core.values[element].finite && !core.values[element].integral);
    assert(!core.values[sum].finite && !core.values[sum].integral);
    assert(core.values[counter].finite && core.values[counter].integral);
    assert(core.values[counter].lo==0 && core.values[counter].hi==2097215);
    assert(core.values[next].lo==1 && core.values[next].hi==2097216);
    const auto inf=std::numeric_limits<long double>::infinity();
    r::FloatFact unknown{false,false,true,-inf,inf};
    r::FloatFact boundedCounter{true,false,true,0,2097215};
    auto facts=r::analyzeFloatFacts(g,{boundedCounter,unknown});
    assert(facts.size()==g.ssa.definitions.size());
    assert(!facts[element].finite && !facts[element].nonzero && !facts[element].resultNonNaN);
    assert(!facts[sum].finite && !facts[sum].resultNonNaN); // unknown + unknown
    assert(facts[next].finite && facts[next].nonzero && facts[next].resultNonNaN);
    // A finite accumulator can add a guarded, possibly infinite element safely.
    facts=r::analyzeFloatFacts(g,{boundedCounter,{true,false,true,0,10}});
    assert(facts[sum].resultNonNaN && !facts[sum].finite);
    auto finitePolicy=g;
    finitePolicy.listElementsFinite=true;
    assert(r::verify(finitePolicy));
    facts=r::analyzeFloatFacts(finitePolicy,{boundedCounter,unknown});
    const auto max=std::numeric_limits<double>::max();
    assert(facts[element].finite && !facts[element].nonzero && !facts[element].resultNonNaN);
    assert(facts[element].lo==-max && facts[element].hi==max);
    assert(facts[sum].resultNonNaN && !facts[sum].finite);
    auto finiteCore=r::analyze(finitePolicy);
    assert(!finiteCore.integerSafe && !finiteCore.values[element].finite &&
           !finiteCore.values[element].integral);
    assert(finiteCore.values[next].hi==core.values[next].hi);
    // Finite elements can still overflow; proof removes only the NaN guard.
    auto product=emit(finitePolicy,r::Op::Multiply,element,element);
    facts=r::analyzeFloatFacts(finitePolicy,{boundedCounter,unknown});
    assert(facts[product].resultNonNaN && !facts[product].finite && !facts[product].nonzero);
    // Without an element-finite guard, unknown * unknown still requires ORD.
    finitePolicy.listElementsFinite=false;
    facts=r::analyzeFloatFacts(finitePolicy,{boundedCounter,unknown});
    assert(!facts[product].resultNonNaN && !facts[element].finite);
    // The annotation alone does not require list bindings or change verification.
    r::Graph noLists;
    noLists.ssa.cfg.createBlock(); noLists.instructions.resize(1);
    noLists.listElementsFinite=true;
    auto literal=emit(noLists,r::Op::Constant,0,0,1);
    noLists.ssa.cfg.block(0).terminator={TerminatorKind::Return,0,0,0,literal};
    assert(r::verify(noLists));
    auto bad=g;
    bad.listSlots=0; // no binding; graph does not contain host pointers to validate
    assert(!r::verify(bad));
    bad=g; bad.instructions[0][element].slot=1;
    assert(!r::verify(bad));
    bad=g; bad.instructions[0][element].slot=std::numeric_limits<uint32_t>::max();
    assert(!r::verify(bad));
    bad=g; bad.instructions[0][element].lhs=unused;
    assert(!r::verify(bad));
    bad=g; bad.instructions[0][element].lhs=sum; // forward index definition
    assert(!r::verify(bad));
    bad=g; bad.ssa.definitions[element].type=Type::I1;
    assert(!r::verify(bad));
    r::Graph booleanIndex;
    booleanIndex.ssa.cfg.createBlock(); booleanIndex.instructions.resize(1);
    booleanIndex.listSlots=1;
    auto p=emit(booleanIndex,r::Op::Parameter);
    auto condition=emit(booleanIndex,r::Op::Less,p,p);
    auto read=emit(booleanIndex,r::Op::ListRead,condition,unused);
    booleanIndex.inputs={p}; booleanIndex.outputs={read}; booleanIndex.written={true};
    booleanIndex.ssa.cfg.block(0).terminator={TerminatorKind::Return,0,0,0,read};
    assert(!r::verify(booleanIndex)); // dominating I1 index must still be rejected
    bad=g; bad.written[1]=false; // changed scalar output can't claim readonly
    assert(!r::verify(bad));
    bad=g; bad.inputs.push_back(element); // ListRead isn't a scalar Parameter
    bad.outputs.push_back(element); bad.written.push_back(false);
    assert(!r::verify(bad));
    // List slot IDs and scalar slots are separate namespaces.
    auto independent=g;
    independent.listSlots=4;
    independent.instructions[0][element].slot=3;
    assert(r::verify(independent));
    std::cout<<"numeric region list smoke: passed\n";
}
