// IR-001: actual GC, RT-generated string roots, and SSA provenance.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/ir/source_ssa_gc_audit.hpp"
#include "../../src/jit/rt_generated_ssa_roots.hpp"
#include "../../src/core/sstr.hpp"
#include <cassert>
#include <cstdio>
using namespace xyo;
using namespace xyo::ir;

static NativeRtSsaCapture make(uint64_t bits) {
    NativeRtSsaCapture s;
    s.sourceRevision = 19; s.digestLo = 21; s.digestHi = 23;
    s.sourcePc = 27; s.siteId = 29;
    s.descriptors = {
        (uint64_t(64) << 32) | 1,
        (uint64_t(64) << 32) | 2,
        (uint64_t(0x100 | 64) << 32) | 3,
        (uint64_t(64) << 32) | 4
    };
    s.words = {bits, Value::emptyStr().bits,
               val::kTagStr | 0x12340, Value::num(42).bits};
    s.aliasArguments.assign(4, 0);
    s.compilerAliasArguments = s.aliasArguments;
    s.generatedStrings = {1, 1, 0, 0};
    s.compilerGeneratedStrings = s.generatedStrings;
    s.numericComplete = true;
    s.fingerprint = s.computeFingerprint();
    return s;
}

static SourceTypedSsaManifest manifest(const NativeRtSsaCapture& s) {
    return {s.sourceRevision, s.digestLo, s.digestHi,
            s.procedureId, s.sourcePc, s.siteId, s.descriptors};
}

int main() {
    auto& gc = Gc::get();
    auto saved = make(Value::string(
        SStr::create(u"IR-001 GC-rooted SSA generated value")).bits);
    const auto expected = manifest(saved);
    assert(expected.valid());
    assert(rt::retainGeneratedSsaRoots(saved));
    assert(rt::verifyGeneratedSsaRoots(saved));

    auto report = auditDetachedSsaGcRoots(saved, expected);
    assert(report.knownGeneratedRootsConsistent());
    assert(report.capturedSsaWords == 4);
    assert(report.certifiedGeneratedStrings == 2);
    assert(report.rootedGeneratedStrings == 1);
    assert(report.immediateGeneratedStrings == 1);
    assert(!report.missing(SourceSsaGcGap::UnrootedBoxedString));
    assert(report.missing(SourceSsaGcGap::UnmappedSourceLocal));
    assert(!report.executable());

    // Real collection: a shared detached copy owns the exact roots.
    gc.collect();
    auto copy = saved;
    saved.generatedStringRoots.reset();
    saved.generatedStringHandles.clear();
    gc.collect();
    assert(auditDetachedSsaGcRoots(
        copy, expected).knownGeneratedRootsConsistent());

    auto changed = copy;
    changed.words[0] ^= 1;
    changed.fingerprint = changed.computeFingerprint();
    assert(auditDetachedSsaGcRoots(changed, expected)
           .missing(SourceSsaGcGap::InvalidRootHandle));

    changed = copy;
    changed.generatedStrings[0] = 0;
    changed.fingerprint = changed.computeFingerprint();
    assert(auditDetachedSsaGcRoots(changed, expected)
           .missing(SourceSsaGcGap::CorruptCompilerEvidence));

    changed = copy;
    changed.compilerGeneratedStrings.clear();
    changed.generatedStrings.clear();
    changed.fingerprint = changed.computeFingerprint();
    assert(auditDetachedSsaGcRoots(changed, expected)
           .missing(SourceSsaGcGap::MissingCompilerEvidence));

    changed = copy;
    changed.generatedStrings[1] = 0;
    changed.compilerGeneratedStrings[1] = 0;
    changed.generatedStringHandles[1] = copy.generatedStringHandles[0];
    changed.fingerprint = changed.computeFingerprint();
    assert(auditDetachedSsaGcRoots(changed, expected)
           .missing(SourceSsaGcGap::InvalidRootHandle));

    const auto handle = copy.generatedStringHandles[0];
    assert(copy.generatedStringRoots->release(handle));
    assert(auditDetachedSsaGcRoots(copy, expected)
           .missing(SourceSsaGcGap::InvalidRootHandle));

    // A minted root handle does not make fabricated pointer bits a
    // real allocated GcType::Str object. Check membership first.
    auto fakeRoot = make(val::kTagStr | 0x123456);
    fakeRoot.generatedStringHandles.resize(4);
    fakeRoot.generatedStringRoots = std::make_shared<GcValueHandles>(gc);
    const auto fakeHandle = fakeRoot.generatedStringRoots->retain(
        Value{fakeRoot.words[0]});
    assert(fakeHandle);
    fakeRoot.generatedStringHandles[0] = *fakeHandle;
    fakeRoot.fingerprint = fakeRoot.computeFingerprint();
    assert(auditDetachedSsaGcRoots(fakeRoot, expected)
           .missing(SourceSsaGcGap::InvalidRootHandle));
    fakeRoot.generatedStringRoots.reset();

    // No arbitrary pointer dereference; unproven boxed heap strings
    // are explicitly classified as unrooted potential values.
    auto unproven = make(val::kTagStr | 0x123456);
    unproven.generatedStrings.assign(4, 0);
    unproven.compilerGeneratedStrings = unproven.generatedStrings;
    unproven.generatedStringHandles.resize(4);
    unproven.fingerprint = unproven.computeFingerprint();
    report = auditDetachedSsaGcRoots(unproven, expected);
    assert(report.knownGeneratedRootsConsistent());
    assert(report.unrootedBoxedStrings == 1);
    assert(report.missing(SourceSsaGcGap::UnrootedBoxedString));
    assert(!report.executable());

    auto stale = expected; ++stale.siteId;
    assert(auditDetachedSsaGcRoots(unproven, stale)
           .missing(SourceSsaGcGap::InvalidTypedCapture));

    // Original Scratch argument must be checked against the separate
    // rooted CallFrame, NEVER inferred from its LLVM alias index.
    auto arg = make(Value::num(1).bits);
    arg.descriptors = {(uint64_t(0x200 | 64) << 32) | 0x80000001u};
    arg.words = {Value::string(
        SStr::create(u"source procedure argument")).bits};
    arg.generatedStrings = {0}; arg.compilerGeneratedStrings = {0};
    arg.aliasArguments = {1}; arg.compilerAliasArguments = {1};
    arg.generatedStringHandles = {{}};
    arg.fingerprint = arg.computeFingerprint();
    report = auditDetachedSsaGcRoots(arg, manifest(arg));
    assert(report.unverifiedArgumentStrings == 1);
    assert(report.missing(SourceSsaGcGap::UnverifiedArgumentRoot));

    auto pointers = unproven;
    pointers.unresolvedPointers = 1;
    pointers.unsupported = 1;
    pointers.numericComplete = false;
    pointers.pointerOrigins[0] = 1;
    pointers.fingerprint = pointers.computeFingerprint();
    report = auditDetachedSsaGcRoots(pointers, expected);
    assert(report.missing(SourceSsaGcGap::UnresolvedNativePointer));
    std::puts("PASS IR-001 detached SSA exact GC handles, real GC, "
              "forged/lost roots, unrooted/alias/native pointer gaps");
}
