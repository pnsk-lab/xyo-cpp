// IR-001: executable revision is fixed when compilation begins, not when
// the first native procedure call happens. No unrelated source graph can
// make a stale executable's CallFrame eligible for interpreter adoption.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../../src/jit/procedure_frame_state.hpp"
#include "../../src/core/sstr.hpp"

#include <cassert>
#include <iostream>
#include <utility>

using namespace xyo;
using namespace xyo::ir;

static void add(Blocks& blocks, const char* id, const char* opcode,
                const char* next = "") {
    Block block;
    block.id = id;
    block.opcode = opcode;
    block.next = next;
    blocks.createBlock(std::move(block));
}

// Simulate a detached image that replaces the numeric return-PC key with
// an alias, but keeps the original *valid* SourceReturnSite payload.
// This must not turn a copied source dictionary into a second valid PC.
static std::unique_ptr<SelfContainedSourceContinuation>
forgeReturnSiteAlias(const SelfContainedSourceContinuation& source,
                     uint64_t originalPc, uint64_t aliasPc) {
    assert(source.valid() && originalPc != aliasPc &&
           aliasPc != CallFrameAbi::kNoPc);
    auto result = std::make_unique<SelfContainedSourceContinuation>();
    auto dictionary = std::make_shared<SourceFrameSymbols>(*source.symbols);
    const auto originalSite = dictionary->returnSite(originalPc);
    assert(originalSite && !dictionary->returnSite(aliasPc));
    assert(dictionary->returns.emplace(aliasPc, *originalSite).second);
    result->symbols = std::move(dictionary);

    const auto& original = source.frames->frames();
    const auto layout = original.layout();
    const auto* words = original.words();
    const size_t stride = layout.strideWords();
    result->frames = std::make_unique<RootedScratchCallStack>(layout);
    for (uint32_t i = 0; i < original.depth(); ++i) {
        const auto* frame = words + size_t(i) * stride;
        const uint32_t arity =
            uint32_t(frame[CallFrameAbi::kArgumentCountWord]);
        const uint32_t locals =
            uint32_t(frame[CallFrameAbi::kLocalCountWord]);
        std::vector<uint64_t> args;
        for (uint32_t arg = 0; arg < arity; ++arg) {
            const auto value = source.frames->argument(i, arg);
            assert(value);
            args.push_back(value->bits);
        }
        const auto remap = [&](uint64_t pc) {
            return pc == originalPc ? aliasPc : pc;
        };
        assert(result->frames->push(
            frame[CallFrameAbi::kProcedureIdWord],
            remap(frame[CallFrameAbi::kReturnPcWord]),
            remap(frame[CallFrameAbi::kResumePcWord]), args, locals));
        for (uint32_t slot = 0; slot < locals; ++slot)
            assert(result->frames->setNumericLocal(
                slot, frame[CallFrameAbi::localWord(
                    layout.argumentCapacity, slot)]));
    }
    assert(result->frames->valid());
    return result;
}

// A deep but non-recursive Scratch script should not cause the source
// ownership resolver to repeatedly ascend a 2k-long NEXT parent chain.
// Every statement remains root-owned, while its separate callee body
// belongs to the procedure even though both live in the same snapshot.
static void verifyLongExecutableChain() {
    Blocks blocks(nullptr);
    add(blocks, "long-hat", "event_whenflagclicked");
    std::string previous = "long-hat";
    constexpr int kStatements = 2048;
    for (int i = 0; i < kStatements; ++i) {
        const std::string id = "long-statement-" + std::to_string(i);
        add(blocks, id.c_str(), "data_changevariableby");
        blocks.getBlock(previous)->next = id;
        blocks.getBlock(id)->parent = previous;
        previous = id;
    }
    add(blocks, "long-call", "procedures_call");
    blocks.getBlock(previous)->next = "long-call";
    blocks.getBlock("long-call")->parent = previous;
    blocks.getBlock("long-call")->mutation.emplace();
    blocks.getBlock("long-call")->mutation->attrs["proccode"] =
        "long procedure";
    add(blocks, "long-definition", "procedures_definition", "long-body");
    add(blocks, "long-prototype", "procedures_prototype");
    add(blocks, "long-body", "data_changevariableby");
    blocks.getBlock("long-definition")->inputs.set(
        "custom_block", {"long-prototype", ""});
    blocks.getBlock("long-prototype")->parent = "long-definition";
    blocks.getBlock("long-prototype")->mutation.emplace();
    blocks.getBlock("long-prototype")->mutation->attrs["proccode"] =
        "long procedure";
    blocks.getBlock("long-body")->parent = "long-definition";

    const auto source = ScratchCompilationUnit::capture(
        blocks, blocks.getBlock("long-hat"));
    assert(source.valid());
    NativeProcedureFrameSession session(source, &blocks, source.sourceRevision,
                                        scratchSourceDigest(source));
    assert(session.enabled());
    session.enter("long-call", "long procedure", nullptr, 0);
    assert(session.enabled());
    session.statementStarted("long-body");
    assert(session.enabled());
    const auto image = session.exportSelfContained();
    assert(image && image->valid());
    const auto owner = [&](std::string_view id) -> uint64_t {
        const auto& symbols = *image->symbols;
        const auto it = std::find(symbols.blockIds.begin(),
                                  symbols.blockIds.end(), id);
        assert(it != symbols.blockIds.end());
        return symbols.statementOwners[size_t(it - symbols.blockIds.begin())];
    };
    assert(owner("long-hat") == 0);
    assert(owner("long-statement-0") == 0);
    assert(owner("long-statement-2047") == 0);
    assert(owner("long-call") == 0);
    assert(owner("long-body") == 1);
    assert(owner("long-prototype") == CallFrameAbi::kNoPc);

    // Repeated scheduler handoffs no longer ascend all 2048 lexical
    // NEXT parents for this single callsite. The source proof is built
    // once and remains immutable even if a diagnostic copy of the
    // surrounding SourceFrameSymbols modifies a return-site descriptor.
    const auto decoded = image->decodeFrames();
    assert(decoded && decoded->size() == 2 &&
           decoded->back().caller.has_value());
    const auto& call = *decoded->back().caller;
    assert(image->symbols->verifiedCallsites &&
           image->symbols->verifiedCallsites->size() == 1);
    for (size_t i = 0; i < 50000; ++i)
        assert(image->symbols->validReturnTarget(call));
    auto tampered = std::make_shared<SourceFrameSymbols>(*image->symbols);
    const auto proofBefore = tampered->verifiedCallsites;
    auto& fake = tampered->returns.at(call.returnPc);
    fake.kind = SourceReturnKind::ProcedureExit;
    fake.targetPc = 0;
    assert(proofBefore == tampered->verifiedCallsites &&
           !tampered->validReturnTarget(fake));
}

// Clone-start scripts are event-like roots in Scratch. They must use the
// same exact caller proof, not an unverified generic control container.
static void verifyCloneRootCaller() {
    Blocks blocks(nullptr);
    add(blocks, "clone-hat", "control_start_as_clone", "clone-call");
    add(blocks, "clone-call", "procedures_call", "clone-after");
    add(blocks, "clone-after", "data_changevariableby");
    add(blocks, "clone-def", "procedures_definition", "clone-body");
    add(blocks, "clone-prototype", "procedures_prototype");
    add(blocks, "clone-body", "data_changevariableby");
    blocks.getBlock("clone-call")->parent = "clone-hat";
    blocks.getBlock("clone-after")->parent = "clone-call";
    blocks.getBlock("clone-body")->parent = "clone-def";
    blocks.getBlock("clone-def")->inputs.set(
        "custom_block", {"clone-prototype", ""});
    blocks.getBlock("clone-prototype")->parent = "clone-def";
    blocks.getBlock("clone-prototype")->mutation.emplace();
    blocks.getBlock("clone-prototype")->mutation->attrs["proccode"] =
        "clone helper";
    blocks.getBlock("clone-call")->mutation.emplace();
    blocks.getBlock("clone-call")->mutation->attrs["proccode"] =
        "clone helper";
    const auto snapshot = ScratchCompilationUnit::capture(
        blocks, blocks.getBlock("clone-hat"));
    assert(snapshot.valid());
    NativeProcedureFrameSession session(
        snapshot, &blocks, snapshot.sourceRevision,
        scratchSourceDigest(snapshot));
    assert(session.enabled());
    session.enter("clone-call", "clone helper", nullptr, 0);
    assert(session.enabled());
    const auto image = session.exportSelfContained();
    assert(image && image->valid());
    const auto child = image->decodeFrames();
    assert(child && child->size() == 2 && child->back().caller &&
           child->back().caller->callerProcedureId == 0 &&
           child->back().caller->branchPath.empty());
    const auto caller = image->symbols->structuralCallerContext(
        child->back().caller->callBlockPc);
    assert(caller && caller->procedureId == 0 &&
           caller->branches.empty() && caller->loops.empty());
}

