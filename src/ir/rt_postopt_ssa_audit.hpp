// RT-001: audit ACTUAL optimized LLVM IR at the SSA capture boundary.
// Diagnostic only: SSA liveness and compiler origins are NOT GC roots.
// Post-lowering register allocation / stack maps are still unproved.
#pragma once

#include "llvm_ssa_liveness.hpp"
#include "../jit/rt_ssa_extra_capture.hpp"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace xyo::rt {
// Check the actual post-O2/O3 buffer at an asynchronous translation
// callsite. This proves that a bounded, exact native alloca is populated
// in the same basic block before invoking the Thread-owned root bridge.
// It does NOT prove any uninstrumented i64 values or machine register
// locations are GC roots; every report must keep gc_safe=0.
struct PostoptAsyncRootSite {
    std::string functionName;
    uint32_t siteOrdinal = 0;
    // Borrowed, strictly for post-optimizer machine-location diagnostics.
    llvm::CallInst* reporter = nullptr;
    std::vector<llvm::Value*> livePointerValues;
    std::vector<llvm::Value*> liveI64Values;
    std::array<uint32_t, kPointerRegionCount> pointerRegions{};
    uint32_t declaredWords = 0;
    uint32_t initializedWords = 0;
    uint32_t liveI64 = 0;
    uint32_t liveArgumentI64 = 0;
    uint32_t coveredLiveI64 = 0;
    uint32_t unknownI64 = 0;
    uint32_t livePointers = 0;
    bool signatureValid = false;
    bool bufferValid = false;
    bool completeStores = false;
    // This is only i64 SSA coverage at the optimized LLVM callsite,
    // not a proof about pointer-typed SSA or machine registers.
    bool coveredAllLiveI64 = false;
};

