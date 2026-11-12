// IR-001: actual compiler-proven SSA source-argument aliases and
// native generated string temporary -> exact rooted CallFrame bridge.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/source_ssa_location_bridge.hpp"
#include "../../src/ir/source_reporter_ssa_location_bridge.hpp"
#include "../../src/ir/source_reporter_storage_owner_audit.hpp"
#include "../../src/ir/source_reporter_input_slot_audit.hpp"
#include "../../src/ir/source_reported_entry_destination_audit.hpp"
#include "../../src/ir/source_scalar_prefix_audit.hpp"
#include "../../src/ir/source_reported_entry_prefix_plan.hpp"
#include "../../src/engine/runtime.hpp"
#include "../../src/ir/source_reporter_input_roots.hpp"
#include "../../src/ir/source_list_read_index_trace.hpp"
#include "../../src/jit/rt_generated_ssa_roots.hpp"
#include "../../src/ir/gc_call_frame_values.hpp"
#include "../../src/jit/rt_ssa_extra_capture.hpp"
#include "../../src/core/sstr.hpp"
#include "../../src/core/slist.hpp"
#include "../../src/ir/source_list_content_epoch.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <array>
#include <cassert>
#include <cstdio>

using namespace xyo;
using namespace xyo::ir;

static uint64_t tag(uint32_t id, uint32_t kindWidth) {
    return uint64_t(id) | (uint64_t(kindWidth) << 32);
}

// Uses the SAME real LLVM producer/alias analyzers as the native
// compiler's RT-001 SSA descriptor emission.
struct CompilerOrigin {
    uint32_t unchangedArgument = 0;
    uint32_t untrustedMixedArgument = 0;
    uint32_t nativeStringProducer = 0;
    uint32_t untrustedHostProducer = 0;
};

