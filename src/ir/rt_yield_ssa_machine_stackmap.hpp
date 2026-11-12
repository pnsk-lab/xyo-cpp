// RT-001: opt-in SSA liveness -> machine StackMap *location census*
// immediately before the real native yield call boundary.
//
// DO NOT interpret a location as a heap root. A boxed i64 may be a
// number, object, tagged string, invalid pointer, or ordinary counter;
// the compiler must prove ownership/lifetime separately. The Fiber is
// suspended *inside* a host helper, rather than at this LLVM callsite.
// This header never authorizes native Fiber garbage collection.
#pragma once

#include "../ir/llvm_ssa_liveness.hpp"
#include "../jit/rt_ssa_extra_capture.hpp"
#include "../core/value.hpp"
#include "rt_yield_edge_stackmap.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace xyo::rt {

// One-to-one with the variadic StackMap operands AFTER (id, patch bytes).
// LLVM's instruction ID is compilation-local, not a stable GC root ID.
// Prove ONLY the identity of an internal scheduler tick counter.
// This is not a GC root classifier.  The counter in Codegen::emitLoopYield
// has the exact phi recurrence: PHI(initial=load ctx.yieldCheckN,
// backedge=add(PHI, 1)).  The JitCtx layout offset is provided by the
// backend ABI, never guessed from an arbitrary pointer GEP.
inline bool isNativeYieldTickIncrement(
    const llvm::Value* source, const llvm::Argument* trustedCtx,
    uint32_t yieldCheckOffset) {
    if (!source || !trustedCtx || yieldCheckOffset == UINT32_MAX ||
        !trustedCtx->getType()->isPointerTy() ||
        trustedCtx->getArgNo() != 0)
        return false;
    const auto* increment = llvm::dyn_cast<llvm::BinaryOperator>(source);
    if (!increment || increment->getOpcode() != llvm::Instruction::Add ||
        !increment->getType()->isIntegerTy(64))
        return false;
    const llvm::PHINode* phi = nullptr;
    for (unsigned i = 0; i < 2; ++i) {
        const auto* one = llvm::dyn_cast<llvm::ConstantInt>(
            increment->getOperand(i));
        if (one && one->equalsInt(1))
            phi = llvm::dyn_cast<llvm::PHINode>(
                increment->getOperand(1 - i));
    }
    if (!phi || phi->getType() != increment->getType() ||
        phi->getFunction() != increment->getFunction() ||
        phi->getNumIncomingValues() != 2)
        return false;
    const llvm::LoadInst* initialization = nullptr;
    unsigned recurrenceCount = 0;
    for (llvm::Value* v : phi->incoming_values()) {
        if (v == increment) {
            ++recurrenceCount;
        } else {
            initialization = llvm::dyn_cast<llvm::LoadInst>(v);
        }
    }
    if (recurrenceCount != 1 || !initialization ||
        !initialization->getType()->isIntegerTy(64))
        return false;
    const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(
        initialization->getPointerOperand()->stripPointerCasts());
    if (!gep || !gep->getSourceElementType()->isIntegerTy(8) ||
        gep->getNumIndices() != 1 ||
        gep->getPointerOperand()->stripPointerCasts() != trustedCtx)
        return false;
    const auto* displacement = llvm::dyn_cast<llvm::ConstantInt>(
        gep->idx_begin()->get());
    return displacement &&
        !displacement->isNegative() &&
        displacement->getValue().getActiveBits() <= 32 &&
        displacement->getZExtValue() == yieldCheckOffset;
}