// A completed nested procedure call with a NEXT successor has a
// backend-neutral, verified target PC. This is not an SSA deopt claim.
static void verifyPostCallNextBlock() {
    Blocks blocks(nullptr);
    add(blocks, "next-hat", "event_whenflagclicked", "call-outer");
    add(blocks, "call-outer", "procedures_call");
    add(blocks, "def-outer", "procedures_definition", "call-inner");
    add(blocks, "proto-outer", "procedures_prototype");
    add(blocks, "call-inner", "procedures_call", "after-inner");
    add(blocks, "after-inner", "data_changevariableby",
        "same-owner-decoy");
    add(blocks, "same-owner-decoy", "data_changevariableby");
    add(blocks, "def-inner", "procedures_definition", "inner-body");
    add(blocks, "proto-inner", "procedures_prototype");
    add(blocks, "inner-body", "data_changevariableby");
    blocks.getBlock("call-outer")->parent = "next-hat";
    blocks.getBlock("call-inner")->parent = "def-outer";
    blocks.getBlock("after-inner")->parent = "call-inner";
    blocks.getBlock("same-owner-decoy")->parent = "after-inner";
    blocks.getBlock("inner-body")->parent = "def-inner";
    for (const auto& [def, proto, name] : {
            std::tuple<const char*, const char*, const char*>{
                "def-outer", "proto-outer", "outer"},
            {"def-inner", "proto-inner", "inner"}}) {
        blocks.getBlock(def)->inputs.set("custom_block", {proto, ""});
        blocks.getBlock(proto)->parent = def;
        blocks.getBlock(proto)->mutation.emplace();
        blocks.getBlock(proto)->mutation->attrs["proccode"] = name;
    }
    blocks.getBlock("call-outer")->mutation.emplace();
    blocks.getBlock("call-outer")->mutation->attrs["proccode"] = "outer";
    blocks.getBlock("call-inner")->mutation.emplace();
    blocks.getBlock("call-inner")->mutation->attrs["proccode"] = "inner";

    const auto snapshot = ScratchCompilationUnit::capture(
        blocks, blocks.getBlock("next-hat"));
    assert(snapshot.valid());
    NativeProcedureFrameSession session(
        snapshot, &blocks, snapshot.sourceRevision,
        scratchSourceDigest(snapshot));
    assert(session.enabled());
    session.enter("call-outer", "outer", nullptr, 0);
    assert(session.enabled());
    session.bodyStarted();
    session.enter("call-inner", "inner", nullptr, 0);
    assert(session.enabled());
    session.leave();
    auto handoff = session.exportSelfContained();
    assert(handoff && handoff->valid());
    assert(handoff->deoptBoundary() == SourceDeoptBoundary::PostCall);
    // Synthetic compiler manifest demonstrates raw SSA sidecar shape and
    // exact leaf/source binding. This is NOT LLVM live-register capture.
    assert(!handoff->diagnosticSsa);
    const auto leaf = handoff->frames->frames().top();
    assert(leaf);
    SourceSsaManifest manifest{
        handoff->symbols->sourceRevision,
        handoff->symbols->sourceDigest.lo,
        handoff->symbols->sourceDigest.hi,
        leaf->procedureId, leaf->resumePc,
        {{1, 1}, {9, 32}, {42, 64}}};
    std::vector<uint64_t> rawSsa{
        1, 0xdeadbeefULL, 0xfedcba9876543210ULL};
    assert(handoff->captureDiagnosticSsa(manifest, rawSsa));
    rawSsa[1] = 0; // native source buffer may be mutated or destroyed
    assert(handoff->matchesSsaManifest(manifest));
    assert(handoff->diagnosticSsa->values[1] == 0xdeadbeefULL);
    const auto preserved = handoff->diagnosticSsa->values;
    auto wrongSource = manifest;
    wrongSource.digestHi ^= 1;
    assert(!handoff->captureDiagnosticSsa(wrongSource, rawSsa));
    assert(handoff->diagnosticSsa->values == preserved);
    auto wrongPc = manifest;
    ++wrongPc.resumePc;
    assert(!handoff->captureDiagnosticSsa(wrongPc, rawSsa));
    assert(handoff->diagnosticSsa->values == preserved);
    const uint64_t tooShort[] = {1};
    assert(!handoff->captureDiagnosticSsa(manifest, tooShort));
    assert(handoff->diagnosticSsa->values == preserved);
    auto badManifest = manifest;
    ++badManifest.resumePc;
    assert(!handoff->matchesSsaManifest(badManifest));
    badManifest = manifest;
    badManifest.slots[1].id = 1; // duplicate SSA ID
    assert(!handoff->matchesSsaManifest(badManifest));
    badManifest = manifest;
    badManifest.digestLo ^= 1;
    assert(!handoff->matchesSsaManifest(badManifest));
    handoff->diagnosticSsa->values[0] = 2; // exceeds i1 width
    assert(!handoff->matchesSsaManifest(manifest));
    handoff->diagnosticSsa->values[0] = 1;
    handoff->diagnosticSsa->values.pop_back(); // missing live SSA
    assert(!handoff->matchesSsaManifest(manifest));
    handoff->diagnosticSsa.reset();
    assert(!handoff->matchesSsaManifest(manifest));
    // An explicitly empty SSA set is distinct from an absent snapshot.
    SourceSsaManifest emptyManifest = manifest;
    emptyManifest.slots.clear();
    const std::span<const uint64_t> emptyWords;
    handoff->diagnosticSsa = SourceSsaSnapshot::capture(
        emptyManifest, emptyWords);
    assert(handoff->matchesSsaManifest(emptyManifest));
    // A valid diagnostic snapshot must never change execution gating.
    assert(handoff->valid() && handoff->deoptRequirements());
    handoff->diagnosticSsa.reset();

    const auto postCallNeeds = handoff->deoptRequirements();
    assert(postCallNeeds &&
           postCallNeeds->needs(SourceDeoptNeed::EffectContinuity) &&
           postCallNeeds->needs(SourceDeoptNeed::LiveSsaValues));
    const auto next = handoff->postCallSuccessor();
    assert(next && next->kind == SourceReturnKind::NextBlock &&
           next->callerProcedureId != 0 &&
           handoff->symbols->block(next->targetPc) ==
               std::optional<std::string_view>("after-inner"));
    // The decoy is a *valid executable block of the same procedure*.
    // A weak owner-only check would accept this forged return.
    const auto& symbols = *handoff->symbols;
    const auto decoy = std::find(symbols.blockIds.begin(),
        symbols.blockIds.end(), "same-owner-decoy");
    assert(decoy != symbols.blockIds.end());
    const uint64_t decoyPc = uint64_t(decoy - symbols.blockIds.begin() + 1);
    assert(symbols.statementOwners[size_t(decoyPc - 1)] ==
           next->callerProcedureId);
    auto diverted = session.exportSelfContained();
    assert(diverted && diverted->valid());
    auto divertedSymbols =
        std::make_shared<SourceFrameSymbols>(*diverted->symbols);
    divertedSymbols->returns.at(next->returnPc).targetPc = decoyPc;
    diverted->symbols = std::move(divertedSymbols);
    assert(!diverted->valid() && !diverted->postCallSuccessor());

    // Detached symbol dictionaries must not rename an AUTHENTIC source PC
    // to some other executable block with the same lexical procedure
    // owner. Return kind/PC/source-digest can all remain unchanged.
    const auto forgedNames = [&](auto mutate) {
        auto image = session.exportSelfContained();
        assert(image && image->valid());
        auto copied = std::make_shared<SourceFrameSymbols>(*image->symbols);
        mutate(*copied);
        image->symbols = std::move(copied);
        assert(!image->valid() && !image->postCallSuccessor());
    };
    forgedNames([&](SourceFrameSymbols& symbols) {
        symbols.blockIds[size_t(next->targetPc - 1)] =
            "same-owner-decoy";
    });
    forgedNames([&](SourceFrameSymbols& symbols) {
        symbols.blockIds[size_t(next->callBlockPc - 1)] =
            "same-owner-decoy";
    });
    forgedNames([&](SourceFrameSymbols& symbols) {
        symbols.procedureIds[size_t(next->callerProcedureId - 1)] =
            "def-inner";
    });

    // Both the saved parent resume PC and the nested return PC point
    // to an alias key. Its payload is the original valid callsite.
    // Proof comparison alone must not certify a forged dictionary key.
    const auto activeAlias =
        forgeReturnSiteAlias(*session.exportSelfContained(),
                             next->returnPc, uint64_t{1} << 48);
    assert(!activeAlias->valid() && !activeAlias->postCallSuccessor());
    // Direct validation must not bypass the return-PC dictionary. A
    // standalone record with a correct proof but no canonical key cannot
    // certify a handoff.
    auto missingCanonical =
        std::make_shared<SourceFrameSymbols>(*activeAlias->symbols);
    missingCanonical->returns.erase(next->returnPc);
    assert(!missingCanonical->validReturnTarget(*next));
    // Even rewriting the copied record's own PC to match the alias
    // cannot turn it into a compiled callsite: the immutable source proof
    // still binds the authentic call block to its original return PC.
    auto selfConsistentAlias =
        std::make_shared<SourceFrameSymbols>(*activeAlias->symbols);
    const uint64_t aliasPc = uint64_t{1} << 48;
    selfConsistentAlias->returns.at(aliasPc).returnPc = aliasPc;
    assert(selfConsistentAlias->returnSite(aliasPc));
    activeAlias->symbols = std::move(selfConsistentAlias);
    assert(!activeAlias->valid() && !activeAlias->postCallSuccessor());

    // An authentic call cannot be reclassified as an artificial
    // procedure exit, even when its return PC and caller are unchanged.
    auto forgedExit = session.exportSelfContained();
    assert(forgedExit && forgedExit->valid());
    auto wrongKind =
        std::make_shared<SourceFrameSymbols>(*forgedExit->symbols);
    auto& altered = wrongKind->returns.at(next->returnPc);
    altered.kind = SourceReturnKind::ProcedureExit;
    altered.targetPc = 0;
    forgedExit->symbols = std::move(wrongKind);
    assert(!forgedExit->valid() && !forgedExit->postCallSuccessor());

    session.statementStarted("after-inner");
    auto effect = session.exportSelfContained();
    assert(effect && effect->valid() && !effect->postCallSuccessor());
    assert(effect->deoptBoundary() == SourceDeoptBoundary::UnresolvedBody);
    session.leave();
    auto root = session.exportSelfContained();
    assert(root && root->valid() && !root->postCallSuccessor());
    assert(root->deoptBoundary() == SourceDeoptBoundary::PostCall);
    // The original root call is the last statement under the event hat:
    // its valid synthetic return PC means ScriptExit, not a guessed NEXT.
    const auto rootSuccessor = root->rootPostCallSuccessor();
    assert(rootSuccessor &&
           rootSuccessor->kind == SourceReturnKind::ScriptExit &&
           rootSuccessor->targetPc == 0 &&
           root->symbols->block(rootSuccessor->callBlockPc) ==
               std::optional<std::string_view>("call-outer"));
}

