// IR-001: exact FROZEN Scratch input slot -> detached LLVM reporter word.
// Does not evaluate Scratch, reconstruct an interpreter StackFrame, or
// retain unrooted GC-bearing NaN-box words.
#pragma once

#include "source_reporter_ssa_location_bridge.hpp"
#include "source_reporter_immediate.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace xyo::ir {

enum class SourceReporterCapturedKind : uint8_t {
    BoxedImmediate, RootedHeapObservation, UnrootedHeap, NonBoxedSsa,
    InvalidBoxed
};

struct SourceReporterCapturedInput {
    uint64_t reporterPc = 0;
    uint64_t descriptor = 0;
    std::string reporterBlock; // would key ReportedEntry.opCachedId
    std::string parentBlock;
    std::string parentInput;   // would key parent ArgValues
    uint32_t ordinal = 0;
    bool selectedShadow = false;
    SourceReporterCapturedKind kind = SourceReporterCapturedKind::NonBoxedSsa;
    // Only canonical, pointer-free Scratch immediate bits may be copied.
    // No SStr*, list item addresses or raw unverified i64 heap bits.
    std::optional<uint64_t> immediateBits;
    bool uniquelyOwnedInput = false;
};

struct SourceReporterInputSlotAudit {
    uint64_t siteId = 0;
    uint32_t immediateCount = 0;
    uint32_t rootedHeapObservations = 0;
    uint32_t unrootedHeapObservations = 0;
    uint32_t nonBoxedCount = 0;
    uint32_t invalidBoxedCount = 0;
    uint32_t ambiguousInputs = 0;
    std::vector<SourceReporterCapturedInput> inputs;
    // An evaluated reporter input is not by itself an interpreter
    // StackFrame/ReportedEntry or a replay-safe source continuation.
    bool sourceEnvironmentComplete() const { return false; }
    bool executable() const { return false; }
};


template<class Detached>
std::optional<SourceReporterInputSlotAudit> auditReporterInputSlots(
    const SourceReporterCompiledSite& compiled,
    const SourceReporterSsaLocationAudit& rootedAudit,
    const SourceTypedSsaManifest& manifest,
    const Detached& detached) {
    if (!manifest.valid() || !compiled.physicalSiteId ||
        compiled.physicalSiteId != manifest.siteId ||
        compiled.revision != manifest.revision ||
        compiled.digestLo != manifest.digestLo ||
        compiled.digestHi != manifest.digestHi ||
        compiled.sourcePc != manifest.resumePc ||
        compiled.physicalSiteId != rootedAudit.physicalSiteId ||
        compiled.sourcePc != rootedAudit.sourcePc ||
        compiled.reporters.size() != rootedAudit.reporters.size() ||
        manifest.descriptors.size() != detached.words.size())
        return std::nullopt;

    SourceReporterInputSlotAudit result;
    result.siteId = compiled.physicalSiteId;
    result.inputs.reserve(compiled.reporters.size());
    std::set<std::pair<std::string, std::string>> usedSlots;
    for (size_t n = 0; n < compiled.reporters.size(); ++n) {
        const auto& binding = compiled.reporters[n];
        const auto& location = rootedAudit.reporters[n];
        if (!binding.reporterPc || binding.reporterBlock.empty() ||
            binding.parentBlock.empty() || binding.inputName.empty() ||
            !binding.inputOrdinal ||
            binding.reporterPc != location.reporterPc ||
            binding.descriptor != location.descriptor ||
            binding.origin != location.origin)
            return std::nullopt;

        const auto it = std::lower_bound(
            manifest.descriptors.begin(), manifest.descriptors.end(),
            binding.descriptor,
            [](uint64_t a, uint64_t b) {
                return uint32_t(a) < uint32_t(b);
            });
        if (it == manifest.descriptors.end() ||
            *it != binding.descriptor)
            return std::nullopt;
        const size_t index = size_t(
            it - manifest.descriptors.begin());

        SourceReporterCapturedInput observation;
        observation.reporterPc = binding.reporterPc;
        observation.descriptor = binding.descriptor;
        observation.reporterBlock = binding.reporterBlock;
        observation.parentBlock = binding.parentBlock;
        observation.parentInput = binding.inputName;
        observation.ordinal = binding.inputOrdinal;
        observation.selectedShadow = binding.selectedShadow;
        observation.uniquelyOwnedInput = usedSlots.emplace(
            binding.parentBlock, binding.inputName).second;
        if (!observation.uniquelyOwnedInput)
            ++result.ambiguousInputs;

        const uint32_t upper = uint32_t(binding.descriptor >> 32);
        const uint32_t family = upper & ~uint32_t{255};
        const uint32_t width = upper & 255;
        if (width == 64 && (family == 0 || family == 0x200)) {
            const uint64_t bits = detached.words[index];
            if (pointerFreeScratchImmediate(bits)) {
                observation.kind = SourceReporterCapturedKind::BoxedImmediate;
                observation.immediateBits = bits;
                ++result.immediateCount;
            } else if ((bits >> 48) == (val::kTagStr >> 48)) {
                // A separately rooted CallFrame/generated string OR an
                // individually retained exact SOURCE input handle is proof.
                // Never dereference the pointer encoded in the SSA word.
                bool rooted = location.verifiedHeapStringRoot;
                if (!rooted && detached.reporterInputRoots &&
                    detached.reporterInputHandles.size() ==
                        manifest.descriptors.size()) {
                    const auto handle = detached.reporterInputHandles[index];
                    if (handle) {
                        const auto value =
                            detached.reporterInputRoots->resolve(handle);
                        rooted = value && value->bits == bits;
                    }
                }
                observation.kind = rooted
                    ? SourceReporterCapturedKind::RootedHeapObservation
                    : SourceReporterCapturedKind::UnrootedHeap;
                if (rooted) ++result.rootedHeapObservations;
                else ++result.unrootedHeapObservations;
            } else {
                observation.kind = SourceReporterCapturedKind::InvalidBoxed;
                ++result.invalidBoxedCount;
            }
        } else {
            observation.kind = SourceReporterCapturedKind::NonBoxedSsa;
            ++result.nonBoxedCount;
        }
        result.inputs.push_back(std::move(observation));
    }
    return result;
}

} // namespace xyo::ir
