// IR-001: independently required Scratch source-state slots and
// compiler-proven LLVM SSA numeric bindings for one exact checkpoint.
// This is a diagnostic contract, NOT executable source deoptimization.
// Even a complete numeric subset is not a complete Scratch frame.
#pragma once

#include "source_typed_ssa_bridge.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace xyo::ir {

enum class SourceResumeSlotKind : uint8_t {
    NumericExpression = 1, // compiler-lowered expression, raw numeric only
    NumericLoopTemporary = 2, // compiler-lowered loop numeric only
    ScratchVariable = 3, // boxed GC/persistence requirements NOT satisfied
    ScratchList = 4, // GC/identity/alias requirements NOT satisfied
    OpaqueExpression = 5, // unknown native expression; never certify
};

// A slot ID is a source-lowering-provided index for the EXACT statement
// and must come from a frozen source snapshot. It is not an LLVM ID.
struct SourceResumeSlot {
    uint64_t statementPc = 0;
    uint32_t sourceSlot = 0;
    SourceResumeSlotKind kind = SourceResumeSlotKind::OpaqueExpression;
    uint8_t width = 0; // i1/i32/i64 when raw numeric; 0 for nonnumeric

    bool operator==(const SourceResumeSlot&) const = default;
};

inline bool validSourceResumeSlot(const SourceResumeSlot& slot) {
    if (!slot.statementPc || !slot.sourceSlot) return false;
    if (slot.kind == SourceResumeSlotKind::NumericExpression ||
        slot.kind == SourceResumeSlotKind::NumericLoopTemporary)
        return slot.width == 1 || slot.width == 32 || slot.width == 64;
    if (slot.kind == SourceResumeSlotKind::ScratchVariable ||
        slot.kind == SourceResumeSlotKind::ScratchList ||
        slot.kind == SourceResumeSlotKind::OpaqueExpression)
        return slot.width == 0;
    return false;
}

struct SourceResumeRequirements {
    // A separate compiler freeze of the exact typed SSA site.
    SourceTypedSsaManifest site;
    // Every source value demanded by a resume must be listed, including
    // nonnumeric/unmapped ones. If that enumeration is unproven, this
    // whole manifest MUST NOT be considered a source-completeness proof.
    std::vector<SourceResumeSlot> required;

    bool valid() const {
        if (!site.valid() || required.empty() || required.size() > 4096)
            return false;
        for (size_t i = 0; i < required.size(); ++i) {
            if (!validSourceResumeSlot(required[i])) return false;
            if (i && !(required[i - 1].statementPc < required[i].statementPc ||
                (required[i - 1].statementPc == required[i].statementPc &&
                 required[i - 1].sourceSlot < required[i].sourceSlot)))
                return false;
        }
        return true;
    }
};

// Require the ORIGINAL compiler-owned, frozen source dictionary
// before accepting SSA-lowering labels. SourceFrameSymbols checks the
// separately retained statement names/identity; additionally derive
// each statement owner from NEXT/SUBSTACK/PARENT source topology.
// This does not authenticate arbitrary user-mutated detached symbols:
// callers must supply the compiler-owned original proof object.
template<class FrozenSymbols>
bool verifyFrozenSourceResumeSlots(
    const SourceResumeRequirements& requirements,
    const FrozenSymbols& frozen) {
    if (!requirements.valid() || !frozen.verifiedSourceIdentity ||
        frozen.sourceRootPc == 0 ||
        frozen.verifiedSourceIdentity->rootPc != frozen.sourceRootPc ||
        frozen.sourceRevision != requirements.site.revision ||
        frozen.verifiedSourceIdentity->revision != frozen.sourceRevision ||
        frozen.sourceDigest.lo != requirements.site.digestLo ||
        frozen.sourceDigest.hi != requirements.site.digestHi ||
        frozen.verifiedSourceIdentity->digest != frozen.sourceDigest ||
        frozen.blockIds.size() != frozen.statementOwners.size() ||
        frozen.blockIds.size() != frozen.sourceTopology.size() ||
        !frozen.block(frozen.sourceRootPc) ||
        (requirements.site.procedureId != 0 &&
         !frozen.procedure(requirements.site.procedureId)))
        return false;
    for (const auto& slot : requirements.required) {
        if (slot.statementPc > frozen.statementOwners.size() ||
            !frozen.block(slot.statementPc) ||
            frozen.statementOwners[size_t(slot.statementPc - 1)] !=
                requirements.site.procedureId)
            return false;
        const auto structural = frozen.structuralCallerContext(
            slot.statementPc);
        if (!structural ||
            structural->procedureId != requirements.site.procedureId)
            return false;
        if (slot.kind == SourceResumeSlotKind::NumericLoopTemporary &&
            std::none_of(frozen.loops.begin(), frozen.loops.end(),
                [&](const auto& loop) {
                    return loop.sourcePc == slot.statementPc &&
                           loop.ownerProcedureId ==
                               requirements.site.procedureId;
                }))
            return false;
    }
    return true;
}

