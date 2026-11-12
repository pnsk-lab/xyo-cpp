// RT-001: additional native LLVM SSA diagnostic capture candidates.
// This report is deliberately NOT a certificate for deoptimization:
// captured i64 arguments can contain GC-sensitive Scratch Value bits,
// pointers are never copied, and source effects are not modeled.
#pragma once

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

namespace xyo::rt {

// A conservative origin classification, NOT a GC ownership proof.
// A derived pointer may point to the stack, static data, or managed
// heap; any unresolved pointer still disqualifies full SSA recovery.
enum class PointerOrigin : uint32_t {
    Argument, StackAlloca, DerivedGepOrCast, CallResult,
    LoadedPointer, PhiOrSelect, Other, Count
};
inline constexpr size_t kPointerOriginCount =
    static_cast<size_t>(PointerOrigin::Count);

inline PointerOrigin classifyPointerOrigin(const llvm::Value& value) {
    if (llvm::isa<llvm::Argument>(value)) return PointerOrigin::Argument;
    if (llvm::isa<llvm::AllocaInst>(value)) return PointerOrigin::StackAlloca;
    if (llvm::isa<llvm::GetElementPtrInst>(value) ||
        llvm::isa<llvm::BitCastInst>(value) ||
        llvm::isa<llvm::AddrSpaceCastInst>(value))
        return PointerOrigin::DerivedGepOrCast;
    if (llvm::isa<llvm::CallBase>(value))
        return PointerOrigin::CallResult;
    if (llvm::isa<llvm::LoadInst>(value))
        return PointerOrigin::LoadedPointer;
    if (llvm::isa<llvm::PHINode>(value) ||
        llvm::isa<llvm::SelectInst>(value))
        return PointerOrigin::PhiOrSelect;
    return PointerOrigin::Other;
}

// RT-001: conservative NATIVE ACTIVATION stack lineage. Returning an
// AllocaInst means the pointer depends only on this single current
// LLVM function's stack object, not that it can be kept after Fiber
// destruction. It MUST NOT be treated as a GC root or as a restorable
// interpreter pointer. Cycles and conflicting PHI/SELECT arms reject.
inline const llvm::AllocaInst* nativeStackPointerRoot(
    const llvm::Value* pointer, std::unordered_set<const llvm::Value*>& path,
    unsigned depth = 0) {
    if (!pointer || !pointer->getType()->isPointerTy() || depth >= 64 ||
        !path.insert(pointer).second)
        return nullptr;
    const llvm::AllocaInst* result = nullptr;
    if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(pointer)) {
        result = alloca;
    } else if (const auto* gep =
                   llvm::dyn_cast<llvm::GetElementPtrInst>(pointer)) {
        result = nativeStackPointerRoot(
            gep->getPointerOperand(), path, depth + 1);
    } else if (const auto* cast =
                   llvm::dyn_cast<llvm::BitCastInst>(pointer)) {
        result = nativeStackPointerRoot(
            cast->getOperand(0), path, depth + 1);
    } else if (const auto* phi =
                   llvm::dyn_cast<llvm::PHINode>(pointer)) {
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            const auto* root = nativeStackPointerRoot(
                phi->getIncomingValue(i), path, depth + 1);
            if (!root || (result && result != root)) {
                result = nullptr;
                break;
            }
            result = root;
        }
    } else if (const auto* select =
                   llvm::dyn_cast<llvm::SelectInst>(pointer)) {
        const auto* yes = nativeStackPointerRoot(
            select->getTrueValue(), path, depth + 1);
        const auto* no = nativeStackPointerRoot(
            select->getFalseValue(), path, depth + 1);
        if (yes && yes == no) result = yes;
    }
    // AddrSpaceCast, IntToPtr, Load, Call and pointer Arguments all
    // have unverified allocation/GC ownership and stay unresolved.
    path.erase(pointer);
    return result;
}

inline const llvm::AllocaInst* nativeStackPointerRoot(
    const llvm::Value* pointer) {
    std::unordered_set<const llvm::Value*> path;
    return nativeStackPointerRoot(pointer, path);
}

// RT-001 compiler-only GC hazard census. A region describes the
// ADDRESS held in a live pointer SSA word, not the lifetime or GC
// reachability of any object loaded FROM that address. In particular,
// NativeStack can contain boxed GC values, and HostContext can contain
// pointers to managed objects. None of these labels authorizes a
// collection, deoptimization, or raw pointer restoration.
enum class PointerRegion : uint32_t {
    NativeStack = 0,
    HostContext = 1,
    StaticGlobal = 2,
    Unknown = 3,
    Count
};
inline constexpr size_t kPointerRegionCount =
    static_cast<size_t>(PointerRegion::Count);

