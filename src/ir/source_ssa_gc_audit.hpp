// IR-001: read-only detached LLVM SSA / GC root coverage audit.
// Only exact RT-owned handles are resolved. Raw NaN-boxed words are
// NEVER dereferenced or turned into new roots.
#pragma once

#include "source_typed_ssa_bridge.hpp"
#include "../core/gc.hpp"
#include "../core/value.hpp"

#include <cstddef>
#include <cstdint>

namespace xyo::ir {

enum class SourceSsaGcGap : uint16_t {
    InvalidTypedCapture = 1u << 0,
    MissingCompilerEvidence = 1u << 1,
    CorruptCompilerEvidence = 1u << 2,
    InvalidRootHandle = 1u << 3,
    UnrootedBoxedString = 1u << 4,
    UnverifiedArgumentRoot = 1u << 5,
    UnresolvedNativePointer = 1u << 6,
    UnresolvedOtherSsa = 1u << 7,
    UnmappedSourceLocal = 1u << 8,
};

struct SourceSsaGcAudit {
    uint16_t gaps = 0;
    uint32_t capturedSsaWords = 0;
    uint32_t certifiedGeneratedStrings = 0;
    uint32_t rootedGeneratedStrings = 0;
    uint32_t immediateGeneratedStrings = 0;
    uint32_t unrootedBoxedStrings = 0;
    uint32_t unverifiedArgumentStrings = 0;
    uint32_t unresolvedNativePointers = 0;
    uint32_t unresolvedOtherSsa = 0;

    bool missing(SourceSsaGcGap gap) const {
        return (gaps & uint16_t(gap)) != 0;
    }
    // Does NOT imply entire LLVM SSA, GC or source-local completeness.
    bool knownGeneratedRootsConsistent() const {
        constexpr uint16_t mask =
            uint16_t(SourceSsaGcGap::InvalidTypedCapture) |
            uint16_t(SourceSsaGcGap::MissingCompilerEvidence) |
            uint16_t(SourceSsaGcGap::CorruptCompilerEvidence) |
            uint16_t(SourceSsaGcGap::InvalidRootHandle);
        return (gaps & mask) == 0;
    }
    bool executable() const { return false; }
};

// Requires a separately retained compiler manifest. Never creates,
// changes or releases roots. Does not infer root ownership from a tag.
template<class DetachedCapture>
SourceSsaGcAudit auditDetachedSsaGcRoots(
    const DetachedCapture& saved,
    const SourceTypedSsaManifest& compilerManifest) {
    SourceSsaGcAudit report;
    if (!verifyDetachedTypedSsa(saved, compilerManifest)) {
        report.gaps |= uint16_t(SourceSsaGcGap::InvalidTypedCapture);
        return report;
    }

    report.capturedSsaWords = uint32_t(saved.words.size());
    report.unresolvedNativePointers = saved.unresolvedPointers;
    report.unresolvedOtherSsa = saved.unresolvedOther;
    if (saved.unresolvedPointers)
        report.gaps |= uint16_t(SourceSsaGcGap::UnresolvedNativePointer);
    if (saved.unresolvedOther)
        report.gaps |= uint16_t(SourceSsaGcGap::UnresolvedOtherSsa);
    // Neither metadata nor rooted sidecar maps every LLVM instruction
    // to a source-level Scratch local.
    report.gaps |= uint16_t(SourceSsaGcGap::UnmappedSourceLocal);

    const size_t count = saved.words.size();
    if (saved.generatedStrings.size() != count ||
        saved.compilerGeneratedStrings.size() != count ||
        saved.aliasArguments.size() != count ||
        saved.compilerAliasArguments.size() != count ||
        saved.generatedStringHandles.size() != count) {
        report.gaps |= uint16_t(SourceSsaGcGap::MissingCompilerEvidence);
        return report;
    }
    if (saved.generatedStrings != saved.compilerGeneratedStrings ||
        saved.aliasArguments != saved.compilerAliasArguments) {
        report.gaps |= uint16_t(SourceSsaGcGap::CorruptCompilerEvidence);
        return report;
    }

    for (size_t i = 0; i < count; ++i) {
        const uint32_t id = uint32_t(saved.descriptors[i]);
        const uint32_t kindWidth = uint32_t(saved.descriptors[i] >> 32);
        const uint32_t family = kindWidth & ~uint32_t{255};
        const uint32_t width = kindWidth & 255u;
        const uint32_t proof = saved.generatedStrings[i];
        const uint32_t alias = saved.aliasArguments[i];
        const auto handle = saved.generatedStringHandles[i];
        // Floating SSA bits are not Scratch boxed values.
        const bool boxedCandidate = (family == 0 || family == 0x200) &&
                                    width == 64;
        const Value word{saved.words[i]};
        if (proof > 1 || (proof &&
            (family != 0 || width != 64 || (id & 0x80000000u) ||
             alias)) || (alias && !boxedCandidate)) {
            report.gaps |= uint16_t(SourceSsaGcGap::CorruptCompilerEvidence);
            return report;
        }
        if (proof) {
            ++report.certifiedGeneratedStrings;
            if (!word.isString()) {
                report.gaps |= uint16_t(SourceSsaGcGap::CorruptCompilerEvidence);
                return report;
            }
            if (word.isEmptyString()) {
                if (handle) {
                    report.gaps |= uint16_t(SourceSsaGcGap::InvalidRootHandle);
                    return report;
                }
                ++report.immediateGeneratedStrings;
            } else {
                if (!handle || !saved.generatedStringRoots) {
                    report.gaps |= uint16_t(SourceSsaGcGap::InvalidRootHandle);
                    return report;
                }
                const auto resolved = saved.generatedStringRoots->resolve(handle);
                if (!resolved || resolved->bits != word.bits) {
                    report.gaps |= uint16_t(SourceSsaGcGap::InvalidRootHandle);
                    return report;
                }
                // A handle can contain fabricated boxed bits: validate
                // GC header membership BEFORE touching its type.
                const uintptr_t address =
                    reinterpret_cast<uintptr_t>(resolved->asStrPtr());
                if (address < sizeof(GcHeader)) {
                    report.gaps |= uint16_t(SourceSsaGcGap::InvalidRootHandle);
                    return report;
                }
                auto* header = reinterpret_cast<GcHeader*>(
                    address - sizeof(GcHeader));
                if (!Gc::get().isObject(header) ||
                    header->type != GcType::Str) {
                    report.gaps |= uint16_t(SourceSsaGcGap::InvalidRootHandle);
                    return report;
                }
                ++report.rootedGeneratedStrings;
            }
            continue;
        }
        if (handle) {
            report.gaps |= uint16_t(SourceSsaGcGap::InvalidRootHandle);
            return report;
        }
        if (boxedCandidate && word.isString() && !word.isEmptyString()) {
            if (alias) {
                ++report.unverifiedArgumentStrings;
                report.gaps |= uint16_t(SourceSsaGcGap::UnverifiedArgumentRoot);
            } else {
                ++report.unrootedBoxedStrings;
                report.gaps |= uint16_t(SourceSsaGcGap::UnrootedBoxedString);
            }
        }
    }
    if (saved.generatedStringRoots &&
        saved.generatedStringRoots->size() != report.rootedGeneratedStrings)
        report.gaps |= uint16_t(SourceSsaGcGap::InvalidRootHandle);
    return report;
}

} // namespace xyo::ir
