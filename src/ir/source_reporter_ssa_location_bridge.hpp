// IR-001: connect compiler-observed Scratch reporter SSA descriptors to
// RT-001's independently verified detached capture and GC-rooted locations.
// Diagnostic only: no Scratch local/reporter restoration slot or effect proof.
#pragma once

#include "llvm_source_reporter_trace.hpp"
#include "source_ssa_location_bridge.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace xyo::ir {

// A SEPARATE, frozen compiler artifact describing exactly which Scratch
// source reporter produced each SSA definition at a physical checkpoint.
// Never reconstruct this from the mutable detached RT state. The actual
// compiler integration must capture it before IR spill insertion.
struct SourceReporterCompiledSite {
    uint64_t revision = 0;
    uint64_t digestLo = 0;
    uint64_t digestHi = 0;
    uint64_t physicalSiteId = 0;
    uint64_t sourcePc = 0;
    SourceResumePhase phase = SourceResumePhase::UnknownBody;
    std::string sourceLoopBlock;
    uint32_t loopDepth = 0;
    std::vector<std::string> loopPath;
    std::vector<SourceReporterLiveSsaBinding> reporters;

    bool matches(const SourceReporterCheckpointGap& observed,
                 const SourceTypedSsaManifest& expected) const {
        return expected.valid() &&
            revision == expected.revision &&
            digestLo == expected.digestLo &&
            digestHi == expected.digestHi &&
            physicalSiteId == expected.siteId &&
            sourcePc == expected.resumePc &&
            observed.physicalSiteId == physicalSiteId &&
            observed.sourcePc == sourcePc &&
            observed.phase == phase &&
            observed.sourceLoopBlock == sourceLoopBlock &&
            observed.portableLoopPathDepth == loopDepth &&
            observed.sourceLoopPath == loopPath &&
            observed.reporterSsaBindings == reporters &&
            observed.liveReporterObservations == reporters.size();
    }
};

// Compiler-only creation. A consumer must keep this object separate from
// an untrusted mutable reporter observation and the RT-owned snapshot.
// Creating both from the same tampered input is not independent evidence.
inline std::optional<SourceReporterCompiledSite> freezeReporterSourceSite(
    const SourceReporterCheckpointGap& source,
    const SourceTypedSsaManifest& manifest) {
    if (!manifest.valid() || !source.sourcePc ||
        !source.physicalSiteId || source.sourcePc != manifest.resumePc ||
        source.physicalSiteId != manifest.siteId ||
        source.liveReporterObservations !=
            source.reporterSsaBindings.size())
        return std::nullopt;
    SourceReporterCompiledSite result{
        manifest.revision, manifest.digestLo, manifest.digestHi,
        source.physicalSiteId, source.sourcePc, source.phase,
        source.sourceLoopBlock, source.portableLoopPathDepth,
        source.sourceLoopPath, source.reporterSsaBindings
    };
    return result.matches(source, manifest)
        ? std::optional<SourceReporterCompiledSite>(std::move(result))
        : std::nullopt;
}

// Owned by the published CompiledScript, not by a mutable Thread or a
// detached GC/SSA image. The codegen emitter freezes this before any
// synthetic SSA spill is inserted. Entries are indexed by physical
// LLVM CallBase ordinal, never solely by Scratch source PC.
struct SourceReporterRuntimeSites {
    struct Entry {
        SourceReporterCheckpointGap gap;
        SourceReporterCompiledSite frozen;
    };
    std::vector<Entry> sites;

    const Entry* find(uint64_t physicalSiteId) const {
        auto it = std::find_if(sites.begin(), sites.end(),
            [&](const Entry& entry) {
                return entry.frozen.physicalSiteId == physicalSiteId;
            });
        return it == sites.end() ? nullptr : &*it;
    }
};

struct SourceReporterSsaLocation {
    uint64_t reporterPc = 0;
    uint64_t descriptor = 0;
    SourceReporterOrigin origin = SourceReporterOrigin::Unknown;
    SourceSsaLocationKind location = SourceSsaLocationKind::Unmapped;
    uint32_t argument = 0;
    // Only a proven boxed heap string requires an exact GC root handle.
    // Other i64 words are never assumed to be safe source Values.
    bool observedHeapString = false;
    bool verifiedHeapStringRoot = false;
};

struct SourceReporterSsaLocationAudit {
    uint64_t physicalSiteId = 0;
    uint64_t sourcePc = 0;
    uint64_t rootedFrameRevision = 0;
    uint32_t mappedArguments = 0;
    uint32_t mappedGeneratedStrings = 0;
    // Counts string-valued SSA observations with verified GC roots.
    // Multiple observations may alias one underlying rooted object.
    uint32_t verifiedHeapStringObservations = 0;
    uint32_t unresolvedReporters = 0;
    uint32_t unsupportedLlvmValues = 0;
    std::vector<SourceReporterSsaLocation> reporters;
    bool sourceEnvironmentComplete() const { return false; }
    bool executable() const { return false; }
};