// Only the explicit ABI-proven JitCtx* argument may be host-owned.
// Do not infer this from a function name or arbitrary pointer argument.
// Loads, IntToPtr, AddrSpaceCast, calls, cycles and mixed PHI/SELECT
// always fail closed; even a load *from* JitCtx may return an SStr*.
// NativeStack is accepted only with the stricter single-alloca proof.
inline PointerRegion classifyPointerRegion(
    const llvm::Value* pointer, const llvm::Argument* trustedContext,
    std::unordered_set<const llvm::Value*>& path,
    unsigned depth = 0) {
    using Region = PointerRegion;
    if (!pointer || !pointer->getType()->isPointerTy() ||
        depth >= 64 || !path.insert(pointer).second)
        return Region::Unknown;
    Region result = Region::Unknown;
    if (nativeStackPointerRoot(pointer)) {
        result = Region::NativeStack;
    } else if (pointer == trustedContext && trustedContext) {
        result = Region::HostContext;
    } else if (llvm::isa<llvm::GlobalVariable>(pointer)) {
        result = Region::StaticGlobal;
    } else if (const auto* constant =
                   llvm::dyn_cast<llvm::ConstantExpr>(pointer)) {
        // LLVM folds static GEP/bitcast address computations into
        // ConstantExpr rather than ordinary SSA instructions. A
        // constant-int address, ptrtoint/inttoptr or a load is NOT
        // equivalent to a static allocation.
        if (constant->getOpcode() == llvm::Instruction::GetElementPtr ||
            constant->getOpcode() == llvm::Instruction::BitCast)
            result = classifyPointerRegion(
                constant->getOperand(0), trustedContext, path, depth + 1);
        if (result != Region::StaticGlobal)
            result = Region::Unknown;
    } else if (const auto* gep =
                   llvm::dyn_cast<llvm::GetElementPtrInst>(pointer)) {
        const Region source = classifyPointerRegion(
            gep->getPointerOperand(), trustedContext, path, depth + 1);
        if (source == Region::HostContext ||
            source == Region::StaticGlobal)
            result = source;
    } else if (const auto* cast =
                   llvm::dyn_cast<llvm::BitCastInst>(pointer)) {
        const Region source = classifyPointerRegion(
            cast->getOperand(0), trustedContext, path, depth + 1);
        if (source == Region::HostContext ||
            source == Region::StaticGlobal)
            result = source;
    } else if (const auto* phi =
                   llvm::dyn_cast<llvm::PHINode>(pointer)) {
        if (phi->getNumIncomingValues() != 0) {
            const Region first = classifyPointerRegion(
                phi->getIncomingValue(0), trustedContext, path, depth + 1);
            if (first == Region::HostContext ||
                first == Region::StaticGlobal) {
                result = first;
                for (unsigned i = 1; i < phi->getNumIncomingValues(); ++i)
                    if (classifyPointerRegion(
                            phi->getIncomingValue(i), trustedContext,
                            path, depth + 1) != first) {
                        result = Region::Unknown;
                        break;
                    }
            }
        }
    } else if (const auto* choice =
                   llvm::dyn_cast<llvm::SelectInst>(pointer)) {
        const Region yes = classifyPointerRegion(
            choice->getTrueValue(), trustedContext, path, depth + 1);
        if (yes == Region::HostContext || yes == Region::StaticGlobal) {
            const Region no = choice->getTrueValue() == choice->getFalseValue()
                ? yes : classifyPointerRegion(
                    choice->getFalseValue(), trustedContext, path, depth + 1);
            if (yes == no) result = yes;
        }
    }
    path.erase(pointer);
    return result;
}

inline PointerRegion classifyPointerRegion(
    const llvm::Value* pointer,
    const llvm::Argument* trustedContext = nullptr) {
    std::unordered_set<const llvm::Value*> path;
    return classifyPointerRegion(pointer, trustedContext, path);
}

// Compiler-only cause of a pointer whose OWN address has unproven
// provenance. A load's address region is NOT provenance of its value.
enum class UnknownPointerCause : uint32_t {
    LoadStack, LoadHost, LoadStatic, LoadUnknown,
    CallReturn, IntOrAddressCast, ForeignArgument, PhiSelect, Other, Count
};
inline constexpr size_t kUnknownPointerCauseCount =
    static_cast<size_t>(UnknownPointerCause::Count);