inline std::vector<PostoptAsyncRootSite> auditPostoptAsyncRoots(
    llvm::Module& module) {
    std::vector<PostoptAsyncRootSite> reports;
    const llvm::DataLayout& layout = module.getDataLayout();
    for (llvm::Function& fn : module) {
        if (fn.isDeclaration()) continue;
        uint32_t siteOrdinal = 0;
        const llvm::Argument* context =
            !fn.arg_empty() && fn.getArg(0)->getType()->isPointerTy()
                ? fn.getArg(0) : nullptr;
        for (llvm::BasicBlock& bb : fn)
            for (llvm::Instruction& instruction : bb) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call || !call->getCalledFunction() ||
                    call->getCalledFunction()->getName() !=
                        "xyo_rt_async_reporter_rooted")
                    continue;
                PostoptAsyncRootSite report;
                report.functionName = fn.getName().str();
                report.siteOrdinal = ++siteOrdinal;
                report.reporter = llvm::dyn_cast<llvm::CallInst>(call);
                const auto* ft = call->getCalledFunction()->getFunctionType();
                const auto* count = call->arg_size() == 4
                    ? llvm::dyn_cast<llvm::ConstantInt>(
                        call->getArgOperand(3)) : nullptr;
                report.signatureValid =
                    context && call->arg_size() == 4 && !ft->isVarArg() &&
                    ft->getNumParams() == 4 &&
                    ft->getReturnType()->isIntegerTy(64) &&
                    ft->getParamType(0)->isPointerTy() &&
                    ft->getParamType(1)->isPointerTy() &&
                    ft->getParamType(2)->isPointerTy() &&
                    ft->getParamType(3)->isIntegerTy(32) &&
                    call->getArgOperand(0) == context &&
                    count && count->getValue().getActiveBits() <= 7 &&
                    count->getZExtValue() >= 1 &&
                    count->getZExtValue() <= 64;
                const auto live = ir::analyzeLlvmCheckpointLiveness(fn, *call);
                const auto extra = analyzeExtraLiveSsa(fn, *call, context);
                report.livePointers = extra.unresolvedPointer;
                report.pointerRegions = extra.pointerRegions;
                for (const auto& detail : extra.livePointers)
                    report.livePointerValues.push_back(detail.definition);
                for (const auto& numeric : live.numeric) {
                    if (numeric.width != 64) continue;
                    ++report.liveI64;
                    report.liveI64Values.push_back(
                        const_cast<llvm::Instruction*>(numeric.definition));
                    const llvm::Value* v = numeric.definition;
                    if (!sameBoxedArgumentRoot(v) &&
                        !trustedGeneratedStringSource(v) &&
                        !trustedAsyncReporterValueSource(v, context))
                        ++report.unknownI64;
                }
                // The instruction-only liveness report omits native
                // function argument registers. The extra report includes
                // those still live after this reporter; counting them
                // prevents declaring a buffer complete while a boxed
                // procedure argument remains unrooted.
                for (const auto& candidate : extra.values) {
                    if (candidate.kindWidth != 0x200u + 64u) continue;
                    ++report.liveI64;
                    ++report.liveArgumentI64;
                    report.liveI64Values.push_back(candidate.definition);
                    if (!sameBoxedArgumentRoot(candidate.definition))
                        ++report.unknownI64;
                }
                if (!report.signatureValid || !live.validCheckpoint ||
                    !extra.valid) {
                    reports.push_back(std::move(report));
                    continue;
                }
                report.declaredWords = uint32_t(count->getZExtValue());
                int64_t bufferOffset = 0;
                const llvm::Value* bufferBase =
                    llvm::GetPointerBaseWithConstantOffset(
                        call->getArgOperand(2), bufferOffset, layout);
                const auto* alloca =
                    llvm::dyn_cast<llvm::AllocaInst>(bufferBase);
                const auto allocation = alloca
                    ? alloca->getAllocationSize(layout) : std::nullopt;
                report.bufferValid =
                    alloca && bufferOffset == 0 &&
                    allocation && !allocation->isScalable() &&
                    allocation->getFixedValue() >=
                        uint64_t(report.declaredWords) * sizeof(uint64_t) &&
                    alloca->getFunction() == &fn;
                if (!report.bufferValid) {
                    reports.push_back(std::move(report));
                    continue;
                }
                // Only same-basic-block preceding stores can establish
                // initialization here. An alias or conditional store
                // requires MemorySSA/CFG proof and is conservatively
                // rejected. Scan backwards (last store wins).
                std::vector<bool> initialized(report.declaredWords);
                std::unordered_set<const llvm::Value*> capturedValues;
                bool ambiguous = false;
                for (auto it = call->getIterator();
                     it != bb.begin() && report.initializedWords <
                         report.declaredWords;) {
                    const llvm::Instruction& previous = *--it;
                    if (const auto* store =
                            llvm::dyn_cast<llvm::StoreInst>(&previous)) {
                        int64_t offset = 0;
                        const auto* base =
                            llvm::GetPointerBaseWithConstantOffset(
                                store->getPointerOperand(), offset, layout);
                        if (base != alloca) continue;
                        if (store->isVolatile() || store->isAtomic() ||
                            !store->getValueOperand()->getType()
                                ->isIntegerTy(64) ||
                            offset < 0 || (offset % 8) != 0 ||
                            uint64_t(offset / 8) >= report.declaredWords) {
                            ambiguous = true;
                            break;
                        }
                        const auto index = size_t(offset / 8);
                        if (!initialized[index]) {
                            initialized[index] = true;
                            ++report.initializedWords;
                            capturedValues.insert(store->getValueOperand());
                        }
                    } else if (previous.mayWriteToMemory() &&
                               !llvm::isa<llvm::DbgInfoIntrinsic>(&previous) &&
                               !llvm::isa<llvm::LifetimeIntrinsic>(&previous)) {
                        // Even a call could mutate the buffer if the
                        // alloca escaped. Without a full alias proof,
                        // fail closed instead of reusing stale stores.
                        ambiguous = true;
                        break;
                    }
                }
                report.completeStores = !ambiguous &&
                    report.initializedWords == report.declaredWords;
                // Compare exact post-O3 live i64 SSA Value* against the
                // values placed in this site's root array. The optimizer
                // may fold loads to constants; those are copied as words
                // but have no live numeric instruction to compare.
                for (const auto& numeric : live.numeric)
                    if (numeric.width == 64 &&
                        capturedValues.contains(numeric.definition))
                        ++report.coveredLiveI64;
                for (const auto& candidate : extra.values)
                    if (candidate.kindWidth == 0x200u + 64u &&
                        capturedValues.contains(candidate.definition))
                        ++report.coveredLiveI64;
                report.coveredAllLiveI64 =
                    report.completeStores &&
                    report.coveredLiveI64 == report.liveI64;
                reports.push_back(std::move(report));
            }
    }
    return reports;
}

