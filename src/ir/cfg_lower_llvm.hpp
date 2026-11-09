// LLVM lowering of the executable CFG subset.
// All blocks and PHIs are created before the instruction pass; incoming edges
// are wired last, so a loop backedge can refer to values defined later.
#pragma once

#include "executable.hpp"
#include "execution_abi.hpp"
#include "../core/value.hpp"
#include <bit>
#include <algorithm>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace xyo::ir {

class LlvmCfgBackend {
public:
    LlvmCfgBackend(llvm::LLVMContext& context, llvm::Module& module,
                   std::string functionName = "main")
        : context_(context), module_(module), builder_(context),
          functionName_(std::move(functionName)) {}

    void start(const Executable& program) {
        externalSlots_ = program.externalValueSlots;
        resumable_ = program.resumable;
        program_ = &program;
        auto* signature = llvm::FunctionType::get(
            llvm::Type::getInt64Ty(context_),
            externalSlots_ ? (resumable_ ?
                std::vector<llvm::Type*>{
                    llvm::PointerType::get(context_, 0),
                    llvm::PointerType::get(context_, 0)}
                : std::vector<llvm::Type*>{
                    llvm::PointerType::get(context_, 0)})
                : std::vector<llvm::Type*>{}, false);
        function_ = llvm::Function::Create(
            const_cast<llvm::FunctionType*>(signature),
            llvm::GlobalValue::ExternalLinkage, functionName_, module_);
        const auto count = program.ssa.cfg.blocks().size();
        blocks_.resize(count);
        for (size_t i = 0; i < count; ++i)
            blocks_[i] = llvm::BasicBlock::Create(
                context_, "cfg." + std::to_string(i), function_);
        values_.resize(program.ssa.definitions.size());
        phiValues_.resize(count);
        phiDestinations_.resize(count);
        for (ValueId id = 0; id < program.ssa.definitions.size(); ++id) {
            const auto& def = program.ssa.definitions[id];
            if (def.phi) {
                auto& indices = phiDestinations_[def.block];
                if (indices.size() <= def.phiIndex)
                    indices.resize(def.phiIndex + 1, kNoValue);
                indices[def.phiIndex] = id;
            }
        }
        for (size_t i = 0; i < count; ++i)
            phiValues_[i].resize(program.ssa.cfg.block(i).phis.size());
        definitions_ = &program.ssa.definitions;
        if (resumable_) {
            // Only restore SSA values actually used in a CFG block and
            // defined outside it (or stored as an incoming PHI). Loading
            // every SSA value in every block makes a resumable function
            // quadratic in its block/value count, even at LLVM -O0.
            const size_t values = program.ssa.definitions.size();
            std::vector<std::vector<uint8_t>> needs(count,
                std::vector<uint8_t>(values, 0));
            const auto mark = [&](BlockId block, ValueId id) {
                if (id == kNoValue) return;
                const auto& def = program.ssa.definitions[id];
                if (def.phi || def.block != block)
                    needs[block][id] = 1;
            };
            for (BlockId block = 0; block < count; ++block) {
                for (const auto& inst : program.instructions[block]) {
                    switch (inst.op) {
                    case ExecOp::StoreVar:
                    case ExecOp::StoreNumber:
                    case ExecOp::ChangeVar:
                    case ExecOp::ChangeNumber:
                    case ExecOp::EncodeNumber:
                    case ExecOp::RepeatCount:
                    case ExecOp::FloorF64:
                    case ExecOp::CeilF64:
                    case ExecOp::SqrtF64:
                    case ExecOp::AbsF64:
                    case ExecOp::RoundF64:
                    case ExecOp::NotI1:
                        mark(block, inst.lhs);
                        break;
                    case ExecOp::AddI64: case ExecOp::SubI64:
                    case ExecOp::LessI64:
                    case ExecOp::AddF64: case ExecOp::SubF64:
                    case ExecOp::MulF64: case ExecOp::DivF64:
                    case ExecOp::LessF64: case ExecOp::GreaterF64:
                    case ExecOp::EqualF64:
                    case ExecOp::AndI1: case ExecOp::OrI1:
                        mark(block, inst.lhs);
                        mark(block, inst.rhs);
                        break;
                    default: break;
                    }
                }
                const auto& term = program.ssa.cfg.block(block).terminator;
                if (term.kind == TerminatorKind::Branch)
                    mark(block, term.condition);
                else if (term.kind == TerminatorKind::Return)
                    mark(block, term.result);
            }
            for (BlockId to = 0; to < count; ++to) {
                for (const auto& phi : program.ssa.cfg.block(to).phis)
                    for (const auto& input : phi.inputs)
                        mark(input.predecessor, input.value);
            }
            frameLiveIns_.resize(count);
            for (BlockId block = 0; block < count; ++block)
                for (ValueId id = 0; id < values; ++id)
                    if (needs[block][id])
                        frameLiveIns_[block].push_back(id);
        }
        slots_.reserve(program.variableSlots);
        // Allocate all mutable slots in the entry block and initialize to
        // zero before any user instructions. One allocation per invocation.
        auto* entry = resumable_
            ? llvm::BasicBlock::Create(context_, "resume.entry", function_,
                                       blocks_[0])
            : blocks_[0];
        builder_.SetInsertPoint(entry);
        for (uint32_t i = 0; i < program.variableSlots; ++i) {
            if (externalSlots_) {
                auto* slot = builder_.CreateInBoundsGEP(
                    type(Type::I64), function_->getArg(0),
                    builder_.getInt32(i), "staged.slot");
                slots_.push_back(slot);
            } else {
                auto* slot = builder_.CreateAlloca(
                    type(Type::I64), nullptr,
                    "slot." + std::to_string(i));
                builder_.CreateStore(
                    llvm::ConstantInt::get(type(Type::I64), 0), slot);
                slots_.push_back(slot);
            }
        }
        if (resumable_) {
            auto* pc = builder_.CreateLoad(type(Type::I64), frameWord(NumericExecutionAbi::kPcWord));
            auto* invalid = llvm::BasicBlock::Create(
                context_, "resume.invalid", function_);
            auto* dispatch = builder_.CreateSwitch(pc, invalid, count);
            for (size_t i = 0; i < count; ++i)
                dispatch->addCase(
                    builder_.getInt64(i), blocks_[i]);
            builder_.SetInsertPoint(invalid);
            builder_.CreateRet(
                llvm::ConstantInt::get(type(Type::I64), NumericExecutionAbi::status(NumericRunStatus::Failed)));
        }
    }
    void declarePhi(BlockId block, uint32_t index, const Phi& phi) {
        if (resumable_) return;
        builder_.SetInsertPoint(blocks_[block]);
        const auto name = "phi." + std::to_string(block) + "." +
                          std::to_string(index);
        auto* value = builder_.CreatePHI(type(phi.type), phi.inputs.size(), name);
        phiValues_[block][index] = value;
        for (ValueId id = 0; id < definitions_->size(); ++id) {
            const auto& def = (*definitions_)[id];
            if (def.phi && def.block == block && def.phiIndex == index)
                values_[id] = value;
        }
    }
    void beginBlock(BlockId block) {
        builder_.SetInsertPoint(blocks_[block]);
        if (resumable_) {
            // Cross-block uses and incoming PHIs must be restored from the
            // frame. In-block SSA definitions are generated by emit().
            for (ValueId id : frameLiveIns_[block])
                values_[id] = readFrameValue(id);
        }
    }
    void emit(const ExecInstruction& inst) {
        llvm::Value* value = nullptr;
        if (inst.op == ExecOp::LessF64 || inst.op == ExecOp::GreaterF64 ||
            inst.op == ExecOp::EqualF64) {
            auto* invalid = builder_.CreateFCmpUNO(
                values_[inst.lhs], values_[inst.rhs]);
            auto* failure = llvm::BasicBlock::Create(
                context_, "compare.nan.bailout", function_);
            auto* valid = llvm::BasicBlock::Create(
                context_, "compare.numeric", function_);
            builder_.CreateCondBr(invalid, failure, valid);
            builder_.SetInsertPoint(failure);
            builder_.CreateRet(builder_.getInt64(resumable_
                ? NumericExecutionAbi::status(NumericRunStatus::Failed)
                : Value::undefined().bits));
            builder_.SetInsertPoint(valid);
        }
        switch (inst.op) {
        case ExecOp::ConstI64:
            value = llvm::ConstantInt::get(type(Type::I64),
                                            uint64_t(inst.literal));
            break;
        case ExecOp::AddI64:
            value = builder_.CreateAdd(values_[inst.lhs], values_[inst.rhs]);
            break;
        case ExecOp::SubI64:
            value = builder_.CreateSub(values_[inst.lhs], values_[inst.rhs]);
            break;
        case ExecOp::LessI64:
            value = builder_.CreateICmpSLT(values_[inst.lhs], values_[inst.rhs]);
            break;
        case ExecOp::LoadVar:
            value = builder_.CreateLoad(type(Type::I64), slots_[inst.slot]);
            break;
        case ExecOp::StoreVar:
            builder_.CreateStore(values_[inst.lhs], slots_[inst.slot]);
            return;
        case ExecOp::ChangeVar: {
            auto* previous = builder_.CreateLoad(type(Type::I64),
                                                 slots_[inst.slot]);
            value = builder_.CreateAdd(previous, values_[inst.lhs]);
            builder_.CreateStore(value, slots_[inst.slot]);
            break;
        }
        case ExecOp::ConstF64:
            value = llvm::ConstantFP::get(
                type(Type::F64),
                std::bit_cast<double>(uint64_t(inst.literal)));
            break;
        case ExecOp::AddF64:
            value = builder_.CreateFAdd(
                scratchNumber(values_[inst.lhs]),
                scratchNumber(values_[inst.rhs]));
            break;
        case ExecOp::SubF64:
            value = builder_.CreateFSub(
                scratchNumber(values_[inst.lhs]),
                scratchNumber(values_[inst.rhs]));
            break;
        case ExecOp::MulF64:
            value = builder_.CreateFMul(
                scratchNumber(values_[inst.lhs]),
                scratchNumber(values_[inst.rhs]));
            break;
        case ExecOp::DivF64:
            value = builder_.CreateFDiv(
                scratchNumber(values_[inst.lhs]),
                scratchNumber(values_[inst.rhs]));
            break;
        case ExecOp::FloorF64:
        case ExecOp::CeilF64:
        case ExecOp::SqrtF64:
            value = builder_.CreateUnaryIntrinsic(
                inst.op == ExecOp::FloorF64 ? llvm::Intrinsic::floor :
                inst.op == ExecOp::CeilF64 ? llvm::Intrinsic::ceil :
                                           llvm::Intrinsic::sqrt,
                scratchNumber(values_[inst.lhs]));
            break;
        case ExecOp::AbsF64:
            value = builder_.CreateUnaryIntrinsic(
                llvm::Intrinsic::fabs, scratchNumber(values_[inst.lhs]));
            break;
        case ExecOp::RoundF64: {
            // Scratch/ECMAScript Math.round: nearest-even + one on the
            // half-integers whose tie went toward -infinity. -0 survives.
            auto* input = scratchNumber(values_[inst.lhs]);
            auto* even = builder_.CreateUnaryIntrinsic(
                llvm::Intrinsic::roundeven, input);
            auto* halfDown = builder_.CreateFCmpOEQ(
                builder_.CreateFSub(input, even),
                llvm::ConstantFP::get(type(Type::F64), 0.5));
            value = builder_.CreateSelect(halfDown,
                builder_.CreateFAdd(even,
                    llvm::ConstantFP::get(type(Type::F64), 1.0)), even);
            break;
        }
        case ExecOp::NotI1:
            value = builder_.CreateNot(values_[inst.lhs]);
            break;
        case ExecOp::AndI1:
            value = builder_.CreateAnd(values_[inst.lhs], values_[inst.rhs]);
            break;
        case ExecOp::OrI1:
            value = builder_.CreateOr(values_[inst.lhs], values_[inst.rhs]);
            break;
        case ExecOp::LessF64:
            value = builder_.CreateFCmpOLT(
                scratchNumber(values_[inst.lhs]),
                scratchNumber(values_[inst.rhs]));
            break;
        case ExecOp::GreaterF64:
            value = builder_.CreateFCmpOGT(
                scratchNumber(values_[inst.lhs]),
                scratchNumber(values_[inst.rhs]));
            break;
        case ExecOp::EqualF64:
            value = builder_.CreateFCmpOEQ(
                scratchNumber(values_[inst.lhs]),
                scratchNumber(values_[inst.rhs]));
            break;
        case ExecOp::LoadNumber:
            value = builder_.CreateBitCast(
                builder_.CreateLoad(type(Type::I64), slots_[inst.slot]),
                type(Type::F64));
            break;
        case ExecOp::StoreNumber:
            builder_.CreateStore(encodeNumber(values_[inst.lhs]),
                                 slots_[inst.slot]);
            return;
        case ExecOp::ChangeNumber: {
            value = builder_.CreateFAdd(
                loadNumber(inst.slot),
                scratchNumber(values_[inst.lhs]));
            builder_.CreateStore(encodeNumber(value), slots_[inst.slot]);
            break;
        }
        case ExecOp::EncodeNumber:
            value = encodeNumber(values_[inst.lhs]);
            break;
        case ExecOp::RepeatCount: {
            auto* number = scratchNumber(values_[inst.lhs]);
            auto* zero = llvm::ConstantFP::get(type(Type::F64), 0.0);
            auto* half = llvm::ConstantFP::get(type(Type::F64), 0.5);
            auto* positive = builder_.CreateSelect(
                builder_.CreateFCmpOGT(number, zero), number, zero);
            auto* candidate = builder_.CreateFAdd(positive, half);

            // Match jsnum::jsRound: for 0 <= n < 0.5 the result must
            // remain +0 even when (n + 0.5) rounds to 1 in IEEE-754.
            // Check the rounded candidate against the shared cap; a
            // source within the cap may round up past it.
            auto* tooLarge = builder_.CreateFCmpOGE(
                candidate,
                llvm::ConstantFP::get(type(Type::F64),
                    double(inst.literal) + 1.0));
            auto* failure = llvm::BasicBlock::Create(
                context_, "repeat.count.bailout", function_);
            auto* valid = llvm::BasicBlock::Create(
                context_, "repeat.count.valid", function_);
            builder_.CreateCondBr(tooLarge, failure, valid);
            builder_.SetInsertPoint(failure);
            builder_.CreateRet(builder_.getInt64(NumericExecutionAbi::status(NumericRunStatus::Failed)));
            builder_.SetInsertPoint(valid);
            auto* rounded = builder_.CreateSelect(
                builder_.CreateFCmpOLT(number, half), zero, candidate);
            value = builder_.CreateFPToUI(rounded, type(Type::I64));
            break;
        }
        }
        values_[inst.result] = value;
        if (resumable_)
            builder_.CreateStore(asBits(value), frameWord(NumericExecutionAbi::valueWord(inst.result)));
    }
    void terminate(BlockId from, const Terminator& terminator) {
        if (resumable_) {
            switch (terminator.kind) {
            case TerminatorKind::Jump:
                resumeJump(from, terminator.trueBlock);
                return;
            case TerminatorKind::Branch: {
                auto* yes = llvm::BasicBlock::Create(
                    context_, "resume.edge.true", function_);
                auto* no = llvm::BasicBlock::Create(
                    context_, "resume.edge.false", function_);
                builder_.CreateCondBr(values_[terminator.condition], yes, no);
                builder_.SetInsertPoint(yes);
                resumeJump(from, terminator.trueBlock);
                builder_.SetInsertPoint(no);
                resumeJump(from, terminator.falseBlock);
                return;
            }
            case TerminatorKind::Return:
                builder_.CreateStore(asBits(values_[terminator.result]),
                                     frameWord(NumericExecutionAbi::kResultWord));
                builder_.CreateStore(
                    llvm::ConstantInt::get(type(Type::I64), NumericExecutionAbi::kCompletedPc),
                    frameWord(NumericExecutionAbi::kPcWord));
                builder_.CreateRet(
                    llvm::ConstantInt::get(type(Type::I64),
                        NumericExecutionAbi::status(NumericRunStatus::Completed)));
                return;
            case TerminatorKind::None:
                builder_.CreateUnreachable();
                return;
            }
        }
        switch (terminator.kind) {
        case TerminatorKind::Jump:
            builder_.CreateBr(blocks_[terminator.trueBlock]);
            break;
        case TerminatorKind::Branch:
            // Our CFG canonicalizes identical destinations into a single
            // predecessor edge. LLVM must match that edge multiplicity for
            // its PHI nodes; a redundant conditional branch would create
            // two incoming LLVM edges to the same target.
            if (terminator.trueBlock == terminator.falseBlock)
                builder_.CreateBr(blocks_[terminator.trueBlock]);
            else
                builder_.CreateCondBr(values_[terminator.condition],
                                      blocks_[terminator.trueBlock],
                                      blocks_[terminator.falseBlock]);
            break;
        case TerminatorKind::Return:
            builder_.CreateRet(values_[terminator.result]);
            break;
        case TerminatorKind::None: break;
        }
    }
    void wirePhi(BlockId block, uint32_t index, const Phi& phi) {
        if (resumable_) return;
        auto* node = phiValues_[block][index];
        for (const auto& input : phi.inputs)
            node->addIncoming(values_[input.value],
                              blocks_[input.predecessor]);
    }
    void finish() {}
    llvm::Function* function() const { return function_; }

private:
    llvm::Value* frameWord(uint32_t index) {
        return builder_.CreateInBoundsGEP(
            type(Type::I64), function_->getArg(1),
            builder_.getInt32(index));
    }
    llvm::Value* asBits(llvm::Value* value) {
        if (value->getType()->isDoubleTy())
            return builder_.CreateBitCast(value, type(Type::I64));
        if (value->getType()->isIntegerTy(64)) return value;
        return builder_.CreateZExt(value, type(Type::I64));
    }
    llvm::Value* readFrameValue(ValueId id) {
        auto* bits = builder_.CreateLoad(type(Type::I64),
                                         frameWord(NumericExecutionAbi::valueWord(id)));
        switch ((*definitions_)[id].type) {
        case Type::F64: return builder_.CreateBitCast(bits, type(Type::F64));
        case Type::I1: return builder_.CreateTrunc(bits, type(Type::I1));
        case Type::I32: return builder_.CreateTrunc(bits, type(Type::I32));
        case Type::I64: return bits;
        }
        return bits;
    }
    void resumeJump(BlockId from, BlockId to) {
        const auto& phis = program_->ssa.cfg.block(to).phis;
        std::vector<llvm::Value*> incoming;
        incoming.reserve(phis.size());
        for (const auto& phi : phis) {
            const auto it = std::find_if(phi.inputs.begin(), phi.inputs.end(),
                [from](const PhiInput& edge) {
                    return edge.predecessor == from;
                });
            incoming.push_back(asBits(values_[it->value]));
        }
        // Pre-read all sources before publishing any destination: parallel
        // PHI semantics, including mutually swapping carried values.
        for (size_t i = 0; i < phis.size(); ++i)
            builder_.CreateStore(incoming[i],
                frameWord(NumericExecutionAbi::valueWord(phiDestinations_[to][i])));
        const auto& boundaries = program_->yieldBoundaries;
        const bool checkpoint = std::any_of(
            boundaries.begin(), boundaries.end(),
            [from, to](const LoopYieldBoundary& y) {
                return y.latch == from && y.header == to;
            });
        if (checkpoint) {
            builder_.CreateStore(
                llvm::ConstantInt::get(type(Type::I64), to), frameWord(NumericExecutionAbi::kPcWord));
            builder_.CreateRet(
                llvm::ConstantInt::get(type(Type::I64),
                    NumericExecutionAbi::status(NumericRunStatus::Yielded)));
        } else {
            builder_.CreateBr(blocks_[to]);
        }
    }
    llvm::Value* scratchNumber(llvm::Value* number) {
        return builder_.CreateSelect(
            builder_.CreateFCmpUNO(number, number),
            llvm::ConstantFP::get(type(Type::F64), 0.0), number);
    }
    llvm::Value* encodeNumber(llvm::Value* number) {
        auto* bits = builder_.CreateBitCast(number, type(Type::I64));
        auto* nan = builder_.CreateFCmpUNO(number, number);
        return builder_.CreateSelect(nan,
            llvm::ConstantInt::get(type(Type::I64), xyo::val::kNan),
            bits);
    }
    llvm::Value* loadNumber(uint32_t slot) {
        auto* bits = builder_.CreateLoad(type(Type::I64), slots_[slot]);
        auto* canonical = builder_.CreateICmpEQ(
            bits, llvm::ConstantInt::get(type(Type::I64),
                                         xyo::val::kNan));
        auto* number = builder_.CreateBitCast(bits, type(Type::F64));
        return builder_.CreateSelect(
            canonical, llvm::ConstantFP::get(type(Type::F64), 0.0), number);
    }
    llvm::Type* type(Type type) const {
        switch (type) {
        case Type::I1: return llvm::Type::getInt1Ty(context_);
        case Type::I32: return llvm::Type::getInt32Ty(context_);
        case Type::I64: return llvm::Type::getInt64Ty(context_);
        case Type::F64: return llvm::Type::getDoubleTy(context_);
        }
        return nullptr;
    }
    llvm::LLVMContext& context_;
    llvm::Module& module_;
    llvm::IRBuilder<> builder_;
    llvm::Function* function_ = nullptr;
    std::vector<llvm::BasicBlock*> blocks_;
    std::vector<llvm::Value*> values_;
    std::vector<llvm::Value*> slots_;
    std::vector<std::vector<llvm::PHINode*>> phiValues_;
    std::vector<std::vector<ValueId>> phiDestinations_;
    std::vector<std::vector<ValueId>> frameLiveIns_;
    const std::vector<SsaDefinition>* definitions_ = nullptr;
    const Executable* program_ = nullptr;
    std::string functionName_;
    bool externalSlots_ = false;
    bool resumable_ = false;
};

} // namespace xyo::ir