inline const char* unknownPointerCauseName(UnknownPointerCause cause) {
    switch (cause) {
    case UnknownPointerCause::LoadStack: return "load_stack";
    case UnknownPointerCause::LoadHost: return "load_host";
    case UnknownPointerCause::LoadStatic: return "load_static";
    case UnknownPointerCause::LoadUnknown: return "load_unknown";
    case UnknownPointerCause::CallReturn: return "call_return";
    case UnknownPointerCause::IntOrAddressCast: return "int_or_addr_cast";
    case UnknownPointerCause::ForeignArgument: return "foreign_argument";
    case UnknownPointerCause::PhiSelect: return "phi_select";
    case UnknownPointerCause::Other: return "other";
    case UnknownPointerCause::Count: break;
    }
    return "invalid";
}
inline UnknownPointerCause classifyUnknownPointerCause(
    const llvm::Value* value, const llvm::Argument* trustedContext,
    std::unordered_set<const llvm::Value*>& active, unsigned depth = 0) {
    using Cause = UnknownPointerCause;
    if (!value || !value->getType()->isPointerTy() ||
        depth >= 64 || !active.insert(value).second)
        return Cause::Other;
    Cause result = Cause::Other;
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value)) {
        switch (classifyPointerRegion(load->getPointerOperand(), trustedContext)) {
        case PointerRegion::NativeStack: result = Cause::LoadStack; break;
        case PointerRegion::HostContext: result = Cause::LoadHost; break;
        case PointerRegion::StaticGlobal: result = Cause::LoadStatic; break;
        default: result = Cause::LoadUnknown; break;
        }
    } else if (llvm::isa<llvm::CallBase>(value)) {
        result = Cause::CallReturn;
    } else if (llvm::isa<llvm::IntToPtrInst>(value) ||
               llvm::isa<llvm::AddrSpaceCastInst>(value)) {
        result = Cause::IntOrAddressCast;
    } else if (llvm::isa<llvm::Argument>(value)) {
        result = Cause::ForeignArgument;
    } else if (llvm::isa<llvm::PHINode>(value) ||
               llvm::isa<llvm::SelectInst>(value)) {
        result = Cause::PhiSelect;
    } else if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
        result = classifyUnknownPointerCause(
            gep->getPointerOperand(), trustedContext, active, depth + 1);
    } else if (const auto* cast = llvm::dyn_cast<llvm::BitCastInst>(value)) {
        result = classifyUnknownPointerCause(
            cast->getOperand(0), trustedContext, active, depth + 1);
    } else if (const auto* expr = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
        if (expr->getOpcode() == llvm::Instruction::GetElementPtr ||
            expr->getOpcode() == llvm::Instruction::BitCast)
            result = classifyUnknownPointerCause(
                expr->getOperand(0), trustedContext, active, depth + 1);
        else if (expr->getOpcode() == llvm::Instruction::IntToPtr ||
                 expr->getOpcode() == llvm::Instruction::AddrSpaceCast)
            result = Cause::IntOrAddressCast;
    }
    active.erase(value);
    return result;
}
inline UnknownPointerCause classifyUnknownPointerCause(
    const llvm::Value* value, const llvm::Argument* trustedContext = nullptr) {
    std::unordered_set<const llvm::Value*> active;
    return classifyUnknownPointerCause(value, trustedContext, active);
}