// The root script was previously pushed with zero locals. A repeat
// enclosing its FIRST native procedure call therefore had no captured
// remaining count, despite the child frame being fully rooted. The new
// root frame uses the same loop-slot schema as a nested procedure.
static void verifyRootCountedRepeat() {
    Blocks blocks(nullptr);
    add(blocks, "root-repeat-hat", "event_whenflagclicked",
        "root-repeat-outer");
    add(blocks, "root-repeat-outer", "control_repeat");
    add(blocks, "root-repeat-inner", "control_repeat");
    add(blocks, "root-repeat-call", "procedures_call");
    add(blocks, "root-repeat-after", "data_changevariableby");
    add(blocks, "root-repeat-def", "procedures_definition", "root-proc-body");
    add(blocks, "root-repeat-proto", "procedures_prototype");
    add(blocks, "root-proc-body", "data_changevariableby");

    blocks.getBlock("root-repeat-outer")->parent = "root-repeat-hat";
    blocks.getBlock("root-repeat-outer")->inputs.set(
        "SUBSTACK", {"root-repeat-inner", ""});
    blocks.getBlock("root-repeat-inner")->parent = "root-repeat-outer";
    blocks.getBlock("root-repeat-inner")->inputs.set(
        "SUBSTACK", {"root-repeat-call", ""});
    blocks.getBlock("root-repeat-call")->parent = "root-repeat-inner";
    blocks.getBlock("root-repeat-after")->parent = "root-repeat-call";
    blocks.getBlock("root-repeat-call")->next = "root-repeat-after";
    blocks.getBlock("root-repeat-call")->mutation.emplace();
    blocks.getBlock("root-repeat-call")->mutation->attrs["proccode"] =
        "root procedure";
    blocks.getBlock("root-repeat-def")->inputs.set(
        "custom_block", {"root-repeat-proto", ""});
    blocks.getBlock("root-repeat-proto")->parent = "root-repeat-def";
    blocks.getBlock("root-repeat-proto")->mutation.emplace();
    blocks.getBlock("root-repeat-proto")->mutation->attrs["proccode"] =
        "root procedure";
    blocks.getBlock("root-proc-body")->parent = "root-repeat-def";

    const auto source = ScratchCompilationUnit::capture(
        blocks, blocks.getBlock("root-repeat-hat"));
    assert(source.valid());
    const auto digest = scratchSourceDigest(source);
    const auto make = [&]() {
        return std::make_unique<NativeProcedureFrameSession>(
            source, &blocks, source.sourceRevision, digest);
    };
    auto missing = make();
    assert(missing->enabled());
    // No first-call path: do NOT manufacture a counted-loop remainder.
    missing->enter("root-repeat-call", "root procedure", nullptr, 0);
    assert(!missing->enabled() && !missing->exportSelfContained());

    auto session = make();
    assert(session->enabled());
    const char* ids[] = {"root-repeat-outer", "root-repeat-inner"};
    const uint64_t counters[] = {
        Value::num(19).bits, Value::num(7).bits
    };
    session->checkpointPath(ids, counters, 2,
        SourceResumePhase::InsideLoopBody);
    assert(session->enabled());
    const auto before = session->sourceCheckpoint();
    assert(before && before->frame.procedureId == 0 &&
           before->frame.locals == session->frames().layout().localCapacity &&
           before->loops.size() == 2 &&
           before->loops[0].counterBits == counters[0] &&
           before->loops[1].counterBits == counters[1]);

    session->enter("root-repeat-call", "root procedure", nullptr, 0);
    assert(session->enabled());
    const auto image = session->exportSelfContained();
    assert(image && image->valid());
    const auto decoded = image->decodeFrames();
    assert(decoded && decoded->size() == 2);
    assert((*decoded)[0].frame.procedureId == 0 &&
           (*decoded)[0].phase == SourceResumePhase::AwaitingCallee &&
           (*decoded)[0].loops.size() == 2 &&
           (*decoded)[0].loops[0].counterBits == counters[0] &&
           (*decoded)[0].loops[1].counterBits == counters[1]);
    assert((*decoded)[1].caller &&
           (*decoded)[1].caller->callerProcedureId == 0 &&
           (*decoded)[1].caller->callerLoopPath.size() == 2);
    auto renamedRootLoop = session->exportSelfContained();
    assert(renamedRootLoop && renamedRootLoop->valid());
    auto renamedRootLoopSymbols =
        std::make_shared<SourceFrameSymbols>(*renamedRootLoop->symbols);
    renamedRootLoopSymbols->blockIds[size_t(
        (*decoded)[0].loops.back().sourcePc - 1)] =
        "root-repeat-call";
    renamedRootLoop->symbols = std::move(renamedRootLoopSymbols);
    assert(!renamedRootLoop->valid());

    session->leave();
    assert(session->enabled() && session->depth() == 1);
    const auto root = session->exportSelfContained();
    assert(root && root->valid() &&
           !root->postCallSuccessor());
    const auto sourceNext = root->rootPostCallSuccessor();
    assert(sourceNext && sourceNext->kind == SourceReturnKind::NextBlock &&
           sourceNext->callerLoopPath.size() == 2 &&
           root->symbols->block(sourceNext->targetPc) ==
               std::optional<std::string_view>("root-repeat-after"));
    // Root-only returns must not turn a valid numeric return target
    // into another same-procedure block by changing only blockIds.
    auto forgedRootName = session->exportSelfContained();
    assert(forgedRootName && forgedRootName->valid());
    auto renamedRoot =
        std::make_shared<SourceFrameSymbols>(*forgedRootName->symbols);
    renamedRoot->blockIds[size_t(sourceNext->targetPc - 1)] =
        "root-repeat-call";
    forgedRootName->symbols = std::move(renamedRoot);
    assert(!forgedRootName->valid() &&
           !forgedRootName->rootPostCallSuccessor());

    // The origin hat/revision/digest belong to the *compiled source*.
    // They cannot be retargeted merely by copying a detached symbol
    // dictionary, even when the replacement root PC names a real block.
    const auto forgeProvenance = [&](auto mutate) {
        auto image = session->exportSelfContained();
        assert(image && image->valid());
        auto copied = std::make_shared<SourceFrameSymbols>(*image->symbols);
        mutate(*copied);
        image->symbols = std::move(copied);
        assert(!image->valid() && !image->rootPostCallSuccessor());
    };
    forgeProvenance([&](SourceFrameSymbols& symbols) {
        symbols.sourceRootPc = sourceNext->targetPc;
    });
    forgeProvenance([&](SourceFrameSymbols& symbols) {
        ++symbols.sourceRevision;
    });
    forgeProvenance([&](SourceFrameSymbols& symbols) {
        symbols.sourceDigest.lo ^= 1;
    });

    const auto rootAlias =
        forgeReturnSiteAlias(*root, sourceNext->returnPc,
                             (uint64_t{1} << 48) + 1);
    assert(!rootAlias->valid() && !rootAlias->rootPostCallSuccessor());
    const auto after = root->decodeFrames();
    assert(after && after->size() == 1 &&
           after->front().phase == SourceResumePhase::AfterCallee &&
           after->front().loops.size() == 2 &&
           after->front().loops[0].counterBits == counters[0] &&
           after->front().loops[1].counterBits == counters[1]);

    // A copied root frame with corrupted counted-loop Value bits must
    // fail even though its source PCs and synthetic return PC are valid.
    auto wrong = std::make_unique<SelfContainedSourceContinuation>();
    wrong->symbols = root->symbols;
    const auto layout = root->frames->frames().layout();
    wrong->frames = std::make_unique<RootedScratchCallStack>(layout);
    assert(wrong->frames->push(
        0, CallFrameAbi::kNoPc, after->front().frame.resumePc, {},
        layout.localCapacity));
    const auto* words = root->frames->frames().words();
    for (uint32_t i = 0; i < layout.localCapacity; ++i) {
        uint64_t bits = words[CallFrameAbi::localWord(
            layout.argumentCapacity, i)];
        if (i == 0) bits = val::kUndefined;
        assert(wrong->frames->setNumericLocal(i, bits));
    }
    assert(!wrong->valid() && !wrong->rootPostCallSuccessor());

    // Starting a later statement expires the root AfterCallee phase.
    session->statementStarted("root-repeat-after");
    assert(session->enabled());
    const auto expired = session->sourceCheckpoint();
    assert(expired && expired->phase == SourceResumePhase::UnknownBody);
    const auto afterStatement = session->exportSelfContained();
    assert(afterStatement && afterStatement->valid() &&
           !afterStatement->rootPostCallSuccessor());
    // The root can yield at its own loop latch after a completed child.
    // A loop source PC is NOT a synthetic post-call return-site PC.
    session->checkpointPath(ids, counters, 2,
                            SourceResumePhase::AfterLoopBody);
    const auto rootLoop = session->exportSelfContained();
    assert(rootLoop && rootLoop->valid() &&
           !rootLoop->rootPostCallSuccessor());
    const auto loopFrames = rootLoop->decodeFrames();
    assert(loopFrames && loopFrames->size() == 1 &&
           loopFrames->front().phase ==
               SourceResumePhase::AfterLoopBody &&
           loopFrames->front().loops.size() == 2 &&
           loopFrames->front().frame.resumePc ==
               loopFrames->front().loops.back().sourcePc);
    session->beforeYield();
    session->afterYield();
    assert(session->enabled());

    // Once the inner root repeat has completed, its numeric counter
    // slot MUST be the explicit no-counter sentinel. A detached image
    // may not claim that a dormant loop still owns executable state.
    session->checkpointPath(ids, counters, 1,
                            SourceResumePhase::AfterLoopBody);
    auto outerOnly = session->exportSelfContained();
    assert(outerOnly && outerOnly->valid());
    const auto only = outerOnly->decodeFrames();
    assert(only && only->front().loops.size() == 1);
    const auto inner = std::find_if(
        outerOnly->symbols->loops.begin(), outerOnly->symbols->loops.end(),
        [&](const auto& loop) {
            return outerOnly->symbols->block(loop.sourcePc) ==
                std::optional<std::string_view>("root-repeat-inner");
        });
    assert(inner != outerOnly->symbols->loops.end());
    const auto innerSlot = inner->localSlot;
    const auto* original = outerOnly->frames->frames().words();
    assert(original[CallFrameAbi::localWord(
        layout.argumentCapacity, innerSlot)] ==
           CallFrameAbi::kNoLoopCounter);

    auto dormant = std::make_unique<SelfContainedSourceContinuation>();
    dormant->symbols = outerOnly->symbols;
    dormant->frames = std::make_unique<RootedScratchCallStack>(layout);
    assert(dormant->frames->push(0, CallFrameAbi::kNoPc,
                               only->front().frame.resumePc, {},
                               layout.localCapacity));
    for (uint32_t i = 0; i < layout.localCapacity; ++i) {
        uint64_t bits = original[CallFrameAbi::localWord(
            layout.argumentCapacity, i)];
        if (i == innerSlot) bits = Value::num(99).bits;
        assert(dormant->frames->setNumericLocal(i, bits));
    }
    assert(!dormant->valid());
}

