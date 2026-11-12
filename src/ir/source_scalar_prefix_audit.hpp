// IR-001: historical data_variable values for missing ReportedEntry
// prefix operations. Strictly DIAGNOSTIC; does not write ArgValues or
// authorize native Fiber -> interpreter adoption.
#pragma once

#include "source_reported_entry_destination_audit.hpp"
#include "source_scalar_read_events.hpp"
#include "source_list_read_index_trace.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xyo::ir {

struct SourceScalarPrefixItem {
    SourceReportedEntryMissingOp missing;
    SourceScalarReadEvent evaluated;
};

struct SourceScalarLiveItem {
    SourceScalarReadEvent evaluated;
    uint64_t descriptor = 0;
};

struct SourceScalarPrefixAudit {
    // Dead SSA reporters recovered from historical evaluation events.
    uint32_t matched = 0;
    uint32_t missing = 0;
    uint32_t ambiguous = 0;
    uint32_t mismatchedOwner = 0;
    uint32_t rejected = 0;
    uint32_t orderUnproven = 0;
    // Still-live scalar SSA checked against INDEPENDENT read-time values.
    uint32_t matchedLive = 0;
    uint32_t missingLive = 0;
    uint32_t ambiguousLive = 0;
    uint32_t invalidLive = 0;
    std::vector<SourceScalarLiveItem> live;
    std::vector<SourceScalarPrefixItem> items;
    std::shared_ptr<GcValueHandles> roots;
    bool sourceEnvironmentComplete() const { return false; }
    bool executable() const { return false; }
};

inline std::optional<Value> resolveScalarObservedValue(
        const SourceScalarReadEvent& event,
        const SourceScalarPrefixAudit& audit) {
    if (event.rejected) return std::nullopt;
    if (event.resultImmediate &&
        pointerFreeScratchImmediate(*event.resultImmediate))
        return Value{*event.resultImmediate};
    if (!event.resultRoot || !audit.roots) return std::nullopt;
    const auto rooted = audit.roots->resolve(event.resultRoot);
    if (!rooted || !rooted->isString() || rooted->isEmptyString())
        return std::nullopt;
    return *rooted;
}

inline std::optional<Value> resolveScalarPrefixItem(
        const SourceScalarPrefixItem& item,
        const SourceScalarPrefixAudit& audit) {
    return resolveScalarObservedValue(item.evaluated, audit);
}

// Compare all live compiler-labelled scalar SSA against the independent
// actual read event. The SSA itself is never used to synthesize history.
// The detached capture owns exact GC handles for the whole observation.
template<class Detached, class SourceIdAtPc>
void auditLiveScalarReadWords(
        SourceScalarPrefixAudit& audit,
        const SourceReporterCompiledSite& compiled,
        const SourceTypedSsaManifest& manifest,
        const Detached& detached, const SourceScalarReadEvents& events,
        const Blocks& blocks, SourceIdAtPc&& sourceIdAtPc) {
    if (!manifest.valid() || manifest.siteId != compiled.physicalSiteId ||
        manifest.revision != compiled.revision ||
        manifest.descriptors.size() != detached.words.size() ||
        compiled.digestLo != manifest.digestLo ||
        compiled.digestHi != manifest.digestHi ||
        events.overflow) {
        ++audit.invalidLive;
        return;
    }
    for (const auto& binding : compiled.reporters) {
        if (binding.origin != SourceReporterOrigin::VariableRead ||
            binding.storage.kind != SourceReporterStorageKind::ScalarVariable)
            continue;
        const auto* source = blocks.getBlock(binding.reporterBlock);
        if (!source || source->opcode != "data_variable" ||
            binding.storage.id.empty()) {
            ++audit.invalidLive;
            continue;
        }
        const SourceScalarReadEvent* selected = nullptr;
        unsigned count = 0;
        for (const auto& event : events.pending)
            if (event.reporterPc == binding.reporterPc &&
                sourceIdAtPc(event.reporterPc) == binding.reporterBlock) {
                if (++count == 1) selected = &event;
            }
        if (count == 0) {
            ++audit.missingLive;
            continue;
        }
        if (count != 1 || !selected) {
            ++audit.ambiguousLive;
            continue;
        }
        const auto srcField = source->fields.find("VARIABLE");
        const auto word = std::lower_bound(
            manifest.descriptors.begin(), manifest.descriptors.end(),
            binding.descriptor,
            [](uint64_t a, uint64_t b) {
                return uint32_t(a) < uint32_t(b);
            });
        if (srcField == source->fields.end() ||
            srcField->second.id != binding.storage.id ||
            selected->variableId != binding.storage.id ||
            selected->rejected ||
            word == manifest.descriptors.end() ||
            *word != binding.descriptor) {
            ++audit.invalidLive;
            continue;
        }
        const auto value = resolveScalarObservedValue(*selected, audit);
        if (!value || value->bits !=
                detached.words[size_t(word - manifest.descriptors.begin())]) {
            ++audit.invalidLive;
            continue;
        }
        ++audit.matchedLive;
        audit.live.push_back({*selected, binding.descriptor});
    }
}