// Follow the *address expression* of an unknown pointer-valued load.
// Each encountered LLVM Load dereferences memory whose contents might
// themselves be a GC pointer, regardless of the final address region.
// This is diagnostic only, not a transitive GC root/provenance proof.
// Mixed PHI/SELECT and cycles terminate the analysis without guessing.
enum class PointerLoadTerminal : uint32_t {
    NativeStack, HostContext, StaticGlobal, ForeignArgument,
    CallReturn, IntegerOrAddressCast, Merge, Cycle, DepthLimit, Other
};
inline const char* pointerLoadTerminalName(PointerLoadTerminal terminal) {
    switch (terminal) {
    case PointerLoadTerminal::NativeStack: return "stack";
    case PointerLoadTerminal::HostContext: return "host";
    case PointerLoadTerminal::StaticGlobal: return "static";
    case PointerLoadTerminal::ForeignArgument: return "foreign_arg";
    case PointerLoadTerminal::CallReturn: return "call_return";
    case PointerLoadTerminal::IntegerOrAddressCast: return "int_or_addr_cast";
    case PointerLoadTerminal::Merge: return "merge";
    case PointerLoadTerminal::Cycle: return "cycle";
    case PointerLoadTerminal::DepthLimit: return "depth_limit";
    case PointerLoadTerminal::Other: return "other";
    }
    return "invalid";
}
// Returns the byte offset of an address derived ONLY from a native
// JitCtx pointer via single-index constant i8 or ptr GEPs and bitcasts.
// This is a field locator, not a GC root or a lifetime/alias proof.
inline std::optional<uint64_t> constantHostContextOffset(
    const llvm::Value* address, const llvm::Argument* trustedContext) {
    if (!trustedContext || !address) return std::nullopt;
    std::unordered_set<const llvm::Value*> seen;
    uint64_t offset = 0;
    for (unsigned depth = 0; depth < 64; ++depth) {
        if (!address || !seen.insert(address).second) return std::nullopt;
        if (address == trustedContext) return offset;
        if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(address)) {
            if (gep->getNumIndices() != 1) return std::nullopt;
            const auto* index =
                llvm::dyn_cast<llvm::ConstantInt>(gep->idx_begin()->get());
            if (!index || index->isNegative() ||
                index->getValue().getActiveBits() > 63)
                return std::nullopt;
            const llvm::Type* element = gep->getSourceElementType();
            const uint64_t stride = element->isPointerTy()
                ? sizeof(void*) : element->isIntegerTy(8) ? 1 : 0;
            if (!stride) return std::nullopt;
            const uint64_t n = index->getZExtValue();
            if (n > (std::numeric_limits<uint64_t>::max() - offset) / stride)
                return std::nullopt;
            offset += n * stride;
            address = gep->getPointerOperand();
        } else if (const auto* cast =
                       llvm::dyn_cast<llvm::BitCastInst>(address)) {
            address = cast->getOperand(0);
        } else if (const auto* expr =
                       llvm::dyn_cast<llvm::ConstantExpr>(address)) {
            if (expr->getOpcode() != llvm::Instruction::BitCast)
                return std::nullopt;
            address = expr->getOperand(0);
        } else {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

struct PointerLoadChain {
    PointerLoadTerminal terminal = PointerLoadTerminal::Other;
    uint32_t dereferences = 0;
    uint32_t addressDerivations = 0;
    // The FIRST dereferenced pointer field in JitCtx, if its offset
    // is a single-index constant GEP. Never a GC ownership proof.
    std::optional<uint64_t> hostContextFieldOffset;
};
inline PointerLoadChain tracePointerLoadChain(
    const llvm::Value* value, const llvm::Argument* trustedContext = nullptr) {
    PointerLoadChain result;
    std::unordered_set<const llvm::Value*> seen;
    const llvm::Value* innermostLoadAddress = nullptr;
    // At most 64 distinct SSA/constant nodes. Do not recurse into PHI
    // arms; choosing one arm could silently hide a GC heap pointer.
    for (unsigned depth = 0; depth < 64; ++depth) {
        if (!value || !value->getType()->isPointerTy()) return result;
        if (!seen.insert(value).second) {
            result.terminal = PointerLoadTerminal::Cycle;
            return result;
        }
        if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value)) {
            ++result.dereferences;
            // Last load in reverse order is the first dereference.
            innermostLoadAddress = load->getPointerOperand();
            value = innermostLoadAddress;
        } else if (const auto* gep =
                       llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
            ++result.addressDerivations;
            value = gep->getPointerOperand();
        } else if (const auto* cast =
                       llvm::dyn_cast<llvm::BitCastInst>(value)) {
            ++result.addressDerivations;
            value = cast->getOperand(0);
        } else if (const auto* expr =
                       llvm::dyn_cast<llvm::ConstantExpr>(value)) {
            if (expr->getOpcode() == llvm::Instruction::GetElementPtr ||
                expr->getOpcode() == llvm::Instruction::BitCast) {
                ++result.addressDerivations;
                value = expr->getOperand(0);
            } else if (expr->getOpcode() == llvm::Instruction::IntToPtr ||
                       expr->getOpcode() == llvm::Instruction::AddrSpaceCast) {
                result.terminal = PointerLoadTerminal::IntegerOrAddressCast;
                return result;
            } else {
                return result;
            }
        } else if (llvm::isa<llvm::AllocaInst>(value)) {
            result.terminal = PointerLoadTerminal::NativeStack;
            return result;
        } else if (value == trustedContext && trustedContext) {
            result.terminal = PointerLoadTerminal::HostContext;
            result.hostContextFieldOffset =
                constantHostContextOffset(innermostLoadAddress, trustedContext);
            return result;
        } else if (llvm::isa<llvm::GlobalVariable>(value)) {
            result.terminal = PointerLoadTerminal::StaticGlobal;
            return result;
        } else if (llvm::isa<llvm::Argument>(value)) {
            result.terminal = PointerLoadTerminal::ForeignArgument;
            return result;
        } else if (llvm::isa<llvm::CallBase>(value)) {
            result.terminal = PointerLoadTerminal::CallReturn;
            return result;
        } else if (llvm::isa<llvm::IntToPtrInst>(value) ||
                   llvm::isa<llvm::AddrSpaceCastInst>(value)) {
            result.terminal = PointerLoadTerminal::IntegerOrAddressCast;
            return result;
        } else if (llvm::isa<llvm::PHINode>(value) ||
                   llvm::isa<llvm::SelectInst>(value)) {
            result.terminal = PointerLoadTerminal::Merge;
            return result;
        } else {
            return result;
        }
    }
    result.terminal = PointerLoadTerminal::DepthLimit;
    return result;
}

struct UnknownLoadChainDetail {
    uint32_t instructionId = 0;
    PointerLoadChain chain;
};

struct UnknownPointerDetail {
    uint32_t instructionId = 0;
    UnknownPointerCause cause = UnknownPointerCause::Other;
};