// Strict, shape-only recognition of a native Variable* pointer borrowed
// from JitCtx::varSlotData[constant index]. This cannot establish that
// the Variable still belongs to its Target after an async wait, or that
// its Value payload is GC-rooted. Never promote PointerRegion::Unknown.
inline std::optional<uint32_t> asyncBorrowedVarSlotIndex(
    const llvm::Value* value, const llvm::Argument* context,
    const llvm::DataLayout& layout, uint64_t fieldOffset) {
    if (!context || !context->getType()->isPointerTy() ||
        layout.getPointerSize() != sizeof(void*) ||
        fieldOffset > uint64_t(INT64_MAX))
        return std::nullopt;
    const auto* variable = llvm::dyn_cast<llvm::LoadInst>(value);
    if (!variable || !variable->isSimple() ||
        !variable->getType()->isPointerTy()) return std::nullopt;
    int64_t elementOffset = 0;
    const llvm::Value* arrayBase =
        llvm::GetPointerBaseWithConstantOffset(
            variable->getPointerOperand(), elementOffset, layout);
    const auto* array = llvm::dyn_cast<llvm::LoadInst>(arrayBase);
    if (!array || !array->isSimple() ||
        !array->getType()->isPointerTy() ||
        elementOffset < 0 ||
        uint64_t(elementOffset) % sizeof(void*) != 0)
        return std::nullopt;
    int64_t contextOffset = 0;
    const llvm::Value* contextBase =
        llvm::GetPointerBaseWithConstantOffset(
            array->getPointerOperand(), contextOffset, layout);
    if (contextBase != context || contextOffset != int64_t(fieldOffset))
        return std::nullopt;
    const uint64_t slot = uint64_t(elementOffset) / sizeof(void*);
    if (slot > UINT32_MAX) return std::nullopt;
    return uint32_t(slot);
}