// A checkpoint must describe the full, exact lexical loop nesting of
// the current procedure. Forged PCs from reordered or missing ancestors
// are not portable resume points, even when each individual PC exists.
static void verifyLoopTopology() {
    // Enable opt-in loop diagnostic capture for fresh sessions in this test.
    assert(setenv("XYO_JIT_CALLFRAME_LOOP_SSA_DIAGNOSTIC", "1", 1) == 0);
    Blocks blocks(nullptr);
    add(blocks, "hat-loops", "event_whenflagclicked", "invoke-loops");
    add(blocks, "invoke-loops", "procedures_call");
    add(blocks, "def-loops", "procedures_definition", "outer-loop");
    add(blocks, "proto-loops", "procedures_prototype");
    add(blocks, "outer-loop", "control_repeat");
    add(blocks, "inner-loop", "control_repeat");
    add(blocks, "loop-body", "data_changevariableby");
    blocks.getBlock("invoke-loops")->parent = "hat-loops";
    blocks.getBlock("invoke-loops")->mutation.emplace();
    blocks.getBlock("invoke-loops")->mutation->attrs["proccode"] = "loops";
    blocks.getBlock("def-loops")->inputs.set(
        "custom_block", {"proto-loops", ""});
    blocks.getBlock("proto-loops")->parent = "def-loops";
    blocks.getBlock("proto-loops")->mutation.emplace();
    blocks.getBlock("proto-loops")->mutation->attrs["proccode"] = "loops";
    blocks.getBlock("outer-loop")->parent = "def-loops";
    blocks.getBlock("outer-loop")->inputs.set(
        "SUBSTACK", {"inner-loop", ""});
    blocks.getBlock("inner-loop")->parent = "outer-loop";
    blocks.getBlock("inner-loop")->inputs.set(
        "SUBSTACK", {"loop-body", ""});
    blocks.getBlock("loop-body")->parent = "inner-loop";

    const auto source = ScratchCompilationUnit::capture(
        blocks, blocks.getBlock("hat-loops"));
    assert(source.valid());
    const auto digest = scratchSourceDigest(source);
    const auto validSession = [&]() {
        return std::make_unique<NativeProcedureFrameSession>(
            source, &blocks, source.sourceRevision, digest);
    };
    auto good = validSession();
    assert(good->enabled());
    good->enter("invoke-loops", "loops", nullptr, 0);
    assert(good->enabled());
    const char* correct[] = {"outer-loop", "inner-loop"};
    const uint64_t counters[] = {
        Value::num(4).bits, Value::num(3).bits
    };
    good->checkpointPath(correct, counters, 2);
    // IR-001: exact compiler inputs versus the independently read
    // live CallFrame. All mutations below must refuse the same frame.
    const auto readback = good->verifyLoopPathReadback(
        correct, counters, 2, SourceResumePhase::AfterLoopBody);
    assert(readback && readback->depth == 2 &&
           readback->counted == 2 && readback->uncounted == 0);
    const char* reversedIds[] = {"inner-loop", "outer-loop"};
    const char* foreignIds[] = {"outer-loop", "loop-body"};
    const uint64_t wrongCounter[] = {counters[0], Value::num(9).bits};
    const uint64_t missingCounter[] = {
        counters[0], CallFrameAbi::kNoLoopCounter};
    assert(!good->verifyLoopPathReadback(
        reversedIds, counters, 2, SourceResumePhase::AfterLoopBody));
    assert(!good->verifyLoopPathReadback(
        foreignIds, counters, 2, SourceResumePhase::AfterLoopBody));
    assert(!good->verifyLoopPathReadback(
        correct, wrongCounter, 2, SourceResumePhase::AfterLoopBody));
    assert(!good->verifyLoopPathReadback(
        correct, missingCounter, 2, SourceResumePhase::AfterLoopBody));
    assert(!good->verifyLoopPathReadback(
        correct, counters, 1, SourceResumePhase::AfterLoopBody));
    assert(!good->verifyLoopPathReadback(
        correct, counters, 2, SourceResumePhase::InsideLoopBody));
    assert(!good->verifyLoopPathReadback(
        correct, counters, 0, SourceResumePhase::AfterLoopBody));
    // Missing repeat values remain valid observation-only CallFrames.
    // They cannot prove a recovered counter but must not stop native JIT.
    auto missing = validSession();
    missing->enter("invoke-loops", "loops", nullptr, 0);
    const uint64_t absent[] = {
        counters[0], CallFrameAbi::kNoLoopCounter};
    missing->checkpointPath(correct, absent, 2);
    assert(missing->enabled());
    assert(missing->sourceCheckpoint() &&
           missing->sourceCheckpoint()->loops[1].counterBits ==
               CallFrameAbi::kNoLoopCounter);
    assert(!missing->verifyLoopPathReadback(
        correct, absent, 2, SourceResumePhase::AfterLoopBody));
    assert(missing->enabled());
    auto missingImage = missing->exportSelfContained();
    assert(missingImage && missingImage->valid() &&
           !missingImage->decodeFrames()->empty());
    // Decimal and negative-zero counter words must remain bit-exact:
    // no integer normalization or inferred Scratch loop count.
    auto fraction = validSession();
    fraction->enter("invoke-loops", "loops", nullptr, 0);
    const uint64_t fractionalBits[] = {
        Value::num(-0.0).bits, Value::num(2.5).bits};
    fraction->checkpointPath(correct, fractionalBits, 2);
    const auto fractional = fraction->verifyLoopPathReadback(
        correct, fractionalBits, 2, SourceResumePhase::AfterLoopBody);
    assert(fractional && fractional->counted == 2);
    const auto sourceWords = fraction->sourceCheckpoint();
    assert(sourceWords && sourceWords->loops[0].counterBits ==
           fractionalBits[0] && sourceWords->loops[1].counterBits ==
           fractionalBits[1]);

    auto image = good->exportSelfContained();
    assert(image && image->valid());
    assert(image->deoptBoundary() == SourceDeoptBoundary::LoopLatch);
    assert(image->diagnosticSsa);
    assert(image->diagnosticSsa->values.size() == 2);
    // Lexical loop slots are sorted by source block ID, not nest order.
    const auto& slot0 = image->symbols->loops[0];
    const auto& slot1 = image->symbols->loops[1];
    assert(image->diagnosticSsa->values[0] ==
           (image->symbols->block(slot0.sourcePc) ==
                    std::optional<std::string_view>("outer-loop")
                ? counters[0] : counters[1]));
    assert(image->diagnosticSsa->values[1] ==
           (image->symbols->block(slot1.sourcePc) ==
                    std::optional<std::string_view>("outer-loop")
                ? counters[0] : counters[1]));
    SourceSsaManifest captured{
        image->symbols->sourceRevision,
        image->symbols->sourceDigest.lo,
        image->symbols->sourceDigest.hi,
        image->diagnosticSsa->procedureId,
        image->diagnosticSsa->resumePc, {{0, 64}, {1, 64}}};
    assert(image->matchesSsaManifest(captured));
    const auto latchNeeds = image->deoptRequirements();
    assert(latchNeeds &&
           latchNeeds->needs(SourceDeoptNeed::EffectContinuity) &&
           latchNeeds->needs(SourceDeoptNeed::LiveSsaValues) &&
           !latchNeeds->needs(SourceDeoptNeed::ExactStatementPc));
    // A completed loop checkpoint is recoverable only at the exact loop
    // yield boundary. The first subsequent Scratch statement invalidates
    // the clean phase before its reporter/effects can execute.
    good->statementStarted("loop-body");
    assert(good->enabled());
    auto afterStatement = good->sourceCheckpoint();
    assert(afterStatement &&
           afterStatement->phase == SourceResumePhase::UnknownBody);
    auto unknownImage = good->exportSelfContained();
    auto unknownFrames = unknownImage
        ? unknownImage->decodeFrames() : std::nullopt;
    assert(unknownFrames &&
           unknownFrames->back().phase == SourceResumePhase::UnknownBody);
    assert(!unknownImage->diagnosticSsa);
    assert(image->matchesSsaManifest(captured)); // detached copy survives
    assert(unknownImage->deoptBoundary() == SourceDeoptBoundary::UnresolvedBody);
    const auto unresolvedNeeds = unknownImage->deoptRequirements();
    assert(unresolvedNeeds &&
           unresolvedNeeds->needs(SourceDeoptNeed::EffectContinuity) &&
           unresolvedNeeds->needs(SourceDeoptNeed::LiveSsaValues) &&
           unresolvedNeeds->needs(SourceDeoptNeed::ExactStatementPc));
    good->checkpointPath(correct, counters, 2);
    assert(good->sourceCheckpoint() &&
           good->sourceCheckpoint()->phase ==
               SourceResumePhase::AfterLoopBody);
    auto invalidStatement = validSession();
    invalidStatement->enter("invoke-loops", "loops", nullptr, 0);
    invalidStatement->statementStarted("missing-compiled-source-block");
    assert(!invalidStatement->enabled());
    assert(!invalidStatement->exportSelfContained());

    const auto decoded = image->decodeFrames();
    assert(decoded && decoded->size() == 2 &&
           decoded->back().loops.size() == 2);
    const auto& symbols = *image->symbols;
    const uint64_t outerPc = decoded->back().loops[0].sourcePc;
    const uint64_t innerPc = decoded->back().loops[1].sourcePc;
    const auto outer = std::find_if(symbols.loops.begin(), symbols.loops.end(),
        [&](const auto& l) { return l.sourcePc == outerPc; });
    const auto inner = std::find_if(symbols.loops.begin(), symbols.loops.end(),
        [&](const auto& l) { return l.sourcePc == innerPc; });
    assert(outer != symbols.loops.end() && inner != symbols.loops.end());
    assert(outer->ownerProcedureId == inner->ownerProcedureId &&
           outer->ownerProcedureId == decoded->back().frame.procedureId);
    assert(outer->enclosingLoops.empty());
    assert(inner->enclosingLoops.size() == 1 &&
           inner->enclosingLoops.front() == outerPc);

    auto reordered = validSession();
    reordered->enter("invoke-loops", "loops", nullptr, 0);
    const char* wrongOrder[] = {"inner-loop", "outer-loop"};
    reordered->checkpointPath(wrongOrder, counters, 2);
    assert(!reordered->enabled() && !reordered->exportSelfContained());

    auto skipped = validSession();
    skipped->enter("invoke-loops", "loops", nullptr, 0);
    const char* missingParent[] = {"inner-loop"};
    skipped->checkpointPath(missingParent, counters, 1);
    assert(!skipped->enabled() && !skipped->exportSelfContained());

    // Unlike checkpointPath, a detached image may be provided by another
    // backend. Validate it independently of the native session.
    auto forged = std::make_unique<SelfContainedSourceContinuation>();
    forged->symbols = image->symbols;
    const auto layout = image->frames->frames().layout();
    const auto ret = decoded->back().caller->returnPc;
    const auto proc = decoded->back().frame.procedureId;
    forged->frames = std::make_unique<RootedScratchCallStack>(layout);
    assert(forged->frames->push(
        0, CallFrameAbi::kNoPc, symbols.sourceRootPc, {}, 0));
    assert(forged->frames->setResumePc(ret));
    assert(forged->frames->push(
        proc, ret, innerPc, {}, layout.localCapacity));
    const auto n = uint32_t(symbols.loops.size());
    for (uint32_t i = 0; i < n; ++i)
        assert(forged->frames->setNumericLocal(
            i, CallFrameAbi::kNoLoopCounter));
    // Both valid source PCs appear, but in impossible reverse nesting.
    assert(forged->frames->setNumericLocal(outer->localSlot, counters[0]));
    assert(forged->frames->setNumericLocal(inner->localSlot, counters[1]));
    assert(forged->frames->setNumericLocal(n, innerPc));
    assert(forged->frames->setNumericLocal(n + 1, outerPc));
    assert(forged->frames->setNumericLocal(2 * n, 2));
    assert(forged->frames->setNumericLocal(
        2 * n + 1, uint64_t(SourceResumePhase::AfterLoopBody)));
    assert(forged->frames->setResumePc(outerPc));
    assert(forged->frames->valid());
    assert(!forged->valid());
}