// Evidence category only; even a matching boxed source is not a
// liveness or GC ownership certificate. Each category is exhaustive
// only for the current emitted *machine operands*.
inline const char* classifyYieldI64Origin(
    const llvm::Value* value, const llvm::Argument* context,
    uint64_t varSlotDataOffset = UINT64_MAX,
    uint64_t variableValueOffset = UINT64_MAX,
    uint32_t yieldCheckOffset = UINT32_MAX) {
    if (!value || !value->getType()->isIntegerTy(64))
        return "unknown_invalid";
    if (isNativeYieldTickIncrement(value, context, yieldCheckOffset))
        return "numeric_scheduler_tick";
    // The existing bounded provenance routines check *exact* LLVM IR
    // shapes, never assume an i64 bit-pattern is a managed pointer.
    if (sameBoxedArgumentRoot(value))
        return "candidate_argument_alias";
    if (trustedGeneratedStringSource(value))
        return "candidate_generated_string";
    if (context && varSlotDataOffset != UINT64_MAX &&
        variableValueOffset != UINT64_MAX) {
        const auto origin = classifyNativeVariableValueLoad(
            value, context, varSlotDataOffset, variableValueOffset);
        if (origin == NativeVariableValueOrigin::SlotArray)
            return "candidate_variable_slot";
        if (origin == NativeVariableValueOrigin::CheckedResolver)
            return "candidate_checked_variable";
    }
    if (trustedAsyncReporterValueSource(value, context))
        return "candidate_async_reporter";
    if (const auto* zext = llvm::dyn_cast<llvm::ZExtInst>(value)) {
        // NaN-boxed heap strings require high tag 0x7FF9. A zero
        // extension of <=32 bits *cannot* encode that tag. This only
        // excludes the tagged-string representation; it does NOT
        // establish that the integer is safe to ignore as a raw
        // pointer, and we do not remove it from the machine StackMap.
        static_assert((xyo::val::kTagStr >> 32) != 0);
        const llvm::Type* source = zext->getSrcTy();
        if (source->isIntegerTy() &&
            source->getIntegerBitWidth() <= 32)
            return "no_tagged_string_zext";
    }
    if (const auto* cast = llvm::dyn_cast<llvm::BitCastInst>(value);
        cast && cast->getSrcTy()->isDoubleTy())
        return "unknown_float_bits";
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value)) {
        // Region of the address holding i64, *not* the type/ownership
        // of the loaded value. Even a stack or JitCtx load may contain
        // a borrowed NaN-boxed Scratch string.
        switch (classifyPointerRegion(
            load->getPointerOperand(), context)) {
        case PointerRegion::NativeStack:
            return "unknown_load_stack";
        case PointerRegion::HostContext:
            return "unknown_load_context";
        case PointerRegion::StaticGlobal:
            return "unknown_load_static";
        default:
            return "unknown_load_unresolved";
        }
    }
    if (llvm::isa<llvm::PHINode>(value) ||
        llvm::isa<llvm::SelectInst>(value))
        return "unknown_merge";
    if (llvm::isa<llvm::CallBase>(value))
        return "unknown_call";
    if (llvm::isa<llvm::BinaryOperator>(value))
        return "unknown_bit_arithmetic";
    if (llvm::isa<llvm::Argument>(value))
        return "unknown_argument";
    return "unknown_other";
}

inline bool isPotentialBoxedYieldOrigin(llvm::StringRef name) {
    return name.starts_with("candidate_");
}

struct YieldSsaOperandSource {
    uint32_t location = 0;  // 1-based, same as llvm-readobj location #N
    std::string family;     // helper_arg / i64_instruction / i64_argument / pointer
    uint32_t sourceId = UINT32_MAX;
    std::string opcode;     // LLVM opcode, or "argument"
    std::string semantic;   // scheduler_tick / unclassified
    std::string origin;     // evidence only; candidate_/unknown_/numeric_
};

struct YieldSsaMachineSite {
    std::string functionName;
    std::string helper;
    uint64_t stackmapId = 0;
    uint32_t ordinal = 0;
    uint32_t liveI64 = 0;
    uint32_t excludedNumericSchedulerTicks = 0;
    uint32_t livePointer = 0;
    uint32_t nonI64Numeric = 0;
    uint32_t liveFloat = 0;
    uint32_t otherUnsupported = 0;
    uint32_t expectedLocations = 0;
    std::vector<YieldSsaOperandSource> sources;
    bool livenessValid = false;
    bool emitted = false;
};

