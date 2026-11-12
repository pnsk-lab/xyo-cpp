// IR-001: a *non-executable* ordered ReportedEntry source value plan.
// Historical values come exclusively from actual native evaluation
// observers, NEVER a re-read of Variable::value/SList or replayed op.
// The plan stores either canonical pointer-free Value bits or an exact
// GC root handle/provider. It never stores raw GC pointer bits.
#pragma once

#include "source_scalar_prefix_audit.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace xyo::ir {

struct SourceReportedPrefixValue {
    std::string statement;
    std::string reporterBlock;
    std::string parentBlock;
    std::string parentInput;
    std::string sourceOwnerId; // exact source VARIABLE or LIST field ID
    uint32_t operationIndex = 0;
    uint64_t reporterPc = 0;
    uint64_t evaluationSequence = 0;
    std::optional<uint64_t> immediateBits;
    RootedValueHandle root;
    std::shared_ptr<GcValueHandles> rootProvider;

    std::optional<Value> inspectValue() const {
        if (immediateBits) {
            if (root || !pointerFreeScratchImmediate(*immediateBits))
                return std::nullopt;
            return Value{*immediateBits};
        }
        if (!root || !rootProvider) return std::nullopt;
        const auto value = rootProvider->resolve(root);
        if (!value || !value->isString() || value->isEmptyString())
            return std::nullopt;
        return *value;
    }
};

struct SourceReportedPrefixPlan {
    uint64_t siteId = 0;
    uint64_t sourceRevision = 0;
    uint32_t capturedOperations = 0;
    uint32_t missingValues = 0;
    uint32_t unsupportedReporters = 0;
    uint32_t ambiguousValues = 0;
    uint32_t invalidRoots = 0;
    uint32_t orderViolations = 0;
    uint32_t gapBeforeLast = 0;
    std::vector<SourceReportedPrefixValue> values;
    // A plan does not have the correct interpreter StackFrame, return PC,
    // full source operations, all machine roots, or effect replay proof.
    bool hasSourceStackFrame() const { return false; }
    bool replaySafe() const { return false; }
    bool executable() const { return false; }
};