// Verify return-site metadata from both conditional arms and require the
// child frame procedure ID to agree with its original Scratch callsite.
static void verifyLexicalCallsites() {
    Blocks blocks(nullptr);
    add(blocks, "hat", "event_whenflagclicked", "branch");
    add(blocks, "branch", "control_if_else", "branch-successor");
    add(blocks, "branch-successor", "data_changevariableby",
        "branch-decoy");
    add(blocks, "branch-decoy", "data_changevariableby");
    add(blocks, "nested-branch", "control_if");
    add(blocks, "then-call", "procedures_call");
    add(blocks, "else-call", "procedures_call");
    blocks.getBlock("branch")->parent = "hat";
    blocks.getBlock("branch-successor")->parent = "branch";
    blocks.getBlock("branch-decoy")->parent = "branch-successor";
    blocks.getBlock("branch")->inputs.set(
        "SUBSTACK", {"nested-branch", ""});
    blocks.getBlock("nested-branch")->parent = "branch";
    blocks.getBlock("nested-branch")->inputs.set(
        "SUBSTACK", {"then-call", ""});
    blocks.getBlock("branch")->inputs.set(
        "SUBSTACK2", {"else-call", ""});
    blocks.getBlock("then-call")->parent = "nested-branch";
    blocks.getBlock("else-call")->parent = "branch";

    const auto definition = [&](const char* id, const char* prototype,
                                const char* body, const char* code) {
        add(blocks, id, "procedures_definition", body);
        add(blocks, prototype, "procedures_prototype");
        add(blocks, body, "data_changevariableby");
        blocks.getBlock(id)->inputs.set(
            "custom_block", {prototype, ""});
        blocks.getBlock(prototype)->parent = id;
        blocks.getBlock(prototype)->mutation.emplace();
        blocks.getBlock(prototype)->mutation->attrs["proccode"] = code;
        blocks.getBlock(body)->parent = id;
    };
    definition("def-then", "proto-then", "body-then", "do then");
    definition("def-else", "proto-else", "body-else", "do else");
    blocks.getBlock("then-call")->mutation.emplace();
    blocks.getBlock("then-call")->mutation->attrs["proccode"] = "do then";
    blocks.getBlock("else-call")->mutation.emplace();
    blocks.getBlock("else-call")->mutation->attrs["proccode"] = "do else";

    // The then procedure calls the else procedure from inside a counted
    // repeat; its active lexical loop is part of the parent-frame ABI.
    add(blocks, "then-loop", "control_repeat");
    add(blocks, "subcall", "procedures_call");
    blocks.getBlock("body-then")->next = "then-loop";
    blocks.getBlock("then-loop")->parent = "body-then";
    blocks.getBlock("then-loop")->inputs.set(
        "SUBSTACK", {"subcall", ""});
    blocks.getBlock("subcall")->parent = "then-loop";
    blocks.getBlock("subcall")->mutation.emplace();
    blocks.getBlock("subcall")->mutation->attrs["proccode"] = "do else";

    const auto source = ScratchCompilationUnit::capture(
        blocks, blocks.getBlock("hat"));
    assert(source.valid());
    const auto digest = scratchSourceDigest(source);
    const auto ownedSession = [&]() {
        return std::make_unique<NativeProcedureFrameSession>(
            source, &blocks, source.sourceRevision, digest);
    };

    auto validOwner = ownedSession();
    validOwner->enter("then-call", "do then", nullptr, 0);
    assert(validOwner->enabled());
    validOwner->statementStarted("body-then");
    assert(validOwner->enabled());
    assert(validOwner->sourceCheckpoint() &&
           validOwner->sourceCheckpoint()->phase ==
               SourceResumePhase::UnknownBody);
    auto forgedStatementOwner = ownedSession();
    forgedStatementOwner->enter("then-call", "do then", nullptr, 0);
    forgedStatementOwner->statementStarted("body-else");
    assert(!forgedStatementOwner->enabled() &&
           !forgedStatementOwner->exportSelfContained());
    auto forgedReporterStatement = ownedSession();
    forgedReporterStatement->enter("then-call", "do then", nullptr, 0);
    forgedReporterStatement->statementStarted("proto-then");
    assert(!forgedReporterStatement->enabled());
    auto forgedCallOwner = ownedSession();
    forgedCallOwner->enter("then-call", "do then", nullptr, 0);
    forgedCallOwner->enter("else-call", "do else", nullptr, 0);
    assert(!forgedCallOwner->enabled() &&
           !forgedCallOwner->exportSelfContained());

    NativeProcedureFrameSession thenSession(
        source, &blocks, source.sourceRevision, digest);
    assert(thenSession.enabled());
    thenSession.enter("then-call", "do then", nullptr, 0);
    assert(thenSession.enabled());
    auto thenImage = thenSession.exportSelfContained();
    assert(thenImage && thenImage->valid());
    const auto thenFrames = thenImage->decodeFrames();
    assert(thenFrames && thenFrames->size() == 2);
    const auto thenSite = thenFrames->back().caller;
    assert(thenSite && thenSite->callerProcedureId == 0);
    assert(thenSite->calleeProcedureId ==
           thenFrames->back().frame.procedureId);
    assert(thenSite->calleeEntryPc &&
           thenImage->symbols->block(thenSite->calleeEntryPc) ==
               std::optional<std::string_view>("body-then"));
    assert(thenSite->argumentCount == 0);
    assert(thenSite->branchPath.size() == 2);
    assert(thenSite->branchPath[0].arm == 1);
    assert(thenSite->branchPath[1].arm == 1);
    assert(thenImage->symbols->block(
        thenSite->branchPath[1].controlPc) ==
        std::optional<std::string_view>("nested-branch"));
    assert(thenImage->symbols->block(
        thenSite->branchPath[0].controlPc) ==
        std::optional<std::string_view>("branch"));

    // Numeric PCs alone are insufficient: names for the enclosing
    // controller and the callee entry must still be source-authentic.
    const auto renameThen = [&](uint64_t pc, const char* otherName) {
        auto image = thenSession.exportSelfContained();
        assert(image && image->valid());
        auto cloned =
            std::make_shared<SourceFrameSymbols>(*image->symbols);
        assert(pc && pc <= cloned->blockIds.size());
        cloned->blockIds[size_t(pc - 1)] = otherName;
        image->symbols = std::move(cloned);
        assert(!image->valid());
    };
    renameThen(thenSite->branchPath[0].controlPc, "branch-decoy");
    renameThen(thenSite->branchPath[1].controlPc, "branch-decoy");
    renameThen(thenSite->calleeEntryPc, "body-else");

    // The outer IF/ELSE contains both real arms. Swapping only its
    // metadata arm from then to else must still fail: checking that the
    // block has an ELSE substack is not enough to prove the call's path.
    auto forgedArm = thenSession.exportSelfContained();
    assert(forgedArm && forgedArm->valid());
    auto forgedArmSymbols =
        std::make_shared<SourceFrameSymbols>(*forgedArm->symbols);
    forgedArmSymbols->returns.at(thenSite->returnPc).branchPath[0].arm = 2;
    forgedArm->symbols = std::move(forgedArmSymbols);
    assert(!forgedArm->valid());

    NativeProcedureFrameSession elseSession(
        source, &blocks, source.sourceRevision, digest);
    elseSession.enter("else-call", "do else", nullptr, 0);
    assert(elseSession.enabled());
    auto elseImage = elseSession.exportSelfContained();
    assert(elseImage && elseImage->valid());
    const auto elseFrames = elseImage->decodeFrames();
    assert(elseFrames && elseFrames->size() == 2);
    const auto elseSite = elseFrames->back().caller;
    assert(elseSite && elseSite->callerProcedureId == 0);
    assert(elseSite->calleeProcedureId ==
           elseFrames->back().frame.procedureId);
    assert(elseSite->returnPc != thenSite->returnPc);
    assert(elseSite->branchPath.size() == 1);
    assert(elseSite->branchPath[0].arm == 2);
    assert(thenSite->kind == SourceReturnKind::AfterBranch &&
           elseSite->kind == SourceReturnKind::AfterBranch);
    assert(thenSite->targetPc == elseSite->targetPc &&
           thenImage->symbols->block(thenSite->targetPc) ==
               std::optional<std::string_view>("branch-successor"));

    // These fields were previously only checked against the *other*
    // forged frame, not the original callsite. Bind the immutable proof
    // to the actual synthetic return PC, callee, entry and arity.
    const auto verifyCorruptSite = [&](auto corrupt) {
        auto image = thenSession.exportSelfContained();
        assert(image && image->valid());
        auto copied = std::make_shared<SourceFrameSymbols>(*image->symbols);
        corrupt(copied->returns.at(thenSite->returnPc));
        image->symbols = std::move(copied);
        assert(!image->valid() && !image->postCallSuccessor());
    };
    verifyCorruptSite([&](SourceReturnSite& site) {
        site.returnPc = elseSite->returnPc;
    });
    verifyCorruptSite([&](SourceReturnSite& site) {
        site.calleeProcedureId = elseSite->calleeProcedureId;
    });
    verifyCorruptSite([&](SourceReturnSite& site) {
        site.calleeEntryPc = elseSite->calleeEntryPc;
    });
    verifyCorruptSite([&](SourceReturnSite& site) {
        ++site.argumentCount;
    });

    // Stronger adversary: change BOTH the recorded callee ID/entry
    // and the nested numeric frame's procedure/entry. The original
    // owner, valid return PC, argument count, branch arms and exact
    // source successor still agree, but the callsite cannot suddenly
    // invoke the other definition.
    auto forgedCallee = thenSession.exportSelfContained();
    assert(forgedCallee && forgedCallee->valid());
    const auto forgedLayout = forgedCallee->frames->frames().layout();
    const auto* original = forgedCallee->frames->frames().words();
    auto forgedCalleeFrames =
        std::make_unique<RootedScratchCallStack>(forgedLayout);
    assert(forgedCalleeFrames->push(
        0, CallFrameAbi::kNoPc, thenSite->returnPc, {},
        forgedLayout.localCapacity));
    for (uint32_t local = 0; local < forgedLayout.localCapacity; ++local)
        assert(forgedCalleeFrames->setNumericLocal(
            local, original[CallFrameAbi::localWord(
                forgedLayout.argumentCapacity, local)]));
    assert(forgedCalleeFrames->push(
        elseSite->calleeProcedureId, thenSite->returnPc,
        elseSite->calleeEntryPc, {}, forgedLayout.localCapacity));
    const auto loopCount = uint32_t(forgedCallee->symbols->loops.size());
    for (uint32_t local = 0; local < loopCount; ++local)
        assert(forgedCalleeFrames->setNumericLocal(
            local, CallFrameAbi::kNoLoopCounter));
    assert(forgedCalleeFrames->setNumericLocal(2 * loopCount, 0));
    assert(forgedCalleeFrames->setNumericLocal(
        2 * loopCount + 1,
        uint64_t(SourceResumePhase::ProcedureEntry)));
    assert(forgedCalleeFrames->valid());
    auto renamed = std::make_shared<SourceFrameSymbols>(
        *forgedCallee->symbols);
    auto& site = renamed->returns.at(thenSite->returnPc);
    site.calleeProcedureId = elseSite->calleeProcedureId;
    site.calleeEntryPc = elseSite->calleeEntryPc;
    forgedCallee->frames = std::move(forgedCalleeFrames);
    forgedCallee->symbols = std::move(renamed);
    assert(!forgedCallee->valid());

    // Both real calls are root-owned and have the *same* AfterBranch
    // successor. A substituted callBlockPc is not a legitimate way
    // to change which lexical arm the frame actually executed.
    auto forgedCallPc = thenSession.exportSelfContained();
    assert(forgedCallPc && forgedCallPc->valid());
    auto forgedCallSymbols =
        std::make_shared<SourceFrameSymbols>(*forgedCallPc->symbols);
    forgedCallSymbols->returns.at(thenSite->returnPc).callBlockPc =
        elseSite->callBlockPc;
    forgedCallPc->symbols = std::move(forgedCallSymbols);
    assert(!forgedCallPc->valid());

    // Both arms must converge on the original NEXT after the branch,
    // not on another same-owner reachable root-script statement.
    auto forgedBranch = thenSession.exportSelfContained();
    assert(forgedBranch && forgedBranch->valid());
    const auto& symbolBlocks = forgedBranch->symbols->blockIds;
    const auto decoy = std::find(symbolBlocks.begin(), symbolBlocks.end(),
                                 "branch-decoy");
    assert(decoy != symbolBlocks.end());
    auto branchSymbols =
        std::make_shared<SourceFrameSymbols>(*forgedBranch->symbols);
    branchSymbols->returns.at(thenSite->returnPc).targetPc =
        uint64_t(decoy - symbolBlocks.begin() + 1);
    forgedBranch->symbols = std::move(branchSymbols);
    assert(!forgedBranch->valid());

    // A source loop from the THEN procedure exists in the same symbols
    // table, but may not be claimed by an ELSE procedure activation.
    auto wrongOwner = std::make_unique<SelfContainedSourceContinuation>();
    wrongOwner->symbols = elseImage->symbols;
    const auto elseLayout = elseImage->frames->frames().layout();
    wrongOwner->frames = std::make_unique<RootedScratchCallStack>(elseLayout);
    assert(wrongOwner->frames->push(
        0, CallFrameAbi::kNoPc, wrongOwner->symbols->sourceRootPc, {}, 0));
    assert(wrongOwner->frames->setResumePc(elseSite->returnPc));
    const auto thenLoopIt = std::find_if(
        wrongOwner->symbols->loops.begin(),
        wrongOwner->symbols->loops.end(), [&](const auto& loop) {
            return wrongOwner->symbols->block(loop.sourcePc) ==
                std::optional<std::string_view>("then-loop");
        });
    assert(thenLoopIt != wrongOwner->symbols->loops.end());
    assert(wrongOwner->frames->push(
        elseSite->calleeProcedureId, elseSite->returnPc,
        thenLoopIt->sourcePc, {}, elseLayout.localCapacity));
    const uint32_t count = uint32_t(wrongOwner->symbols->loops.size());
    for (uint32_t i = 0; i < count; ++i)
        assert(wrongOwner->frames->setNumericLocal(
            i, CallFrameAbi::kNoLoopCounter));
    assert(wrongOwner->frames->setNumericLocal(
        thenLoopIt->localSlot, Value::num(2).bits));
    assert(wrongOwner->frames->setNumericLocal(
        count, thenLoopIt->sourcePc));
    assert(wrongOwner->frames->setNumericLocal(2 * count, 1));
    assert(wrongOwner->frames->setNumericLocal(
        2 * count + 1, uint64_t(SourceResumePhase::AfterLoopBody)));
    assert(wrongOwner->frames->valid());
    assert(!wrongOwner->valid());

    thenSession.bodyStarted();
    const char* loopIds[] = {"then-loop"};
    const uint64_t countBits[] = {Value::num(2).bits};
    thenSession.checkpointPath(loopIds, countBits, 1,
        SourceResumePhase::InsideLoopBody);
    thenSession.enter("subcall", "do else", nullptr, 0);
    assert(thenSession.enabled());
    auto nestedImage = thenSession.exportSelfContained();
    assert(nestedImage && nestedImage->valid());
    const auto nestedFrames = nestedImage->decodeFrames();
    assert(nestedFrames && nestedFrames->size() == 3);
    const auto inner = nestedFrames->back().caller;
    assert(inner && inner->callerProcedureId == thenSite->calleeProcedureId);
    assert(inner->calleeProcedureId == elseSite->calleeProcedureId);
    assert(inner->callerLoopPath.size() == 1);
    const auto loopPc = nestedImage->symbols->block(
        inner->callerLoopPath.front());
    assert(loopPc == std::optional<std::string_view>("then-loop"));
    assert((*nestedFrames)[1].phase == SourceResumePhase::AwaitingCallee);
    assert((*nestedFrames)[1].loops.size() == 1);

    // Reconstruct independently rooted words with a valid procedure
    // chain, but alter only the parent source-phase or its loop depth.
    // The return PC, callee and argument count remain authentic.
    const auto forgedParent = [&](SourceResumePhase phase, bool path) {
        auto image = std::make_unique<SelfContainedSourceContinuation>();
        image->symbols = nestedImage->symbols;
        const auto layout = nestedImage->frames->frames().layout();
        image->frames = std::make_unique<RootedScratchCallStack>(layout);
        assert(image->frames->push(
            0, CallFrameAbi::kNoPc, image->symbols->sourceRootPc, {}, 0));
        assert(image->frames->setResumePc(thenSite->returnPc));
        assert(image->frames->push(
            thenSite->calleeProcedureId, thenSite->returnPc,
            inner->returnPc, {}, layout.localCapacity));
        assert(image->frames->setNumericLocal(
            0, path ? countBits[0] : CallFrameAbi::kNoLoopCounter));
        assert(image->frames->setNumericLocal(
            1, path ? inner->callerLoopPath.front() : 0));
        assert(image->frames->setNumericLocal(2, path ? 1 : 0));
        assert(image->frames->setNumericLocal(3, uint64_t(phase)));
        assert(image->frames->push(
            inner->calleeProcedureId, inner->returnPc,
            inner->calleeEntryPc, {}, layout.localCapacity));
        assert(image->frames->setNumericLocal(
            0, CallFrameAbi::kNoLoopCounter));
        assert(image->frames->setNumericLocal(
            3, uint64_t(SourceResumePhase::ProcedureEntry)));
        assert(image->frames->valid());
        return image;
    };
    assert(forgedParent(SourceResumePhase::AwaitingCallee, true)->valid());
    assert(!forgedParent(SourceResumePhase::UnknownBody, true)->valid());
    assert(!forgedParent(SourceResumePhase::AwaitingCallee, false)->valid());

    // Forging the nested callsite's lexical loop path *together* with
    // the parent frame's loop-depth/local words must still fail. The
    // previously present parent-frame vs site check alone would see
    // both forged copies as self-consistent.
    auto forgedLoopAgreement =
        forgedParent(SourceResumePhase::AwaitingCallee, false);
    auto forgedLoopSymbols =
        std::make_shared<SourceFrameSymbols>(
            *forgedLoopAgreement->symbols);
    forgedLoopSymbols->returns.at(inner->returnPc).callerLoopPath.clear();
    forgedLoopAgreement->symbols = std::move(forgedLoopSymbols);
    assert(!forgedLoopAgreement->valid());

    // A source script remains a valid root-only handoff after its nested
    // callees return. Its saved resume is the final root callsite's
    // synthetic return PC, not the original event hat source PC.
    thenSession.leave();
    assert(thenSession.enabled() && thenSession.depth() == 2);
    auto completedCallee = thenSession.sourceCheckpoint();
    assert(completedCallee &&
           completedCallee->phase == SourceResumePhase::AfterCallee);
    auto completedImage = thenSession.exportSelfContained();
    assert(completedImage && completedImage->valid());
    const auto loopSuccessor = completedImage->postCallSuccessor();
    assert(loopSuccessor &&
           loopSuccessor->kind == SourceReturnKind::LoopLatch &&
           completedImage->symbols->block(loopSuccessor->targetPc) ==
               std::optional<std::string_view>("then-loop") &&
           loopSuccessor->callerProcedureId == thenSite->calleeProcedureId);

    // The synthetic return PC still exists, but it cannot claim that
    // this child belongs to the root's alternate callsite.
    auto wrongReturn = std::make_unique<SelfContainedSourceContinuation>();
    wrongReturn->symbols = completedImage->symbols;
    wrongReturn->frames = std::make_unique<RootedScratchCallStack>(
        completedImage->frames->frames().layout());
    const auto layout = wrongReturn->frames->frames().layout();
    assert(wrongReturn->frames->push(
        0, CallFrameAbi::kNoPc, wrongReturn->symbols->sourceRootPc, {}, 0));
    assert(wrongReturn->frames->setResumePc(thenSite->returnPc));
    assert(wrongReturn->frames->push(
        thenSite->calleeProcedureId, thenSite->returnPc,
        elseSite->returnPc, {}, layout.localCapacity));
    assert(wrongReturn->frames->setNumericLocal(0, countBits[0]));
    assert(wrongReturn->frames->setNumericLocal(
        1, inner->callerLoopPath.front()));
    assert(wrongReturn->frames->setNumericLocal(2, 1));
    assert(wrongReturn->frames->setNumericLocal(
        3, uint64_t(SourceResumePhase::AfterCallee)));
    assert(!wrongReturn->valid() && !wrongReturn->postCallSuccessor());

    // Even an authentic source return site must not be redirected to
    // another procedure's existing block PC.
    auto wrongTarget = thenSession.exportSelfContained();
    assert(wrongTarget && wrongTarget->valid());
    auto alteredSymbols =
        std::make_shared<SourceFrameSymbols>(*wrongTarget->symbols);
    auto& alteredSite = alteredSymbols->returns.at(inner->returnPc);
    alteredSite.kind = SourceReturnKind::NextBlock;
    alteredSite.targetPc = elseSite->calleeEntryPc;
    wrongTarget->symbols = std::move(alteredSymbols);
    assert(!wrongTarget->valid() && !wrongTarget->postCallSuccessor());

    // Source execution moves on: the old post-call position cannot
    // describe a later wait or side effect in this caller.
    thenSession.statementStarted("body-then");
    completedCallee = thenSession.sourceCheckpoint();
    assert(completedCallee &&
           completedCallee->phase == SourceResumePhase::UnknownBody);
    auto oldPostCall = thenSession.exportSelfContained();
    assert(oldPostCall && oldPostCall->valid() &&
           !oldPostCall->postCallSuccessor());
    thenSession.leave();
    assert(thenSession.enabled() && thenSession.depth() == 1);
    auto rootOnly = thenSession.exportSelfContained();
    assert(rootOnly && rootOnly->valid() &&
           !rootOnly->postCallSuccessor());
    const auto rootBranch = rootOnly->rootPostCallSuccessor();
    assert(rootBranch &&
           rootBranch->kind == SourceReturnKind::AfterBranch &&
           rootBranch->branchPath.size() == 2 &&
           rootOnly->symbols->block(rootBranch->targetPc) ==
               std::optional<std::string_view>("branch-successor"));
    // The root has a verified AfterCallee phase, and its original
    // synthetic return PC must follow the *exact* branch successor.
    auto divertedRoot = thenSession.exportSelfContained();
    assert(divertedRoot && divertedRoot->valid());
    const auto& rootBlocks = divertedRoot->symbols->blockIds;
    const auto rootDecoy = std::find(rootBlocks.begin(), rootBlocks.end(),
                                     "branch-decoy");
    assert(rootDecoy != rootBlocks.end());
    auto divertedRootSymbols =
        std::make_shared<SourceFrameSymbols>(*divertedRoot->symbols);
    divertedRootSymbols->returns.at(thenSite->returnPc).targetPc =
        uint64_t(rootDecoy - rootBlocks.begin() + 1);
    divertedRoot->symbols = std::move(divertedRootSymbols);
    assert(!divertedRoot->valid() &&
           !divertedRoot->postCallSuccessor() &&
           !divertedRoot->rootPostCallSuccessor());

    auto invalidRoot = std::make_unique<SelfContainedSourceContinuation>();
    invalidRoot->symbols = rootOnly->symbols;
    invalidRoot->frames = std::make_unique<RootedScratchCallStack>(
        rootOnly->frames->frames().layout());
    assert(invalidRoot->frames->push(
        0, CallFrameAbi::kNoPc, invalidRoot->symbols->sourceRootPc, {}, 0));
    assert(invalidRoot->frames->setResumePc(0xDEADBEEFULL));
    assert(!invalidRoot->valid());

    // A correct return PC does NOT permit a caller to invent another
    // callee's frame. The GC-rooted handoff validator checks source
    // procedure identity rather than relying on executable state bits.
    auto forged = std::make_unique<SelfContainedSourceContinuation>();
    forged->symbols = thenImage->symbols;
    forged->frames = std::make_unique<RootedScratchCallStack>(
        thenImage->frames->frames().layout());
    const uint64_t rootPc = thenImage->frames->frames().top()->resumePc;
    assert(forged->frames->push(
        0, CallFrameAbi::kNoPc, 1, {}, 0));
    assert(forged->frames->setResumePc(thenSite->returnPc));
    assert(forged->frames->push(
        elseSite->calleeProcedureId, thenSite->returnPc,
        thenSite->calleeEntryPc, {},
        thenImage->frames->frames().layout().localCapacity));
    assert(forged->frames->valid());
    assert(!forged->valid());
    (void)rootPc;
}

