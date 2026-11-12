// IR-001: real execution-time Scratch list INDEX/result evidence.
// Never re-evaluate a reporter (especially INDEX="random"), and never
// treat SList::storageEpoch as a list-content mutation generation.
#pragma once

#include "../core/value.hpp"
#include "../core/sstr.hpp"
#include "gc_value_handles.hpp"
#include "source_list_read_events.hpp"
#if defined(XYO_JIT)
#include "source_reporter_input_slot_audit.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace xyo::ir {

// The source checkpoint audit is LLVM/JIT-only; events remain common.
#if defined(XYO_JIT)
struct SourceListReadCheckpointAudit {
    uint32_t matchedReads = 0;
    uint32_t indexPositions = 0;
    uint32_t missingReads = 0;
    uint32_t ambiguousReads = 0;
    uint32_t mismatchedValues = 0;
    uint32_t rejectedReads = 0;
    uint32_t epochStableSinceObserver = 0;
    uint32_t epochChangedSinceObserver = 0;
    uint32_t epochUntracked = 0;
    std::vector<SourceListReadEvent> evidence;
    std::shared_ptr<GcValueHandles> roots;
    // Actual list contents/index can change with NO storageEpoch change.
    bool contentUnchangedProven() const { return false; }
    bool executable() const { return false; }
};

template<class Detached>
SourceListReadCheckpointAudit auditListReadsAtCheckpoint(
        const SourceReporterCompiledSite& compiled,
        const SourceTypedSsaManifest& manifest,
        const Detached& detached, const SourceListReadEvents& trace) {
    SourceListReadCheckpointAudit audit;
    if (!manifest.valid() || compiled.physicalSiteId != manifest.siteId ||
        compiled.sourcePc != manifest.resumePc ||
        compiled.revision != manifest.revision ||
        compiled.digestLo != manifest.digestLo ||
        compiled.digestHi != manifest.digestHi ||
        detached.words.size() != manifest.descriptors.size() ||
        trace.overflow) {
        ++audit.rejectedReads;
        return audit;
    }
    audit.roots = trace.roots;
    const uint64_t checkpointEpoch = sourceListContentEpoch();
    for (const auto& binding : compiled.reporters) {
        if (binding.origin != SourceReporterOrigin::ListRead ||
            binding.storage.opcode != "data_itemoflist")
            continue;
        const SourceListReadEvent* matching = nullptr;
        uint32_t count = 0;
        for (const auto& event : trace.pending) {
            if (event.reporterPc != binding.reporterPc) continue;
            if (++count == 1) matching = &event;
        }
        if (!count) {
            ++audit.missingReads;
            continue;
        }
        if (count != 1) {
            ++audit.ambiguousReads;
            continue;
        }
        if (!matching || matching->rejected ||
            matching->listId != binding.storage.id ||
            binding.storage.indexBlock.empty() ||
            !binding.inputOrdinal || matching->sequence == 0) {
            ++audit.rejectedReads;
            continue;
        }
        const auto it = std::lower_bound(
            manifest.descriptors.begin(), manifest.descriptors.end(),
            binding.descriptor,
            [](uint64_t a, uint64_t b) {
                return uint32_t(a) < uint32_t(b);
            });
        if (it == manifest.descriptors.end() ||
            *it != binding.descriptor) {
            ++audit.rejectedReads;
            continue;
        }
        const size_t idx = size_t(it - manifest.descriptors.begin());
        const uint64_t word = detached.words[idx];
        bool matches = matching->resultImmediate &&
                       *matching->resultImmediate == word;
        if (!matches && matching->resultRoot && trace.roots) {
            const auto rooted = trace.roots->resolve(matching->resultRoot);
            matches = rooted && rooted->bits == word;
        }
        if (!matches) {
            ++audit.mismatchedValues;
            continue;
        }
        ++audit.matchedReads;
        if (matching->numericPosition)
            ++audit.indexPositions;
        if (!matching->contentEpochAtReadStart ||
            !matching->contentEpochAfterObserver || !checkpointEpoch) {
            ++audit.epochUntracked;
        } else if (sourceListContentEpochValid(
                       matching->contentEpochAtReadStart,
                       matching->contentEpochAfterObserver) &&
                   sourceListContentEpochValid(
                       matching->contentEpochAfterObserver,
                       checkpointEpoch)) {
            ++audit.epochStableSinceObserver;
        } else {
            ++audit.epochChangedSinceObserver;
        }
        audit.evidence.push_back(*matching); // only safe values/handles
    }
    return audit;
}

// Verify a previously frozen event again at a scheduler safe point.
// Never reconstruct the event from the detached words or re-read a list.
template<class Detached>
bool verifyListReadEvidence(
        const SourceReporterCompiledSite& compiled,
        const SourceTypedSsaManifest& manifest,
        const Detached& detached,
        const SourceListReadCheckpointAudit& evidence) {
    if (!manifest.valid() || compiled.physicalSiteId != manifest.siteId ||
        compiled.sourcePc != manifest.resumePc ||
        detached.words.size() != manifest.descriptors.size() ||
        evidence.missingReads || evidence.ambiguousReads ||
        evidence.mismatchedValues || evidence.rejectedReads ||
        evidence.matchedReads != evidence.evidence.size())
        return false;
    for (const auto& event : evidence.evidence) {
        if (!event.reporterPc || !event.sequence || event.rejected ||
            event.listId.empty())
            return false;
        // A stale saved read must not survive ANY later observed list
        // mutation, even when all saved SSA words/GC handles still match.
        if (!sourceListContentEpochValid(
                event.contentEpochAtReadStart,
                event.contentEpochAfterObserver) ||
            !sourceListContentEpochValid(
                event.contentEpochAfterObserver, sourceListContentEpoch()))
            return false;
        auto reporter = std::find_if(compiled.reporters.begin(),
            compiled.reporters.end(), [&](const auto& b) {
                return b.reporterPc == event.reporterPc &&
                    b.origin == SourceReporterOrigin::ListRead &&
                    b.storage.opcode == "data_itemoflist" &&
                    !b.storage.indexBlock.empty() &&
                    b.storage.id == event.listId;
            });
        if (reporter == compiled.reporters.end()) return false;
        const auto word = std::lower_bound(
            manifest.descriptors.begin(), manifest.descriptors.end(),
            reporter->descriptor,
            [](uint64_t a, uint64_t b) {
                return uint32_t(a) < uint32_t(b);
            });
        if (word == manifest.descriptors.end() ||
            *word != reporter->descriptor)
            return false;
        const uint64_t bits = detached.words[
            size_t(word - manifest.descriptors.begin())];
        if (event.resultImmediate) {
            if (*event.resultImmediate != bits) return false;
        } else {
            if (!event.resultRoot || !evidence.roots) return false;
            const auto rooted = evidence.roots->resolve(event.resultRoot);
            if (!rooted || rooted->bits != bits) return false;
        }
    }
    return true;
}

#endif // XYO_JIT

} // namespace xyo::ir