// The compiler site MUST be independently obtained from the frozen
// physical LLVM CallBase trace and compared against its separately retained
// SourceReporterCompiledSite. That compiler-side origin manifest must not
// be reconstructed from the RT snapshot or an altered runtime observation.
// A source loop PC alone cannot distinguish
// two separate checkpoints in the same loop; physicalSiteId must match the
// separately emitted RT-001 native capture site's 1-based module ordinal.
//
// Exact roots for compiler-proven procedure argument aliases and trusted
// generated strings are delegated to the existing RT-001 validators. An
// unclassified SSA Value remains unknown even when its bits look like an
// immediate number. This never maps a value to an interpreter local.
template<class Detached, class RootedFrames>
std::optional<SourceReporterSsaLocationAudit> auditSourceReporterSsaLocations(
    const SourceReporterCheckpointGap& compilerSite,
    const SourceReporterCompiledSite& frozenReporterSite,
    const SourceTypedSsaManifest& compilerManifest,
    const Detached& detached, const RootedFrames& rootedFrames) {
    if (!frozenReporterSite.matches(compilerSite, compilerManifest) ||
        !compilerSite.sourcePc || !compilerSite.physicalSiteId ||
        compilerSite.sourcePc != compilerManifest.resumePc ||
        compilerSite.physicalSiteId != compilerManifest.siteId ||
        (compilerSite.phase != SourceResumePhase::InsideLoopBody &&
         compilerSite.phase != SourceResumePhase::AfterLoopBody) ||
        compilerSite.sourceLoopBlock.empty() ||
        compilerSite.portableLoopPathDepth == 0 ||
        compilerSite.portableLoopPathDepth > 32 ||
        compilerSite.sourceLoopPath.size() !=
            compilerSite.portableLoopPathDepth ||
        compilerSite.sourceLoopPath.back() != compilerSite.sourceLoopBlock ||
        compilerSite.liveReporterObservations !=
            compilerSite.reporterSsaBindings.size())
        return std::nullopt;
    std::set<std::string> uniquePath;
    for (const auto& loop : compilerSite.sourceLoopPath)
        if (loop.empty() || !uniquePath.insert(loop).second)
            return std::nullopt;

    const auto roots = auditSourceSsaLocations(
        detached, compilerManifest, rootedFrames);
    if (!roots || roots->locations.size() !=
                      compilerManifest.descriptors.size() ||
        roots->locations.size() != detached.words.size())
        return std::nullopt;

    SourceReporterSsaLocationAudit result;
    result.sourcePc = compilerSite.sourcePc;
    result.physicalSiteId = compilerSite.physicalSiteId;
    result.rootedFrameRevision = roots->rootedFrameRevision;
    result.unsupportedLlvmValues = compilerSite.unsupportedLlvmValues;
    result.reporters.reserve(compilerSite.reporterSsaBindings.size());

    std::set<std::pair<uint64_t, uint64_t>> uniqueReporterValues;
    for (const auto& observed : compilerSite.reporterSsaBindings) {
        if (!observed.reporterPc ||
            !uniqueReporterValues.emplace(
                observed.reporterPc, observed.descriptor).second)
            return std::nullopt;
        const auto it = std::lower_bound(
            compilerManifest.descriptors.begin(),
            compilerManifest.descriptors.end(), observed.descriptor,
            [](uint64_t a, uint64_t b) {
                return uint32_t(a) < uint32_t(b);
            });
        if (it == compilerManifest.descriptors.end() ||
            *it != observed.descriptor)
            return std::nullopt;
        const size_t index = size_t(
            it - compilerManifest.descriptors.begin());
        const auto& location = roots->locations[index];
        if (location.descriptor != observed.descriptor)
            return std::nullopt;

        const Value boxed{detached.words[index]};
        const uint32_t descriptorKind =
            uint32_t(observed.descriptor >> 32);
        const uint32_t family = descriptorKind & ~uint32_t{255};
        const bool boxedHeapString =
            (descriptorKind & 255u) == 64 &&
            (family == 0 || family == 0x200) &&
            boxed.isString() && !boxed.isEmptyString();
        const bool verifiedRoot =
            boxedHeapString &&
            (location.kind == SourceSsaLocationKind::ProcedureArgument ||
             location.kind == SourceSsaLocationKind::NativeGeneratedString);
        if (location.kind == SourceSsaLocationKind::ProcedureArgument)
            ++result.mappedArguments;
        else if (location.kind ==
                 SourceSsaLocationKind::NativeGeneratedString)
            ++result.mappedGeneratedStrings;
        else
            ++result.unresolvedReporters;
        if (verifiedRoot)
            ++result.verifiedHeapStringObservations;
        result.reporters.push_back({
            observed.reporterPc, observed.descriptor, observed.origin,
            location.kind, location.argument, boxedHeapString, verifiedRoot
        });
    }
    return result;
}

} // namespace xyo::ir
