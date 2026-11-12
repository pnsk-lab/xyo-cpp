// RT-001: map a REAL native yield call edge to a machine StackMap PC.
// Diagnostic only. A stackmap immediately before the call does NOT
// promise saved Fiber register homes, boxed Scratch Value root coverage,
// or permission for native garbage collection.
#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace xyo::rt {

struct YieldEdgeStackmapSite {
    std::string functionName;
    std::string yieldHelper;
    uint32_t ordinal = 0;
    uint64_t id = 0;
    uint32_t locationCount = 0;
    bool emitted = false;
};

// Different hash domain from SSA/async StackMaps. The ordinal is the
// index of this direct yield call in the optimized function's IR.
inline uint64_t yieldEdgeStackmapId(llvm::StringRef functionName,
                                    uint32_t ordinal) {
    uint64_t hash = 14695981039346656037ULL;
    auto feed = [&hash](uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    for (char byte : llvm::StringRef("rt001.yield.edge"))
        feed(static_cast<uint8_t>(byte));
    feed(0xff);
    for (char byte : functionName)
        feed(static_cast<uint8_t>(byte));
    feed(0xfe);
    for (unsigned i = 0; i < 4; ++i)
        feed(static_cast<uint8_t>(ordinal >> (8u * i)));
    return hash;
}

inline std::vector<YieldEdgeStackmapSite>
emitExperimentalYieldEdgeStackmaps(llvm::Module& module) {
    std::vector<YieldEdgeStackmapSite> sites;
    std::unordered_set<uint64_t> emittedIds;
    // Make the declaration only after inspecting at least one candidate.
    llvm::Function* stackmap = nullptr;

    for (llvm::Function& fn : module) {
        if (fn.isDeclaration() ||
            !fn.getName().starts_with("xyo_script_"))
            continue;
        uint32_t ordinal = 0;
        for (llvm::BasicBlock& bb : fn) {
            for (llvm::Instruction& instruction : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                if (!call || !call->getCalledFunction()) continue;
                const llvm::StringRef helper =
                    call->getCalledFunction()->getName();
                if (helper != "xyo_yield" &&
                    helper != "xyo_yield_fast") continue;

                YieldEdgeStackmapSite site;
                site.functionName = fn.getName().str();
                site.yieldHelper = helper.str();
                site.ordinal = ++ordinal;
                site.id = yieldEdgeStackmapId(
                    fn.getName(), site.ordinal);

                // No pointer value, target context, or function ABI
                // may be guessed. A different yield helper or ABI is
                // not an authorized insertion site.
                const auto* signature =
                    call->getCalledFunction()->getFunctionType();
                if (fn.arg_empty() ||
                    !fn.getArg(0)->getType()->isPointerTy() ||
                    !call->getType()->isVoidTy() ||
                    call->arg_size() != 1 ||
                    signature->isVarArg() ||
                    signature->getNumParams() != 1 ||
                    !signature->getReturnType()->isVoidTy() ||
                    !signature->getParamType(0)->isPointerTy() ||
                    !call->getArgOperand(0)->getType()->isPointerTy() ||
                    call->isMustTailCall() ||
                    // Full context must match the original function ABI.
                    // The fast helper's only argument is the Fiber*.
                    (helper == "xyo_yield" &&
                     call->getArgOperand(0) != fn.getArg(0)) ||
                    !emittedIds.insert(site.id).second) {
                    sites.push_back(std::move(site));
                    continue;
                }

                if (!stackmap)
                    stackmap = llvm::Intrinsic::getOrInsertDeclaration(
                        &module, llvm::Intrinsic::experimental_stackmap);
                llvm::IRBuilder<> builder(call);
                // Exactly one *pointer-location* operand, unrelated
                // to whether it is GC-owned. This is a call-edge
                // geometry experiment, not a liveness/root census.
                builder.CreateCall(stackmap, {
                    builder.getInt64(site.id),
                    builder.getInt32(0),
                    call->getArgOperand(0),
                });
                site.locationCount = 1;
                site.emitted = true;
                sites.push_back(std::move(site));
            }
        }
    }
    return sites;
}

// RT-001: explicit *unmapped* native-suspension boundary census.
// Direct xyo_yield/_fast sites can be machine-matched. A nested helper
// (wait/broadcast/async reporter/...) can suspend DEEPER in the call
// stack, so a StackMap in its direct caller is not a certified capture
// of the actual native Fiber frame. Never count it as "mapped".
struct UnmappedYieldEdge {
    std::string functionName;
    std::string helperName;
    std::string reason;
    uint32_t ordinal = 0;
};

// Keep this conservative list aligned with Codegen::helperMayYield.
// A differential test compares the two tables to reject catalog drift.
// The rooted async reporter is explicitly added: its backend-inserted
// call may not flow through Codegen::noteCall.
inline bool knownPotentiallyYieldingHelper(llvm::StringRef name) {
    static constexpr const char* helpers[] = {
        "xyo_yield", "xyo_yield_fast", "xyo_loop_yield",
        "xyo_wait_yield", "xyo_wait_event_yield", "xyo_rt_wait",
        "xyo_stop_all", "xyo_stop_script", "xyo_broadcast_wait_v",
        "xyo_rt_compat", "xyo_rt_reporter", "xyo_rt_async_reporter",
        "xyo_rt_nested_osr_arg_get",
        "xyo_mot_glidexy", "xyo_mot_glideto",
        "xyo_looks_saysecs", "xyo_looks_thinksecs",
        "xyo_looks_switchbackdropwait", "xyo_sns_askwait",
        "xyo_ctl_deleteclone", "xyo_snd_playwait",
        "xyo_snd_seteffect", "xyo_snd_changeeffect",
        "xyo_snd_setvolume", "xyo_snd_changevolume",
        "xyo_rt_async_reporter_rooted",
    };
    for (const char* helper : helpers)
        if (name == helper) return true;
    return false;
}

inline std::vector<UnmappedYieldEdge>
auditUnmappedYieldEdges(llvm::Module& module) {
    std::vector<UnmappedYieldEdge> unmapped;
    for (llvm::Function& fn : module) {
        if (fn.isDeclaration() ||
            !fn.getName().starts_with("xyo_script_"))
            continue;
        uint32_t ordinal = 0;
        for (llvm::BasicBlock& bb : fn) {
            for (llvm::Instruction& inst : bb) {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (!call || call->isInlineAsm()) continue;
                const llvm::Function* callee = call->getCalledFunction();
                if (callee && callee->isIntrinsic()) continue;
                ++ordinal;
                const llvm::StringRef name =
                    callee ? callee->getName() : llvm::StringRef();
                const char* reason = nullptr;
                if (!callee) {
                    reason = "indirect_call";
                } else if (name.starts_with("xyo_script_")) {
                    reason = "nested_jit_call";
                } else if (name == "xyo_yield" ||
                           name == "xyo_yield_fast") {
                    // The direct call is mapped only if its immediately
                    // preceding instruction is the exact StackMap
                    // intrinsic inserted by emitExperimentalYield...
                    const auto* direct =
                        llvm::dyn_cast<llvm::CallInst>(call);
                    const auto* previous = direct
                        ? llvm::dyn_cast_or_null<llvm::CallInst>(
                            direct->getPrevNode()) : nullptr;
                    const llvm::Function* prevFn = previous
                        ? previous->getCalledFunction() : nullptr;
                    if (!prevFn || !prevFn->isIntrinsic() ||
                        prevFn->getIntrinsicID() !=
                            llvm::Intrinsic::experimental_stackmap)
                        reason = "unmapped_direct_yield";
                } else if (knownPotentiallyYieldingHelper(name)) {
                    reason = "nested_helper";
                }
                if (reason)
                    unmapped.push_back({
                        fn.getName().str(),
                        callee ? name.str() : "<indirect>",
                        reason, ordinal
                    });
            }
        }
    }
    return unmapped;
}

}  // namespace xyo::rt