// Diagnostic StackMap immediately BEFORE the native reporter bridge.
// It records the register/stack homes LLVM chooses for each *live* SSA
// value at this instruction, including pointer regions and opaque i64.
// A real Fiber switch occurs inside the callee, so these are NOT the
// suspended Fiber's register homes. Machine GC must remain prohibited.
struct ExperimentalAsyncPointerOrigin {
    // StackMap location #0 is the explicit JitCtx; pointer SSA words
    // occupy #1..livePointers in exactly this order.
    uint32_t locationIndex = 0;
    uint32_t llvmValueId = 0;
    PointerRegion region = PointerRegion::Unknown;
    UnknownPointerCause unknownCause = UnknownPointerCause::Other;
    std::string definitionKind;
    std::string calleeName;
    std::optional<uint32_t> candidateBorrowedVarSlot;
    bool hasUnknownLoadChain = false;
    PointerLoadChain unknownLoadChain;
};
struct ExperimentalAsyncStackmapSite {
    std::string functionName;
    uint32_t siteOrdinal = 0;
    uint64_t stackmapId = 0;
    uint32_t livePointers = 0;
    uint32_t liveI64 = 0;
    std::array<uint32_t, kPointerRegionCount> pointerRegions{};
    std::vector<ExperimentalAsyncPointerOrigin> pointerOrigins;
    bool emitted = false;
};
inline uint64_t asyncStackmapProbeId(
    llvm::StringRef functionName, uint32_t ordinal) {
    uint64_t hash = 14695981039346656037ULL;
    auto feed = [&](uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    for (const char c : llvm::StringRef("rt001.async.stackmap"))
        feed(static_cast<uint8_t>(c));
    feed(0xff);
    for (char c : functionName) feed(static_cast<uint8_t>(c));
    feed(0xfe);
    for (unsigned i = 0; i < 4; ++i)
        feed(static_cast<uint8_t>(ordinal >> (8u * i)));
    return hash;
}
inline std::vector<ExperimentalAsyncStackmapSite>
emitExperimentalAsyncStackmaps(
    llvm::Module& module,
    std::optional<uint64_t> trustedVarSlotFieldOffset = std::nullopt) {
    const auto sites = auditPostoptAsyncRoots(module);
    std::vector<ExperimentalAsyncStackmapSite> records;
    std::unordered_set<uint64_t> emittedIds;
    auto* stackmap = llvm::Intrinsic::getOrInsertDeclaration(
        &module, llvm::Intrinsic::experimental_stackmap);
    for (const auto& site : sites) {
        ExperimentalAsyncStackmapSite record;
        record.functionName = site.functionName;
        record.siteOrdinal = site.siteOrdinal;
        record.stackmapId = asyncStackmapProbeId(
            site.functionName, site.siteOrdinal);
        record.livePointers = site.livePointers;
        record.liveI64 = site.liveI64;
        record.pointerRegions = site.pointerRegions;
        // Capture the exact order passed to llvm.experimental.stackmap.
        // LLVM may allocate a register for the address of a load, but
        // that is NOT the provenance of the loaded pointer value.
        const llvm::Argument* context =
            !site.reporter->getFunction()->arg_empty()
            ? site.reporter->getFunction()->getArg(0) : nullptr;
        for (unsigned i = 0; i < site.livePointerValues.size(); ++i) {
            const auto* v = site.livePointerValues[i];
            ExperimentalAsyncPointerOrigin info;
            info.locationIndex = i + 1;
            info.region = classifyPointerRegion(v, context);
            info.unknownCause = info.region == PointerRegion::Unknown
                ? classifyUnknownPointerCause(v, context)
                : UnknownPointerCause::Other;
            if (info.region == PointerRegion::Unknown &&
                trustedVarSlotFieldOffset) {
                info.candidateBorrowedVarSlot =
                    asyncBorrowedVarSlotIndex(
                        v, context, module.getDataLayout(),
                        *trustedVarSlotFieldOffset);
            }
            if (info.region == PointerRegion::Unknown &&
                (info.unknownCause == UnknownPointerCause::LoadStack ||
                 info.unknownCause == UnknownPointerCause::LoadHost ||
                 info.unknownCause == UnknownPointerCause::LoadStatic ||
                 info.unknownCause == UnknownPointerCause::LoadUnknown)) {
                info.hasUnknownLoadChain = true;
                info.unknownLoadChain = tracePointerLoadChain(v, context);
            }
            if (const auto* instruction =
                    llvm::dyn_cast<llvm::Instruction>(v)) {
                info.definitionKind = instruction->getOpcodeName();
                if (const auto* called =
                        llvm::dyn_cast<llvm::CallBase>(instruction)) {
                    if (const llvm::Function* target =
                            called->getCalledFunction())
                        info.calleeName = target->getName().str();
                }
            } else if (const auto* arg =
                           llvm::dyn_cast<llvm::Argument>(v)) {
                info.definitionKind = "argument";
                info.llvmValueId = 0x80000000u | arg->getArgNo();
            } else if (llvm::isa<llvm::GlobalValue>(v)) {
                info.definitionKind = "global";
            } else if (llvm::isa<llvm::ConstantExpr>(v)) {
                info.definitionKind = "constexpr";
            } else {
                info.definitionKind = "other";
            }
            record.pointerOrigins.push_back(std::move(info));
        }
        uint64_t regionSum = 0;
        for (const auto count : site.pointerRegions) regionSum += count;
        const size_t operandCount =
            site.livePointerValues.size() + site.liveI64Values.size();
        if (!site.signatureValid || !site.bufferValid ||
            !site.completeStores || !site.coveredAllLiveI64 ||
            !site.reporter ||
            regionSum != site.livePointers ||
            site.livePointerValues.size() != site.livePointers ||
            site.liveI64Values.size() != site.liveI64 ||
            operandCount > 128 ||
            record.pointerOrigins.size() != site.livePointers ||
            !emittedIds.insert(record.stackmapId).second) {
            records.push_back(std::move(record));
            continue;
        }
        llvm::IRBuilder<> builder(site.reporter);
        std::vector<llvm::Value*> operands;
        operands.reserve(3 + operandCount);
        operands.push_back(builder.getInt64(record.stackmapId));
        operands.push_back(builder.getInt32(0));
        // Explicit JitCtx for comparison with the post-O3 pointer SSA
        // census. Keep individual sites and operand counts deterministic.
        operands.push_back(site.reporter->getArgOperand(0));
        bool typesValid = true;
        for (auto* ptr : site.livePointerValues) {
            if (!ptr || !ptr->getType()->isPointerTy()) {
                typesValid = false;
                break;
            }
            operands.push_back(ptr);
        }
        for (auto* word : site.liveI64Values) {
            if (!word || !word->getType()->isIntegerTy(64)) {
                typesValid = false;
                break;
            }
            operands.push_back(word);
        }
        if (typesValid) {
            builder.CreateCall(stackmap, operands);
            record.emitted = true;
        }
        records.push_back(std::move(record));
    }
    return records;
}

struct PostoptSsaSite {
    std::string functionName;
    uint64_t siteId = 0;
    uint32_t declaredPointers = 0;
    uint32_t capturedWords = 0;
    uint32_t postoptNumeric = 0;
    uint32_t postoptLivePointers = 0;
    uint32_t postoptUnknownPointers = 0;
    uint32_t postoptStackPointers = 0;
    uint32_t postoptHostPointers = 0;
    uint32_t postoptStaticPointers = 0;
    uint32_t postoptOther = 0;
    bool signatureValid = false;
    bool reportValid = false;
    // Diagnostic-only, borrowed while the LLVM Module is live. Retained
    // solely for post-optimizer probe insertion, never used as GC roots.
    llvm::CallInst* captureCall = nullptr;
    std::vector<llvm::Value*> unknownPointerValues;
    // NO gcSafe permission: this is a conservative diagnostic.
};

inline std::vector<PostoptSsaSite> auditPostoptSsaCaptures(
    llvm::Module& module) {
    std::vector<PostoptSsaSite> sites;
    for (llvm::Function& function : module) {
        if (function.isDeclaration()) continue;
        for (llvm::BasicBlock& block : function) {
            for (llvm::Instruction& instruction : block) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call || !call->getCalledFunction() ||
                    call->getCalledFunction()->getName() !=
                        "xyo_rt_capture_llvm_ssa")
                    continue;
                PostoptSsaSite result;
                result.functionName = function.getName().str();
                const llvm::Argument* trustedContext =
                    !function.arg_empty() &&
                    function.getArg(0)->getType()->isPointerTy()
                        ? function.getArg(0) : nullptr;
                const auto* site = call->arg_size() > 9
                    ? llvm::dyn_cast<llvm::ConstantInt>(
                        call->getArgOperand(9)) : nullptr;
                const auto* prePointers = call->arg_size() > 10
                    ? llvm::dyn_cast<llvm::ConstantInt>(
                        call->getArgOperand(10)) : nullptr;
                const auto* captured = call->arg_size() > 3
                    ? llvm::dyn_cast<llvm::ConstantInt>(
                        call->getArgOperand(3)) : nullptr;
                const llvm::FunctionType* signature =
                    call->getCalledFunction()->getFunctionType();
                result.signatureValid =
                    trustedContext && call->arg_size() == 17 &&
                    !signature->isVarArg() &&
                    signature->getNumParams() == 17 &&
                    signature->getReturnType()->isVoidTy() &&
                    call->getArgOperand(0) == trustedContext &&
                    site && site->getType()->isIntegerTy(64) &&
                    prePointers && prePointers->getType()->isIntegerTy(32) &&
                    captured && captured->getType()->isIntegerTy(32);
                if (site) result.siteId = site->getZExtValue();
                if (prePointers)
                    result.declaredPointers =
                        uint32_t(prePointers->getZExtValue());
                if (captured)
                    result.capturedWords = uint32_t(captured->getZExtValue());
                // Fail closed for unexpected helpers; still count live
                // values, but do NOT trust a malformed manifest.
                const auto report = analyzeExtraLiveSsa(
                    function, *call, trustedContext);
                const auto numeric = ir::analyzeLlvmCheckpointLiveness(
                    function, *call);
                result.postoptNumeric =
                    uint32_t(numeric.numeric.size());
                result.postoptLivePointers = report.unresolvedPointer;
                result.postoptUnknownPointers =
                    report.pointerRegions[size_t(PointerRegion::Unknown)];
                result.postoptStackPointers =
                    report.pointerRegions[size_t(PointerRegion::NativeStack)];
                result.postoptHostPointers =
                    report.pointerRegions[size_t(PointerRegion::HostContext)];
                result.postoptStaticPointers =
                    report.pointerRegions[size_t(PointerRegion::StaticGlobal)];
                result.postoptOther = report.unresolvedOther;
                result.reportValid =
                    result.signatureValid && report.valid &&
                    numeric.validCheckpoint;
                // Replay the SAME function-local numbering as the
                // pre-existing ExtraLiveSsaReport. Only the Unknown
                // pointer candidates are added to the machine stackmap.
                // This doesn't interpret pointer bits or imply GC roots.
                if (result.reportValid) {
                    std::unordered_set<uint32_t> ids;
                    for (const auto& detail : report.unknownPointerDetails)
                        ids.insert(detail.instructionId);
                    if (ids.size() != result.postoptUnknownPointers)
                        result.reportValid = false;
                    for (llvm::Argument& argument : function.args()) {
                        if (argument.getArgNo() > 0x7fffffffu) continue;
                        if (ids.erase(
                                0x80000000u | argument.getArgNo()))
                            result.unknownPointerValues.push_back(&argument);
                    }
                    uint32_t instructionId = 0;
                    for (llvm::BasicBlock& inner : function) {
                        for (llvm::Instruction& candidate : inner) {
                            if (ids.erase(instructionId))
                                result.unknownPointerValues.push_back(
                                    &candidate);
                            ++instructionId;
                        }
                    }
                    if (!ids.empty() ||
                        result.unknownPointerValues.size() !=
                            result.postoptUnknownPointers)
                        result.reportValid = false;
                    result.captureCall =
                        llvm::dyn_cast<llvm::CallInst>(call);
                    if (!result.captureCall ||
                        !result.captureCall->getNextNode())
                        result.reportValid = false;
                }
                sites.push_back(std::move(result));
            }
        }
    }
    return sites;
}