int main() {
    Blocks blocks(nullptr);
    add(blocks, "hat", "event_whenflagclicked", "invoke");
    add(blocks, "invoke", "procedures_call", "after");
    add(blocks, "after", "data_changevariableby");
    add(blocks, "definition", "procedures_definition", "body");
    add(blocks, "prototype", "procedures_prototype");
    add(blocks, "body", "data_changevariableby");
    // Real Scratch graph parent links distinguish a linear NEXT edge
    // from entry into a SUBSTACK or a procedure body.
    blocks.getBlock("invoke")->parent = "hat";
    blocks.getBlock("after")->parent = "invoke";
    blocks.getBlock("body")->parent = "definition";
    blocks.getBlock("prototype")->parent = "definition";
    blocks.getBlock("invoke")->mutation.emplace();
    blocks.getBlock("invoke")->mutation->attrs["proccode"] = "sample %s";
    blocks.getBlock("invoke")->mutation->attrs["argumentids"] = "[\"arg0\"]";
    blocks.getBlock("definition")->inputs.set(
        "custom_block", {"prototype", ""});
    blocks.getBlock("prototype")->mutation.emplace();
    blocks.getBlock("prototype")->mutation->attrs["proccode"] = "sample %s";
    blocks.getBlock("prototype")->mutation->attrs["argumentids"] = "[\"arg0\"]";
    blocks.getBlock("prototype")->mutation->attrs["argumentnames"] = "[\"value\"]";
    blocks.getBlock("prototype")->mutation->attrs["argumentdefaults"] = "[\"\"]";

    const auto source =
        ScratchCompilationUnit::capture(blocks, blocks.getBlock("hat"));
    assert(source.valid());
    assert(source.sourceRevision != 0);
    assert(source.sourceRevision == blocks.revision());
    const auto digest = scratchSourceDigest(source);
    assert(digest);

    // An explicitly versioned native session can independently export
    // source-decoded, rooted CallFrames.
    NativeProcedureFrameSession fresh(
        source, &blocks, source.sourceRevision, digest);
    assert(fresh.enabled() && fresh.sourceUnchanged());
    assert(fresh.compiledRevision() == source.sourceRevision);
    const uint64_t numericArgs[] = {Value::num(123).bits};
    fresh.enter("invoke", "sample %s", numericArgs, 1);
    assert(fresh.enabled() && fresh.depth() == 2);
    auto image = fresh.exportSelfContained();
    assert(image && image->valid() && image->symbols);
    assert(image->symbols->sourceRevision == source.sourceRevision);
    assert(image->symbols->sourceDigest == digest);
    assert(image->decodeFrames() && image->decodeFrames()->size() == 2);

    // A scheduler handoff must outlive the native stack, including its
    // original GC provider. Detach only the separately rooted image.
    std::unique_ptr<SelfContainedSourceContinuation> detached;
    {
        NativeProcedureFrameSession temporary(
            source, &blocks, source.sourceRevision, digest);
        assert(temporary.enabled());
        const uint64_t stringArg[] = {
            Value::string(SStr::fromUtf8("detached root 世界")).bits
        };
        temporary.enter("invoke", "sample %s", stringArg, 1);
        assert(temporary.enabled() && temporary.liveHeapRoots() == 1);
        detached = temporary.exportSelfContained();
        assert(detached && detached->valid());
        assert(detached->frames->rootedArguments() == 1);
    }
    Gc::get().collect();
    assert(detached && detached->valid());
    assert(detached->frames->argument(1, 0));
    assert(SStr::toUtf8(detached->frames->argument(1, 0)->asStrPtr())
           == "detached root 世界");
    assert(detached->symbols->sourceRevision == source.sourceRevision);

    // The same return-PC alias attack must not bypass validation when
    // a child frame owns a non-ASCII GC-backed Scratch string argument.
    // Preserve the forged sidecar through a full GC to ensure rejection
    // is due to source PC provenance rather than accidentally losing
    // the live heap argument while copying the numeric frame arena.
    const auto detachedFrames = detached->decodeFrames();
    assert(detachedFrames && detachedFrames->size() == 2 &&
           detachedFrames->back().caller);
    auto stringAlias = forgeReturnSiteAlias(
        *detached, detachedFrames->back().caller->returnPc,
        (uint64_t{1} << 48) + 2);
    assert(stringAlias->frames->rootedArguments() == 1);
    Gc::get().collect();
    assert(stringAlias->frames->argument(1, 0) &&
           SStr::toUtf8(stringAlias->frames->argument(1, 0)->asStrPtr())
               == "detached root 世界");
    assert(!stringAlias->valid() && !stringAlias->postCallSuccessor());

    // The old observational helper does not know which source revision
    // produced its LLVM instructions. Never hand it off to the deoptimizer.
    NativeProcedureFrameSession legacy(source);
    assert(legacy.enabled());
    assert(!legacy.sourceUnchanged());
    assert(!legacy.exportSelfContained());

    // A source compiled from a different revision is immediately invalid,
    // even if the currently reachable source blocks are otherwise equal.
    NativeProcedureFrameSession staleAtConstruction(
        source, &blocks, source.sourceRevision + 1, digest);
    assert(!staleAtConstruction.enabled());
    assert(!staleAtConstruction.exportSelfContained());

    // Direct editor writes to Block fields need not call resetCache().
    // Their revision remains the same, but structural fingerprint must
    // prevent re-entry under the wrong interpreter source code.
    const auto originalBody = blocks.getBlock("body")->opcode;
    const uint64_t unchangedRevision = blocks.revision();
    blocks.getBlock("body")->opcode = "looks_say";
    assert(blocks.revision() == unchangedRevision);
    assert(fresh.sourceUnchanged()); // cheap revision check only
    assert(!fresh.sourceContentMatches());
    assert(!fresh.exportSelfContained());
    NativeProcedureFrameSession silentBodyEdit(
        ScratchCompilationUnit::capture(blocks, blocks.getBlock("hat")),
        &blocks, unchangedRevision, digest);
    assert(!silentBodyEdit.enabled());
    blocks.getBlock("body")->opcode = originalBody;
    assert(blocks.revision() == unchangedRevision);
    assert(fresh.sourceContentMatches());
    assert(fresh.exportSelfContained());

    // The full digest includes procedure defaults and mutation attributes,
    // not only the executed block IDs and opcode strings.
    auto& attrs = blocks.getBlock("prototype")->mutation->attrs;
    attrs["warp"] = "true";
    assert(blocks.revision() == unchangedRevision);
    assert(!fresh.exportSelfContained());
    attrs.erase("warp");
    assert(fresh.exportSelfContained());

    // Input edges and literal field Values also change semantics without
    // necessarily changing the source revision in the raw editor graph.
    auto* body = blocks.getBlock("body");
    body->fields["NUM"].value = Value::num(7);
    assert(blocks.revision() == unchangedRevision);
    assert(!fresh.exportSelfContained());
    body->fields.erase("NUM");
    assert(fresh.exportSelfContained());

    const std::string next = blocks.getBlock("invoke")->next;
    blocks.getBlock("invoke")->next.clear();
    assert(blocks.revision() == unchangedRevision);
    assert(!fresh.exportSelfContained());
    blocks.getBlock("invoke")->next = next;
    assert(fresh.exportSelfContained());

    // A version-only observer lacks a compile-time content fingerprint
    // and is not authorized to migrate between executors.
    NativeProcedureFrameSession versionOnly(
        source, &blocks, unchangedRevision);
    assert(versionOnly.enabled() && versionOnly.sourceUnchanged());
    assert(!versionOnly.exportSelfContained());

    // Editing via the normal Blocks API increments revision. A session
    // whose native code was compiled before this edit cannot export again,
    // and the stale source snapshot must be rejected at a new session.
    add(blocks, "unrelated-edit", "control_wait");
    assert(blocks.revision() != source.sourceRevision);
    assert(!fresh.sourceUnchanged());
    assert(!fresh.exportContinuation());
    assert(!fresh.exportSelfContained());
    fresh.enter("invoke", "sample %s", numericArgs, 1);
    assert(!fresh.enabled());
    NativeProcedureFrameSession newlyStale(
        source, &blocks, source.sourceRevision, digest);
    assert(!newlyStale.enabled());

    // Previously exported frames own their GC roots and source symbols.
    // They remain structurally valid for diagnostics, but their old
    // revision must NOT authorize execution against the edited Blocks.
    assert(image->valid());
    assert(image->symbols->sourceRevision != blocks.revision());
    assert(detached->valid());
    Gc::get().collect();
    assert(SStr::toUtf8(detached->frames->argument(1, 0)->asStrPtr())
           == "detached root 世界");

    const auto edited =
        ScratchCompilationUnit::capture(blocks, blocks.getBlock("hat"));
    assert(edited.valid() && edited.sourceRevision == blocks.revision());
    NativeProcedureFrameSession upToDate(
        edited, &blocks, edited.sourceRevision,
        scratchSourceDigest(edited));
    assert(upToDate.enabled() && upToDate.sourceUnchanged());
    assert(upToDate.exportSelfContained());

    verifyLexicalCallsites();
    verifyLoopTopology();
    verifyRootCountedRepeat();
    verifyCloneRootCaller();
    verifyPostCallNextBlock();
    verifyLongExecutableChain();

    std::cout << "PASS native compiled-source revision provenance and "
                 "stale CallFrame rejection\n";
}
