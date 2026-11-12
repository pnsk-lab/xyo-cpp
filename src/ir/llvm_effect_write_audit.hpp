// IR-001: post-optimization LLVM write-path audit.
// Compiler diagnostics only; NEVER a certificate for source deoptimization.
// A guarded path means that an external native write-fence call is ordered
// between a traced effect and a potential write on every intra-procedural
// CFG path. It proves neither inter-procedural effects nor GC/source parity.
#pragma once

#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xyo::ir {

struct LlvmEffectWriteAudit {
    uint64_t tracedEffects = 0;
    uint64_t checkpoints = 0;
    uint64_t writeFences = 0;
    uint64_t memoryWrites = 0;
    uint64_t unguardedWrites = 0;
    uint64_t unguardedOpaqueCalls = 0;
    uint64_t locallyProvenCalls = 0;
    uint64_t localStackCandidates = 0;
    uint64_t isolatedStackWrites = 0; // syntactically non-escaping, nonvolatile
    uint64_t unguardedIsolatedStackWrites = 0; // NOT a deopt proof
    uint64_t functionsWithCheckpointsButNoEffects = 0;
    // This flag is intentionally never an executable deopt guarantee.
    bool executable() const { return false; }

    LlvmEffectWriteAudit& operator+=(const LlvmEffectWriteAudit& other) {
        tracedEffects += other.tracedEffects;
        checkpoints += other.checkpoints;
        writeFences += other.writeFences;
        memoryWrites += other.memoryWrites;
        unguardedWrites += other.unguardedWrites;
        unguardedOpaqueCalls += other.unguardedOpaqueCalls;
        locallyProvenCalls += other.locallyProvenCalls;
        localStackCandidates += other.localStackCandidates;
        isolatedStackWrites += other.isolatedStackWrites;
        unguardedIsolatedStackWrites +=
            other.unguardedIsolatedStackWrites;
        functionsWithCheckpointsButNoEffects +=
            other.functionsWithCheckpointsButNoEffects;
        return *this;
    }
};

enum class EffectAuditInstruction : uint8_t {
    None, SourceEffect, WriteFence, Checkpoint, MemoryWrite, OpaqueCall
};

inline bool isIr001SourceEffect(const llvm::StringRef name) {
    return name == "xyo_ir001_broadcast_v_source" ||
           name == "xyo_ir001_pen_stamp_source";
}

inline EffectAuditInstruction classifyEffectAuditInstruction(
    const llvm::Instruction& instruction) {
    if (llvm::isa<llvm::StoreInst, llvm::AtomicRMWInst,
                  llvm::AtomicCmpXchgInst, llvm::FenceInst>(&instruction))
        return EffectAuditInstruction::MemoryWrite;
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        load && (load->isVolatile() || load->isAtomic()))
        return EffectAuditInstruction::MemoryWrite;
    if (llvm::isa<llvm::MemIntrinsic>(&instruction))
        return EffectAuditInstruction::MemoryWrite;
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (!call) return EffectAuditInstruction::None;
    const llvm::Function* called = call->getCalledFunction();
    if (called) {
        const auto name = called->getName();
        if (isIr001SourceEffect(name))
            return EffectAuditInstruction::SourceEffect;
        if (name == "xyo_ir001_vm_write")
            return EffectAuditInstruction::WriteFence;
        if (name == "xyo_ir001_effect_checkpoint")
            return EffectAuditInstruction::Checkpoint;
        // The native CallFrame helper is bookkeeping, not a Scratch
        // observable mutation. It does not close an IR-001 interval.
        if (name.starts_with("xyo_proc_frame_"))
            return EffectAuditInstruction::None;
    }
    // LLVM's own side-effect-free intrinsics have compiler-controlled
    // implementations. Everything ELSE crosses a trust boundary:
    // a host/external symbol may modify Scratch state despite a broad
    // readnone/readonly/nounwind attribute or unverified declarations.
    // Treat unknown indirect calls and inline asm the same way.
    if (called && called->isIntrinsic() && !call->mayHaveSideEffects())
        return EffectAuditInstruction::None;
    return EffectAuditInstruction::OpaqueCall;
}

// This narrow syntactic classification reduces audit noise ONLY.
// Never use it to declare LLVM SSA/source locals reconstructible.
// Escape is checked for the WHOLE alloca's use-def chain; indirect
// pointers, atomics, volatile stores and potentially captured buffers
// remain unproven. LLVM's capture analysis errs toward "captured".
inline bool isIr001IsolatedStackStore(const llvm::StoreInst& store) {
    if (store.isVolatile() || store.isAtomic()) return false;
    const llvm::Value* base =
        llvm::getUnderlyingObject(store.getPointerOperand());
    const auto* local = llvm::dyn_cast<llvm::AllocaInst>(base);
    return local && !llvm::PointerMayBeCaptured(local, true);
}

