// Portable SSA graph regression, independent of LLVM and WASM dependencies.
// Keep assertions active in Release / -DNDEBUG CI builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/numeric.hpp"
#include "../../src/ir/control_flow.hpp"
#include "../../src/ir/diamond.hpp"
#include "../../src/ir/loop.hpp"
#include "../../src/ir/guard.hpp"
#include "../../src/ir/deopt.hpp"
#include "../../src/ir/verifier.hpp"
#include "../../src/ir/ssa.hpp"
#include "../../src/ir/slot_access.hpp"
#include "../../src/ir/value_abi.hpp"
#include "../../src/ir/execution_abi.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
struct ReferenceLowerer {
    double parameters[2];
    double parameter(uint32_t index) { return parameters[index]; }
    double binary(xyo::ir::Op op, double a, double b) {
        switch (op) {
        case xyo::ir::Op::Add: return a + b;
        case xyo::ir::Op::Sub: return a - b;
        case xyo::ir::Op::Mul: return a * b;
        case xyo::ir::Op::Div: return a / b;
        case xyo::ir::Op::LessThan: return a < b ? 1.0 : 0.0;
        case xyo::ir::Op::GreaterThan: return a > b ? 1.0 : 0.0;
        default: return std::numeric_limits<double>::quiet_NaN();
        }
    }
};
}