// RT-001: exact, *bit-preserving* ancestry of an i64 SSA value.
// A nonzero result says that every possible value is the unchanged
// original native procedure i64 argument N (N >= 1). This is NOT a
// general pointer/GC provenance proof. Arithmetic, casts, loads,
// freezes, calls and mixed PHI/SELECT inputs are deliberately excluded.
// Self-referential PHI cycles are fail-closed: even a loop phi with one
// argument incoming may carry undefined/poison without a full proof.
inline uint32_t sameBoxedArgumentRoot(
    const llvm::Value* value,
    std::unordered_set<const llvm::Value*>& active,
    unsigned& budget, unsigned depth = 0) {
    // A DAG can share the same deep subgraph between both SELECT arms.
    // Bound total work as well as recursion depth: never let codegen
    // grow exponentially while resolving a diagnostic alias.
    if (!value || !value->getType()->isIntegerTy(64) ||
        depth >= 64 || budget == 0 || !active.insert(value).second)
        return 0;
    --budget;
    uint32_t result = 0;
    if (const auto* arg = llvm::dyn_cast<llvm::Argument>(value)) {
        const uint64_t index = arg->getArgNo();
        if (index > 0 && index <= UINT32_MAX)
            result = uint32_t(index);
    } else if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            const uint32_t incoming = sameBoxedArgumentRoot(
                phi->getIncomingValue(i), active, budget, depth + 1);
            if (!incoming || (result && result != incoming)) {
                result = 0;
                break;
            }
            result = incoming;
        }
    } else if (const auto* choice = llvm::dyn_cast<llvm::SelectInst>(value)) {
        const auto* yes = choice->getTrueValue();
        const auto* no = choice->getFalseValue();
        const uint32_t a = sameBoxedArgumentRoot(
            yes, active, budget, depth + 1);
        const uint32_t b = yes == no ? a :
            sameBoxedArgumentRoot(
                no, active, budget, depth + 1);
        if (a && a == b) result = a;
    }
    active.erase(value);
    return result;
}

inline uint32_t sameBoxedArgumentRoot(const llvm::Value* value) {
    std::unordered_set<const llvm::Value*> active;
    unsigned budget = 512;
    return sameBoxedArgumentRoot(value, active, budget);
}

// RT-001: a separate certificate for a boxed string generated IN the
// current native activation, not derived from a source procedure argument.
// A known host join result can only be an emitted boxed Scratch string
// (or undefined if its effect returns an explicit error). A PHI/SELECT
// retains this classification ONLY if EVERY possible incoming arm is
// similarly classified. The returned bit is compiler provenance, NOT
// permission to interpret an arbitrary SSA word as a GC object.
// Actual heap membership and type are checked at the host safe point.
inline bool trustedGeneratedStringSource(
    const llvm::Value* value,
    std::unordered_set<const llvm::Value*>& active,
    unsigned& budget, unsigned depth = 0) {
    if (!value || !value->getType()->isIntegerTy(64) ||
        depth >= 64 || budget == 0 || !active.insert(value).second)
        return false;
    --budget;
    bool result = false;
    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(value)) {
        const auto* target = call->getCalledFunction();
        if (target) {
            const auto name = target->getName();
            const auto* signature = target->getFunctionType();
            // Reject any symbol-name match with an incorrect
            // Scratch boxed-Value host helper function signature.
            result = !signature->isVarArg() &&
                     signature->getReturnType()->isIntegerTy(64) &&
                     signature->getNumParams() == 2 &&
                     signature->getParamType(0)->isIntegerTy(64) &&
                     signature->getParamType(1)->isIntegerTy(64) &&
                     call->arg_size() == 2 &&
                     (name == "xyo_join" ||
                      name == "xyo_join_suffix" ||
                      name == "xyo_join_strings");
        }
    } else if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value)) {
        result = phi->getNumIncomingValues() > 0;
        for (unsigned i = 0; result && i < phi->getNumIncomingValues(); ++i)
            result = trustedGeneratedStringSource(
                phi->getIncomingValue(i), active, budget, depth + 1);
    } else if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value)) {
        const auto* yes = select->getTrueValue();
        const auto* no = select->getFalseValue();
        result = trustedGeneratedStringSource(yes, active, budget,
                                              depth + 1);
        if (result && yes != no)
            result = trustedGeneratedStringSource(no, active, budget,
                                                   depth + 1);
    }
    active.erase(value);
    return result;
}

inline bool trustedGeneratedStringSource(const llvm::Value* value) {
    std::unordered_set<const llvm::Value*> active;
    unsigned budget = 512;
    return trustedGeneratedStringSource(value, active, budget);
}

