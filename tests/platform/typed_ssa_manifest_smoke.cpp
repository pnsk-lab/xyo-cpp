#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/source_typed_ssa.hpp"
#include "../../src/ir/source_typed_ssa_bridge.hpp"
#include "../../src/ir/source_deopt_audit.hpp"
#include "../../src/engine/thread.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace xyo::ir;

static uint64_t descriptor(uint32_t id, uint32_t kind, uint32_t width) {
    return uint64_t(id) | (uint64_t(kind | width) << 32);
}

int main() {
    constexpr auto integer = uint32_t(SourceSsaKind::IntegerInstruction);
    constexpr auto floating = uint32_t(SourceSsaKind::FloatingInstruction);
    constexpr auto intArg = uint32_t(SourceSsaKind::IntegerArgument);
    constexpr auto floatArg = uint32_t(SourceSsaKind::FloatingArgument);
    SourceTypedSsaManifest manifest{
        12, 13, 14, 3, 15, 16,
        {descriptor(0, integer, 1), descriptor(8, integer, 32),
         descriptor(9, floating, 32), descriptor(11, floating, 64),
         descriptor(0x80000000u, intArg, 64),
         descriptor(0x80000001u, floatArg, 32)}};
    assert(manifest.valid());
    std::vector<uint64_t> source{
        1, 0xffffffffu, 0x7fc00001u, 0x7ff8000000000001ULL,
        0xffffffffffffffffULL, 0x80000000u};
    auto copy = SourceTypedSsaSnapshot::capture(manifest, source);
    assert(copy && copy->valid() && copy->matches(manifest));
    source[0] = 0; source[3] = 0;
    assert(copy->words[0] == 1 &&
           copy->words[3] == 0x7ff8000000000001ULL);

    // Runtime source digests permit one zero half (but not both zero).
    auto oneZeroDigest = manifest;
    oneZeroDigest.digestLo = 0;
    assert(oneZeroDigest.valid());
    oneZeroDigest.digestHi = 0;
    assert(!oneZeroDigest.valid());

    auto wrong = manifest;
    ++wrong.siteId;
    assert(!copy->matches(wrong));
    wrong = manifest; ++wrong.resumePc;
    assert(!copy->matches(wrong));
    wrong = manifest; ++wrong.procedureId;
    assert(!copy->matches(wrong));
    wrong = manifest; wrong.digestLo ^= 1;
    assert(!copy->matches(wrong));

    wrong = manifest;
    wrong.descriptors[2] = descriptor(9, 0x400, 32);
    assert(!wrong.valid() && !SourceTypedSsaSnapshot::capture(wrong, source));
    wrong = manifest;
    wrong.descriptors[2] = descriptor(9, floating, 1);
    assert(!wrong.valid());
    wrong = manifest;
    wrong.descriptors[2] = descriptor(8, floating, 32);
    assert(!wrong.valid());
    wrong = manifest; wrong.descriptors.erase(wrong.descriptors.begin());
    assert(!SourceTypedSsaSnapshot::capture(wrong, source));

    source = copy->words;
    source[2] = uint64_t{1} << 32;
    assert(!SourceTypedSsaSnapshot::capture(manifest, source));
    source = copy->words;
    source[0] = 2;
    assert(!SourceTypedSsaSnapshot::capture(manifest, source));

    wrong = manifest;
    wrong.descriptors.assign(257, descriptor(0, integer, 64));
    assert(!wrong.valid()); // matches RT-001 max detached count
    wrong = manifest;
    wrong.siteId = 0;
    assert(!wrong.valid());
    wrong = manifest;
    wrong.descriptors[1] = descriptor(8, floating, 32);
    assert(!copy->matches(wrong)); // same ID/width but wrong kind
    auto corrupted = *copy;
    corrupted.words[5] |= uint64_t{1} << 40;
    assert(!corrupted.valid());

    // Exact RT-001 owned structure (no simulated layout). This adapter
    // only reads its diagnostics; no RT runtime or deopt state is modified.
    xyo::NativeRtSsaCapture detached;
    detached.sourceRevision = manifest.revision;
    detached.digestLo = manifest.digestLo;
    detached.digestHi = manifest.digestHi;
    detached.procedureId = manifest.procedureId;
    detached.sourcePc = manifest.resumePc;
    detached.siteId = manifest.siteId;
    detached.descriptors = manifest.descriptors;
    detached.words = copy->words;
    detached.unresolvedPointers = 2;
    detached.unresolvedOther = 1;
    detached.unsupported = 3;
    detached.pointerOrigins[0] = 1;
    detached.pointerOrigins[2] = 1;
    detached.numericComplete = false;
    detached.fingerprint = detached.computeFingerprint();
    auto bridged = verifyDetachedTypedSsa(detached, manifest);
    assert(bridged && bridged->matches(manifest));
    detached.words[0] = 0;
    assert(bridged->words[0] == 1);
    assert(!verifyDetachedTypedSsa(detached, manifest));
    detached.words = copy->words;
    detached.fingerprint = detached.computeFingerprint();
    assert(verifyDetachedTypedSsa(detached, manifest));
    detached.siteId++;
    detached.fingerprint = detached.computeFingerprint();
    assert(!verifyDetachedTypedSsa(detached, manifest));
    detached.siteId--;
    detached.pointerOrigins[1] = 1;
    detached.fingerprint = detached.computeFingerprint();
    assert(!verifyDetachedTypedSsa(detached, manifest));
    detached.pointerOrigins[1] = 0;
    detached.numericComplete = true;
    detached.fingerprint = detached.computeFingerprint();
    assert(!verifyDetachedTypedSsa(detached, manifest));
    detached.numericComplete = false;
    detached.fingerprint = detached.computeFingerprint();
    assert(verifyDetachedTypedSsa(detached, manifest));
    // Consistent pointer totals do not authenticate impossible native
    // stack allocation provenance, even if FNV is recomputed.
    detached.stackDerivedPointers = detached.unresolvedPointers + 1;
    detached.fingerprint = detached.computeFingerprint();
    assert(!verifyDetachedTypedSsa(detached, manifest));
    detached.stackDerivedPointers = 1;
    detached.distinctStackAllocas = 2;
    detached.fingerprint = detached.computeFingerprint();
    assert(!verifyDetachedTypedSsa(detached, manifest));
    detached.stackDerivedPointers = 0;
    detached.distinctStackAllocas = 0;
    detached.fingerprint = detached.computeFingerprint();
    assert(verifyDetachedTypedSsa(detached, manifest));
    auto wrongDescriptor = manifest;
    wrongDescriptor.descriptors[1] ^= uint64_t{0x100} << 32;
    assert(!verifyDetachedTypedSsa(detached, wrongDescriptor));
    auto damaged = detached;
    damaged.fingerprint ^= 1;
    assert(!verifyDetachedTypedSsa(damaged, manifest));
    damaged = detached;
    damaged.unresolvedOther = UINT32_MAX;
    damaged.unresolvedPointers = UINT32_MAX;
    damaged.unsupported = 1;
    damaged.fingerprint = damaged.computeFingerprint();
    assert(!verifyDetachedTypedSsa(damaged, manifest));

    // A complete raw numeric capture still cannot replay Scratch effects
    // or recover arbitrary LLVM temporaries into source VM locals.
    auto latch = auditSourceDeopt(SourceResumePhase::AfterLoopBody,
                                  &detached, manifest);
    assert(latch.boundary == SourceDeoptBoundary::LoopLatch);
    assert(latch.missing(SourceDeoptGap::EffectCommit));
    assert(latch.missing(SourceDeoptGap::LiveSsa));
    assert(latch.missing(SourceDeoptGap::GcRootContinuity));
    assert(latch.missing(SourceDeoptGap::UnresolvedPointers));
    assert(!latch.missing(SourceDeoptGap::InvalidSnapshot));
    assert(!latch.executable());
    auto unknown = auditSourceDeopt(SourceResumePhase::UnknownBody,
                                    &detached, manifest);
    assert(unknown.missing(SourceDeoptGap::ExactStatement));
    auto suspended = auditSourceDeopt(SourceResumePhase::AwaitingCallee,
                                      &detached, manifest);
    assert(suspended.missing(SourceDeoptGap::ActiveCallee));
    // A phase that needs no reconstructed SSA still needs an effect proof.
    auto entryAudit = auditSourceDeopt(SourceResumePhase::ProcedureEntry,
                                       &detached, manifest);
    assert(entryAudit.missing(SourceDeoptGap::EffectCommit));
    assert(!entryAudit.missing(SourceDeoptGap::LiveSsa));
    assert(!entryAudit.executable());
    const xyo::NativeRtSsaCapture* missingCapture = nullptr;
    auto absent = auditSourceDeopt(SourceResumePhase::AfterLoopBody,
                                   missingCapture, manifest);
    assert(absent.missing(SourceDeoptGap::InvalidSnapshot));
    assert(!absent.executable());
    auto stale = manifest;
    ++stale.siteId;
    auto invalid = auditSourceDeopt(SourceResumePhase::AfterLoopBody,
                                    &detached, stale);
    assert(invalid.missing(SourceDeoptGap::InvalidSnapshot));
    assert(!invalid.executable());
    // Even a fully numeric state (no unresolved pointers/other SSA)
    // does not demonstrate committed effects or source-level SSA rebuild.
    auto numericOnly = detached;
    numericOnly.unresolvedPointers = 0;
    numericOnly.unresolvedOther = 0;
    numericOnly.unsupported = 0;
    numericOnly.pointerOrigins = {};
    numericOnly.numericComplete = true;
    numericOnly.fingerprint = numericOnly.computeFingerprint();
    auto complete = auditSourceDeopt(SourceResumePhase::AfterLoopBody,
                                     &numericOnly, manifest);
    assert(!complete.missing(SourceDeoptGap::InvalidSnapshot));
    assert(!complete.missing(SourceDeoptGap::UnresolvedPointers));
    assert(complete.missing(SourceDeoptGap::EffectCommit));
    assert(complete.missing(SourceDeoptGap::LiveSsa));
    assert(complete.missing(SourceDeoptGap::GcRootContinuity));
    assert(!complete.executable());

    // Independent host journal observations do not prove side-effect
    // replay safety. Correlation detects stale checkpoints and source drift.
    SourceEffectSequenceTracker effects({
        manifest.revision, manifest.digestLo, manifest.digestHi,
        manifest.procedureId});
    auto effectSequence = effects.begin(55);
    assert(effectSequence && *effectSequence == 1);
    assert(!effects.checkpoint(manifest.siteId, manifest.resumePc,
                               SourceResumePhase::AfterLoopBody));
    assert(effects.commit(55, *effectSequence));
    auto effectFrontier = effects.checkpoint(
        manifest.siteId, manifest.resumePc, SourceResumePhase::AfterLoopBody);
    assert(effectFrontier);
    auto combined = auditSourceDeoptWithEffectFrontier(
        SourceResumePhase::AfterLoopBody, &numericOnly, manifest,
        &*effectFrontier, *effectFrontier, 1);
    assert(!combined.missing(SourceDeoptGap::EffectFrontierMismatch));
    assert(combined.missing(SourceDeoptGap::EffectCommit));
    assert(combined.missing(SourceDeoptGap::LiveSsa));
    assert(!combined.executable());
    combined = auditSourceDeoptWithEffectFrontier(
        SourceResumePhase::AfterLoopBody, &numericOnly, manifest,
        &*effectFrontier, *effectFrontier, 0);
    assert(combined.missing(SourceDeoptGap::EffectFrontierMismatch));
    auto staleFrontier = *effectFrontier;
    staleFrontier.committedCount = 0;
    combined = auditSourceDeoptWithEffectFrontier(
        SourceResumePhase::AfterLoopBody, &numericOnly, manifest,
        &staleFrontier, *effectFrontier, 1);
    assert(combined.missing(SourceDeoptGap::EffectFrontierMismatch));
    combined = auditSourceDeoptWithEffectFrontier(
        SourceResumePhase::AwaitingCallee, &numericOnly, manifest,
        &*effectFrontier, *effectFrontier, 1);
    assert(combined.missing(SourceDeoptGap::EffectFrontierMismatch));
    auto alienFrontier = *effectFrontier;
    ++alienFrontier.source.digestHi;
    combined = auditSourceDeoptWithEffectFrontier(
        SourceResumePhase::AfterLoopBody, &numericOnly, manifest,
        &*effectFrontier, alienFrontier, 1);
    assert(combined.missing(SourceDeoptGap::EffectFrontierMismatch));
    combined = auditSourceDeoptWithEffectFrontier(
        SourceResumePhase::AfterLoopBody, &numericOnly, manifest,
        nullptr, *effectFrontier, 1);
    assert(combined.missing(SourceDeoptGap::EffectFrontierMismatch));

    // All tagged widths preserve the raw float NaN payload; never decode
    // those bits as a pointer or a Scratch object.
    assert(copy->words[2] == 0x7fc00001u);
    std::cout << "PASS typed detached SSA diagnostics\n";
}