// Internal/private functions cannot be replaced by a foreign symbol.
// A direct callee is locally harmless ONLY if every reachable operation
// is a non-escaping nonvolatile local-store, read-only pure LLVM op,
// or another similarly proven internal callee. Recursive SCCs are
// rejected (Visiting -> Unsafe), along with indirect/unknown calls.
class LlvmLocallyPureCallees final {
public:
    bool proven(const llvm::Function* function) {
        if (!function || function->isDeclaration() ||
            !function->hasLocalLinkage() || function->isVarArg())
            return false;
        const auto found = state_.find(function);
        if (found != state_.end())
            return found->second == State::Proven;
        state_[function] = State::Visiting;
        bool pure = true;
        std::unordered_set<const llvm::BasicBlock*> reached;
        std::vector<const llvm::BasicBlock*> work{
            &function->getEntryBlock()};
        while (!work.empty()) {
            const auto* block = work.back();
            work.pop_back();
            if (!reached.insert(block).second) continue;
            for (const auto* successor : llvm::successors(block))
                work.push_back(successor);
        }
        for (const auto& block : *function) {
            if (!reached.contains(&block)) continue;
            for (const auto& instruction : block) {
                if (const auto* store =
                        llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
                    if (!isIr001IsolatedStackStore(*store)) pure = false;
                } else if (llvm::isa<llvm::AtomicRMWInst,
                                     llvm::AtomicCmpXchgInst,
                                     llvm::FenceInst,
                                     llvm::MemIntrinsic>(&instruction)) {
                    pure = false;
                } else if (const auto* load =
                               llvm::dyn_cast<llvm::LoadInst>(
                                   &instruction)) {
                    if (load->isVolatile() || load->isAtomic())
                        pure = false;
                } else if (const auto* call =
                               llvm::dyn_cast<llvm::CallBase>(
                                   &instruction)) {
                    const auto* callee = call->getCalledFunction();
                    if (call->isInlineAsm() || !callee ||
                        (call->mayThrow() && !callee->doesNotThrow()) ||
                        isIr001SourceEffect(callee->getName()) ||
                        callee->getName() == "xyo_ir001_vm_write" ||
                        callee->getName() == "xyo_ir001_effect_checkpoint" ||
                        callee->getName().starts_with("xyo_proc_frame_")) {
                        pure = false;
                    } else if (callee->hasLocalLinkage() &&
                               !callee->isDeclaration()) {
                        if (!proven(callee)) pure = false;
                    } else if (!callee->isIntrinsic() ||
                               call->mayHaveSideEffects()) {
                        // No external host callback is trusted merely
                        // because the IR carries purity attributes.
                        pure = false;
                    }
                } else if (instruction.mayHaveSideEffects()) {
                    pure = false;
                }
            }
        }
        state_[function] = pure ? State::Proven : State::Unsafe;
        return pure;
    }

    bool provenCall(const llvm::CallBase& call) {
        const auto* target = call.getCalledFunction();
        return !call.isInlineAsm() && target &&
               (!call.mayThrow() || target->doesNotThrow()) &&
               proven(target);
    }

private:
    enum class State : uint8_t { Visiting, Proven, Unsafe };
    std::unordered_map<const llvm::Function*, State> state_;
};

inline EffectAuditInstruction classifyInterproceduralEffect(
    const llvm::Instruction& instruction,
    LlvmLocallyPureCallees* callees) {
    const auto initial = classifyEffectAuditInstruction(instruction);
    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (!call || !callees) return initial;
    const auto* callee = call->getCalledFunction();
    if (callee && callee->hasLocalLinkage() &&
        !callee->isDeclaration()) {
        return callees->provenCall(*call)
            ? EffectAuditInstruction::None
            : EffectAuditInstruction::OpaqueCall;
    }
    if (call->isInlineAsm() && call->mayHaveSideEffects())
        return EffectAuditInstruction::OpaqueCall;
    return initial;
}