// A live i64 Load can contain a boxed Scratch string. Recognize only
// the exact compiler-native Variable::value address shapes. This is an
// address-origin proof, NOT a proof that the value is a GC object or that
// it stays rooted after an asynchronous suspension.
// Offsets come from the compiled JitCtx/Variable ABI, never from IR hints.
enum class NativeVariableValueOrigin : uint8_t {
    Unknown, SlotArray, CheckedResolver
};
inline NativeVariableValueOrigin classifyNativeVariableValueLoad(
    const llvm::Value* value, const llvm::Argument* trustedContext,
    uint64_t varSlotDataOffset, uint64_t variableValueOffset) {
    using Origin = NativeVariableValueOrigin;
    if (!trustedContext || trustedContext->getArgNo() != 0 ||
        !trustedContext->getType()->isPointerTy() ||
        !value || !value->getType()->isIntegerTy(64))
        return Origin::Unknown;
    const auto* word = llvm::dyn_cast<llvm::LoadInst>(value);
    if (!word || word->isVolatile() || word->isAtomic())
        return Origin::Unknown;
    const auto* field = llvm::dyn_cast<llvm::GEPOperator>(
        word->getPointerOperand());
    if (!field || !field->getSourceElementType()->isIntegerTy(8) ||
        field->getNumIndices() != 1)
        return Origin::Unknown;
    const auto* fieldIndex = llvm::dyn_cast<llvm::ConstantInt>(
        field->idx_begin()->get());
    if (!fieldIndex || fieldIndex->isNegative() ||
        fieldIndex->getValue().getActiveBits() > 63 ||
        fieldIndex->getZExtValue() != variableValueOffset)
        return Origin::Unknown;

    const llvm::Value* owner = field->getPointerOperand();
    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(owner)) {
        const llvm::Function* helper = call->getCalledFunction();
        if (!helper || helper->getName() != "xyo_varslot_checked" ||
            helper->isVarArg() || call->arg_size() != 5 ||
            call->getArgOperand(0) != trustedContext ||
            !helper->getReturnType()->isPointerTy() ||
            helper->getFunctionType()->getNumParams() != 5 ||
            !helper->getFunctionType()->getParamType(0)->isPointerTy() ||
            !helper->getFunctionType()->getParamType(1)->isIntegerTy(64) ||
            !helper->getFunctionType()->getParamType(2)->isPointerTy() ||
            !helper->getFunctionType()->getParamType(3)->isPointerTy() ||
            !helper->getFunctionType()->getParamType(4)->isIntegerTy(32))
            return Origin::Unknown;
        const auto* listFlag = llvm::dyn_cast<llvm::ConstantInt>(
            call->getArgOperand(4));
        const auto* slotIndex = llvm::dyn_cast<llvm::ConstantInt>(
            call->getArgOperand(1));
        if (!listFlag || !listFlag->isZero() || !slotIndex ||
            slotIndex->isNegative())
            return Origin::Unknown;
        return Origin::CheckedResolver;
    }
    const auto* slotLoad = llvm::dyn_cast<llvm::LoadInst>(owner);
    if (!slotLoad || slotLoad->isVolatile() || slotLoad->isAtomic() ||
        !slotLoad->getType()->isPointerTy())
        return Origin::Unknown;
    const auto* slotAddress = llvm::dyn_cast<llvm::GEPOperator>(
        slotLoad->getPointerOperand());
    if (!slotAddress ||
        !slotAddress->getSourceElementType()->isPointerTy() ||
        slotAddress->getNumIndices() != 1)
        return Origin::Unknown;
    const auto* slotIndex = llvm::dyn_cast<llvm::ConstantInt>(
        slotAddress->idx_begin()->get());
    if (!slotIndex || slotIndex->isNegative()) return Origin::Unknown;
    const auto* arrayLoad = llvm::dyn_cast<llvm::LoadInst>(
        slotAddress->getPointerOperand());
    if (!arrayLoad || arrayLoad->isVolatile() || arrayLoad->isAtomic() ||
        !arrayLoad->getType()->isPointerTy() ||
        constantHostContextOffset(arrayLoad->getPointerOperand(),
                                  trustedContext) != varSlotDataOffset)
        return Origin::Unknown;
    return Origin::SlotArray;
}

// Exact boxed Scratch reporter return; runtime still validates GC ownership.
inline bool trustedAsyncReporterValueSource(
    const llvm::Value* value, const llvm::Argument* context) {
    if (!context || context->getArgNo() != 0 ||
        !context->getType()->isPointerTy()) return false;
    const auto* c = llvm::dyn_cast_or_null<llvm::CallBase>(value);
    if (!c || c->getFunction() != context->getParent() ||
        !c->getType()->isIntegerTy(64) || !c->arg_size() ||
        c->getArgOperand(0) != context) return false;
    const auto* fn = c->getCalledFunction();
    if (!fn || fn->isVarArg()) return false;
    const auto* t = fn->getFunctionType();
    if (!t->getReturnType()->isIntegerTy(64)) return false;
    if (fn->getName() == "xyo_rt_async_reporter")
        return c->arg_size() == 2 && t->getNumParams() == 2 &&
            t->getParamType(0)->isPointerTy() && t->getParamType(1)->isPointerTy();
    if (fn->getName() == "xyo_rt_async_reporter_rooted")
        return c->arg_size() == 4 && t->getNumParams() == 4 &&
            t->getParamType(0)->isPointerTy() &&
            t->getParamType(1)->isPointerTy() &&
            t->getParamType(2)->isPointerTy() &&
            t->getParamType(3)->isIntegerTy(32);
    return false;
}