// A separate, deterministic StackMap ID namespace; doesn't collide
// with earlier direct yield geometry records or SSA reporter markers.
inline uint64_t yieldSsaStackmapId(llvm::StringRef name,
                                   uint32_t ordinal) {
    uint64_t hash = 14695981039346656037ULL;
    auto feed = [&hash](uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    for (char ch : llvm::StringRef("rt001.yield.live.ssa"))
        feed(static_cast<uint8_t>(ch));
    feed(0xff);
    for (char ch : name) feed(static_cast<uint8_t>(ch));
    feed(0xfe);
    for (unsigned i = 0; i < 4; ++i)
        feed(static_cast<uint8_t>(ordinal >> (8u * i)));
    return hash;
}

enum class YieldSsaSelection {
    All, I64Only, I64WithoutSchedulerTicks, PointersOnly, ContextOnly
};

inline std::vector<YieldSsaMachineSite>
emitExperimentalYieldSsaSnapshots(
    llvm::Module& module,
    YieldSsaSelection selection = YieldSsaSelection::All,
    uint32_t pointerLimit = UINT32_MAX,
    uint32_t yieldCheckOffset = UINT32_MAX,
    uint64_t varSlotDataOffset = UINT64_MAX,
    uint64_t variableValueOffset = UINT64_MAX) {
    std::vector<YieldSsaMachineSite> results;
    std::unordered_set<uint64_t> emittedIds;
    llvm::Function* stackmap = nullptr;

    for (llvm::Function& fn : module) {
        if (fn.isDeclaration() ||
            !fn.getName().starts_with("xyo_script_"))
            continue;
        const llvm::Argument* context =
            !fn.arg_empty() && fn.getArg(0)->getType()->isPointerTy()
                ? fn.getArg(0) : nullptr;
        uint32_t ordinal = 0;
        for (llvm::BasicBlock& bb : fn) {
            for (llvm::Instruction& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call || !call->getCalledFunction()) continue;
                const llvm::StringRef name =
                    call->getCalledFunction()->getName();
                if (name != "xyo_yield" &&
                    name != "xyo_yield_fast")
                    continue;

                YieldSsaMachineSite site;
                site.functionName = fn.getName().str();
                site.helper = name.str();
                site.ordinal = ++ordinal;
                site.stackmapId = yieldSsaStackmapId(
                    fn.getName(), site.ordinal);

                // Only emit if the independent direct call-edge
                // geometry probe ALREADY validated this call. The
                // previous instruction must be the one-argument
                // stackmap bearing the exact expected ID.
                auto* previous = llvm::dyn_cast_or_null<llvm::CallInst>(
                    call->getPrevNode());
                const llvm::Function* prevFn =
                    previous ? previous->getCalledFunction() : nullptr;
                const auto* previousId = previous &&
                    previous->arg_size() > 0
                    ? llvm::dyn_cast<llvm::ConstantInt>(
                          previous->getArgOperand(0)) : nullptr;
                const auto* prior =
                    previous ? llvm::dyn_cast_or_null<llvm::CallInst>(
                        previous->getPrevNode()) : nullptr;
                const llvm::Function* priorFn =
                    prior ? prior->getCalledFunction() : nullptr;
                const auto* priorId = prior && prior->arg_size() > 0
                    ? llvm::dyn_cast<llvm::ConstantInt>(
                          prior->getArgOperand(0)) : nullptr;
                const bool alreadyEmitted =
                    priorFn && priorFn->isIntrinsic() &&
                    priorFn->getIntrinsicID() ==
                        llvm::Intrinsic::experimental_stackmap &&
                    priorId &&
                    priorId->getZExtValue() == site.stackmapId;
                if (!context || !prevFn || !prevFn->isIntrinsic() ||
                    prevFn->getIntrinsicID() !=
                        llvm::Intrinsic::experimental_stackmap ||
                    alreadyEmitted ||
                    !previousId ||
                    previousId->getZExtValue() !=
                        yieldEdgeStackmapId(fn.getName(), site.ordinal) ||
                    !emittedIds.insert(site.stackmapId).second) {
                    results.push_back(std::move(site));
                    continue;
                }

                const auto numeric =
                    xyo::ir::analyzeLlvmCheckpointLiveness(fn, *call);
                const auto extra =
                    analyzeExtraLiveSsa(fn, *call, context);
                site.livenessValid =
                    numeric.validCheckpoint && extra.valid;

                // First location is the direct helper argument, which
                // is an ABI pointer, NOT necessarily a managed root.
                std::vector<llvm::Value*> locations;
                std::unordered_set<llvm::Value*> seen;
                const auto add = [&](llvm::Value* value,
                                     const char* family, uint32_t sourceId) {
                    if (!value || !seen.insert(value).second) return;
                    locations.push_back(value);
                    const auto* definition =
                        llvm::dyn_cast<llvm::Instruction>(value);
                    site.sources.push_back({
                        static_cast<uint32_t>(locations.size()),
                        family, sourceId,
                        definition ? definition->getOpcodeName()
                                   : llvm::isa<llvm::Argument>(value)
                                       ? "argument" : "other",
                        isNativeYieldTickIncrement(
                            value, context, yieldCheckOffset)
                            ? "scheduler_tick" : "unclassified",
                        classifyYieldI64Origin(
                            value, context, varSlotDataOffset,
                            variableValueOffset, yieldCheckOffset)
                    });
                };
                add(call->getArgOperand(0), "helper_arg",
                    UINT32_MAX);

                for (const auto& value : numeric.numeric) {
                    if (value.width == 64) {
                        ++site.liveI64;
                        const bool knownSchedulerTick =
                            isNativeYieldTickIncrement(
                                value.definition, context,
                                yieldCheckOffset);
                        if (selection ==
                                YieldSsaSelection::I64WithoutSchedulerTicks &&
                            knownSchedulerTick) {
                            // This value is an INTERNAL numeric loop
                            // counter, not a Scratch boxed GC reference.
                            // Excluding it from the experimental i64
                            // location census does NOT prove the other
                            // locations are roots.
                            ++site.excludedNumericSchedulerTicks;
                        } else if (
                            selection == YieldSsaSelection::All ||
                            selection == YieldSsaSelection::I64Only ||
                            selection ==
                                YieldSsaSelection::I64WithoutSchedulerTicks) {
                            add(const_cast<llvm::Instruction*>(
                                value.definition), "i64_instruction",
                                value.id);
                        }
                    } else {
                        ++site.nonI64Numeric;
                    }
                }
                for (const auto& value : extra.values) {
                    if (value.kindWidth == 0x200u + 64u) {
                        ++site.liveI64;
                        if (selection == YieldSsaSelection::All ||
                            selection == YieldSsaSelection::I64Only ||
                            selection ==
                                YieldSsaSelection::I64WithoutSchedulerTicks)
                            add(value.definition, "i64_argument",
                                value.id);
                    } else if (value.kindWidth == 0x100u + 32u ||
                               value.kindWidth == 0x100u + 64u ||
                               value.kindWidth == 0x300u + 32u ||
                               value.kindWidth == 0x300u + 64u) {
                        ++site.liveFloat;
                    } else {
                        ++site.otherUnsupported;
                    }
                }
                uint32_t capturedPointers = 0;
                for (const auto& pointer : extra.livePointers) {
                    ++site.livePointer;
                    if ((selection == YieldSsaSelection::All ||
                         selection == YieldSsaSelection::PointersOnly) &&
                        capturedPointers < pointerLimit) {
                        add(pointer.definition, "pointer",
                            pointer.instructionId);
                        ++capturedPointers;
                    }
                }
                site.otherUnsupported += extra.unresolvedOther;
                site.expectedLocations = uint32_t(locations.size());
                if (site.sources.size() != locations.size()) {
                    results.push_back(std::move(site));
                    continue;
                }

                // Bound diagnostic overhead and refuse an inconsistent
                // census. Does not classify any value as an owned root.
                if (!site.livenessValid ||
                    locations.empty() || locations.size() > 256 ||
                    extra.livePointers.size() !=
                        extra.unresolvedPointer) {
                    results.push_back(std::move(site));
                    continue;
                }

                if (!stackmap)
                    stackmap = llvm::Intrinsic::getOrInsertDeclaration(
                        &module, llvm::Intrinsic::experimental_stackmap);
                llvm::IRBuilder<> builder(previous);
                std::vector<llvm::Value*> args{
                    builder.getInt64(site.stackmapId),
                    builder.getInt32(0)
                };
                args.insert(args.end(), locations.begin(), locations.end());
                builder.CreateCall(stackmap, args);
                site.emitted = true;
                results.push_back(std::move(site));
            }
        }
    }
    return results;
}

} // namespace xyo::rt