inline LlvmEffectWriteAudit auditLlvmEffectWrites(
    const llvm::Function& function,
    LlvmLocallyPureCallees* callees = nullptr) {
    LlvmEffectWriteAudit result;
    if (function.isDeclaration()) return result;

    std::unordered_set<const llvm::BasicBlock*> reachable;
    std::vector<const llvm::BasicBlock*> work{&function.getEntryBlock()};
    while (!work.empty()) {
        auto* block = work.back();
        work.pop_back();
        if (!reachable.insert(block).second) continue;
        for (const llvm::BasicBlock* succ : llvm::successors(block))
            work.push_back(succ);
    }

    // Forward dataflow, two-state may-analysis. A traced delivery opens
    // an interval; the write fence expires it; a checkpoint closes it.
    // CFG joins use OR. Fixpoint accounts for loop-carried intervals.
    std::unordered_map<const llvm::BasicBlock*, bool> outPending;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& block : function) {
            if (!reachable.contains(&block)) continue;
            bool pending = false;
            for (const llvm::BasicBlock* pred : llvm::predecessors(&block))
                if (reachable.contains(pred))
                    pending = pending || outPending[pred];
            for (const auto& instruction : block) {
                switch (classifyInterproceduralEffect(instruction, callees)) {
                case EffectAuditInstruction::SourceEffect:
                    pending = true;
                    break;
                case EffectAuditInstruction::WriteFence:
                case EffectAuditInstruction::Checkpoint:
                    pending = false;
                    break;
                default:
                    break;
                }
            }
            if (outPending[&block] != pending) {
                outPending[&block] = pending;
                changed = true;
            }
        }
    }

    for (const auto& block : function) {
        if (!reachable.contains(&block)) continue;
        bool pending = false;
        for (const llvm::BasicBlock* pred : llvm::predecessors(&block))
            if (reachable.contains(pred))
                pending = pending || outPending[pred];
        for (const auto& instruction : block) {
            const auto kind =
                classifyInterproceduralEffect(instruction, callees);
            if (const auto* call =
                    llvm::dyn_cast<llvm::CallBase>(&instruction);
                call && callees && callees->provenCall(*call))
                ++result.locallyProvenCalls;
            switch (kind) {
            case EffectAuditInstruction::SourceEffect:
                ++result.tracedEffects;
                pending = true;
                break;
            case EffectAuditInstruction::WriteFence:
                ++result.writeFences;
                pending = false;
                break;
            case EffectAuditInstruction::Checkpoint:
                ++result.checkpoints;
                pending = false;
                break;
            case EffectAuditInstruction::MemoryWrite: {
                ++result.memoryWrites;
                const auto* store =
                    llvm::dyn_cast<llvm::StoreInst>(&instruction);
                const bool isolated =
                    store && isIr001IsolatedStackStore(*store);
                if (store && llvm::isa<llvm::AllocaInst>(
                        llvm::getUnderlyingObject(
                            store->getPointerOperand())))
                    ++result.localStackCandidates;
                if (isolated) {
                    ++result.isolatedStackWrites;
                    if (pending)
                        ++result.unguardedIsolatedStackWrites;
                } else if (pending) {
                    ++result.unguardedWrites;
                }
                break;
            }
            case EffectAuditInstruction::OpaqueCall:
                if (pending) ++result.unguardedOpaqueCalls;
                break;
            case EffectAuditInstruction::None:
                break;
            }
        }
    }
    if (result.checkpoints && !result.tracedEffects)
        result.functionsWithCheckpointsButNoEffects = 1;
    return result;
}

inline LlvmEffectWriteAudit auditLlvmEffectWrites(
    const llvm::Module& module) {
    LlvmLocallyPureCallees callees;
    LlvmEffectWriteAudit result;
    for (const auto& function : module)
        result += auditLlvmEffectWrites(function, &callees);
    return result;
}

// A second, checkpoint-specific forward may-analysis. Unlike the
// function-wide diagnostic counters, this follows unguarded writes to
// the precise checkpoint where they can affect source continuation.
//
// NoSource is a may-bit: true when SOME CFG path reaches a checkpoint
// without a local, newly observed source event since its last checkpoint.
// A native write fence cancels pending unproven writes (the real runtime
// has already invalidated its diagnostic), but it does not invent a
// missing source event. CFG joins use OR and loops run to a fixed point.
struct LlvmEffectCheckpointState {
    bool activeSource = false;
    bool unguardedWrite = false;
    bool possibleWithoutSource = false;
    bool operator==(const LlvmEffectCheckpointState&) const = default;

    void merge(const LlvmEffectCheckpointState& other) {
        activeSource |= other.activeSource;
        unguardedWrite |= other.unguardedWrite;
        possibleWithoutSource |= other.possibleWithoutSource;
    }
};

inline void advanceLlvmEffectCheckpointState(
    LlvmEffectCheckpointState& state,
    const llvm::Instruction& instruction,
    const std::unordered_map<const llvm::AllocaInst*, bool>& isolated,
    LlvmLocallyPureCallees* callees) {
    switch (classifyInterproceduralEffect(instruction, callees)) {
    case EffectAuditInstruction::SourceEffect:
        state.activeSource = true;
        state.possibleWithoutSource = false;
        break;
    case EffectAuditInstruction::WriteFence:
        state.activeSource = false;
        state.unguardedWrite = false;
        break;
    case EffectAuditInstruction::Checkpoint:
        state = {false, false, true};
        break;
    case EffectAuditInstruction::OpaqueCall:
        if (state.activeSource) state.unguardedWrite = true;
        break;
    case EffectAuditInstruction::MemoryWrite: {
        if (!state.activeSource) break;
        const auto* store =
            llvm::dyn_cast<llvm::StoreInst>(&instruction);
        bool safeLocal = false;
        if (store) {
            const auto* local = llvm::dyn_cast<llvm::AllocaInst>(
                llvm::getUnderlyingObject(store->getPointerOperand()));
            if (local) {
                const auto found = isolated.find(local);
                safeLocal = found != isolated.end() && found->second &&
                            !store->isVolatile() && !store->isAtomic();
            }
        }
        if (!safeLocal) state.unguardedWrite = true;
        break;
    }
    case EffectAuditInstruction::None:
        break;
    }
}