// A conservative census of live 64-bit words at an asynchronous reporter.
// These SSA words may hold boxed Scratch strings, plain integers, raw
// counters, or non-GC data. Even an exact argument alias has no standalone
// heap-lifetime guarantee. The result is diagnostics, NEVER a GC certificate.
struct LiveI64ProvenanceCensus {
    uint32_t total = 0;
    uint32_t argumentAliases = 0;
    uint32_t generatedStrings = 0;
    uint32_t variableSlotValues = 0;
    uint32_t checkedVariableValues = 0;
    uint32_t asyncReporterValues = 0;
    uint32_t unclassified = 0;

    void observe(const llvm::Value* value,
                 const llvm::Argument* trustedContext = nullptr,
                 uint64_t varSlotDataOffset = 0,
                 uint64_t variableValueOffset = 0) {
        if (!value || !value->getType()->isIntegerTy(64)) return;
        ++total;
        if (sameBoxedArgumentRoot(value)) {
            ++argumentAliases;
        } else if (trustedGeneratedStringSource(value)) {
            ++generatedStrings;
        } else if (const auto origin = classifyNativeVariableValueLoad(
                       value, trustedContext, varSlotDataOffset,
                       variableValueOffset);
                   origin == NativeVariableValueOrigin::SlotArray) {
            ++variableSlotValues;
        } else if (origin == NativeVariableValueOrigin::CheckedResolver) {
            ++checkedVariableValues;
        } else if (trustedAsyncReporterValueSource(value, trustedContext)) {
            ++asyncReporterValues;
        } else {
            ++unclassified;
        }
    }

    bool complete() const {
        return total == argumentAliases + generatedStrings + variableSlotValues +
               checkedVariableValues + asyncReporterValues + unclassified;
    }
};

struct ExtraLiveSsaValue {
    llvm::Value* definition = nullptr;
    uint32_t id = 0;
    // High half of the detached descriptor, distinct from existing
    // instruction integer widths (1, 32, 64).
    // 0x100 + width = floating-point instruction
    // 0x200 + width = integer function argument
    // 0x300 + width = floating-point function argument
    uint32_t kindWidth = 0;
};

struct LivePointerSsaDetail {
    llvm::Value* definition = nullptr;
    uint32_t instructionId = 0;
    PointerRegion region = PointerRegion::Unknown;
};
struct ExtraLiveSsaReport {
    std::vector<ExtraLiveSsaValue> values;
    // Every conservatively live pointer, INCLUDING pointer function args.
    // Borrowed LLVM Value* until the Module is destroyed. Never GC roots.
    std::vector<LivePointerSsaDetail> livePointers;
    uint32_t unresolvedPointer = 0;
    uint32_t unresolvedOther = 0;
    std::array<uint32_t, kPointerOriginCount> pointerOrigins{};
    // Separate hazard census; not serialized to the runtime SSA image.
    std::array<uint32_t, kPointerRegionCount> pointerRegions{};
    std::array<uint32_t, kUnknownPointerCauseCount> unknownPointerCauses{};
    std::vector<UnknownPointerDetail> unknownPointerDetails;
    std::vector<UnknownLoadChainDetail> unknownLoadChains;
    uint32_t stackDerivedPointers = 0;
    uint32_t distinctStackAllocas = 0;
    uint32_t capturedFloat = 0;
    uint32_t capturedIntegerArgs = 0;
    uint32_t capturedFloatArgs = 0;
    bool valid = false;

    uint32_t representedPreviouslyUnsupported() const {
        return capturedFloat + capturedIntegerArgs + capturedFloatArgs;
    }
    uint32_t remainingUnsupported() const {
        return unresolvedPointer + unresolvedOther;
    }
};