static CompilerOrigin llvmCompilerOrigins() {
    llvm::LLVMContext c;
    llvm::Module m("ir001.source.ssa.locations", c);
    llvm::IRBuilder<> b(c);
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(b.getVoidTy(),
            {b.getPtrTy(), b.getInt64Ty(), b.getInt64Ty(),
             b.getInt1Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, "probe", m);
    auto args = fn->arg_begin();
    (void)*args++; // JitCtx*, never a Scratch argument
    llvm::Value* original = &*args++;
    llvm::Value* alternative = &*args++;
    llvm::Value* condition = &*args;
    b.SetInsertPoint(llvm::BasicBlock::Create(c, "entry", fn));
    auto* exact = b.Insert(llvm::SelectInst::Create(
        condition, original, original));
    auto* mixed = b.Insert(llvm::SelectInst::Create(
        condition, original, alternative));
    auto join = m.getOrInsertFunction("xyo_join",
        llvm::FunctionType::get(b.getInt64Ty(),
            {b.getInt64Ty(), b.getInt64Ty()}, false));
    auto unknown = m.getOrInsertFunction("untrusted_join",
        llvm::FunctionType::get(b.getInt64Ty(),
            {b.getInt64Ty(), b.getInt64Ty()}, false));
    auto* produced = b.CreateCall(join, {original, alternative});
    auto* foreign = b.CreateCall(unknown, {original, alternative});
    auto sink = m.getOrInsertFunction("sink",
        llvm::FunctionType::get(b.getVoidTy(),
            {b.getInt64Ty(), b.getInt64Ty(),
             b.getInt64Ty(), b.getInt64Ty()}, false));
    b.CreateCall(sink, {exact, mixed, produced, foreign});
    b.CreateRetVoid();
    assert(!llvm::verifyFunction(*fn, &llvm::errs()));
    return {xyo::rt::sameBoxedArgumentRoot(exact),
            xyo::rt::sameBoxedArgumentRoot(mixed),
            uint32_t(xyo::rt::trustedGeneratedStringSource(produced)),
            uint32_t(xyo::rt::trustedGeneratedStringSource(foreign))};
}

static NativeRtSsaCapture capture(uint64_t argumentString,
                                  uint64_t generatedString,
                                  uint64_t number,
                                  const CompilerOrigin& compiler) {
    NativeRtSsaCapture result;
    result.sourceRevision = 311;
    result.digestLo = 53; result.digestHi = 59;
    result.procedureId = 7; result.sourcePc = 17; result.siteId = 23;
    // Native LLVM IDs are strictly sorted; original procedure args
    // live in the high-bit range, never interpreted as GC pointers.
    result.descriptors = {
        tag(8, 64),                   // PHI/SELECT alias of arg 1
        tag(9, 64),                   // compiler-produced boxed SStr
        tag(12, 0x100 | 64),          // raw double (not a Scratch Value)
        tag(0x80000001u, 0x200 | 64), // direct original Scratch arg 1
        tag(0x80000002u, 0x200 | 64), // direct original Scratch arg 2
    };
    result.words = {
        argumentString, generatedString, val::kTagStr | 0x123456u,
        argumentString, number
    };
    result.aliasArguments = {
        compiler.unchangedArgument, 0, 0, 1, 2};
    result.compilerAliasArguments = result.aliasArguments;
    result.generatedStrings = {
        0, compiler.nativeStringProducer, 0, 0, 0};
    result.compilerGeneratedStrings = result.generatedStrings;
    result.numericComplete = true;
    result.fingerprint = result.computeFingerprint();
    return result;
}

static SourceTypedSsaManifest manifest(const NativeRtSsaCapture& s) {
    return {s.sourceRevision, s.digestLo, s.digestHi,
            s.procedureId, s.sourcePc, s.siteId, s.descriptors};
}

static void reporterInterpreterDestinationSmoke() {
    Runtime runtime;
    Blocks blocks(&runtime);
    // Use real runtime opcodes: [data_variable, data_itemoflist,
    // operator_add, data_changevariableby] after recursive flattening.
    Block variable;
    variable.id = "scalar-read";
    variable.opcode = "data_variable";
    variable.parent = "arithmetic";
    variable.fields["VARIABLE"].id = "score";
    Block item;
    item.id = "list-read";
    item.opcode = "data_itemoflist";
    item.parent = "arithmetic";
    item.fields["LIST"].id = "history";
    Block numericShadow;
    numericShadow.id = "one";
    numericShadow.opcode = "math_number";
    numericShadow.parent = "list-read";
    numericShadow.fields["NUM"].value = Value::num(1);
    item.inputs.set("INDEX", {"one", "one"});
    Block arithmetic;
    arithmetic.id = "arithmetic";
    arithmetic.opcode = "operator_add";
    arithmetic.parent = "change";
    arithmetic.inputs.set("NUM1", {"scalar-read", ""});
    arithmetic.inputs.set("NUM2", {"list-read", ""});
    Block change;
    change.id = "change";
    change.opcode = "data_changevariableby";
    change.fields["VARIABLE"].id = "score";
    change.inputs.set("VALUE", {"arithmetic", ""});
    for (auto& block : {variable, item, numericShadow, arithmetic, change})
        blocks.createBlock(std::move(block));
    BlockCached* statement = blocks.getCachedExecute("change");
    BlockCached* parent = blocks.getCachedExecute("arithmetic");
    BlockCached* first = blocks.getCachedExecute("scalar-read");
    BlockCached* second = blocks.getCachedExecute("list-read");
    assert(statement && parent && first && second);
    assert(statement->ops.size() == 4 &&
           statement->ops[0] == first && statement->ops[1] == second &&
           statement->ops[2] == parent && statement->ops[3] == statement);
    assert(parent->argValues.vals.count("NUM1") == 1 &&
           parent->argValues.vals.count("NUM2") == 1);
    const auto beforeNum1 = parent->argValues.get("NUM1").bits;
    const auto beforeNum2 = parent->argValues.get("NUM2").bits;

    SourceReporterCompiledSite compiled;
    compiled.revision = blocks.revision();
    compiled.physicalSiteId = 19;
    compiled.reporters = {
        {11, tag(1, 64), SourceReporterOrigin::VariableRead,
         "scalar-read", "arithmetic", "NUM1",
         {SourceReporterStorageKind::ScalarVariable,
          "score", "data_variable"}, 1, false},
        {12, tag(2, 64), SourceReporterOrigin::ListRead,
         "list-read", "arithmetic", "NUM2",
         {SourceReporterStorageKind::ListVariable,
          "history", "data_itemoflist", "one"}, 2, false},
    };
    SourceReporterInputSlotAudit values;
    values.siteId = compiled.physicalSiteId;
    for (const auto& reporter : compiled.reporters) {
        SourceReporterCapturedInput input;
        input.descriptor = reporter.descriptor;
        input.reporterPc = reporter.reporterPc;
        input.reporterBlock = reporter.reporterBlock;
        input.parentBlock = reporter.parentBlock;
        input.parentInput = reporter.inputName;
        input.ordinal = reporter.inputOrdinal;
        input.selectedShadow = reporter.selectedShadow;
        input.uniquelyOwnedInput = true;
        input.kind = SourceReporterCapturedKind::BoxedImmediate;
        input.immediateBits = Value::num(7).bits;
        values.inputs.push_back(std::move(input));
    }
    values.inputs[1].immediateBits = Value::num(5).bits;
    const auto resolved = auditReporterInterpreterDestinations(
        compiled, values, blocks);
    assert(resolved && resolved->mapped == 2 &&
           resolved->unavailable == 0 && resolved->rootlessValue == 0 &&
           resolved->incompletePrefix == 0 &&
           resolved->missingPrefixOps.empty() &&
           resolved->contiguousReportedPrefix == 2 &&
           resolved->statementsWithMissingPrefix == 0 &&
           !resolved->hasInterpreterFrame() &&
           !resolved->canReplayEffects() && !resolved->executable());
    assert(resolved->destinations[0].reporterBlock == "scalar-read" &&
           resolved->destinations[0].containingStatement == "change" &&
           resolved->destinations[0].statementOperationIndex == 0 &&
           resolved->destinations[0].statementOperationCount == 4);
    assert(resolved->destinations[1].reporterBlock == "list-read" &&
           resolved->destinations[1].parentInput == "NUM2" &&
           resolved->destinations[1].containingStatement == "change" &&
           resolved->destinations[1].statementOperationIndex == 1);
    assert(parent->argValues.get("NUM1").bits == beforeNum1 &&
           parent->argValues.get("NUM2").bits == beforeNum2);

    // All values derive from actual historical READ events. A complete
    // two-reporter prefix can be represented without writing ArgValues
    // or executing the parent operator.
    SourceScalarPrefixAudit eventScalars;
    SourceScalarReadEvent scalarAtRead;
    scalarAtRead.reporterPc = 11;
    scalarAtRead.variableId = "score";
    scalarAtRead.sequence = 1;
    scalarAtRead.resultImmediate = Value::num(7).bits;
    eventScalars.live.push_back({scalarAtRead, tag(1, 64)});
    eventScalars.matchedLive = 1;
    SourceListReadCheckpointAudit eventLists;
    SourceListReadEvent listAtRead;
    listAtRead.reporterPc = 12;
    listAtRead.listId = "history";
    listAtRead.sequence = 2;
    listAtRead.resultImmediate = Value::num(5).bits;
    eventLists.evidence.push_back(listAtRead);
    eventLists.matchedReads = 1;
    auto planned = planEvaluatedReportedPrefix(
        *resolved, &eventScalars, &eventLists, blocks);
    assert(planned.capturedOperations == 2 &&
           planned.values.size() == 2 &&
           planned.missingValues == 0 &&
           planned.ambiguousValues == 0 &&
           planned.invalidRoots == 0 &&
           planned.orderViolations == 0 &&
           planned.gapBeforeLast == 0 &&
           verifyEvaluatedReportedPrefix(planned, *resolved, blocks) &&
           !planned.hasSourceStackFrame() &&
           !planned.replaySafe() && !planned.executable());
    assert(planned.values[0].reporterBlock == "scalar-read" &&
           planned.values[0].evaluationSequence == 1 &&
           planned.values[0].inspectValue()->bits ==
               Value::num(7).bits);
    assert(planned.values[1].reporterBlock == "list-read" &&
           planned.values[1].evaluationSequence == 2 &&
           planned.values[1].inspectValue()->bits ==
               Value::num(5).bits);
    assert(parent->argValues.get("NUM1").bits == beforeNum1 &&
           parent->argValues.get("NUM2").bits == beforeNum2);
    auto alteredRevision = planned;
    ++alteredRevision.sourceRevision;
    assert(!verifyEvaluatedReportedPrefix(
        alteredRevision, *resolved, blocks));
    Block* sourceList = blocks.getBlock("list-read");
    assert(sourceList);
    sourceList->fields["LIST"].id = "new-history";
    assert(!verifyEvaluatedReportedPrefix(
        planned, *resolved, blocks));
    sourceList->fields["LIST"].id = "history";
    Block* capturedParent = blocks.getBlock("arithmetic");
    assert(capturedParent);
    capturedParent->inputs.set("NUM2", {"scalar-read", ""});
    assert(!verifyEvaluatedReportedPrefix(
        planned, *resolved, blocks));
    capturedParent->inputs.set("NUM2", {"list-read", ""});
    assert(verifyEvaluatedReportedPrefix(
        planned, *resolved, blocks));

    auto badPlan = planned;
    badPlan.values[1].evaluationSequence = 1;
    assert(!verifyEvaluatedReportedPrefix(
        badPlan, *resolved, blocks));
    badPlan = planned;
    badPlan.values[1].immediateBits =
        val::kTagStr | uint64_t{0x9999};
    assert(!verifyEvaluatedReportedPrefix(
        badPlan, *resolved, blocks));
    badPlan = planned;
    badPlan.values[1].reporterBlock = "wrong-op";
    assert(!verifyEvaluatedReportedPrefix(
        badPlan, *resolved, blocks));
    // Historical list string is stored exclusively behind an exact
    // independent GC handle. Copying or clearing the producer evidence
    // does not release the staged plan's rooted value.
    auto rootedLists = eventLists;
    rootedLists.roots = std::make_shared<GcValueHandles>(Gc::get());
    const Value historicalListString =
        Value::string(SStr::create(u"list item already consumed"));
    const auto historicalHandle =
        rootedLists.roots->retain(historicalListString);
    assert(historicalHandle);
    rootedLists.evidence[0].resultImmediate.reset();
    rootedLists.evidence[0].resultRoot = *historicalHandle;
    auto rootedDestinations = *resolved;
    rootedDestinations.destinations[1].valueKind =
        SourceReporterCapturedKind::RootedHeapObservation;
    rootedDestinations.destinations[1].immediateBits.reset();
    const auto rootedPlan = planEvaluatedReportedPrefix(
        rootedDestinations, &eventScalars, &rootedLists, blocks);
    assert(rootedPlan.capturedOperations == 2 &&
           verifyEvaluatedReportedPrefix(
               rootedPlan, rootedDestinations, blocks));
    rootedLists.roots.reset();
    Gc::get().collect();
    assert(rootedPlan.values[1].inspectValue() &&
           rootedPlan.values[1].inspectValue()->bits ==
               historicalListString.bits);
    auto missingRoot = rootedPlan;
    missingRoot.values[1].rootProvider.reset();
    assert(!verifyEvaluatedReportedPrefix(
        missingRoot, rootedDestinations, blocks));
    auto fakeRoot = rootedPlan;
    fakeRoot.values[1].immediateBits =
        val::kTagStr | uint64_t{0x1234};
    assert(!verifyEvaluatedReportedPrefix(
        fakeRoot, rootedDestinations, blocks));
    auto reversedList = eventLists;
    reversedList.evidence[0].sequence = 1;
    const auto outOfOrder = planEvaluatedReportedPrefix(
        *resolved, &eventScalars, &reversedList, blocks);
    assert(outOfOrder.orderViolations == 1 &&
           !outOfOrder.executable());

    auto lateOnly = values;
    auto lateCompiled = compiled;
    lateOnly.inputs.erase(lateOnly.inputs.begin());
    lateCompiled.reporters.erase(lateCompiled.reporters.begin());
    const auto missingEarlier = auditReporterInterpreterDestinations(
        lateCompiled, lateOnly, blocks);
    assert(missingEarlier && missingEarlier->mapped == 1 &&
           missingEarlier->incompletePrefix == 1 &&
           missingEarlier->contiguousReportedPrefix == 0 &&
           missingEarlier->missingPrefixOps.size() == 1 &&
           missingEarlier->missingPrefixOps[0].opCachedId == "scalar-read" &&
           missingEarlier->missingPrefixOps[0].opcode == "data_variable" &&
           !missingEarlier->executable());

    // A scalar reporter can be absent from the LIVE LLVM SSA manifest
    // at a physical checkpoint, but its original source operation was
    // already evaluated. Recover its historical value WITHOUT re-reading
    // the mutable source Variable (whose current value may differ).
    const auto sourceBlockForPc = [](uint64_t pc) -> std::string_view {
        if (pc == 11) return "scalar-read";
        if (pc == 12) return "list-read";
        return {};
    };
    SourceScalarReadEvents previousScalars;
    assert(previousScalars.record(11, "score", Value::num(7), 1));
    SourceListReadCheckpointAudit priorList;
    SourceListReadEvent genuineList;
    genuineList.reporterPc = 12;
    genuineList.sequence = 2;
    priorList.evidence.push_back(genuineList);
    const auto recovered = auditHistoricalScalarPrefix(
        *missingEarlier, previousScalars, &priorList,
        blocks, blocks.revision(), sourceBlockForPc);
    assert(recovered.matched == 1 && recovered.missing == 0 &&
           recovered.orderUnproven == 0 && recovered.rejected == 0 &&
           recovered.items[0].missing.opCachedId == "scalar-read");
    assert(verifyHistoricalScalarPrefix(
        recovered, *missingEarlier, &priorList,
        blocks, blocks.revision(), sourceBlockForPc));
    // Even when LLVM completely drops the scalar's live SSA, the
    // historical read's GC-safe value can fill the missing interpreter
    // prefix before a later actual list-read event.
    priorList.evidence[0].listId = "history";
    priorList.evidence[0].resultImmediate = Value::num(5).bits;
    auto oldSsaPlan = planEvaluatedReportedPrefix(
        *missingEarlier, &recovered, &priorList, blocks);
    assert(oldSsaPlan.capturedOperations == 2 &&
           oldSsaPlan.missingValues == 0 &&
           oldSsaPlan.gapBeforeLast == 0 &&
           verifyEvaluatedReportedPrefix(
               oldSsaPlan, *missingEarlier, blocks) &&
           !oldSsaPlan.executable());
    // The same real evaluated scalar may ALSO be live in LLVM's typed
    // checkpoint after installing the native observer (as in the real
    // nested-list Scratch parity fixture). Independently compare the
    // retained historical Value with the actual detached SSA word.
    auto compiledWithDigest = compiled;
    compiledWithDigest.digestLo = 53;
    compiledWithDigest.digestHi = 59;
    NativeRtSsaCapture liveSsa;
    liveSsa.sourceRevision = blocks.revision();
    liveSsa.digestLo = 53; liveSsa.digestHi = 59;
    liveSsa.procedureId = 7; liveSsa.sourcePc = 17; liveSsa.siteId = 19;
    liveSsa.descriptors = {tag(1, 64), tag(2, 64)};
    liveSsa.words = {Value::num(7).bits, Value::num(5).bits};
    const auto liveManifest = manifest(liveSsa);
    assert(liveManifest.valid());
    SourceScalarPrefixAudit liveObservation;
    liveObservation.roots = previousScalars.roots;
    auditLiveScalarReadWords(
        liveObservation, compiledWithDigest, liveManifest,
        liveSsa, previousScalars, blocks, sourceBlockForPc);
    assert(liveObservation.matchedLive == 1 &&
           liveObservation.missingLive == 0 &&
           liveObservation.invalidLive == 0 &&
           verifyLiveScalarReadWords(
               liveObservation, compiledWithDigest, liveManifest,
               liveSsa, blocks, sourceBlockForPc));
    liveSsa.words[0] = Value::num(8).bits;
    assert(!verifyLiveScalarReadWords(
        liveObservation, compiledWithDigest, liveManifest,
        liveSsa, blocks, sourceBlockForPc));
    liveSsa.words[0] = Value::num(7).bits;
    const auto value = resolveScalarPrefixItem(
        recovered.items.front(), recovered);
    assert(value && value->bits == Value::num(7).bits);
    // A swapped read order must be rejected, even if the Value is equal.
    priorList.evidence[0].sequence = 1;
    const auto reversed = auditHistoricalScalarPrefix(
        *missingEarlier, previousScalars, &priorList,
        blocks, blocks.revision(), sourceBlockForPc);
    assert(reversed.matched == 0 && reversed.orderUnproven == 1 &&
           !verifyHistoricalScalarPrefix(
               recovered, *missingEarlier, &priorList,
               blocks, blocks.revision(), sourceBlockForPc));
    priorList.evidence[0].sequence = 2;
    // An event with the same source PC but a forged scalar variable ID
    // cannot be attached to the prior ReportedEntry prefix.
    SourceScalarReadEvents wrongOwner;
    assert(wrongOwner.record(11, "other-variable", Value::num(7), 1));
    const auto wrongHistoricalOwner = auditHistoricalScalarPrefix(
        *missingEarlier, wrongOwner, &priorList,
        blocks, blocks.revision(), sourceBlockForPc);
    assert(wrongHistoricalOwner.matched == 0 &&
           wrongHistoricalOwner.mismatchedOwner == 1);
    SourceScalarReadEvents duplicatedScalar;
    assert(duplicatedScalar.record(11, "score", Value::num(7), 1));
    assert(duplicatedScalar.record(11, "score", Value::num(7), 2));
    const auto ambiguousHistorical = auditHistoricalScalarPrefix(
        *missingEarlier, duplicatedScalar, &priorList,
        blocks, blocks.revision(), sourceBlockForPc);
    assert(ambiguousHistorical.matched == 0 &&
           ambiguousHistorical.ambiguous == 1);

    // Independent exact GC roots protect the prior scalar String even
    // after the compiler SSA result has been discarded.
    SourceScalarReadEvents scalarString;
    const Value historicalString = Value::string(SStr::create(u"old"));
    assert(scalarString.record(11, "score", historicalString, 1));
    auto rootedString = auditHistoricalScalarPrefix(
        *missingEarlier, scalarString, &priorList,
        blocks, blocks.revision(), sourceBlockForPc);
    assert(rootedString.matched == 1 &&
           rootedString.items[0].evaluated.resultRoot);
    Gc::get().collect();
    const auto afterCollection = resolveScalarPrefixItem(
        rootedString.items[0], rootedString);
    assert(afterCollection &&
           afterCollection->bits == historicalString.bits);
    const auto token = rootedString.items[0].evaluated.resultRoot;
    assert(rootedString.roots);
    assert(rootedString.roots->release(token));
    assert(!verifyHistoricalScalarPrefix(
        rootedString, *missingEarlier, &priorList,
        blocks, blocks.revision(), sourceBlockForPc));
    SourceScalarReadEvents fakeString;
    assert(!fakeString.record(11, "score",
        Value{val::kTagStr | uint64_t{0x987650}}, 1));

    auto forged = values;
    forged.inputs[1].descriptor = tag(1234, 64);
    const auto mismatchedDescriptor = auditReporterInterpreterDestinations(
        compiled, forged, blocks);
    assert(mismatchedDescriptor && mismatchedDescriptor->mapped == 1 &&
           mismatchedDescriptor->unavailable == 1);
    forged = values;
    forged.inputs[1].immediateBits = val::kTagStr | uint64_t{1234};
    const auto forgedString = auditReporterInterpreterDestinations(
        compiled, forged, blocks);
    assert(forgedString && forgedString->mapped == 1 &&
           forgedString->rootlessValue == 1);
    forged = values;
    forged.inputs[1].ordinal = 1;
    const auto wrongOrdinal = auditReporterInterpreterDestinations(
        compiled, forged, blocks);
    assert(wrongOrdinal && wrongOrdinal->mapped == 1 &&
           wrongOrdinal->unavailable == 1);

    forged = values;
    forged.inputs[1].kind = SourceReporterCapturedKind::UnrootedHeap;
    const auto rootless = auditReporterInterpreterDestinations(
        compiled, forged, blocks);
    assert(rootless && rootless->mapped == 1 &&
           rootless->rootlessValue == 1);
    forged = values;
    forged.inputs[0].selectedShadow = true;
    const auto shadow = auditReporterInterpreterDestinations(
        compiled, forged, blocks);
    assert(shadow && shadow->mapped == 1 &&
           shadow->unavailable == 1);

    // A stale cached writer pointer must not be accepted just because
    // the source block/input strings still match.
    Value* originalSlot = second->parentValueSlot;
    second->parentValueSlot = nullptr;
    const auto noCacheSlot = auditReporterInterpreterDestinations(
        compiled, values, blocks);
    assert(noCacheSlot && noCacheSlot->mapped == 1 &&
           noCacheSlot->unavailable == 1);
    second->parentValueSlot = originalSlot;
    // Direct source graph edit may temporarily precede the revision
    // notification during an editor commit. The LIVE input edge itself
    // must reject the stale frozen reporter destination even then.
    Block* sourceParent = blocks.getBlock("arithmetic");
    assert(sourceParent);
    sourceParent->inputs.set("NUM2", {"scalar-read", ""});
    const auto sourceEdited = auditReporterInterpreterDestinations(
        compiled, values, blocks);
    assert(sourceEdited && sourceEdited->mapped == 1 &&
           sourceEdited->unavailable == 1);
    sourceParent->inputs.set("NUM2", {"list-read", ""});
    auto staleRevision = compiled;
    ++staleRevision.revision;
    assert(!auditReporterInterpreterDestinations(
        staleRevision, values, blocks));
    assert(parent->argValues.get("NUM1").bits == beforeNum1 &&
           parent->argValues.get("NUM2").bits == beforeNum2);
}

static void processWideListContentEpochSmoke() {
    // Run this focused unit with
    // XYO_IR001_RUNTIME_REPORTER_AUDIT=1 XYO_RT_CAPTURE_LLVM_SSA=1
    // to prove real SList mutation coverage. It is disabled on normal
    // JIT runs; no performance-sensitive list mutation changes semantics.
    if (!listContentEpochEnabled()) return;
    auto* current = SList::create();
    uint64_t before = sourceListContentEpoch();
    assert(before > 0);
    {
        ScopedSourceListContentWrite first;
        assert(sourceListContentEpoch() == 0);
        {
            ScopedSourceListContentWrite nested;
            assert(sourceListContentEpoch() == 0);
        }
        assert(sourceListContentEpoch() == 0);
    }
    assert(sourceListContentEpoch() > before);
    assert(listContentActiveWriters().load() == 0);
    before = sourceListContentEpoch();
    current->push(Value::num(4));
    assert(sourceListContentEpoch() > before);
    before = sourceListContentEpoch();

    // Observe a stable epoch, then mutate the same list WITHOUT resizing.
    SourceListReadEvents savedRead;
    assert(savedRead.record(12, "history", Value::num(1),
                            Value::num(4), current->len,
                            current->storageEpoch));
    const uint64_t observed =
        savedRead.pending[0].contentEpochAfterObserver;
    assert(observed && sourceListContentEpochValid(
        observed, sourceListContentEpoch()));
    const uint64_t storageEpoch = current->storageEpoch;
    current->replace0(0, Value::num(5));
    assert(current->storageEpoch == storageEpoch);
    assert(!sourceListContentEpochValid(
        observed, sourceListContentEpoch()));
    assert(sourceListContentEpoch() > before);

    before = sourceListContentEpoch();
    current->insert0(0, Value::num(6));
    assert(sourceListContentEpoch() > before);
    before = sourceListContentEpoch();
    current->write0(0, Value::num(7));
    assert(sourceListContentEpoch() > before);
    before = sourceListContentEpoch();
    current->erase0(0);
    assert(sourceListContentEpoch() > before);
    before = sourceListContentEpoch();
    current->clear();
    assert(sourceListContentEpoch() > before);
    before = sourceListContentEpoch();
    // A cleared list's no-op clear has no observable content effect.
    current->clear();
    assert(sourceListContentEpoch() == before);
    // An unrelated list write also invalidates the older read: the
    // global epoch intentionally sacrifices precision for safety.
    auto* unrelated = SList::create();
    unrelated->push(Value::num(1));
    assert(sourceListContentEpoch() > before);
}

static void sourceReporterOwnerAuditSmoke() {
    ScratchScript frozen;
    Block scalar;
    scalar.id = "scalar_reporter";
    scalar.opcode = "data_variable";
    scalar.fields["VARIABLE"].id = "score";
    Block list;
    list.id = "list_reporter";
    list.opcode = "data_itemoflist";
    list.fields["LIST"].id = "history";
    list.inputs.set("INDEX", {"expression_reporter", ""});
    Block expression;
    expression.id = "expression_reporter";
    expression.opcode = "operator_add";
    frozen.nodes = {scalar, list, expression};
    frozen.byId = {{"scalar_reporter", 0},
                   {"list_reporter", 1},
                   {"expression_reporter", 2}};
    Block parent;
    parent.id = "parent";
    parent.inputs.set("FIRST", {"scalar_reporter", ""});
    parent.inputs.set("SECOND", {"list_reporter", ""});
    parent.inputs.set("FALLBACK", {"", "expression_reporter"});
    frozen.nodes.push_back(parent);
    frozen.byId.emplace("parent", 3);
    const auto direct = frozenReporterInputSlot(
        frozen, "parent", "SECOND", "list_reporter");
    assert(direct && direct->ordinal == 2 && !direct->selectedShadow);
    const auto shadow = frozenReporterInputSlot(
        frozen, "parent", "FALLBACK", "expression_reporter");
    assert(shadow && shadow->ordinal == 3 && shadow->selectedShadow);
    assert(!frozenReporterInputSlot(
        frozen, "parent", "SECOND", "scalar_reporter"));
    assert(!frozenReporterInputSlot(
        frozen, "parent", "MISSING", "list_reporter"));

    auto scalarStorage = frozenSourceReporterStorage(
        frozen, "scalar_reporter");
    auto listStorage = frozenSourceReporterStorage(
        frozen, "list_reporter");
    auto exprStorage = frozenSourceReporterStorage(
        frozen, "expression_reporter");
    assert(scalarStorage.kind == SourceReporterStorageKind::ScalarVariable &&
           scalarStorage.id == "score" &&
           scalarStorage.opcode == "data_variable");
    assert(listStorage.kind == SourceReporterStorageKind::ListVariable &&
           listStorage.id == "history" &&
           listStorage.opcode == "data_itemoflist" &&
           listStorage.indexBlock == "expression_reporter");
    assert(exprStorage.kind == SourceReporterStorageKind::ExpressionTemporary &&
           exprStorage.id.empty());

    // Real Scratch Target variable map, not a fabricated Variable*.
    Sprite sprite(nullptr);
    Target target(nullptr, &sprite);
    target.variables.emplace("score",
        Variable::create("score", u"score",
                         Variable::kScalarType, false));
    target.variables.emplace("history",
        Variable::create("history", u"history",
                         Variable::kListType, false));
    std::vector<SourceReporterLiveSsaBinding> reporters = {
        {3, tag(1, 64), SourceReporterOrigin::VariableRead,
         "scalar_reporter", "parent", "VALUE", scalarStorage},
        {4, tag(2, 64), SourceReporterOrigin::ListRead,
         "list_reporter", "parent", "ITEM", listStorage},
        {5, tag(3, 64), SourceReporterOrigin::ComputedExpression,
         "expression_reporter", "parent", "VALUE", exprStorage},
    };
    const auto original = auditCompiledReporterStorageOwners(
        reporters, target);
    assert(original && original->typedScalarOwners == 1 &&
           original->typedListOwners == 1 &&
           original->nonStorageOrigins == 1 &&
           original->ownershipComplete() &&
           !original->sourceValueRestorable() && !original->executable());
    assert(original->observations[1].sourceVariableId == "history" &&
           original->observations[1].state ==
               SourceReporterOwnerState::TypedList);

    // Deleting and recreating an ID invalidates the old structural
    // generation even if a new variable has an identical display name.
    target.variables.erase("history");
    assert(original->epoch != target.variableBindingStamp());
    const auto missing = auditCompiledReporterStorageOwners(
        reporters, target);
    assert(missing && missing->missingOwners == 1 &&
           !missing->ownershipComplete());
    target.variables.emplace("history",
        Variable::create("history", u"history",
                         Variable::kScalarType, false));
    const auto wrongType = auditCompiledReporterStorageOwners(
        reporters, target);
    assert(wrongType && wrongType->wrongTypeOwners == 1 &&
           !wrongType->ownershipComplete() &&
           original->epoch != wrongType->epoch);
    target.variables.erase("history");
    target.variables.emplace("history",
        Variable::create("history", u"history",
                         Variable::kListType, false));
    const auto rebound = auditCompiledReporterStorageOwners(
        reporters, target);
    assert(rebound && rebound->ownershipComplete() &&
           rebound->epoch != original->epoch);

    auto forged = reporters;
    forged[1].storage.opcode = "data_variable";
    assert(!auditCompiledReporterStorageOwners(forged, target));
    forged = reporters;
    forged[1].storage.id.clear();
    assert(!auditCompiledReporterStorageOwners(forged, target));
    forged = reporters;
    forged[1].reporterBlock.clear();
    assert(!auditCompiledReporterStorageOwners(forged, target));
    // An expression can be attributed but cannot be assigned a
    // variable/list storage owner by a forged compiler-side label.
    forged = reporters;
    forged[2].storage.kind = SourceReporterStorageKind::ListVariable;
    forged[2].storage.id = "history";
    assert(!auditCompiledReporterStorageOwners(forged, target));
}

int main() {
    reporterInterpreterDestinationSmoke();
    processWideListContentEpochSmoke();
    sourceReporterOwnerAuditSmoke();
    Gc& gc = Gc::get();
    RootedScratchCallStack roots({4, 4, 3}, gc);
    const Value argString = Value::string(
        SStr::create(u"source procedure argument stays rooted"));
    const Value freshString = Value::string(
        SStr::create(u"generated native LLVM string retained"));
    const Value number = Value::num(42);

    assert(roots.push(0, CallFrameAbi::kNoPc, 11,
                      std::span<const uint64_t>{}, 0));
    const std::array args{argString.bits, number.bits};
    assert(roots.push(7, 13, 17, args, 0));
    assert(roots.rootedArguments() == 1);

    const CompilerOrigin compiler = llvmCompilerOrigins();
    assert(compiler.unchangedArgument == 1);
    assert(compiler.untrustedMixedArgument == 0);
    assert(compiler.nativeStringProducer == 1);
    assert(compiler.untrustedHostProducer == 0);

    auto saved = capture(
        argString.bits, freshString.bits, number.bits, compiler);
    const auto expected = manifest(saved);
    assert(expected.valid());
    assert(rt::retainGeneratedSsaRoots(saved));
    auto plan = auditSourceSsaLocations(saved, expected, roots);
    assert(plan && !plan->executable() && !plan->sourceLocalsComplete);
    assert(plan->rootedFrameRevision == roots.frames().revision());
    assert(plan->locations.size() == 5);
    assert(plan->mappedArguments == 3);
    assert(plan->rootedArgumentStrings == 2); // arg and PHI alias
    assert(plan->nativeGeneratedStrings == 1);
    assert(plan->unmappedWords == 1); // float raw bits only
    assert(plan->locations[0].kind == SourceSsaLocationKind::ProcedureArgument);
    assert(plan->locations[0].argument == 1);
    assert(plan->locations[1].kind ==
           SourceSsaLocationKind::NativeGeneratedString);
    assert(plan->locations[1].argument == 0);
    assert(plan->locations[2].kind == SourceSsaLocationKind::Unmapped);
    assert(plan->locations[3].kind == SourceSsaLocationKind::ProcedureArgument);
    assert(plan->locations[3].argument == 1);
    assert(plan->locations[4].argument == 2);

    // IR-001 reporter source labels are compiler inputs from the frozen
    // physical CallBase. A matching source PC alone is insufficient:
    // the independent RT capture must have the exact physical site ID.
    SourceReporterCheckpointGap reporterSite;
    reporterSite.sourcePc = 17;
    reporterSite.physicalSiteId = 23;
    reporterSite.sourceLoopBlock = "loop";
    reporterSite.portableLoopPathDepth = 1;
    reporterSite.sourceLoopPath = {"loop"};
    reporterSite.phase = SourceResumePhase::AfterLoopBody;
    reporterSite.reporterSsaBindings = {
        {101, tag(8, 64), SourceReporterOrigin::ProcedureArgument},
        {102, tag(9, 64), SourceReporterOrigin::ComputedExpression},
        {103, tag(12, 0x100 | 64),
            SourceReporterOrigin::ComputedExpression},
        {104, tag(0x80000001u, 0x200 | 64),
            SourceReporterOrigin::ProcedureArgument},
        {105, tag(0x80000002u, 0x200 | 64),
            SourceReporterOrigin::ProcedureArgument}
    };
    reporterSite.liveReporterObservations = 5;
    for (size_t i = 0; i < reporterSite.reporterSsaBindings.size(); ++i) {
        auto& binding = reporterSite.reporterSsaBindings[i];
        binding.reporterBlock = "reporter-" + std::to_string(i);
        binding.parentBlock = "parent-op";
        binding.inputName = "INPUT-" + std::to_string(i);
        binding.inputOrdinal = uint32_t(i + 1);
    }
    // Frozen by the compiler independently of the mutable RT image.
    const auto compilerReporter = freezeReporterSourceSite(
        reporterSite, expected);
    assert(compilerReporter);
    const auto observed = auditSourceReporterSsaLocations(
        reporterSite, *compilerReporter, expected, saved, roots);
    assert(observed && observed->physicalSiteId == 23 &&
           observed->rootedFrameRevision == roots.frames().revision());
    assert(observed->reporters.size() == 5);
    assert(observed->mappedArguments == 3 &&
           observed->mappedGeneratedStrings == 1 &&
           observed->verifiedHeapStringObservations == 3 &&
           observed->unresolvedReporters == 1 &&
           !observed->sourceEnvironmentComplete() &&
           !observed->executable());
    assert(observed->reporters[0].verifiedHeapStringRoot &&
           observed->reporters[0].argument == 1);
    assert(observed->reporters[1].verifiedHeapStringRoot &&
           observed->reporters[1].location ==
               SourceSsaLocationKind::NativeGeneratedString);
    assert(!observed->reporters[2].verifiedHeapStringRoot &&
           observed->reporters[2].location ==
               SourceSsaLocationKind::Unmapped);
    assert(observed->reporters[3].verifiedHeapStringRoot &&
           observed->reporters[3].argument == 1);
    assert(observed->reporters[4].location ==
               SourceSsaLocationKind::ProcedureArgument &&
           observed->reporters[4].argument == 2 &&
           !observed->reporters[4].observedHeapString &&
           !observed->reporters[4].verifiedHeapStringRoot);

    // The actual evaluated list INDEX/result can be tied to the same
    // frozen physical Scratch reporter SSA descriptor, without rerunning
    // cast::listIndex or assuming storageEpoch protects list contents.
    auto listCompiled = *compilerReporter;
    listCompiled.reporters[1].origin =
        SourceReporterOrigin::ListRead;
    listCompiled.reporters[1].storage.kind =
        SourceReporterStorageKind::ListVariable;
    listCompiled.reporters[1].storage.opcode = "data_itemoflist";
    listCompiled.reporters[1].storage.id = "history";
    listCompiled.reporters[1].storage.indexBlock = "index-literal";
    SourceListReadEvents listTrace;
    assert(listTrace.record(102, "history",
        Value::string(SStr::create(u"1")), freshString, 1, 11));
    assert(listTrace.pending.size() == 1 &&
           listTrace.pending[0].numericPosition == 0 &&
           !listTrace.pending[0].indexBits &&
           listTrace.pending[0].resultRoot &&
           !listTrace.pending[0].resultImmediate);
    SourceListReadEvents missingBegin;
    assert(missingBegin.record(102, "history",
        Value::num(1), freshString, 1, 11,
        std::optional<uint64_t>{0}));
    const auto untracked = auditListReadsAtCheckpoint(
        listCompiled, expected, saved, missingBegin);
    if (listContentEpochEnabled()) {
        assert(untracked.matchedReads == 1 &&
               untracked.epochUntracked == 1 &&
               !verifyListReadEvidence(
                   listCompiled, expected, saved, untracked));
    }

    const auto listIndexed = auditListReadsAtCheckpoint(
        listCompiled, expected, saved, listTrace);
    assert(listIndexed.matchedReads == 1 &&
           listIndexed.indexPositions == 1 &&
           listIndexed.missingReads == 0 &&
           listIndexed.ambiguousReads == 0 &&
           listIndexed.mismatchedValues == 0 &&
           !listIndexed.contentUnchangedProven() &&
           !listIndexed.executable());
    if (listContentEpochEnabled()) {
        assert(listIndexed.epochStableSinceObserver == 1 &&
               listIndexed.epochChangedSinceObserver == 0 &&
               listIndexed.epochUntracked == 0);
    } else {
        assert(listIndexed.epochUntracked == 1);
    }
    // Disabled audit never certifies a replay-safe read. The exact
    // SSA/root pairing is still checked by the location audit itself.
    assert(verifyListReadEvidence(
        listCompiled, expected, saved, listIndexed) ==
        listContentEpochEnabled());
    auto unknownOrigin = listCompiled;
    unknownOrigin.reporters[1].storage.indexBlock.clear();
    const auto notProven = auditListReadsAtCheckpoint(
        unknownOrigin, expected, saved, listTrace);
    assert(notProven.matchedReads == 0 && notProven.rejectedReads == 1);
    listTrace.clear();
    gc.collect();
    // Disabled audit never certifies a replay-safe read. The exact
    // SSA/root pairing is still checked by the location audit itself.
    assert(verifyListReadEvidence(
        listCompiled, expected, saved, listIndexed) ==
        listContentEpochEnabled());
    auto wrongListWord = saved;
    wrongListWord.words[1] = number.bits;
    assert(!verifyListReadEvidence(
        listCompiled, expected, wrongListWord, listIndexed));
    auto lostListRoot = listIndexed;
    assert(lostListRoot.evidence[0].resultRoot);
    lostListRoot.roots.reset();
    assert(!verifyListReadEvidence(
        listCompiled, expected, saved, lostListRoot));
    // A JIT-style direct element overwrite changes contents while the
    // STORAGE epoch is unchanged. The independent GLOBAL content epoch
    // disqualifies an earlier captured list read, including if another
    // list is the one that changed.
    SList* sameStorage = SList::create();
    sameStorage->push(Value::num(4));
    SourceListReadEvents invalidatedTrace;
    assert(invalidatedTrace.record(102, "history",
        Value::num(1), freshString, 1, sameStorage->storageEpoch));
    const uint64_t oldContentEpoch =
        invalidatedTrace.pending[0].contentEpochAfterObserver;
    const uint64_t layoutEpoch = sameStorage->storageEpoch;
    sameStorage->write0(0, Value::num(9));
    assert(sameStorage->storageEpoch == layoutEpoch &&
           sameStorage->get0(0).bits == Value::num(9).bits);
    const auto staleEvent = auditListReadsAtCheckpoint(
        listCompiled, expected, saved, invalidatedTrace);
    if (listContentEpochEnabled()) {
        assert(staleEvent.epochChangedSinceObserver == 1 &&
               staleEvent.epochStableSinceObserver == 0 &&
               !sourceListContentEpochValid(
                   oldContentEpoch, sourceListContentEpoch()) &&
               !verifyListReadEvidence(
                   listCompiled, expected, saved, staleEvent));
    } else {
        assert(staleEvent.epochUntracked == 1);
    }
    // A duplicated evaluation of one Scratch reporter between physical
    // checkpoints cannot be unambiguously bound to the retained SSA.
    SourceListReadEvents repeatedTrace;
    assert(repeatedTrace.record(102, "history",
        Value::num(1), freshString, 1, 11));
    assert(repeatedTrace.record(102, "history",
        Value::num(1), freshString, 1, 11));
    const auto duplicate = auditListReadsAtCheckpoint(
        listCompiled, expected, saved, repeatedTrace);
    assert(duplicate.matchedReads == 0 &&
           duplicate.ambiguousReads == 1);
    // A special randomized Scratch index must NOT invoke RNG again.
    SourceListReadEvents randomTrace;
    assert(randomTrace.record(102, "history",
        Value::string(SStr::create(u"random")), freshString, 1, 11));
    assert(!randomTrace.pending[0].numericPosition &&
           !randomTrace.pending[0].indexBits);
    const auto unknownIndex = auditListReadsAtCheckpoint(
        listCompiled, expected, saved, randomTrace);
    assert(unknownIndex.matchedReads == 1 &&
           unknownIndex.indexPositions == 0);

    // The native i64 Scratch argument is an independent pointer-free
    // historical Value, while heap strings are ONLY ROOT OBSERVATIONS,
    // never unrooted pointer bits copied into a source input.
    const auto inputs = auditReporterInputSlots(
        *compilerReporter, *observed, expected, saved);
    assert(inputs && inputs->inputs.size() == 5 &&
           inputs->rootedHeapObservations == 3 &&
           inputs->immediateCount == 1 &&
           inputs->nonBoxedCount == 1 &&
           inputs->ambiguousInputs == 0 &&
           !inputs->sourceEnvironmentComplete() && !inputs->executable());
    assert(inputs->inputs[0].kind ==
               SourceReporterCapturedKind::RootedHeapObservation &&
           !inputs->inputs[0].immediateBits);
    assert(inputs->inputs[1].kind ==
               SourceReporterCapturedKind::RootedHeapObservation &&
           !inputs->inputs[1].immediateBits);
    assert(inputs->inputs[2].kind ==
               SourceReporterCapturedKind::NonBoxedSsa &&
           !inputs->inputs[2].immediateBits);
    assert(inputs->inputs[3].kind ==
               SourceReporterCapturedKind::RootedHeapObservation &&
           !inputs->inputs[3].immediateBits);
    assert(inputs->inputs[4].kind ==
               SourceReporterCapturedKind::BoxedImmediate &&
           inputs->inputs[4].immediateBits == number.bits &&
           inputs->inputs[4].ordinal == 5);
    // This separate producer source is UNTRUSTED in the existing
    // generated-string compiler mask, yet a real GC string object.
    // A verified source-origin SSA binding + exact membership must root
    // it independently at the native checkpoint, never by NaN-box tag.
    auto historical = saved;
    historical.generatedStringRoots.reset();
    historical.generatedStringHandles.clear();
    historical.generatedStrings[1] = 0;
    historical.compilerGeneratedStrings[1] = 0;
    historical.words[1] = Value::string(
        SStr::create(u"list item that survives mutation")).bits;
    historical.fingerprint = historical.computeFingerprint();
    historical.compilerManifest = expected;
    assert(rt::retainGeneratedSsaRoots(historical));
    const auto historicalSource = auditSourceReporterSsaLocations(
        reporterSite, *compilerReporter, expected, historical, roots);
    assert(historicalSource &&
           historicalSource->mappedGeneratedStrings == 0 &&
           historicalSource->unresolvedReporters == 2);
    auto malicious = historical;
    malicious.words[1] = val::kTagStr | uint64_t{0x200};
    malicious.fingerprint = malicious.computeFingerprint();
    assert(!retainSourceReporterInputStrings(
        malicious, *compilerReporter, *historicalSource, expected));
    assert(malicious.reporterInputHandles.empty() &&
           !malicious.reporterInputRoots);

    assert(retainSourceReporterInputStrings(
        historical, *compilerReporter, *historicalSource, expected));
    assert(verifySourceReporterInputStrings(
        historical, *compilerReporter, expected));
    assert(historical.reporterInputRoots &&
           historical.reporterInputRoots->size() == 2);
    const auto previouslyUnrooted = auditReporterInputSlots(
        *compilerReporter, *historicalSource, expected, historical);
    assert(previouslyUnrooted &&
           previouslyUnrooted->rootedHeapObservations == 3 &&
           previouslyUnrooted->unrootedHeapObservations == 0 &&
           !previouslyUnrooted->inputs[1].immediateBits);
    gc.collect();
    assert(verifySourceReporterInputStrings(
        historical, *compilerReporter, expected));
    const auto historicalCopy = historical;
    historical.reporterInputRoots.reset();
    historical.reporterInputHandles.clear();
    gc.collect();
    assert(verifySourceReporterInputStrings(
        historicalCopy, *compilerReporter, expected));
    auto dropped = historicalCopy;
    assert(dropped.reporterInputHandles[1]);
    dropped.reporterInputRoots->release(
        dropped.reporterInputHandles[1]);
    dropped.reporterInputHandles[1] = {};
    assert(!verifySourceReporterInputStrings(
        dropped, *compilerReporter, expected));
    assert(pointerFreeScratchImmediate(Value::num(12.5).bits));
    assert(pointerFreeScratchImmediate(Value::nan().bits));
    assert(pointerFreeScratchImmediate(Value::emptyStr().bits));
    assert(pointerFreeScratchImmediate(Value::boolean(true).bits));
    assert(!pointerFreeScratchImmediate(
        val::kTagStr | 0xabcdefull));
    assert(!pointerFreeScratchImmediate(
        uint64_t{0xfff8000000000001ull})); // noncanonical negative NaN
    assert(!pointerFreeScratchImmediate(
        val::kTagBase | 123)); // unknown NaN-boxed tag
    auto ambiguousSite = *compilerReporter;
    ambiguousSite.reporters[1].parentBlock =
        ambiguousSite.reporters[0].parentBlock;
    ambiguousSite.reporters[1].inputName =
        ambiguousSite.reporters[0].inputName;
    const auto ambiguous = auditReporterInputSlots(
        ambiguousSite, *observed, expected, saved);
    assert(ambiguous && ambiguous->ambiguousInputs == 1);
    ambiguousSite = *compilerReporter;
    ambiguousSite.reporters[1].inputOrdinal = 0;
    assert(!auditReporterInputSlots(
        ambiguousSite, *observed, expected, saved));

    auto forgedReporterSite = reporterSite;
    ++forgedReporterSite.physicalSiteId; // same source PC, different call
    assert(!auditSourceReporterSsaLocations(
        forgedReporterSite, *compilerReporter, expected, saved, roots));
    forgedReporterSite = reporterSite;
    ++forgedReporterSite.sourcePc;
    assert(!auditSourceReporterSsaLocations(
        forgedReporterSite, *compilerReporter, expected, saved, roots));
    forgedReporterSite = reporterSite;
    forgedReporterSite.phase = SourceResumePhase::UnknownBody;
    assert(!auditSourceReporterSsaLocations(
        forgedReporterSite, *compilerReporter, expected, saved, roots));
    forgedReporterSite = reporterSite;
    forgedReporterSite.sourceLoopPath = {"loop", "loop"};
    forgedReporterSite.portableLoopPathDepth = 2;
    assert(!auditSourceReporterSsaLocations(
        forgedReporterSite, *compilerReporter, expected, saved, roots));
    forgedReporterSite = reporterSite;
    // Retargeting to ANOTHER VALID captured SSA descriptor now fails:
    // it does not match the separately retained source-origin manifest.
    forgedReporterSite.reporterSsaBindings[0].descriptor = tag(9, 64);
    assert(!auditSourceReporterSsaLocations(
        forgedReporterSite, *compilerReporter, expected, saved, roots));
    forgedReporterSite.reporterSsaBindings[0].descriptor ^=
        uint64_t{1} << 31;
    assert(!auditSourceReporterSsaLocations(
        forgedReporterSite, *compilerReporter, expected, saved, roots));
    forgedReporterSite = reporterSite;
    forgedReporterSite.reporterSsaBindings.push_back(
        forgedReporterSite.reporterSsaBindings.front());
    ++forgedReporterSite.liveReporterObservations;
    assert(!auditSourceReporterSsaLocations(
        forgedReporterSite, *compilerReporter, expected, saved, roots));
    forgedReporterSite = reporterSite;
    --forgedReporterSite.liveReporterObservations;
    assert(!auditSourceReporterSsaLocations(
        forgedReporterSite, *compilerReporter, expected, saved, roots));

    // Real collector: both the original procedure argument and the
    // generated boxed SStr survive via INDEPENDENT root providers.
    gc.collect();
    assert(auditSourceSsaLocations(saved, expected, roots));
    assert(auditSourceReporterSsaLocations(
        reporterSite, *compilerReporter, expected, saved, roots));
    auto detachedCopy = saved;
    saved.generatedStringRoots.reset();
    saved.generatedStringHandles.clear();
    gc.collect();
    assert(auditSourceSsaLocations(detachedCopy, expected, roots));
    assert(auditSourceReporterSsaLocations(
        reporterSite, *compilerReporter, expected, detachedCopy, roots));

    auto damaged = detachedCopy;
    damaged.words[0] ^= 1; // forged PHI/SELECT alias bits
    damaged.fingerprint = damaged.computeFingerprint();
    assert(!auditSourceSsaLocations(damaged, expected, roots));

    damaged = detachedCopy;
    damaged.aliasArguments[0] = 2; // provenance rewritten, FNV repaired
    damaged.fingerprint = damaged.computeFingerprint();
    assert(!auditSourceSsaLocations(damaged, expected, roots));

    damaged = detachedCopy;
    damaged.compilerAliasArguments[0] = 2; // compiler copy rewritten
    damaged.aliasArguments[0] = 2;
    damaged.fingerprint = damaged.computeFingerprint();
    assert(!auditSourceSsaLocations(damaged, expected, roots));

    damaged = detachedCopy;
    damaged.words[1] ^= 1; // generated heap string pointer mismatch
    damaged.fingerprint = damaged.computeFingerprint();
    assert(!auditSourceSsaLocations(damaged, expected, roots));

    damaged = detachedCopy;
    damaged.generatedStrings[1] = 0; // compiler mask mismatch
    damaged.fingerprint = damaged.computeFingerprint();
    assert(!auditSourceSsaLocations(damaged, expected, roots));

    damaged = detachedCopy;
    damaged.generatedStringHandles[1] = {};
    assert(!auditSourceSsaLocations(damaged, expected, roots));

    damaged = detachedCopy;
    damaged.aliasArguments.clear(); // missing compiler-origin array
    damaged.compilerAliasArguments.clear();
    damaged.fingerprint = damaged.computeFingerprint();
    assert(!auditSourceSsaLocations(damaged, expected, roots));

    auto stale = expected;
    ++stale.resumePc;
    assert(!auditSourceSsaLocations(detachedCopy, stale, roots));
    stale = expected;
    ++stale.procedureId;
    assert(!auditSourceSsaLocations(detachedCopy, stale, roots));

    // The same valid SSA words cannot attach to a different source
    // frame, or to a frame whose current continuation PC has moved.
    assert(roots.setResumePc(19));
    assert(!auditSourceSsaLocations(detachedCopy, expected, roots));
    assert(roots.setResumePc(17));

    // Root ownership for a string argument cannot be inferred from
    // its saved bits once the actual CallFrame is popped.
    assert(roots.pop() && !auditSourceSsaLocations(detachedCopy, expected, roots));
    std::puts("PASS IR-001 SSA source-location plan: rooted procedure "
              "args/aliases, generated heap strings, 2 full GC cycles, "
              "real LLVM-origin metadata, stale CallFrame fail-closed");
}
