#include "ir/numeric_region_cost.hpp"
#include <cassert>
#include <iostream>
using namespace xyo::ir;
namespace r = xyo::ir::region;
static r::Graph graph(unsigned n) {
    r::Graph g;
    for (unsigned i=0;i<n;++i) g.ssa.cfg.createBlock();
    g.instructions.resize(n); g.exit=n-1;
    g.ssa.definitions={{0,Type::F64,1,false,0},{0,Type::I1,2,false,0}};
    g.instructions[0]={{0,r::Op::Constant,0,0,0,0},{1,r::Op::Equal,0,0,0,0}};
    g.ssa.cfg.block(n-1).terminator={TerminatorKind::Return,0,0,0,0};
    return g;
}
static void jump(r::Graph&g,unsigned a,unsigned b) {
    g.ssa.cfg.block(a).terminator={TerminatorKind::Jump,b};
}
static void branch(r::Graph&g,unsigned a,unsigned yes,unsigned no) {
    g.ssa.cfg.block(a).terminator={TerminatorKind::Branch,yes,no,1};
}
int main() {
    auto one=graph(1); assert(r::verify(one)); assert(!r::hasNestedConditional(one));
    auto diamond=graph(4); branch(diamond,0,1,2);jump(diamond,1,3);jump(diamond,2,3);
    assert(r::verify(diamond));assert(!r::hasNestedConditional(diamond));
    auto flat=graph(7);branch(flat,0,1,2);jump(flat,1,3);jump(flat,2,3);
    branch(flat,3,4,5);jump(flat,4,6);jump(flat,5,6);
    assert(r::verify(flat));assert(!r::hasNestedConditional(flat));
    auto nested=graph(7);branch(nested,0,1,2);jump(nested,1,6);
    branch(nested,2,3,4);jump(nested,3,5);jump(nested,4,5);jump(nested,5,6);
    assert(r::verify(nested));assert(r::hasNestedConditional(nested));
    auto bad=nested; bad.ssa.cfg.block(5).terminator={TerminatorKind::Jump,0};
    assert(!r::verify(bad));assert(r::hasNestedConditional(bad));
    std::cout<<"numeric region CFG cost smoke: passed\n";
}