struct SourceNumericSsaBinding {
    SourceResumeSlot target; // exact source key and expected role
    uint64_t descriptor = 0; // compiler-assigned LLVM ID + typed width
};

struct SourceNumericCoverage {
    uint32_t required = 0;
    uint32_t mappedNumeric = 0;
    uint32_t missing = 0;
    uint32_t unsupportedSourceState = 0;
    // Only *the provided*, independently enumerated numeric subset.
    bool knownNumericSubsetComplete() const {
        return required && mappedNumeric == required &&
               missing == 0 && unsupportedSourceState == 0;
    }
    // Exact source-local environment, GC roots and effect replay remain
    // unproven even if the required numeric subset matches perfectly.
    bool sourceEnvironmentComplete() const { return false; }
    bool executable() const { return false; }
};

// Mappings MUST originate from compiler LLVM instruction identity and
// an independent source block/slot registration (see LLVM bridge).
// A captured word/FNV alone cannot create source-local provenance.
// Invalid duplicates/stale typed descriptors refuse the entire report.
template<class DetachedCapture>
std::optional<SourceNumericCoverage> auditSourceNumericCoverage(
    const DetachedCapture& saved,
    const SourceTypedSsaManifest& independentCompilerManifest,
    const SourceResumeRequirements& originalSourceRequirements,
    const std::vector<SourceNumericSsaBinding>& compilerBindings) {
    if (!originalSourceRequirements.valid() ||
        !verifyDetachedTypedSsa(saved, independentCompilerManifest))
        return std::nullopt;
    const auto& site = originalSourceRequirements.site;
    if (site.revision != independentCompilerManifest.revision ||
        site.digestLo != independentCompilerManifest.digestLo ||
        site.digestHi != independentCompilerManifest.digestHi ||
        site.procedureId != independentCompilerManifest.procedureId ||
        site.resumePc != independentCompilerManifest.resumePc ||
        site.siteId != independentCompilerManifest.siteId ||
        site.descriptors != independentCompilerManifest.descriptors)
        return std::nullopt;

    // Each source slot and each compiler SSA definition is bound at most
    // once. Repeated destination labels would overwrite source state;
    // the same live descriptor reused for distinct destinations is not
    // accepted without a separately proved alias/reuse contract.
    if (compilerBindings.size() > site.descriptors.size())
        return std::nullopt;
    std::vector<uint64_t> usedDescriptors;
    SourceNumericCoverage result;
    result.required = uint32_t(originalSourceRequirements.required.size());

    for (size_t i = 0; i < compilerBindings.size(); ++i) {
        const auto& binding = compilerBindings[i];
        if (!validSourceResumeSlot(binding.target) ||
            (i && !(compilerBindings[i - 1].target.statementPc <
                        binding.target.statementPc ||
                (compilerBindings[i - 1].target.statementPc ==
                     binding.target.statementPc &&
                 compilerBindings[i - 1].target.sourceSlot <
                     binding.target.sourceSlot))))
            return std::nullopt;

        const auto it = std::lower_bound(
            originalSourceRequirements.required.begin(),
            originalSourceRequirements.required.end(), binding.target,
            [](const SourceResumeSlot& a, const SourceResumeSlot& b) {
                return a.statementPc < b.statementPc ||
                    (a.statementPc == b.statementPc &&
                     a.sourceSlot < b.sourceSlot);
            });
        if (it == originalSourceRequirements.required.end() ||
            *it != binding.target)
            return std::nullopt;
        // Scratch variables, lists and opaque values cannot be fully
        // recovered by a raw LLVM i64 even if the bit pattern matches.
        if (binding.target.kind != SourceResumeSlotKind::NumericExpression &&
            binding.target.kind != SourceResumeSlotKind::NumericLoopTemporary)
            return std::nullopt;
        if (std::find(site.descriptors.begin(), site.descriptors.end(),
                      binding.descriptor) == site.descriptors.end())
            return std::nullopt;
        const uint32_t kindWidth = uint32_t(binding.descriptor >> 32);
        if ((kindWidth & ~uint32_t{255}) != 0 ||
            (kindWidth & 255) != binding.target.width)
            return std::nullopt;
        if (std::find(usedDescriptors.begin(), usedDescriptors.end(),
                      binding.descriptor) != usedDescriptors.end())
            return std::nullopt;
        usedDescriptors.push_back(binding.descriptor);
        ++result.mappedNumeric;
    }

    result.missing = result.required - result.mappedNumeric;
    for (const auto& source : originalSourceRequirements.required)
        if (source.kind == SourceResumeSlotKind::ScratchVariable ||
            source.kind == SourceResumeSlotKind::ScratchList ||
            source.kind == SourceResumeSlotKind::OpaqueExpression)
            ++result.unsupportedSourceState;
    return result;
}

} // namespace xyo::ir