// Only the supported scalar (data_variable) and list item
// (data_itemoflist) native reads can be independently paired to
// read-time events. All other reporter types remain unavailable.
inline SourceReportedPrefixPlan planEvaluatedReportedPrefix(
        const SourceReportedEntryDestinationAudit& destinations,
        const SourceScalarPrefixAudit* scalar,
        const SourceListReadCheckpointAudit* list,
        Blocks& blocks) {
    SourceReportedPrefixPlan plan;
    plan.siteId = destinations.siteId;
    plan.sourceRevision = blocks.revision();
    if (!plan.siteId || !plan.sourceRevision) {
        ++plan.missingValues;
        return plan;
    }
    // Include both native-live reporters and compiler-dead historical
    // scalar reporters that the destination audit enumerated as gaps.
    std::map<std::pair<std::string, uint32_t>,
             SourceReportedPrefixValue> byPosition;
    for (const auto& destination : destinations.destinations) {
        const Block* block = blocks.getBlock(destination.reporterBlock);
        if (!block) { ++plan.missingValues; continue; }
        SourceReportedPrefixValue value;
        value.statement = destination.containingStatement;
        value.operationIndex = destination.statementOperationIndex;
        value.reporterBlock = destination.reporterBlock;
        value.parentBlock = destination.parentBlock;
        value.parentInput = destination.parentInput;
        value.reporterPc = destination.reporterPc;

        if (block->opcode == "data_variable") {
            if (!scalar) { ++plan.missingValues; continue; }
            const auto it = std::find_if(scalar->live.begin(),
                scalar->live.end(), [&](const SourceScalarLiveItem& e) {
                    return e.evaluated.reporterPc == value.reporterPc;
                });
            if (it == scalar->live.end()) {
                ++plan.missingValues;
                continue;
            }
            const auto field = block->fields.find("VARIABLE");
            if (field == block->fields.end() ||
                field->second.id != it->evaluated.variableId) {
                ++plan.missingValues;
                continue;
            }
            value.sourceOwnerId = it->evaluated.variableId;
            value.evaluationSequence = it->evaluated.sequence;
            value.immediateBits = it->evaluated.resultImmediate;
            value.root = it->evaluated.resultRoot;
            value.rootProvider = scalar->roots;
        } else if (block->opcode == "data_itemoflist") {
            if (!list) { ++plan.missingValues; continue; }
            const auto it = std::find_if(list->evidence.begin(),
                list->evidence.end(),
                [&](const SourceListReadEvent& e) {
                    return e.reporterPc == value.reporterPc;
                });
            if (it == list->evidence.end()) {
                ++plan.missingValues;
                continue;
            }
            const auto field = block->fields.find("LIST");
            if (field == block->fields.end() ||
                field->second.id != it->listId) {
                ++plan.missingValues;
                continue;
            }
            value.sourceOwnerId = it->listId;
            value.evaluationSequence = it->sequence;
            value.immediateBits = it->resultImmediate;
            value.root = it->resultRoot;
            value.rootProvider = list->roots;
        } else {
            ++plan.unsupportedReporters;
            continue;
        }
        const auto actualValue = value.inspectValue();
        bool agreesWithNativeSsa = false;
        if (actualValue &&
            destination.valueKind ==
                SourceReporterCapturedKind::BoxedImmediate)
            agreesWithNativeSsa = value.immediateBits &&
                destination.immediateBits &&
                *value.immediateBits == *destination.immediateBits;
        else if (actualValue &&
                 destination.valueKind ==
                     SourceReporterCapturedKind::RootedHeapObservation)
            agreesWithNativeSsa =
                !value.immediateBits && bool(value.root) &&
                bool(value.rootProvider) && actualValue->isString() &&
                !actualValue->isEmptyString();
        if (!agreesWithNativeSsa) {
            ++plan.invalidRoots;
            continue;
        }
        const auto key = std::make_pair(
            value.statement, value.operationIndex);
        if (!byPosition.emplace(key, std::move(value)).second)
            ++plan.ambiguousValues;
    }
    if (scalar) for (const auto& item : scalar->items) {
        // A dead SSA source cannot be reconstructed from the CURRENT
        // scalar variable; the event must still own an exact old Value.
        SourceReportedPrefixValue value;
        value.statement = item.missing.statementBlock;
        value.reporterBlock = item.missing.opCachedId;
        value.operationIndex = item.missing.operationIndex;
        value.reporterPc = item.evaluated.reporterPc;
        value.sourceOwnerId = item.evaluated.variableId;
        value.evaluationSequence = item.evaluated.sequence;
        value.immediateBits = item.evaluated.resultImmediate;
        value.root = item.evaluated.resultRoot;
        value.rootProvider = scalar->roots;
        const BlockCached* cache =
            blocks.getCachedExecute(value.reporterBlock);
        if (!cache || !cache->parentValues ||
            cache->parentKey.empty() || !cache->parentValueSlot ||
            !value.inspectValue()) {
            ++plan.invalidRoots;
            continue;
        }
        value.parentInput = cache->parentKey;
        const Block* src = blocks.getBlock(value.reporterBlock);
        if (!src || src->opcode != "data_variable") {
            ++plan.missingValues;
            continue;
        }
        value.parentBlock = src->parent;
        const auto key = std::make_pair(
            value.statement, value.operationIndex);
        if (!byPosition.emplace(key, std::move(value)).second)
            ++plan.ambiguousValues;
    }
    // Inspect each statement in ACTUAL flattened source _ops order,
    // never event order alone. Any unrepresented earlier operation
    // makes the later input unusable as a contiguous reported prefix.
    std::map<std::string, std::vector<SourceReportedPrefixValue>>
        perStatement;
    for (auto& [key, entry] : byPosition)
        perStatement[key.first].push_back(std::move(entry));
    for (auto& [statement, entries] : perStatement) {
        const auto* cache = blocks.getCachedExecute(statement);
        if (!cache) { ++plan.missingValues; continue; }
        uint32_t prefix = 0;
        uint64_t previousSequence = 0;
        for (const auto& entry : entries) {
            if (entry.operationIndex >= cache->ops.size() ||
                !cache->ops[entry.operationIndex] ||
                cache->ops[entry.operationIndex]->id !=
                    entry.reporterBlock) {
                ++plan.missingValues;
                break;
            }
            if (entry.operationIndex != prefix) {
                ++plan.gapBeforeLast;
                break;
            }
            if (!entry.evaluationSequence ||
                entry.evaluationSequence <= previousSequence) {
                ++plan.orderViolations;
                break;
            }
            previousSequence = entry.evaluationSequence;
            ++prefix;
            plan.values.push_back(entry);
            ++plan.capturedOperations;
        }
    }
    return plan;
}

