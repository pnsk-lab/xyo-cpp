// IR-001: exact source slot -> LIVE LLVM SSA instruction -> detached
// typed word, without treating raw numeric coverage as executable deopt.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/llvm_source_numeric_bindings.hpp"
#include "../../src/ir/llvm_source_reporter_trace.hpp"
#include "../../src/ir/source_reporter_ssa_location_bridge.hpp"
#include "../../src/ir/llvm_ssa_spill.hpp"
#include "../../src/engine/thread.hpp"
#include "../../src/jit/procedure_frame_state.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace xyo;
using namespace xyo::ir;

int main() {
    llvm::LLVMContext context;
    llvm::Module module("ir001.frozen.source.numeric.slots", context);
    llvm::IRBuilder<> builder(context);
    auto* function = llvm::Function::Create(
        llvm::FunctionType::get(builder.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "trace_numeric", module);
    builder.SetInsertPoint(
        llvm::BasicBlock::Create(context, "entry", function));
    const auto getI64 = module.getOrInsertFunction(
        "numeric_expression",
        llvm::FunctionType::get(builder.getInt64Ty(), false));
    const auto getI32 = module.getOrInsertFunction(
        "loop_counter",
        llvm::FunctionType::get(builder.getInt32Ty(), false));
    const auto getI1 = module.getOrInsertFunction(
        "unused_boolean",
        llvm::FunctionType::get(builder.getInt1Ty(), false));
    const auto checkpointFn = module.getOrInsertFunction(
        "xyo_proc_frame_checkpoint_path_phase",
        llvm::FunctionType::get(builder.getVoidTy(),
            {builder.getPtrTy(), builder.getPtrTy(),
             builder.getPtrTy(), builder.getInt32Ty(),
             builder.getInt32Ty()}, false));
    const auto sinkFn = module.getOrInsertFunction(
        "consume_numeric",
        llvm::FunctionType::get(builder.getVoidTy(),
            {builder.getInt32Ty(), builder.getInt64Ty(),
             builder.getInt1Ty()}, false));
    // Raw host-produced i64/i32 values may be boxed Scratch Values,
    // so only a guaranteed integer comparison (i1) and its zero
    // extension may be labelled a raw numeric source expression.
    auto* opaqueValue = builder.CreateCall(getI64, {});
    auto* opaqueCounter = builder.CreateCall(getI32, {});
    auto* expression = llvm::cast<llvm::Instruction>(
        builder.CreateZExt(
            builder.CreateICmpEQ(opaqueValue, builder.getInt64(7)),
            builder.getInt64Ty()));
    auto* loopCounter = llvm::cast<llvm::Instruction>(
        builder.CreateZExt(
            builder.CreateICmpEQ(opaqueCounter, builder.getInt32(3)),
            builder.getInt32Ty()));
    auto* anotherNumeric = builder.CreateCall(getI1, {});
    // Deleted by an LLVM rewrite: a WeakTrackingVH must not retain
    // a dangling pointer or fabricate live Scratch source state.
    auto* discarded = builder.CreateCall(getI64, {});
    auto* nullPtr = llvm::ConstantPointerNull::get(builder.getPtrTy());
    auto* checkpoint = builder.CreateCall(checkpointFn,
        {nullPtr, nullPtr, nullPtr,
         builder.getInt32(1), builder.getInt32(2)});
    auto* tooLate = builder.CreateCall(getI64, {});
    // Same LLVM function and source block, but different physical
    // callsite and continuation phase; a source site must point at
    // the EXACT CallBase, not just its containing function.
    auto* checkpointLater = builder.CreateCall(checkpointFn,
        {nullPtr, nullPtr, nullPtr,
         builder.getInt32(1), builder.getInt32(1)});
    builder.CreateCall(sinkFn,
                       {loopCounter, expression, anotherNumeric});
    builder.CreateCall(module.getOrInsertFunction("consume_unverified",
        llvm::FunctionType::get(builder.getVoidTy(),
                               {builder.getInt64Ty()}, false)),
                       {opaqueValue}); // untrusted Call remains live
    builder.CreateCall(module.getOrInsertFunction("consume_late",
        llvm::FunctionType::get(builder.getVoidTy(),
                               {builder.getInt64Ty()}, false)), {tooLate});
    builder.CreateRetVoid();
    assert(!llvm::verifyFunction(*function, &llvm::errs()));

    const auto live = analyzeLlvmCheckpointLiveness(
        *function, *checkpoint);
    assert(live.numericComplete() && live.numeric.size() == 4);
    assert(!provenLlvmRawNumeric(opaqueValue));
    assert(!provenLlvmRawNumeric(opaqueCounter));
    assert(!provenLlvmRawNumeric(anotherNumeric));
    assert(provenLlvmRawNumeric(expression));
    assert(provenLlvmRawNumeric(loopCounter));
    const auto descriptorOf = [&](const llvm::Instruction* instruction) {
        auto it = std::find_if(live.numeric.begin(), live.numeric.end(),
            [&](const LlvmLiveValue& value) {
                return value.definition == instruction;
            });
        assert(it != live.numeric.end());
        return uint64_t(it->id) | (uint64_t(it->width) << 32);
    };
    const auto expressionDescriptor = descriptorOf(expression);
    const auto loopDescriptor = descriptorOf(loopCounter);

    // Real Scratch Blocks -> frozen compiler-local compilation unit ->
    // independent RT-001 source frame symbol table. Numeric SSA slots
    // still use explicit *lowerer-provided* labels: source graph names
    // alone cannot discover the compiler's Value* expression origin.
    Blocks blocks(nullptr);
    for (int i = 1; i <= 10; ++i) {
        Block block;
        char name[8];
        std::snprintf(name, sizeof(name), "s%02d", i);
        block.id = name;
        block.opcode = i == 1
            ? "event_whenflagclicked" : "data_changevariableby";
        if (i < 10) {
            std::snprintf(name, sizeof(name), "s%02d", i + 1);
            block.next = name;
        }
        if (i > 1) {
            std::snprintf(name, sizeof(name), "s%02d", i - 1);
            block.parent = name;
        }
        blocks.createBlock(std::move(block));
    }
    // A reporter is explicitly owned by one frozen Scratch input.
    // It is NOT an executable statement and receives no source-frame
    // statement ownership despite its presence in the PC dictionary.
    Block reporter;
    reporter.id = "zz-reporter";
    reporter.opcode = "data_variable";
    reporter.parent = "s08";
    blocks.createBlock(std::move(reporter));
    blocks.getBlock("s08")->inputs.set(
        "VALUE", {"zz-reporter", ""});
    const auto originalSource = ScratchCompilationUnit::capture(
        blocks, blocks.getBlock("s01"));
    assert(originalSource.valid());
    assert(classifyFrozenReporter(
        originalSource.script, "zz-reporter") ==
        SourceReporterOrigin::VariableRead);
    assert(classifyFrozenReporter(
        originalSource.script, "unknown") ==
        SourceReporterOrigin::Unknown);
    assert(sourceReporterOriginName(
        SourceReporterOrigin::VariableRead) ==
        std::string_view("variable_read"));
    // Source-opcode classification does not infer GC root ownership.
    auto categorized = originalSource.script;
    categorized.nodes[categorized.byId.at("zz-reporter")].opcode =
        "data_itemoflist";
    assert(classifyFrozenReporter(categorized, "zz-reporter") ==
        SourceReporterOrigin::ListRead);
    categorized.nodes[categorized.byId.at("zz-reporter")].opcode =
        "argument_reporter_string_number";
    assert(classifyFrozenReporter(categorized, "zz-reporter") ==
        SourceReporterOrigin::ProcedureArgument);
    categorized.nodes[categorized.byId.at("zz-reporter")].opcode =
        "operator_join";
    assert(classifyFrozenReporter(categorized, "zz-reporter") ==
        SourceReporterOrigin::ComputedExpression);
    categorized.nodes[categorized.byId.at("zz-reporter")].opcode =
        "math_number";
    assert(classifyFrozenReporter(categorized, "zz-reporter") ==
        SourceReporterOrigin::Literal);
    categorized.nodes[categorized.byId.at("zz-reporter")].opcode =
        "sensing_timer";
    assert(classifyFrozenReporter(categorized, "zz-reporter") ==
        SourceReporterOrigin::OtherReporter);
    assert(frozenSourceReporterInputEdge(
        originalSource.script, "s08", "VALUE", "zz-reporter"));
    assert(!frozenSourceReporterInputEdge(
        originalSource.script, "s09", "VALUE", "zz-reporter"));
    assert(!frozenSourceReporterInputEdge(
        originalSource.script, "s08", "TIMES", "zz-reporter"));
    assert(!frozenSourceReporterInputEdge(
        originalSource.script, "s08", "VALUE", "s07"));
    assert(!frozenSourceReporterInputEdge(
        originalSource.script, "s08", "VALUE", "unknown-block"));
    auto fraudulent = originalSource.script;
    const size_t reporterAt = fraudulent.byId.at("zz-reporter");
    fraudulent.nodes[reporterAt].parent = "s07";
    assert(!frozenSourceReporterInputEdge(
        fraudulent, "s08", "VALUE", "zz-reporter"));
    const auto sourceDigest = scratchSourceDigest(originalSource);
    NativeProcedureFrameSession compilerSource(
        originalSource, &blocks,
        originalSource.sourceRevision, sourceDigest);
    assert(compilerSource.enabled());
    const auto immutableSourceFrame = compilerSource.exportSelfContained();
    assert(immutableSourceFrame && immutableSourceFrame->valid());
    const SourceFrameSymbols frozen = *immutableSourceFrame->symbols;
    assert(frozen.block(8) == "s08");
    assert(frozen.block(9) == "s09");
    assert(frozen.block(10) == "s10");

    SourceTypedSsaManifest site;
    site.revision = originalSource.sourceRevision;
    site.digestLo = sourceDigest.lo; site.digestHi = sourceDigest.hi;
    site.procedureId = 0;
    site.resumePc = 19;
    site.siteId = 23;
    for (const auto& value : live.numeric)
        site.descriptors.push_back(
            uint64_t(value.id) | (uint64_t(value.width) << 32));
    assert(site.valid());
    // Independent frozen source-side required environment. The Scratch
    // variable source slot is intentionally unsupported: no LLVM i64
    // value can manufacture its GC/binding identity.
    const SourceResumeSlot expressionSlot{
        8, 1, SourceResumeSlotKind::NumericExpression, 64};
    const SourceResumeSlot loopSlot{
        9, 1, SourceResumeSlotKind::NumericExpression, 32};
    const SourceResumeSlot sourceVariable{
        10, 1, SourceResumeSlotKind::ScratchVariable, 0};
    const SourceResumeRequirements expected{
        site, {expressionSlot, loopSlot, sourceVariable}};
    assert(expected.valid());

    // This proof comes from actual Scratch source ownership and a
    // separately frozen RT-001 dictionary, not an invented table.
    assert(verifyFrozenSourceResumeSlots(expected, frozen));
    // A data_changevariableby source statement is NOT a counted loop.
    auto counterfeitLoop = expected;
    counterfeitLoop.required[1].kind =
        SourceResumeSlotKind::NumericLoopTemporary;
    assert(!verifyFrozenSourceResumeSlots(counterfeitLoop, frozen));
    const std::vector<LlvmSourceNumericRegistration> evidence{
        {loopSlot, loopCounter}, {expressionSlot, expression}};
    const auto bound = bindLlvmSourceNumericSlotsFromFrozenSource(
        *function, *checkpoint, expected, frozen, evidence);
    assert(bound && bound->size() == 2);
    assert((*bound)[0].target == expressionSlot);
    assert((*bound)[0].descriptor == expressionDescriptor);
    assert((*bound)[1].target == loopSlot);
    assert((*bound)[1].descriptor == loopDescriptor);

    auto corruptedSource = frozen;
    corruptedSource.blockIds[8] = "forged-scratch-statement";
    assert(!verifyFrozenSourceResumeSlots(expected, corruptedSource));
    assert(!bindLlvmSourceNumericSlotsFromFrozenSource(
        *function, *checkpoint, expected, corruptedSource, evidence));
    corruptedSource = frozen;
    corruptedSource.statementOwners[8] = 37; // different procedure
    assert(!verifyFrozenSourceResumeSlots(expected, corruptedSource));
    corruptedSource = frozen;
    corruptedSource.statementOwners[7] = CallFrameAbi::kNoPc;
    assert(!verifyFrozenSourceResumeSlots(expected, corruptedSource));
    corruptedSource = frozen;
    corruptedSource.sourceTopology[8].parentPc = 0;
    assert(!verifyFrozenSourceResumeSlots(expected, corruptedSource));
    corruptedSource = frozen;
    ++corruptedSource.sourceDigest.lo;
    assert(!verifyFrozenSourceResumeSlots(expected, corruptedSource));

    NativeRtSsaCapture snapshot;
    snapshot.sourceRevision = site.revision;
    snapshot.digestLo = site.digestLo;
    snapshot.digestHi = site.digestHi;
    snapshot.sourcePc = site.resumePc;
    snapshot.procedureId = site.procedureId;
    snapshot.siteId = site.siteId;
    snapshot.descriptors = site.descriptors;
    // Read-only copied numeric checkpoint words. No GC values here.
    for (const auto& value : live.numeric) {
        const auto width = value.width;
        snapshot.words.push_back(
            width == 1 ? uint64_t(1) :
            width == 32 ? uint64_t(17) : uint64_t(12345));
    }
    snapshot.numericComplete = true;
    snapshot.fingerprint = snapshot.computeFingerprint();
    assert(verifyDetachedTypedSsa(snapshot, site));
    const auto coverage = auditSourceNumericCoverage(
        snapshot, site, expected, *bound);
    assert(coverage);
    assert(coverage->required == 3);
    assert(coverage->mappedNumeric == 2);
    assert(coverage->missing == 1);
    assert(coverage->unsupportedSourceState == 1);
    assert(!coverage->knownNumericSubsetComplete());
    assert(!coverage->sourceEnvironmentComplete());
    assert(!coverage->executable());

    const SourceResumeRequirements numericOnly{
        site, {expressionSlot, loopSlot}};
    const auto subset = auditSourceNumericCoverage(
        snapshot, site, numericOnly, *bound);
    assert(subset && subset->required == 2);
    assert(subset->knownNumericSubsetComplete());
    // Even complete source-requirement NUMERIC subsets do not prove
    // that the source enumerator found every Scratch live local.
    assert(!subset->sourceEnvironmentComplete());
    assert(!subset->executable());

    // A stale/different source ID, wrong typed manifest and forged
    // 32-bit spill with an overwide word must all fail closed.
    auto stale = site;
    ++stale.siteId;
    assert(!auditSourceNumericCoverage(snapshot, stale, numericOnly, *bound));
    stale = site; ++stale.digestLo;
    assert(!auditSourceNumericCoverage(snapshot, stale, numericOnly, *bound));
    auto changed = snapshot;
    for (size_t i = 0; i < changed.descriptors.size(); ++i) {
        if ((changed.descriptors[i] >> 32) == 32) {
            changed.words[i] = uint64_t{1} << 33;
            break;
        }
    }
    changed.fingerprint = changed.computeFingerprint();
    assert(!auditSourceNumericCoverage(changed, site, numericOnly, *bound));

    auto duplicateTarget = evidence;
    duplicateTarget.push_back({loopSlot, loopCounter});
    assert(!bindLlvmSourceNumericSlots(
        *function, *checkpoint, expected, duplicateTarget));
    auto sameDefinition = evidence;
    sameDefinition[1] = {expressionSlot, loopCounter};
    assert(!bindLlvmSourceNumericSlots(
        *function, *checkpoint, expected, sameDefinition));
    auto unknownHost = evidence;
    unknownHost[1].definition = opaqueValue;
    assert(!bindLlvmSourceNumericSlots(
        *function, *checkpoint, expected, unknownHost));
    auto future = evidence;
    future[1].definition = tooLate;
    assert(!bindLlvmSourceNumericSlots(
        *function, *checkpoint, expected, future));
    auto mistyped = evidence;
    mistyped[0].source.width = 64; // original loop counter is i32
    assert(!bindLlvmSourceNumericSlots(
        *function, *checkpoint, expected, mistyped));
    auto notNumeric = evidence;
    notNumeric[1].source = sourceVariable;
    assert(!bindLlvmSourceNumericSlots(
        *function, *checkpoint, expected, notNumeric));

    // The per-source slot identity must belong to the EXACT frozen
    // requirements. Merely live and numerically typed SSA is not
    // enough to establish a Scratch expression's recovery slot.
    auto foreignSite = evidence;
    foreignSite[1].source.statementPc = 12345;
    assert(!bindLlvmSourceNumericSlots(
        *function, *checkpoint, expected, foreignSite));

    auto duplicateBinding = *bound;
    duplicateBinding.push_back((*bound)[0]);
    assert(!auditSourceNumericCoverage(
        snapshot, site, expected, duplicateBinding));
    auto wrongDescriptor = *bound;
    wrongDescriptor[0].descriptor ^= uint64_t{1};
    assert(!auditSourceNumericCoverage(
        snapshot, site, expected, wrongDescriptor));
    auto forgedDestination = *bound;
    forgedDestination[1].target = sourceVariable;
    assert(!auditSourceNumericCoverage(
        snapshot, site, expected, forgedDestination));

    // Silent direct Block edits may leave Blocks::revision unchanged.
    // Rebuilding source symbols from that changed content MUST NOT
    // let the old typed checkpoint attach to the newly captured code.
    const uint64_t oldRevision = blocks.revision();
    blocks.getBlock("s09")->opcode = "looks_hide";
    assert(blocks.revision() == oldRevision);
    const auto changedSource = ScratchCompilationUnit::capture(
        blocks, blocks.getBlock("s01"));
    assert(changedSource.valid());
    const auto changedDigest = scratchSourceDigest(changedSource);
    assert(changedDigest != sourceDigest);
    NativeProcedureFrameSession changedCompiler(
        changedSource, &blocks, changedSource.sourceRevision, changedDigest);
    assert(changedCompiler.enabled());
    const auto changedImage = changedCompiler.exportSelfContained();
    assert(changedImage && changedImage->valid());
    assert(!verifyFrozenSourceResumeSlots(expected, *changedImage->symbols));

    // The production reporter observer uses weak LLVM Value* keys,
    // including ones deleted by subsequent inlining/IR transforms.
    // Synthetic i64 constants may contain a NaN-boxed heap tag and
    // cannot be promoted to recoverable raw numeric Scratch state.
    std::vector<SourceReporterValueObservation> reporters{
        {8, "s08", "s07", "VALUE", llvm::WeakTrackingVH(expression),
            SourceReporterOrigin::ComputedExpression},
        {9, "s09", "s08", "VALUE", llvm::WeakTrackingVH(loopCounter),
            SourceReporterOrigin::ComputedExpression},
        {7, "s07", "s06", "VALUE", llvm::WeakTrackingVH(opaqueValue),
            SourceReporterOrigin::VariableRead},
        {6, "s06", "s05", "VALUE", llvm::WeakTrackingVH(
            builder.getInt64(0x7ff9000000000010ULL)),
            SourceReporterOrigin::Literal},
        {5, "s05", "s04", "VALUE", llvm::WeakTrackingVH(discarded),
            SourceReporterOrigin::ProcedureArgument},
        {4, "s04", "s03", "VALUE", llvm::WeakTrackingVH(tooLate),
            SourceReporterOrigin::ListRead}
    };
    discarded->eraseFromParent();
    const std::vector<SourceReporterCheckpointObservation> actualSites{
        {9, "s09", SourceResumePhase::AfterLoopBody,
         llvm::WeakTrackingVH(checkpoint), 1, {"s09"}},
        {9, "s09", SourceResumePhase::InsideLoopBody,
         llvm::WeakTrackingVH(checkpointLater), 1, {"s09"}}
    };
    const auto provenance = traceSourceReporterValues(
        module, reporters, false, actualSites);
    assert(provenance.checkpointSites == 2);
    assert(provenance.attributedCheckpoints == 2);
    assert(provenance.unmatchedSourceSites == 0);
    assert(provenance.checkpointGaps.size() == 2);
    assert(provenance.observed == 6);
    assert(provenance.remaining == 5 && provenance.discarded == 1);
    assert(provenance.constants == 1);
    assert(provenance.rawNumeric == 1); // synthetic raw LLVM i32
    assert(provenance.boxedOrUnknown == 4);
    assert(provenance.liveAtCheckpoint == 4);
    const auto& firstGap = provenance.checkpointGaps[0];
    const auto& nextGap = provenance.checkpointGaps[1];
    assert(firstGap.sourcePc == 9 && nextGap.sourcePc == 9);
    assert(firstGap.physicalSiteId == 1);
    assert(nextGap.physicalSiteId == 2);
    assert(firstGap.physicalSiteId != nextGap.physicalSiteId);
    // The compiler must freeze its independently observed reporter
    // origins before RT receives a mutable SSA image. Same Scratch PC
    // across two physical CallBases requires two distinct site contracts.
    const auto freezeActualSite = [&](const auto& gap,
                                      llvm::CallBase& nativeMarker) {
        const auto atSite = analyzeLlvmCheckpointLiveness(
            *function, nativeMarker);
        SourceTypedSsaManifest manifest;
        manifest.revision = originalSource.sourceRevision;
        manifest.digestLo = sourceDigest.lo;
        manifest.digestHi = sourceDigest.hi;
        manifest.procedureId = 0;
        manifest.resumePc = gap.sourcePc;
        manifest.siteId = gap.physicalSiteId;
        for (const auto& candidate : atSite.numeric)
            manifest.descriptors.push_back(
                uint64_t(candidate.id) |
                (uint64_t(candidate.width) << 32));
        assert(manifest.valid());
        const auto frozenSource = freezeReporterSourceSite(gap, manifest);
        assert(frozenSource && frozenSource->matches(gap, manifest));
        return std::pair{*frozenSource, std::move(manifest)};
    };
    const auto [siteOne, manifestOne] =
        freezeActualSite(firstGap, *checkpoint);
    const auto [siteTwo, manifestTwo] =
        freezeActualSite(nextGap, *checkpointLater);
    assert(siteOne.physicalSiteId != siteTwo.physicalSiteId);
    assert(!siteOne.matches(nextGap, manifestOne));
    assert(!siteTwo.matches(firstGap, manifestTwo));
    auto retargetSource = nextGap;
    assert(retargetSource.reporterSsaBindings.size() > 1 &&
           retargetSource.reporterSsaBindings.front().descriptor !=
               retargetSource.reporterSsaBindings.back().descriptor);
    retargetSource.reporterSsaBindings.front().descriptor =
        retargetSource.reporterSsaBindings.back().descriptor;
    assert(!siteTwo.matches(retargetSource, manifestTwo));
    assert(firstGap.phase == SourceResumePhase::AfterLoopBody);
    assert(nextGap.phase == SourceResumePhase::InsideLoopBody);
    assert(firstGap.liveReporterObservations == 3);
    assert(nextGap.liveReporterObservations == 4);
    assert(firstGap.reporterSsaBindings.size() ==
           firstGap.liveReporterObservations);
    assert(nextGap.reporterSsaBindings.size() ==
           nextGap.liveReporterObservations);
    // Reuse the exact numeric SSA ID/width emitted by the real liveness
    // analyzer for the same physical LLVM CallBase, not a guessed value.
    const auto verifyReporterDescriptors = [&](const auto& gap,
                                               llvm::CallBase& call) {
        const auto live = analyzeLlvmCheckpointLiveness(*function, call);
        assert(live.validCheckpoint);
        for (const auto& binding : gap.reporterSsaBindings) {
            const auto found = std::find_if(
                live.numeric.begin(), live.numeric.end(),
                [&](const auto& entry) {
                    return binding.descriptor ==
                        (uint64_t(entry.id) |
                         (uint64_t(entry.width) << 32));
                });
            assert(found != live.numeric.end());
            assert(binding.reporterPc != 0);
        }
    };
    verifyReporterDescriptors(firstGap, *checkpoint);
    verifyReporterDescriptors(nextGap, *checkpointLater);
    assert(std::none_of(firstGap.reporterSsaBindings.begin(),
                        firstGap.reporterSsaBindings.end(),
                        [](const auto& item) { return item.reporterPc == 4; }));
    assert(std::any_of(nextGap.reporterSsaBindings.begin(),
                       nextGap.reporterSsaBindings.end(),
                       [](const auto& item) {
                           return item.reporterPc == 4 &&
                               item.origin == SourceReporterOrigin::ListRead;
                       }));
    assert(firstGap.boxedWithoutGcProof == 2);
    assert(nextGap.boxedWithoutGcProof == 3);
    assert(firstGap.numericWithoutSourceSlot == 1);
    assert(nextGap.numericWithoutSourceSlot == 1);
    assert(firstGap.unmappedSourceSlots == 3);
    assert(nextGap.unmappedSourceSlots == 4);
    assert(firstGap.computedExpressions == 2);
    assert(nextGap.computedExpressions == 2);
    assert(firstGap.variableReads == 1 && nextGap.variableReads == 1);
    assert(firstGap.listReads == 0 && nextGap.listReads == 1);
    assert(firstGap.portableLoopPathDepth == 1);
    assert(nextGap.portableLoopPathDepth == 1);
    assert(firstGap.sourceLoopPath == std::vector<std::string>{"s09"});
    assert(nextGap.sourceLoopPath == std::vector<std::string>{"s09"});
    assert(!firstGap.loopPathRestorable());
    assert(!nextGap.loopPathRestorable());
    assert(!firstGap.executable() && !nextGap.executable());

    // Reusing an identical physical checkpoint for multiple source
    // labels, or forging its phase, cannot manufacture two safe sites.
    auto repeatedSourceSite = actualSites;
    repeatedSourceSite.push_back(actualSites[0]);
    const auto repeated = traceSourceReporterValues(
        module, reporters, false, repeatedSourceSite);
    assert(repeated.attributedCheckpoints == 2);
    assert(repeated.unmatchedSourceSites == 1);
    auto forgedPhase = actualSites;
    forgedPhase[0].phase = SourceResumePhase::InsideLoopBody;
    const auto rejected = traceSourceReporterValues(
        module, reporters, false, forgedPhase);
    assert(rejected.attributedCheckpoints == 1);
    assert(rejected.unmatchedSourceSites == 1);
    auto falseLoopDepth = actualSites;
    falseLoopDepth[0].portableLoopPathDepth = 2;
    const auto rejectedDepth = traceSourceReporterValues(
        module, reporters, false, falseLoopDepth);
    assert(rejectedDepth.attributedCheckpoints == 1);
    assert(rejectedDepth.unmatchedSourceSites == 1);
    auto forgedPath = actualSites;
    forgedPath[0].sourceLoopPath = {"s08"};
    const auto rejectedPath = traceSourceReporterValues(
        module, reporters, false, forgedPath);
    assert(rejectedPath.attributedCheckpoints == 1);
    assert(rejectedPath.unmatchedSourceSites == 1);
    auto duplicatedPath = actualSites;
    duplicatedPath[0].sourceLoopPath = {"s09", "s09"};
    const auto rejectedDuplicatedPath = traceSourceReporterValues(
        module, reporters, false, duplicatedPath);
    assert(rejectedDuplicatedPath.attributedCheckpoints == 1);
    assert(rejectedDuplicatedPath.unmatchedSourceSites == 1);
    auto zeroLoopDepth = actualSites;
    zeroLoopDepth[0].portableLoopPathDepth = 0;
    const auto rejectedZeroDepth = traceSourceReporterValues(
        module, reporters, false, zeroLoopDepth);
    assert(rejectedZeroDepth.attributedCheckpoints == 1);
    assert(rejectedZeroDepth.unmatchedSourceSites == 1);
    auto forgedCall = actualSites;
    forgedCall[0].marker = llvm::WeakTrackingVH(opaqueValue);
    const auto rejectedNonCall = traceSourceReporterValues(
        module, reporters, false, forgedCall);
    assert(rejectedNonCall.attributedCheckpoints == 1);
    assert(rejectedNonCall.unmatchedSourceSites == 1);
    assert(!provenance.sourceEnvironmentComplete());
    assert(!provenance.executable());

    // Regression: inserting the FIRST checkpoint's volatile spill
    // renumbers the LLVM instruction-ordinal IDs at the SECOND site.
    // The source->SSA mapping and RT typed capture must therefore reuse
    // their common pre-spill snapshot, never re-run the analyzer later.
    const auto frozenSecond = analyzeLlvmCheckpointLiveness(
        *function, *checkpointLater);
    const auto getDescriptor = [](const LlvmCheckpointLiveness& state,
                                  const llvm::Instruction* definition) {
        const auto it = std::find_if(state.numeric.begin(),
            state.numeric.end(), [&](const auto& entry) {
                return entry.definition == definition;
            });
        assert(it != state.numeric.end());
        return uint64_t(it->id) | (uint64_t(it->width) << 32);
    };
    const uint64_t originalListDescriptor =
        getDescriptor(frozenSecond, tooLate);
    const auto recordedList = std::find_if(
        nextGap.reporterSsaBindings.begin(),
        nextGap.reporterSsaBindings.end(),
        [](const auto& binding) { return binding.reporterPc == 4; });
    assert(recordedList != nextGap.reporterSsaBindings.end() &&
           recordedList->descriptor == originalListDescriptor);
    const auto earlierSpill = emitLlvmNumericSsaSpill(*checkpoint, live);
    assert(earlierSpill && earlierSpill->ids.size() == live.numeric.size());
    const auto renumberedSecond = analyzeLlvmCheckpointLiveness(
        *function, *checkpointLater);
    assert(getDescriptor(renumberedSecond, tooLate) !=
           originalListDescriptor);
    // A frozen *pre-spill* identity is stable even as the LLVM module
    // grows synthetic instructions; neither identity certifies GC roots.
    assert(getDescriptor(frozenSecond, tooLate) ==
           recordedList->descriptor);
    assert(!llvm::verifyModule(module, &llvm::errs()));
    std::puts("PASS IR-001 frozen source slot -> actual live LLVM "
              "definition -> typed detached word; numeric subset "
              "only, source variables fail closed");
}