// FNV-1a with a domain discriminator and exact LLVM function name.
// Function names are unique inside one module. Detect collisions and
// fail closed rather than silently merging stackmap records.
inline uint64_t stackmapProbeId(const llvm::StringRef name,
                                const uint64_t sourceSite) {
    uint64_t hash = 14695981039346656037ULL;
    const auto feed = [&hash](unsigned char byte) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    for (const char c : name)
        feed(static_cast<unsigned char>(c));
    feed(0xff);
    for (unsigned i = 0; i < 8; ++i)
        feed(static_cast<unsigned char>(sourceSite >> (8u * i)));
    return hash;
}

struct ExperimentalStackmapSite {
    std::string functionName;
    uint64_t sourceSite = 0;
    uint64_t stackmapId = 0;
    uint32_t unknownPointerLocations = 0;
    bool emitted = false;
};

// Experimental, diagnostic-only machine-level locations. This runs
// AFTER O2/O3 and uses llvm.experimental.stackmap for JitCtx and EVERY
// post-opt Unknown pointer. It does not map boxed i64 Scratch Values,
// moving GC ownership, or paused Fiber safepoints; a collector MUST NOT
// infer that any of these records are complete or safe to sweep.
inline std::vector<ExperimentalStackmapSite>
emitExperimentalPostoptStackmaps(llvm::Module& module) {
    const auto sites = auditPostoptSsaCaptures(module);
    std::vector<ExperimentalStackmapSite> reports;
    std::unordered_set<uint64_t> emittedIds;
    auto* intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        &module, llvm::Intrinsic::experimental_stackmap);
    for (const auto& site : sites) {
        ExperimentalStackmapSite record;
        record.functionName = site.functionName;
        record.sourceSite = site.siteId;
        record.stackmapId = stackmapProbeId(
            site.functionName, site.siteId);
        record.unknownPointerLocations = site.postoptUnknownPointers;
        // Probe refusal is NOT evidence that a capture is GC safe:
        // malformed manifests or unexpectedly large candidate lists
        // remain uninstrumented and are still denied collection.
        if (!site.reportValid ||
            !site.captureCall ||
            site.postoptUnknownPointers > 64 ||
            !emittedIds.insert(record.stackmapId).second) {
            reports.push_back(std::move(record));
            continue;
        }
        llvm::IRBuilder<> builder(site.captureCall->getNextNode());
        std::vector<llvm::Value*> args;
        args.reserve(3 + site.unknownPointerValues.size());
        args.push_back(builder.getInt64(record.stackmapId));
        args.push_back(builder.getInt32(0)); // shadow bytes
        args.push_back(site.captureCall->getArgOperand(0)); // JitCtx*
        for (llvm::Value* pointer : site.unknownPointerValues) {
            if (!pointer || !pointer->getType()->isPointerTy()) {
                args.clear();
                break;
            }
            args.push_back(pointer);
        }
        if (!args.empty()) {
            builder.CreateCall(intrinsic, args);
            record.emitted = true;
        }
        reports.push_back(std::move(record));
    }
    return reports;
}
} // namespace xyo::rt