template<class Detached, class SourceIdAtPc>
bool verifyLiveScalarReadWords(
        const SourceScalarPrefixAudit& audit,
        const SourceReporterCompiledSite& compiled,
        const SourceTypedSsaManifest& manifest,
        const Detached& detached,
        const Blocks& blocks, SourceIdAtPc&& sourceIdAtPc) {
    if (!manifest.valid() || audit.matchedLive != audit.live.size() ||
        audit.missingLive || audit.ambiguousLive || audit.invalidLive ||
        compiled.physicalSiteId != manifest.siteId ||
        manifest.revision != compiled.revision)
        return false;
    for (const auto& entry : audit.live) {
        const auto it = std::find_if(
            compiled.reporters.begin(), compiled.reporters.end(),
            [&](const auto& binding) {
                return binding.descriptor == entry.descriptor &&
                       binding.reporterPc == entry.evaluated.reporterPc &&
                       binding.storage.id == entry.evaluated.variableId &&
                       binding.origin == SourceReporterOrigin::VariableRead;
            });
        if (it == compiled.reporters.end() ||
            sourceIdAtPc(entry.evaluated.reporterPc) != it->reporterBlock)
            return false;
        const auto* block = blocks.getBlock(it->reporterBlock);
        if (!block || block->opcode != "data_variable") return false;
        const auto field = block->fields.find("VARIABLE");
        if (field == block->fields.end() ||
            field->second.id != entry.evaluated.variableId)
            return false;
        const auto word = std::lower_bound(
            manifest.descriptors.begin(), manifest.descriptors.end(),
            it->descriptor,
            [](uint64_t a, uint64_t b) {
                return uint32_t(a) < uint32_t(b);
            });
        if (word == manifest.descriptors.end() ||
            *word != it->descriptor)
            return false;
        const auto value = resolveScalarObservedValue(entry.evaluated, audit);
        if (!value ||
            value->bits != detached.words[
                size_t(word - manifest.descriptors.begin())])
            return false;
    }
    return true;
}