inline std::vector<llvm::CallBase*> findUnprovenLlvmEffectCheckpoints(
    llvm::Function& function,
    LlvmLocallyPureCallees* callees = nullptr) {
    std::vector<llvm::CallBase*> unproven;
    if (function.isDeclaration()) return unproven;

    std::unordered_set<const llvm::BasicBlock*> reachable;
    std::vector<const llvm::BasicBlock*> work{&function.getEntryBlock()};
    while (!work.empty()) {
        auto* block = work.back();
        work.pop_back();
        if (!reachable.insert(block).second) continue;
        for (const llvm::BasicBlock* succ : llvm::successors(block))
            work.push_back(succ);
    }

    // Cache the expensive whole-alloca capture walk once per allocation.
    std::unordered_map<const llvm::AllocaInst*, bool> isolated;
    for (const auto& block : function)
        for (const auto& instruction : block)
            if (const auto* local =
                    llvm::dyn_cast<llvm::AllocaInst>(&instruction))
                isolated.emplace(
                    local, !llvm::PointerMayBeCaptured(local, true));

    std::unordered_map<const llvm::BasicBlock*,
                       LlvmEffectCheckpointState> out;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& block : function) {
            if (!reachable.contains(&block)) continue;
            LlvmEffectCheckpointState state;
            if (&block == &function.getEntryBlock())
                state.possibleWithoutSource = true;
            for (const llvm::BasicBlock* pred : llvm::predecessors(&block))
                if (reachable.contains(pred)) state.merge(out[pred]);
            for (const auto& instruction : block)
                advanceLlvmEffectCheckpointState(
                    state, instruction, isolated, callees);
            if (!(out[&block] == state)) {
                out[&block] = state;
                changed = true;
            }
        }
    }

    for (auto& block : function) {
        if (!reachable.contains(&block)) continue;
        LlvmEffectCheckpointState state;
        if (&block == &function.getEntryBlock())
            state.possibleWithoutSource = true;
        for (const llvm::BasicBlock* pred : llvm::predecessors(&block))
            if (reachable.contains(pred)) state.merge(out[pred]);
        for (auto& instruction : block) {
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                if (const auto* callee = call->getCalledFunction();
                    callee &&
                    callee->getName() == "xyo_ir001_effect_checkpoint" &&
                    call->arg_size() >= 1 &&
                    call->getArgOperand(0)->getType()->isPointerTy() &&
                    (state.unguardedWrite ||
                     state.possibleWithoutSource)) {
                    // An existing immediate fence already invalidates
                    // this checkpoint, so repeated audits are idempotent.
                    const auto* previous = call->getPrevNode();
                    const auto* previousCall =
                        llvm::dyn_cast_or_null<llvm::CallBase>(previous);
                    if (!previousCall ||
                        !previousCall->getCalledFunction() ||
                        previousCall->getCalledFunction()->getName() !=
                            "xyo_ir001_vm_write")
                        unproven.push_back(call);
                }
            }
            advanceLlvmEffectCheckpointState(
                state, instruction, isolated, callees);
        }
    }
    return unproven;
}

// Opt-in and intentionally over-conservative. Runtime source-state
// diagnostics are expired immediately BEFORE each locally unproven
// checkpoint. It never changes Scratch state, permits detachment or
// asserts source SSA / GC / inter-procedural effect completeness.
inline uint64_t fenceUnprovenLlvmEffectCheckpoints(llvm::Module& module) {
    uint64_t fenced = 0;
    LlvmLocallyPureCallees callees;
    for (auto& function : module) {
        auto checkpoints = findUnprovenLlvmEffectCheckpoints(
            function, &callees);
        if (checkpoints.empty()) continue;
        auto barrier = module.getOrInsertFunction(
            "xyo_ir001_vm_write",
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(module.getContext()),
                {llvm::PointerType::get(module.getContext(), 0)}, false));
        for (auto* checkpoint : checkpoints) {
            llvm::IRBuilder<> ir(checkpoint);
            ir.CreateCall(barrier, {checkpoint->getArgOperand(0)});
            ++fenced;
        }
    }
    return fenced;
}

} // namespace xyo::ir