int main() {
    using namespace xyo::ir;
    // This is the same scalar continuation ABI consumed by LLVM, WASM,
    // the host scheduler, and the staged Scratch variable transaction.
    static_assert(NumericExecutionAbi::kPcWord == 0);
    static_assert(NumericExecutionAbi::kResultWord == 1);
    static_assert(NumericExecutionAbi::valueWord(0) == 2);
    static_assert(NumericExecutionAbi::valueWord(7) == 9);
    static_assert(NumericExecutionAbi::frameWords(7) == 9);
    static_assert(NumericExecutionAbi::byteOffset(
        NumericExecutionAbi::valueWord(7)) == 72);
    static_assert(NumericExecutionAbi::kCompletedPc == UINT64_MAX);
    assert(NumericExecutionAbi::isCommitCandidate(
        NumericExecutionAbi::status(NumericRunStatus::Completed)));
    assert(NumericExecutionAbi::isCommitCandidate(
        NumericExecutionAbi::status(NumericRunStatus::Yielded)));
    assert(!NumericExecutionAbi::isCommitCandidate(
        NumericExecutionAbi::status(NumericRunStatus::Failed)));
    assert(!NumericExecutionAbi::isCommitCandidate(UINT64_MAX));
    static_assert(AtomicWarpExecutionAbi::kHelperScratchWords == 3);
    static_assert(AtomicWarpExecutionAbi::frameWords(2) == 7168);
    static_assert(AtomicWarpExecutionAbi::frameOffsetBytes(5) == 64);
    static_assert(AtomicWarpExecutionAbi::bufferWords(5, 2) == 7176);
    static_assert(AtomicWarpExecutionAbi::status(
        AtomicWarpStatus::Rollback) !=
        NumericExecutionAbi::status(NumericRunStatus::Failed));
    for (Op op : {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::LessThan, Op::GreaterThan}) {
        Function function = binaryFunction(op);
        assert(function.instructions().size() == 3);
        assert(function.instructions()[2].type ==
               (op == Op::LessThan || op == Op::GreaterThan
                    ? Type::I1 : Type::F64));
        ReferenceLowerer lowerer{{12.0, 4.0}};
        const double actual = lower(function, lowerer, 2);
        const double expected = op == Op::Add ? 16.0 :
                                op == Op::Sub ? 8.0 :
                                op == Op::Mul ? 48.0 :
                                op == Op::Div ? 3.0 :
                                op == Op::LessThan ? 0.0 : 1.0;
        assert(actual == expected);
    }
    Function chained;
    auto a = chained.parameter();
    auto b = chained.parameter();
    auto sum = chained.binary(Op::Add, a, b);
    auto product = chained.binary(Op::Mul, sum, a);
    ReferenceLowerer lowerer{{3.0, 7.0}};
    assert(lower(chained, lowerer, product) == 30.0);
    // A conditional diamond with a Phi at the merge point.
    ControlFlow cfg;
    const auto entry = cfg.createBlock();
    const auto thenBlock = cfg.createBlock();
    const auto elseBlock = cfg.createBlock();
    const auto joinBlock = cfg.createBlock();
    cfg.block(entry).terminator = {TerminatorKind::Branch,
                                   thenBlock, elseBlock, 0, 0};
    cfg.block(thenBlock).terminator = {TerminatorKind::Jump, joinBlock};
    cfg.block(elseBlock).terminator = {TerminatorKind::Jump, joinBlock};
    cfg.block(joinBlock).terminator = {TerminatorKind::Return};
    cfg.block(joinBlock).phis.push_back({Type::F64,
                                         {{thenBlock, 0}, {elseBlock, 1}}});
    assert(cfg.validate());
    cfg.block(joinBlock).phis[0].inputs[1].predecessor = thenBlock;
    assert(!cfg.validate()); // Duplicate predecessor cannot form a Phi.
    // Check the same diamond lowering contract for both branches. A reference
    // lowerer models the backend's value merge without LLVM/WASM dependencies.
    struct ReferenceDiamond {
        bool selected = false;
        int yes = 0, no = 0;
        void branch(bool cond, BlockId, BlockId, BlockId) { selected = cond; }
        void otherwise(BlockId) {}
        void finish(BlockId) {}
        int phi(BlockId, BlockId, int a, BlockId, int b) {
            yes = a;
            no = b;
            return selected ? a : b;
        }
    };
    Diamond diamond(true, Type::I32);
    assert(diamond.validate());
    for (bool value : {false, true}) {
        ReferenceDiamond emitter;
        int branches = 0;
        int result = lowerDiamondValue(
            diamond, emitter, value,
            [&]() { ++branches; return 17; },
            [&]() { ++branches; return 29; });
        assert(branches == 2); // Both paths are compiled, one runs.
        assert(emitter.yes == 17 && emitter.no == 29);
        assert(result == (value ? 17 : 29));
    }
    // The void variant emits side-effectful statement bodies in source order.
    struct ReferenceVoid {
        int state = 0;
        void branch(int, BlockId, BlockId, BlockId) { state = 1; }
        void otherwise(BlockId) { assert(state == 2); state = 3; }
        void finish(BlockId) { assert(state == 4); state = 5; }
    };
    ReferenceVoid voidEmitter;
    Diamond voidDiamond;
    lowerDiamondVoid(voidDiamond, voidEmitter, 0,
                     [&]() { assert(voidEmitter.state == 1);
                              voidEmitter.state = 2; },
                     [&]() { assert(voidEmitter.state == 3);
                              voidEmitter.state = 4; });
    assert(voidEmitter.state == 5);
    // The loop graph has a real self-edge; the header's Phi must receive
    // exactly the entry and backedge predecessors.
    BackedgeLoop loop(Type::I64);
    assert(loop.validate());
    loop.cfg.block(loop.header).phis[0].inputs[1].predecessor = loop.exit;
    assert(!loop.validate());
    BackedgeLoop validLoop(Type::I64, 2);
    assert(validLoop.cfg.block(validLoop.header).phis.size() == 2);
    assert(validLoop.validate());
    struct LoopLowerer {
        int state = 0;
        void begin(BlockId) { assert(state == 0); state = 1; }
        void latch(bool cond, BlockId, BlockId) {
            assert(state == 2 && cond); state = 3;
        }
        void finish(BlockId) { assert(state == 3); state = 4; }
    } loopLowerer;
    lowerBackedgeLoop(validLoop, loopLowerer, [&]() {
        assert(loopLowerer.state == 1);
        loopLowerer.state = 2;
        return true;
    });
    assert(loopLowerer.state == 4);
    // Failed guards must preserve their failure policy. Native speculative
    // guards resume the fallback, while WASM aborts before the fast region.
    struct GuardLowerer {
        int calls = 0;
        bool allowed = false;
        GuardFailure lastFailure = GuardFailure::Fallback;
        GuardReason lastReason = GuardReason::NumericValue;
        void require(bool cond, const Guard& guard) {
            ++calls;
            allowed = cond;
            lastFailure = guard.failure;
            lastReason = guard.reason;
        }
    } guards;
    lowerGuard(guards, {GuardReason::NumericValue, GuardFailure::Bail},
               false);
    assert(guards.calls == 1 && !guards.allowed);
    assert(guards.lastFailure == GuardFailure::Bail);
    lowerGuard(guards, {GuardReason::IntegralRange,
                        GuardFailure::Fallback}, true);
    assert(guards.calls == 2 && guards.allowed);
    assert(guards.lastReason == GuardReason::IntegralRange);
    assert(guards.lastFailure == GuardFailure::Fallback);
    // Verifier: valid SSA and a malformed CFG are distinguished even with
    // assertions disabled in a release build.
    assert(verifyNumeric(chained));
    assert(verifyNumeric(binaryFunction(Op::LessThan)));
    Function empty;
    assert(verifyNumeric(empty).error == VerifyError::EmptyFunction);
    assert(verifyControlFlow(validLoop.cfg));
    assert(verifyControlFlow(diamond.cfg));
    assert(verifyControlFlow(cfg).error == VerifyError::BadCFG);
    ControlFlow orphan;
    orphan.createBlock();
    orphan.createBlock();
    orphan.block(0).terminator = {TerminatorKind::Return};
    orphan.block(1).terminator = {TerminatorKind::Return};
    const auto orphanCheck = verifyControlFlow(orphan);
    assert(orphanCheck.error == VerifyError::UnreachableBlock &&
           orphanCheck.index == 1);
    // Snapshot must restore exact NaN-boxed words and continuation state.
    DeoptSnapshot checkpoint;
    const uint64_t original[] = {
        0x7ff8000000000000ULL, 0x7ff8000000000002ULL,
        0x7ff90000deadbeefULL, 0x8000000000000000ULL
    };
    uint64_t staging[4] = {};
    for (size_t i = 0; i < 4; ++i) staging[i] = original[i];
    const ResumePoint cursor{ResumePhase::LoopBody, 2, 13.0, 8};
    assert(checkpoint.capture(staging, 4, cursor));
    staging[0] = 42;
    staging[1] = 0;
    staging[2] = 0;
    ResumePoint recovered;
    assert(!checkpoint.restore(staging, 3, &recovered));
    assert(checkpoint.restore(staging, 4, &recovered));
    for (size_t i = 0; i < 4; ++i) assert(staging[i] == original[i]);
    assert(recovered.phase == ResumePhase::LoopBody);
    assert(recovered.loopIndex == 2 && recovered.loopRemaining == 13.0);
    assert(recovered.blockIndex == 8);
    checkpoint.discard();
    assert(!checkpoint.restore(staging, 4));

    // Exercise the heap-backed capture path without changing slot ownership.
    std::vector<uint64_t> large(64);
    for (size_t i = 0; i < large.size(); ++i) large[i] = 1000 + i;
    assert(checkpoint.capture(large.data(), large.size()));
    large[40] = 0;
    assert(checkpoint.restore(large.data(), large.size()));
    assert(large[40] == 1040);
    assert(!checkpoint.capture(nullptr, 1));
    assert(!checkpoint.capture(large.data(), DeoptSnapshot::kMaxSlots + 1));
    // True cross-block SSA verifier: a diamond, a loop-carried Phi and
    // invalid dominance/forward-reference/type cases.
    SsaGraph typed;
    const auto s0 = typed.cfg.createBlock();
    const auto s1 = typed.cfg.createBlock();
    const auto s2 = typed.cfg.createBlock();
    const auto s3 = typed.cfg.createBlock();
    typed.cfg.block(s0).terminator = {TerminatorKind::Branch,s1,s2};
    typed.cfg.block(s1).terminator = {TerminatorKind::Jump,s3};
    typed.cfg.block(s2).terminator = {TerminatorKind::Jump,s3};
    typed.cfg.block(s3).terminator = {TerminatorKind::Return};
    typed.cfg.block(s3).phis.push_back(
        {Type::F64, {{s1, 1}, {s2, 2}}});
    typed.definitions = {
        {s0,Type::F64,1,false,0}, {s1,Type::F64,1,false,0},
        {s2,Type::F64,1,false,0}, {s3,Type::F64,0,true,0}
    };
    typed.uses = {{s3,0,Type::F64,1}, {s3,3,Type::F64,1}};
    assert(verifySSA(typed));
    typed.uses.push_back({s2,1,Type::F64,2});
    assert(verifySSA(typed).error == VerifyError::NotDominated);
    typed.uses.pop_back();
    typed.cfg.block(s3).phis[0].inputs[1].value = 1;
    assert(verifySSA(typed).error == VerifyError::NotDominated);
    typed.cfg.block(s3).phis[0].inputs[1].value = 2;
    typed.uses[1].expectedType = Type::I32;
    assert(verifySSA(typed).error == VerifyError::BadType);
    typed.uses[1].expectedType = Type::F64;
    typed.uses[0].value = 100;
    assert(verifySSA(typed).error == VerifyError::MissingDefinition);
    typed.uses[0].value = 0;
    typed.definitions[3].type = Type::I32;
    assert(verifySSA(typed).error == VerifyError::BadType);
    typed.definitions[3].type = Type::F64;
    typed.definitions[1].position = 0;
    assert(verifySSA(typed).error == VerifyError::BadOperand);
    typed.definitions[1].position = 1;
    assert(verifySSA(typed));

    SsaGraph cyclic;
    auto e0 = cyclic.cfg.createBlock();
    auto h = cyclic.cfg.createBlock();
    auto x = cyclic.cfg.createBlock();
    cyclic.cfg.block(e0).terminator = {TerminatorKind::Jump,h};
    cyclic.cfg.block(h).terminator = {TerminatorKind::Branch,h,x};
    cyclic.cfg.block(x).terminator = {TerminatorKind::Return};
    cyclic.cfg.block(h).phis.push_back(
        {Type::I64, {{e0,0}, {h,2}}});
    cyclic.definitions = {
        {e0,Type::I64,1,false,0},
        {h,Type::I64,0,true,0},
        {h,Type::I64,1,false,0}
    };
    cyclic.uses = {{h,1,Type::I64,1}};
    assert(verifySSA(cyclic));
    cyclic.uses.push_back({h,2,Type::I64,1});
    assert(verifySSA(cyclic).error == VerifyError::NotDominated);
    cyclic.uses.pop_back();
    cyclic.definitions.push_back({h,Type::I64,0,true,0});
    assert(verifySSA(cyclic).error == VerifyError::DuplicateDefinition);

    uint64_t guarded[2] = {0x7ff8000000000002ULL, 19};
    DeoptSnapshot guardSnapshot;
    assert(guardSnapshot.capture(guarded, 2, cursor));
    guarded[0] = 0;
    ResumePoint guardResume;
    auto rejected = checkSpeculativeGuard(
        false, {GuardReason::NumericValue, GuardFailure::Bail},
        guardSnapshot, guarded, 2, &guardResume);
    assert(!rejected.passed && rejected.restored);
    assert(guarded[0] == 0x7ff8000000000002ULL);
    assert(guardResume.loopRemaining == 13.0);
    auto accepted = checkSpeculativeGuard(
        true, {GuardReason::IntegralRange, GuardFailure::Fallback},
        guardSnapshot, guarded, 2, &guardResume);
    assert(accepted.passed && !accepted.restored);
    guardSnapshot.discard();
    rejected = checkSpeculativeGuard(
        false, {GuardReason::NumericValue, GuardFailure::Bail},
        guardSnapshot, guarded, 2, &guardResume);
    assert(!rejected.passed && !rejected.restored);
    // Real Scratch NaN-boxing semantics across generic slot operations.
    struct RawSlots {
        uint64_t bits[3] = {xyo::Value::num(2.5).bits,
                            xyo::val::kTrue, xyo::val::kNan};
        uint64_t loadSlot(uint32_t id) { return bits[id]; }
        void storeSlot(uint32_t id, uint64_t value) { bits[id] = value; }
        bool isNumericBits(uint64_t value) {
            return xyo::ir::isNumericBits(value);
        }
    } raw;
    static_assert(kSlotBytes == sizeof(xyo::Value));
    assert(lowerSlotLoad(raw, 0u) == xyo::Value::num(2.5).bits);
    assert(lowerNumericSlotGuard(raw, lowerSlotLoad(raw, 0u)));
    assert(!lowerNumericSlotGuard(raw, lowerSlotLoad(raw, 1u)));
    assert(lowerNumericSlotGuard(raw, lowerSlotLoad(raw, 2u)));
    for (uint64_t nonNumeric : {xyo::val::kFalse, xyo::val::kTrue,
                               xyo::val::kEmptyStr, xyo::val::kUndefined,
                               xyo::val::kTagStr | uint64_t{0x1234},
                               xyo::val::kTagColor | uint64_t{0xff1234}}) {
        assert(!isNumericBits(nonNumeric));
        assert(isNumericBits(nonNumeric) == xyo::Value{nonNumeric}.isNumber());
    }
    for (uint64_t numeric : {xyo::val::kNan, xyo::Value::num(0).bits,
                             xyo::Value::num(-0.0).bits,
                             xyo::Value::num(1.25).bits,
                             xyo::Value::num(-3).bits}) {
        assert(isNumericBits(numeric));
        assert(isNumericBits(numeric) == xyo::Value{numeric}.isNumber());
    }
    lowerSlotStore(raw, 1u, xyo::Value::num(91).bits);
    assert(lowerSlotLoad(raw, 1u) == xyo::Value::num(91).bits);
    assert(lowerNumericSlotGuard(raw, lowerSlotLoad(raw, 1u)));
    // The common immediate-value ABI must agree with Scratch's numeric
    // conversion, preserving signed zero and rejecting heap-string pointers.
    for (uint64_t bits : {xyo::val::kNan, xyo::val::kTrue,
                          xyo::val::kFalse, xyo::val::kEmptyStr,
                          xyo::val::kUndefined, xyo::val::kTagColor,
                          xyo::Value::num(-0.0).bits,
                          xyo::Value::num(3.5).bits}) {
        const auto immediate = toNumberImmediate(bits);
        assert(!immediate.needsStringParse);
        const double expected = bits == xyo::val::kTrue ? 1.0 :
            bits == xyo::Value::num(3.5).bits ? 3.5 : 0.0;
        assert(immediate.value == expected);
        if (bits == xyo::Value::num(-0.0).bits)
            assert(std::signbit(immediate.value));
    }
    const auto stringValue = toNumberImmediate(xyo::val::kTagStr | 0x1234);
    assert(stringValue.needsStringParse);
    assert(classifyValueBits(xyo::val::kTagStr | 0x1234) ==
           ImmediateKind::HeapString);
    return 0;
}