// Same conservative reachability/SSA-dominance boundary as IR-001's
// analyzer. False positives increase unsupported counts rather than
// granting deopt; the two reports are compared before emission.
inline ExtraLiveSsaReport analyzeExtraLiveSsa(
    llvm::Function& function, llvm::CallBase& marker,
    const llvm::Argument* trustedContext = nullptr) {
    ExtraLiveSsaReport result;
    if (marker.getFunction() != &function || function.isDeclaration())
        return result;
    // Refuse foreign/mismatched context claims. An omitted context
    // argument leaves every host pointer conservatively unknown.
    if (trustedContext &&
        (trustedContext->getParent() != &function ||
         trustedContext->getArgNo() != 0 ||
         !trustedContext->getType()->isPointerTy()))
        return result;

    std::unordered_set<const llvm::BasicBlock*> reachable;
    std::vector<const llvm::BasicBlock*> work{marker.getParent()};
    while (!work.empty()) {
        const auto* bb = work.back();
        work.pop_back();
        if (!reachable.insert(bb).second) continue;
        for (const llvm::BasicBlock* next : llvm::successors(bb))
            work.push_back(next);
    }

    llvm::DominatorTree dominators(function);
    const auto* markerBlock = marker.getParent();
    const auto usedLater = [&](const llvm::Value& value) {
        for (const llvm::Use& use : value.uses()) {
            const auto* user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
            if (!user || !reachable.contains(user->getParent())) continue;
            if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(user)) {
                if (reachable.contains(
                        phi->getIncomingBlock(use.getOperandNo())))
                    return true;
                continue;
            }
            if (user->getParent() != markerBlock ||
                marker.comesBefore(user))
                return true;
            for (const auto* pred : llvm::predecessors(markerBlock))
                if (reachable.contains(pred)) return true;
        }
        return false;
    };

    std::unordered_set<const llvm::AllocaInst*> referencedStackAllocas;
    const auto classify = [&](llvm::Value* value, uint32_t id,
                              bool isArgument) {
        const auto* type = value->getType();
        const uint32_t family = isArgument ? 0x300 : 0x100;
        if (type->isFloatTy() || type->isDoubleTy()) {
            const uint32_t width = type->isFloatTy() ? 32 : 64;
            result.values.push_back({value, id, family + width});
            if (isArgument) ++result.capturedFloatArgs;
            else ++result.capturedFloat;
        } else if (isArgument && (type->isIntegerTy(1) ||
                                  type->isIntegerTy(32) ||
                                  type->isIntegerTy(64))) {
            const uint32_t width = type->getIntegerBitWidth();
            result.values.push_back({value, id, 0x200 + width});
            ++result.capturedIntegerArgs;
        } else if (type->isPointerTy()) {
            ++result.unresolvedPointer;
            ++result.pointerOrigins[static_cast<size_t>(
                classifyPointerOrigin(*value))];
            const PointerRegion region =
                classifyPointerRegion(value, trustedContext);
            ++result.pointerRegions[static_cast<size_t>(region)];
            result.livePointers.push_back({value, id, region});
            if (region == PointerRegion::Unknown) {
                const auto cause = classifyUnknownPointerCause(
                    value, trustedContext);
                ++result.unknownPointerCauses[static_cast<size_t>(cause)];
                result.unknownPointerDetails.push_back({id, cause});
                if (cause == UnknownPointerCause::LoadStack ||
                    cause == UnknownPointerCause::LoadHost ||
                    cause == UnknownPointerCause::LoadStatic ||
                    cause == UnknownPointerCause::LoadUnknown) {
                    const PointerLoadChain chain =
                        tracePointerLoadChain(value, trustedContext);
                    result.unknownLoadChains.push_back({id, chain});
                }
            }
            if (const auto* root = nativeStackPointerRoot(value)) {
                ++result.stackDerivedPointers;
                referencedStackAllocas.insert(root);
            }
        } else {
            ++result.unresolvedOther;
        }
    };

    for (llvm::Argument& arg : function.args()) {
        if (!usedLater(arg)) continue;
        // Keep function arguments in a separate disjoint ID range,
        // preserving the original compiler-assigned instruction IDs.
        if (arg.getArgNo() >= 0x80000000u) return result;
        classify(&arg, 0x80000000u | arg.getArgNo(), true);
    }

    uint32_t instructionId = 0;
    for (llvm::BasicBlock& bb : function) {
        for (llvm::Instruction& instruction : bb) {
            if (instructionId >= 0x80000000u) return result;
            const uint32_t id = instructionId++;
            if (instruction.getType()->isVoidTy() ||
                !dominators.dominates(&instruction, &marker) ||
                !usedLater(instruction))
                continue;
            const auto* type = instruction.getType();
            // i1/i32/i64 instruction results belong to the existing
            // IR-001 numeric spill; everything else needs classification.
            if (type->isIntegerTy(1) || type->isIntegerTy(32) ||
                type->isIntegerTy(64))
                continue;
            classify(&instruction, id, false);
        }
    }
    result.distinctStackAllocas =
        uint32_t(referencedStackAllocas.size());
    uint64_t regionTotal = 0;
    for (const uint32_t n : result.pointerRegions) regionTotal += n;
    uint64_t unknownTotal = 0;
    for (const uint32_t n : result.unknownPointerCauses)
        unknownTotal += n;
    uint64_t unknownLoadTotal = 0;
    for (size_t i = 0;
         i <= static_cast<size_t>(UnknownPointerCause::LoadUnknown); ++i)
        unknownLoadTotal += result.unknownPointerCauses[i];
    bool completeLoadChainCensus =
        result.unknownLoadChains.size() == unknownLoadTotal;
    for (const auto& item : result.unknownLoadChains)
        if (item.chain.dereferences == 0)
            completeLoadChainCensus = false;
    result.valid =
        completeLoadChainCensus &&
        unknownTotal == result.pointerRegions[
            static_cast<size_t>(PointerRegion::Unknown)] &&
        result.unknownPointerDetails.size() == unknownTotal &&
        regionTotal == result.unresolvedPointer &&
        result.livePointers.size() == result.unresolvedPointer &&
        result.pointerRegions[static_cast<size_t>(
            PointerRegion::NativeStack)] == result.stackDerivedPointers &&
        result.distinctStackAllocas <= result.stackDerivedPointers &&
        result.stackDerivedPointers <= result.unresolvedPointer;
    return result;
}

} // namespace xyo::rt