inline bool verifyEvaluatedReportedPrefix(
        const SourceReportedPrefixPlan& saved,
        const SourceReportedEntryDestinationAudit& destinations,
        Blocks& blocks) {
    if (!saved.siteId || saved.siteId != destinations.siteId ||
        !saved.sourceRevision ||
        saved.sourceRevision != blocks.revision() ||
        !saved.capturedOperations ||
        saved.capturedOperations != saved.values.size() ||
        saved.invalidRoots || saved.ambiguousValues ||
        saved.orderViolations || saved.gapBeforeLast ||
        saved.missingValues)
        return false;
    std::map<std::string, uint64_t> previousSequence;
    std::map<std::string, uint32_t> nextIndex;
    for (const auto& entry : saved.values) {
        // Reject a candidate that was never present in the VERIFIED
        // compiled/native source destinations nor in the explicit
        // historically missing prefix. Merely matching an op ID is
        // insufficient evidence of a prior native evaluation.
        const auto live = std::find_if(
            destinations.destinations.begin(),
            destinations.destinations.end(),
            [&](const auto& destination) {
                return destination.containingStatement == entry.statement &&
                    destination.statementOperationIndex ==
                        entry.operationIndex &&
                    destination.reporterBlock == entry.reporterBlock &&
                    destination.reporterPc == entry.reporterPc &&
                    destination.parentBlock == entry.parentBlock &&
                    destination.parentInput == entry.parentInput;
            });
        if (live == destinations.destinations.end()) {
            const auto historical = std::find_if(
                destinations.missingPrefixOps.begin(),
                destinations.missingPrefixOps.end(),
                [&](const auto& missing) {
                    return missing.statementBlock == entry.statement &&
                        missing.operationIndex == entry.operationIndex &&
                        missing.opCachedId == entry.reporterBlock &&
                        missing.opcode == "data_variable";
                });
            if (historical == destinations.missingPrefixOps.end())
                return false;
        } else if (live->valueKind ==
                       SourceReporterCapturedKind::BoxedImmediate) {
            if (!entry.immediateBits || !live->immediateBits ||
                *entry.immediateBits != *live->immediateBits ||
                bool(entry.root)) return false;
        } else if (live->valueKind ==
                       SourceReporterCapturedKind::RootedHeapObservation) {
            if (entry.immediateBits || !entry.root ||
                !entry.rootProvider) return false;
        } else {
            return false;
        }
        const Block* source = blocks.getBlock(entry.reporterBlock);
        const Block* parent = blocks.getBlock(entry.parentBlock);
        if (!source || !parent || entry.sourceOwnerId.empty() ||
            source->parent != entry.parentBlock)
            return false;
        const BlockInput* parentInput =
            parent->inputs.find(entry.parentInput);
        if (!parentInput ||
            parentInput->block != entry.reporterBlock)
            return false;
        const char* ownerField = nullptr;
        if (source->opcode == "data_variable")
            ownerField = "VARIABLE";
        else if (source->opcode == "data_itemoflist")
            ownerField = "LIST";
        if (!ownerField) return false;
        const auto field = source->fields.find(ownerField);
        if (field == source->fields.end() ||
            field->second.id != entry.sourceOwnerId)
            return false;
        const auto* cache = blocks.getCachedExecute(entry.statement);
        if (!cache || entry.operationIndex >= cache->ops.size() ||
            !cache->ops[entry.operationIndex] ||
            cache->ops[entry.operationIndex]->id != entry.reporterBlock ||
            entry.operationIndex != nextIndex[entry.statement] ||
            entry.evaluationSequence <=
                previousSequence[entry.statement] ||
            !entry.inspectValue())
            return false;
        previousSequence[entry.statement] = entry.evaluationSequence;
        ++nextIndex[entry.statement];
    }
    // This validates only the previously staged source values/ordering;
    // a resumed interpreter StackFrame is still completely unproven.
    return true;
}

} // namespace xyo::ir
