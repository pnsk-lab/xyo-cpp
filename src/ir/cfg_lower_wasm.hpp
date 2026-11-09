// WebAssembly lowering for the executable CFG subset.
// An explicit program counter and loop dispatcher support arbitrary reducible
// or irreducible graphs. Each SSA definition owns a local; incoming Phi values
// are moved in parallel through temporary locals at every CFG edge.
//
// This is a correctness-oriented fallback, not yet an optimized structured
// WASM region lowering. It emits no Scratch runtime side effects.
#pragma once

#include "executable.hpp"
#include "execution_abi.hpp"
#include "../core/value.hpp"
#include <bit>
#include "../wasm/wasm_emit.hpp"

#include <cstdint>
#include <vector>

namespace xyo::ir {

class WasmCfgBackend {
public:
    explicit WasmCfgBackend(xyo::wasm::Code& code, uint32_t firstLocal = 0)
        : code_(code), nextLocal_(firstLocal) {}

    void start(const Executable& program) {
        program_ = &program;
        externalSlots_ = program.externalValueSlots;
        resumable_ = program.resumable;
        if (resumable_ && nextLocal_ < 2) nextLocal_ = 2;
        else if (externalSlots_ && nextLocal_ == 0) nextLocal_ = 1;
        const size_t n = program.ssa.definitions.size();
        locals_.resize(n);
        for (ValueId i = 0; i < n; ++i)
            locals_[i] = allocate(program.ssa.definitions[i].type);
        pc_ = allocate(Type::I32);
        slots_.reserve(program.variableSlots);
        for (uint32_t i = 0; i < program.variableSlots; ++i)
            slots_.push_back(externalSlots_ ? UINT32_MAX :
                             allocate(Type::I64));
        bitsTemp_ = allocate(Type::I64);
        f64Temp_ = allocate(Type::F64);
        roundedTemp_ = allocate(Type::F64);
        // WASM locals start at zero on each invocation. Explicitly clear
        // slots as well so the initial state is visible in emitted code.
        if (!externalSlots_)
            for (uint32_t slot : slots_) {
                code_.i64Const(0);
                code_.localSet(slot);
            }
        temps_.resize(program.ssa.cfg.blocks().size());
        phiDestinations_.resize(program.ssa.cfg.blocks().size());
        for (BlockId block = 0; block < temps_.size(); ++block) {
            const auto& phis = program.ssa.cfg.block(block).phis;
            for (const auto& phi : phis) {
                temps_[block].push_back(allocate(phi.type));
                phiDestinations_[block].push_back(UINT32_MAX);
            }
        }
        for (ValueId id = 0; id < program.ssa.definitions.size(); ++id) {
            const auto& definition = program.ssa.definitions[id];
            if (definition.phi)
                phiDestinations_[definition.block][definition.phiIndex] = id;
        }
        if (resumable_) {
            // Rehydrate SSA locals from the persistent caller-owned frame.
            // frame = [pc, returnBits, value0, value1, ...].
            for (ValueId id = 0; id < locals_.size(); ++id) {
                code_.localGet(1);
                code_.i64Load(3, NumericExecutionAbi::byteOffset(NumericExecutionAbi::valueWord(id)));
                switch (program.ssa.definitions[id].type) {
                case Type::F64: code_.f64ReinterpretI64(); break;
                case Type::I1: case Type::I32: code_.i32WrapI64(); break;
                case Type::I64: break;
                }
                code_.localSet(locals_[id]);
            }
            code_.localGet(1);
            code_.i64Load(3, NumericExecutionAbi::byteOffset(NumericExecutionAbi::kPcWord));
            code_.i32WrapI64();
        } else {
            code_.i32Const(0);
        }
        code_.localSet(pc_);
        code_.block();
        code_.loop();
    }
    void declarePhi(BlockId, uint32_t, const Phi&) {}
    void beginBlock(BlockId block) {
        code_.localGet(pc_);
        code_.i32Const(int32_t(block));
        code_.i32Eq();
        code_.if_();
    }
    void emit(const ExecInstruction& inst) {
        if (inst.op == ExecOp::LessF64 || inst.op == ExecOp::GreaterF64 ||
            inst.op == ExecOp::EqualF64) {
            code_.localGet(locals_[inst.lhs]);
            code_.localGet(locals_[inst.lhs]);
            code_.f64Ne();
            code_.localGet(locals_[inst.rhs]);
            code_.localGet(locals_[inst.rhs]);
            code_.f64Ne();
            code_.i32Or();
            code_.if_();
            code_.i64Const(int64_t(resumable_
                ? NumericExecutionAbi::status(NumericRunStatus::Failed)
                : Value::undefined().bits));
            code_.return_();
            code_.end();
        }
        switch (inst.op) {
        case ExecOp::ConstI64:
            code_.i64Const(inst.literal);
            break;
        case ExecOp::ConstF64:
            code_.f64Const(std::bit_cast<double>(uint64_t(inst.literal)));
            break;
        case ExecOp::LoadVar:
            loadSlotBits(inst.slot);
            break;
        case ExecOp::LoadNumber:
            loadSlotBits(inst.slot);
            code_.f64ReinterpretI64();
            break;
        case ExecOp::StoreVar:
            beginStore(inst.slot);
            code_.localGet(locals_[inst.lhs]);
            endStore(inst.slot);
            return;
        case ExecOp::StoreNumber:
            beginStore(inst.slot);
            encodeNumber(locals_[inst.lhs]);
            endStore(inst.slot);
            return;
        case ExecOp::ChangeVar:
            loadSlotBits(inst.slot);
            code_.localGet(locals_[inst.lhs]);
            code_.i64Add();
            code_.localSet(bitsTemp_);
            beginStore(inst.slot);
            code_.localGet(bitsTemp_);
            endStore(inst.slot);
            code_.localGet(bitsTemp_);
            break;
        case ExecOp::ChangeNumber:
            loadNumber(inst.slot);
            scratchNumber(locals_[inst.lhs]);
            code_.f64Add();
            code_.localSet(f64Temp_);
            beginStore(inst.slot);
            encodeNumber(f64Temp_);
            endStore(inst.slot);
            code_.localGet(f64Temp_);
            break;
        case ExecOp::EncodeNumber:
            encodeNumber(locals_[inst.lhs]);
            break;
        case ExecOp::RepeatCount:
            scratchNumber(locals_[inst.lhs]);
            code_.localSet(f64Temp_);
            code_.localGet(f64Temp_);
            code_.f64Const(0.0);
            code_.localGet(f64Temp_);
            code_.f64Const(0.0);
            code_.f64Gt();
            code_.select();
            code_.f64Const(0.5);
            code_.f64Add();
            code_.localSet(roundedTemp_);
            // Avoid the f64 x+0.5 boundary rounding mistake by matching
            // jsnum::jsRound's 0 <= x < 0.5 special case.
            code_.localGet(roundedTemp_);
            code_.f64Const(double(inst.literal) + 1.0);
            code_.f64Ge();
            code_.if_();
            code_.i64Const(int64_t(NumericExecutionAbi::status(NumericRunStatus::Failed)));
            code_.return_();
            code_.end();
            code_.f64Const(0.0);
            code_.localGet(roundedTemp_);
            code_.localGet(f64Temp_);
            code_.f64Const(0.5);
            code_.f64Lt();
            code_.select();
            code_.i64TruncF64U();
            break;
        case ExecOp::FloorF64:
        case ExecOp::CeilF64:
        case ExecOp::SqrtF64:
            scratchNumber(locals_[inst.lhs]);
            if (inst.op == ExecOp::FloorF64) code_.f64Floor();
            else if (inst.op == ExecOp::CeilF64) code_.f64Ceil();
            else code_.f64Sqrt();
            break;
        case ExecOp::AbsF64:
            scratchNumber(locals_[inst.lhs]);
            code_.f64Abs();
            break;
        case ExecOp::RoundF64:
            scratchNumber(locals_[inst.lhs]);
            code_.localSet(f64Temp_);
            code_.localGet(f64Temp_);
            code_.f64Nearest();
            code_.localSet(roundedTemp_);
            code_.localGet(roundedTemp_);
            code_.f64Const(1.0);
            code_.f64Add();
            code_.localGet(roundedTemp_);
            code_.localGet(f64Temp_);
            code_.localGet(roundedTemp_);
            code_.f64Sub();
            code_.f64Const(0.5);
            code_.f64Eq();
            code_.select();
            break;
        case ExecOp::NotI1:
            code_.localGet(locals_[inst.lhs]);
            code_.i32Eqz();
            break;
        default: {
            const bool floatOp = inst.op == ExecOp::AddF64 ||
                inst.op == ExecOp::SubF64 || inst.op == ExecOp::MulF64 ||
                inst.op == ExecOp::DivF64 || inst.op == ExecOp::LessF64 ||
                inst.op == ExecOp::GreaterF64 ||
                inst.op == ExecOp::EqualF64;
            if (floatOp) {
                scratchNumber(locals_[inst.lhs]);
                scratchNumber(locals_[inst.rhs]);
            } else {
                code_.localGet(locals_[inst.lhs]);
                code_.localGet(locals_[inst.rhs]);
            }
            switch (inst.op) {
            case ExecOp::AddI64: code_.i64Add(); break;
            case ExecOp::SubI64: code_.i64Sub(); break;
            case ExecOp::LessI64: code_.i64LtS(); break;
            case ExecOp::AndI1: code_.i32And(); break;
            case ExecOp::OrI1: code_.i32Or(); break;
            case ExecOp::LessF64: code_.f64Lt(); break;
            case ExecOp::GreaterF64: code_.f64Gt(); break;
            case ExecOp::EqualF64: code_.f64Eq(); break;
            case ExecOp::AddF64: code_.f64Add(); break;
            case ExecOp::SubF64: code_.f64Sub(); break;
            case ExecOp::MulF64: code_.f64Mul(); break;
            case ExecOp::DivF64: code_.f64Div(); break;
            default: code_.unreachable(); break;
            }
            break;
        }
        }
        code_.localSet(locals_[inst.result]);
        if (resumable_) saveFrameValue(inst.result);
    }
    void terminate(BlockId from, const Terminator& term) {
        switch (term.kind) {
        case TerminatorKind::Jump:
            jump(from, term.trueBlock);
            break;
        case TerminatorKind::Branch:
            code_.localGet(locals_[term.condition]);
            code_.if_();
            jump(from, term.trueBlock, false);
            code_.else_();
            jump(from, term.falseBlock, false);
            code_.end();
            code_.br(1); // leave the per-block if, re-enter dispatcher loop
            break;
        case TerminatorKind::Return:
            if (resumable_) {
                code_.localGet(1);
                code_.localGet(locals_[term.result]);
                code_.i64Store(3, NumericExecutionAbi::byteOffset(NumericExecutionAbi::kResultWord));
                code_.localGet(1);
                code_.i64Const(std::bit_cast<int64_t>(
                    NumericExecutionAbi::kCompletedPc));
                code_.i64Store(3, NumericExecutionAbi::byteOffset(NumericExecutionAbi::kPcWord));
                code_.i64Const(int64_t(NumericExecutionAbi::status(NumericRunStatus::Completed)));  // completed
            } else {
                code_.localGet(locals_[term.result]);
            }
            code_.return_();
            break;
        case TerminatorKind::None: code_.unreachable(); break;
        }
        code_.end(); // close this dispatcher arm
    }
    void wirePhi(BlockId, uint32_t, const Phi&) {}
    void finish() {
        code_.unreachable(); // a valid CFG never has an unknown PC
        code_.end(); // loop
        code_.end(); // block
        code_.i64Const(0); // unreachable fallback to satisfy type checker
    }
private:
    void saveFrameValue(ValueId id) {
        code_.localGet(1);
        code_.localGet(locals_[id]);
        switch (program_->ssa.definitions[id].type) {
        case Type::F64: code_.i64ReinterpretF64(); break;
        case Type::I1: case Type::I32: code_.i64ExtendI32U(); break;
        case Type::I64: break;
        }
        code_.i64Store(3, NumericExecutionAbi::byteOffset(NumericExecutionAbi::valueWord(id)));
    }
    bool isYieldEdge(BlockId from, BlockId to) const {
        for (const auto& y : program_->yieldBoundaries)
            if (y.latch == from && y.header == to) return true;
        return false;
    }
    // Raw bits of a Scratch numeric Value. A live Variable is never passed
    // here; external slots are snapshots owned by the host transaction.
    void loadSlotBits(uint32_t slot) {
        if (externalSlots_) {
            code_.localGet(0);
            code_.i64Load(3, slot * 8);
        } else {
            code_.localGet(slots_[slot]);
        }
    }
    void beginStore(uint32_t slot) {
        if (externalSlots_) code_.localGet(0);
        (void)slot;
    }
    void endStore(uint32_t slot) {
        if (externalSlots_) code_.i64Store(3, slot * 8);
        else code_.localSet(slots_[slot]);
    }
    void loadNumber(uint32_t slot) {
        // A numeric guard in the host guarantees no pointer or non-number
        // tag. Scratch's canonical numeric NaN converts to zero in math.
        code_.f64Const(0.0);
        loadSlotBits(slot);
        code_.localTee(bitsTemp_);
        code_.f64ReinterpretI64();
        code_.localGet(bitsTemp_);
        code_.i64Const(int64_t(xyo::val::kNan));
        code_.i64Eq();
        code_.select();
    }
    // Each Scratch numeric operator re-coerces its inputs; an intermediate
    // NaN from 0/0 is treated as zero by a later operator.
    void scratchNumber(uint32_t numberLocal) {
        code_.f64Const(0.0);
        code_.localGet(numberLocal);
        code_.localGet(numberLocal);
        code_.localGet(numberLocal);
        code_.f64Ne();
        code_.select();
    }
    void encodeNumber(uint32_t numberLocal) {
        // Canonicalize any NaN before publishing a Scratch Value. Select
        // preserves negative zero and all other finite/infinite f64 bits.
        code_.i64Const(int64_t(xyo::val::kNan));
        code_.localGet(numberLocal);
        code_.i64ReinterpretF64();
        code_.localGet(numberLocal);
        code_.localGet(numberLocal);
        code_.f64Ne();
        code_.select();
    }
    uint32_t allocate(Type t) {
        const uint32_t id = nextLocal_++;
        switch (t) {
        case Type::I1: case Type::I32: code_.local(xyo::wasm::ValType::I32); break;
        case Type::I64: code_.local(xyo::wasm::ValType::I64); break;
        case Type::F64: code_.local(xyo::wasm::ValType::F64); break;
        }
        return id;
    }
    void jump(BlockId from, BlockId to, bool branch = true) {
        const auto& phis = program_->ssa.cfg.block(to).phis;
        // Parallel moves: read *all* predecessor inputs before committing any.
        for (uint32_t i = 0; i < phis.size(); ++i) {
            const PhiInput* incoming = nullptr;
            for (const auto& candidate : phis[i].inputs) {
                if (candidate.predecessor == from) {
                    incoming = &candidate;
                    break;
                }
            }
            if (!incoming) { code_.unreachable(); return; }
            code_.localGet(locals_[incoming->value]);
            code_.localSet(temps_[to][i]);
        }
        for (uint32_t i = 0; i < phis.size(); ++i) {
            code_.localGet(temps_[to][i]);
            code_.localSet(locals_[phiDestinations_[to][i]]);
            if (resumable_) saveFrameValue(phiDestinations_[to][i]);
        }
        code_.i32Const(int32_t(to));
        code_.localSet(pc_);
        if (resumable_ && isYieldEdge(from, to)) {
            code_.localGet(1);
            code_.i64Const(int64_t(to));
            code_.i64Store(3, NumericExecutionAbi::byteOffset(NumericExecutionAbi::kPcWord));
            code_.i64Const(int64_t(NumericExecutionAbi::status(NumericRunStatus::Yielded))); // yielded
            code_.return_();
        } else if (branch) code_.br(1);
    }

    xyo::wasm::Code& code_;
    const Executable* program_ = nullptr;
    uint32_t nextLocal_ = 0;
    uint32_t pc_ = 0;
    std::vector<uint32_t> locals_;
    std::vector<uint32_t> slots_;
    std::vector<std::vector<uint32_t>> temps_;
    std::vector<std::vector<ValueId>> phiDestinations_;
    uint32_t bitsTemp_ = 0;
    uint32_t f64Temp_ = 0;
    uint32_t roundedTemp_ = 0;
    bool externalSlots_ = false;
    bool resumable_ = false;
};

} // namespace xyo::ir