// The caller supplies an independently compiler-frozen source PC -> block
// identifier lookup (SourceFrameSymbols::block), NOT live source lookup.
// The cached interpreter destination audit proved the real _ops topology;
// the matching original value comes only from the actual execution event.
template<class SourceIdAtPc>
SourceScalarPrefixAudit auditHistoricalScalarPrefix(
        const SourceReportedEntryDestinationAudit& destinations,
        const SourceScalarReadEvents& readEvents,
        const SourceListReadCheckpointAudit* listEvents,
        const Blocks& source, uint64_t sourceRevision,
        SourceIdAtPc&& sourceIdAtPc) {
    SourceScalarPrefixAudit audit;
    audit.roots = readEvents.roots;
    if (!sourceRevision || source.revision() != sourceRevision ||
        readEvents.overflow) {
        ++audit.rejected;
        return audit;
    }
    for (const auto& missing : destinations.missingPrefixOps) {
        if (missing.opcode != "data_variable") {
            ++audit.rejected;
            continue;
        }
        const Block* block = source.getBlock(missing.opCachedId);
        if (!block || block->opcode != "data_variable") {
            ++audit.mismatchedOwner;
            continue;
        }
        const auto field = block->fields.find("VARIABLE");
        if (field == block->fields.end() || field->second.id.empty()) {
            ++audit.mismatchedOwner;
            continue;
        }
        const SourceScalarReadEvent* selected = nullptr;
        uint32_t found = 0;
        for (const auto& event : readEvents.pending) {
            if (sourceIdAtPc(event.reporterPc) != missing.opCachedId)
                continue;
            if (++found == 1) selected = &event;
        }
        if (found == 0) {
            ++audit.missing;
            continue;
        }
        if (found != 1) {
            ++audit.ambiguous;
            continue;
        }
        if (!selected || selected->rejected || !selected->sequence ||
            selected->variableId != field->second.id) {
            ++audit.mismatchedOwner;
            continue;
        }
        SourceScalarPrefixItem item{missing, *selected};
        // Lexical order: a missing reporter must have been evaluated
        // before the next captured reporter in the SAME statement.
        // We can currently prove that ordering only against an ACTUAL
        // retained list-read event carrying the common Thread sequence.
        bool ordered = false;
        if (listEvents) {
            for (const auto& dest : destinations.destinations) {
                if (dest.containingStatement != missing.statementBlock ||
                    dest.statementOperationIndex <= missing.operationIndex)
                    continue;
                for (const auto& later : listEvents->evidence) {
                    if (later.reporterPc == dest.reporterPc &&
                        later.sequence > selected->sequence &&
                        later.sequence != 0 && !later.rejected) {
                        ordered = true;
                        break;
                    }
                }
                if (ordered) break;
            }
        }
        if (!ordered) {
            ++audit.orderUnproven;
            continue;
        }
        if (!resolveScalarPrefixItem(item, audit)) {
            ++audit.rejected;
            continue;
        }
        ++audit.matched;
        audit.items.push_back(std::move(item));
    }
    return audit;
}

template<class SourceIdAtPc>
bool verifyHistoricalScalarPrefix(
        const SourceScalarPrefixAudit& saved,
        const SourceReportedEntryDestinationAudit& destinations,
        const SourceListReadCheckpointAudit* listEvents,
        const Blocks& source, uint64_t sourceRevision,
        SourceIdAtPc&& sourceIdAtPc) {
    if (!sourceRevision || source.revision() != sourceRevision ||
        saved.matched != saved.items.size() || saved.missing ||
        saved.ambiguous || saved.mismatchedOwner || saved.rejected ||
        saved.orderUnproven ||
        saved.items.size() != destinations.missingPrefixOps.size())
        return false;
    for (const auto& item : saved.items) {
        const auto& missing = item.missing;
        if (std::find_if(destinations.missingPrefixOps.begin(),
                destinations.missingPrefixOps.end(),
                [&](const auto& candidate) {
                    return candidate.statementBlock == missing.statementBlock &&
                           candidate.opCachedId == missing.opCachedId &&
                           candidate.operationIndex == missing.operationIndex &&
                           candidate.opcode == missing.opcode;
                }) == destinations.missingPrefixOps.end())
            return false;
        if (sourceIdAtPc(item.evaluated.reporterPc) != missing.opCachedId)
            return false;
        const auto* block = source.getBlock(missing.opCachedId);
        if (!block || block->opcode != "data_variable") return false;
        const auto* field = [&]() -> const BlockField* {
            const auto it = block->fields.find("VARIABLE");
            return it == block->fields.end() ? nullptr : &it->second;
        }();
        if (!field || field->id != item.evaluated.variableId ||
            !resolveScalarPrefixItem(item, saved))
            return false;
        bool ordered = false;
        if (listEvents) for (const auto& dest : destinations.destinations) {
            if (dest.containingStatement != missing.statementBlock ||
                dest.statementOperationIndex <= missing.operationIndex)
                continue;
            for (const auto& later : listEvents->evidence) {
                if (later.reporterPc == dest.reporterPc &&
                    later.sequence > item.evaluated.sequence &&
                    !later.rejected)
                    ordered = true;
            }
        }
        if (!ordered) return false;
    }
    return true;
}

} // namespace xyo::ir
