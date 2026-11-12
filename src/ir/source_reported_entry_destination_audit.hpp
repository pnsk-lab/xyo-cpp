// IR-001: read-only mapping from VERIFIED native reporter input slots
// to the real interpreter BlockCached parent ArgValues and ReportedEntry
// opcode ordering. Deliberately does not write a StackFrame or ArgValues.
// A captured LLVM reporter is NOT evidence that a fresh interpreter
// source continuation can skip its earlier sibling operations/effects.
#pragma once

#include "source_reporter_input_slot_audit.hpp"
#include "../model/blocks.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace xyo::ir {

struct SourceReportedEntryDestination {
    // Exact real BlockCached reporter ID used as ReportedEntry.opCachedId.
    std::string reporterBlock;
    std::string parentBlock;
    std::string parentInput;
    // Outermost currently cached linear execution owner, NOT a
    // suspended StackFrame or source resume PC.
    std::string containingStatement;
    uint64_t reporterPc = 0;
    uint32_t frozenInputOrdinal = 0;
    uint32_t parentOperationIndex = 0;
    uint32_t statementOperationIndex = 0;
    uint32_t statementOperationCount = 0;
    SourceReporterCapturedKind valueKind =
        SourceReporterCapturedKind::NonBoxedSsa;
    // Never retain GC pointers or fields of mutable ArgValues.
    std::optional<uint64_t> immediateBits;
};

struct SourceReportedEntryMissingOp {
    std::string statementBlock;
    std::string opCachedId;
    std::string opcode;
    uint32_t operationIndex = 0;
};

struct SourceReportedEntryDestinationAudit {
    uint64_t siteId = 0;
    uint32_t mapped = 0;
    uint32_t unavailable = 0;
    uint32_t rootlessValue = 0;
    uint32_t duplicatedSource = 0;
    uint32_t incompletePrefix = 0;
    // Only a diagnostics-only *contiguous subset* of the flattened _ops.
    // These are not committed ReportedEntry values, and missing effects,
    // frame identity, source phase or other machine registers still deny
    // native -> interpreter handoff.
    uint32_t contiguousReportedPrefix = 0;
    uint32_t statementsWithMissingPrefix = 0;
    std::vector<SourceReportedEntryDestination> destinations;
    // Explicitly enumerate missing previous _ops. An earlier reporter
    // could be pure yet its historical Value is no longer in native SSA.
    // Re-evaluating even a pure variable read can observe later writes.
    std::vector<SourceReportedEntryMissingOp> missingPrefixOps;
    // The only way to create a real reported prefix is to have a
    // complete interpreter StackFrame at the correct op and all
    // earlier flattened _ops/effect order. Compiler source labels
    // alone never supply it.
    bool hasInterpreterFrame() const { return false; }
    bool canReplayEffects() const { return false; }
    bool executable() const { return false; }
};

// Caller guarantees single-threaded block cache access (native Fiber
// checkpoint in scheduler thread, or a scheduler safe point).
// Cached BlockCached nodes may be lazily CONSTRUCTED. The audit performs
// no Scratch blockFunction calls, no Value writes and no fresh list read.
inline std::optional<SourceReportedEntryDestinationAudit>
auditReporterInterpreterDestinations(
        const SourceReporterCompiledSite& compiled,
        const SourceReporterInputSlotAudit& captured,
        Blocks& blocks) {
    if (!compiled.physicalSiteId ||
        compiled.physicalSiteId != captured.siteId ||
        !compiled.revision || blocks.revision() != compiled.revision ||
        captured.inputs.size() != compiled.reporters.size())
        return std::nullopt;
    SourceReportedEntryDestinationAudit audit;
    audit.siteId = compiled.physicalSiteId;
    std::set<std::pair<std::string, std::string>> usedInputs;
    audit.destinations.reserve(captured.inputs.size());
    constexpr size_t kMaxParentDepth = 32;
    for (size_t i = 0; i < captured.inputs.size(); ++i) {
        const auto& binding = compiled.reporters[i];
        const auto& input = captured.inputs[i];
        if (binding.reporterPc != input.reporterPc ||
            binding.reporterBlock != input.reporterBlock ||
            binding.parentBlock != input.parentBlock ||
            binding.inputName != input.parentInput ||
            binding.inputOrdinal != input.ordinal ||
            binding.descriptor != input.descriptor ||
            binding.selectedShadow != input.selectedShadow ||
            !input.ordinal || !input.uniquelyOwnedInput ||
            input.reporterBlock.empty() || input.parentBlock.empty() ||
            input.parentInput.empty()) {
            ++audit.unavailable;
            continue;
        }
        if (!usedInputs.emplace(
                input.parentBlock, input.parentInput).second) {
            ++audit.duplicatedSource;
            continue;
        }
        // We only support runtime-valued ordinary input edges.
        // Shadow literals are installed in ArgValues before execute(),
        // and BROADCAST_INPUT writes an IdName instead of a Value.
        if (input.selectedShadow || input.parentInput == "BROADCAST_INPUT" ||
            input.kind == SourceReporterCapturedKind::UnrootedHeap ||
            input.kind == SourceReporterCapturedKind::InvalidBoxed ||
            input.kind == SourceReporterCapturedKind::NonBoxedSsa ||
            (input.kind == SourceReporterCapturedKind::BoxedImmediate &&
             (!input.immediateBits ||
              !pointerFreeScratchImmediate(*input.immediateBits))) ||
            (input.kind == SourceReporterCapturedKind::RootedHeapObservation &&
             input.immediateBits)) {
            ++audit.rootlessValue;
            continue;
        }
        const Block* reporter = blocks.getBlock(input.reporterBlock);
        const Block* parent = blocks.getBlock(input.parentBlock);
        if (!reporter || !parent || reporter->parent != parent->id ||
            input.ordinal > parent->inputs.items.size() ||
            parent->inputs.items[input.ordinal - 1].first !=
                input.parentInput) {
            ++audit.unavailable;
            continue;
        }
        const auto& physicalInput =
            parent->inputs.items[input.ordinal - 1].second;
        if (physicalInput.block != input.reporterBlock) {
            ++audit.unavailable;
            continue;
        }
        BlockCached* cachedParent =
            blocks.getCachedExecute(input.parentBlock);
        BlockCached* cachedReporter =
            blocks.getCachedExecute(input.reporterBlock);
        if (!cachedParent || !cachedReporter ||
            !cachedReporter->definedBlockFunction ||
            cachedReporter->isShadowBlock ||
            cachedReporter->parentValues != &cachedParent->argValues ||
            cachedReporter->parentKey != input.parentInput ||
            !cachedReporter->parentValueSlot ||
            cachedParent->argValues.refs.contains(input.parentInput)) {
            ++audit.unavailable;
            continue;
        }
        // Reject stale/mismatched cached Value* and accidental map
        // insertion: NEVER call ArgValues::ref() in this audit.
        const auto parentValue =
            cachedParent->argValues.vals.find(input.parentInput);
        if (parentValue == cachedParent->argValues.vals.end() ||
            &parentValue->second != cachedReporter->parentValueSlot) {
            ++audit.unavailable;
            continue;
        }
        const auto parentOp = std::find(
            cachedParent->ops.begin(), cachedParent->ops.end(),
            cachedReporter);
        if (parentOp == cachedParent->ops.end() ||
            std::count(cachedParent->ops.begin(), cachedParent->ops.end(),
                       cachedReporter) != 1) {
            ++audit.unavailable;
            continue;
        }
        // Walk only genuine input parent edges. Scratch NEXT/SUBSTACK
        // ancestry is NOT a flattened parent reporter operation.
        BlockCached* statement = cachedParent;
        std::set<std::string> visited{input.reporterBlock};
        for (size_t depth = 0; depth < kMaxParentDepth; ++depth) {
            const Block* current = blocks.getBlock(statement->id);
            if (!current || current->parent.empty() ||
                !visited.insert(current->id).second)
                break;
            const Block* ancestor = blocks.getBlock(current->parent);
            if (!ancestor) break;
            const auto linked = std::find_if(
                ancestor->inputs.items.begin(),
                ancestor->inputs.items.end(),
                [&](const auto& kv) {
                    return kv.second.block == current->id &&
                           kv.first.rfind("SUBSTACK", 0) != 0;
                });
            if (linked == ancestor->inputs.items.end()) break;
            BlockCached* owner = blocks.getCachedExecute(ancestor->id);
            if (!owner ||
                std::find(owner->ops.begin(), owner->ops.end(),
                          cachedReporter) == owner->ops.end())
                break;
            statement = owner;
        }
        const auto statementOp = std::find(
            statement->ops.begin(), statement->ops.end(),
            cachedReporter);
        if (statementOp == statement->ops.end() ||
            std::count(statement->ops.begin(), statement->ops.end(),
                       cachedReporter) != 1 ||
            statement->ops.size() > UINT32_MAX) {
            ++audit.unavailable;
            continue;
        }
        SourceReportedEntryDestination destination;
        destination.reporterPc = input.reporterPc;
        destination.reporterBlock = input.reporterBlock;
        destination.parentBlock = input.parentBlock;
        destination.parentInput = input.parentInput;
        destination.containingStatement = statement->id;
        destination.frozenInputOrdinal = input.ordinal;
        destination.parentOperationIndex = uint32_t(
            parentOp - cachedParent->ops.begin());
        destination.statementOperationIndex = uint32_t(
            statementOp - statement->ops.begin());
        destination.statementOperationCount =
            uint32_t(statement->ops.size());
        destination.valueKind = input.kind;
        destination.immediateBits = input.immediateBits;
        // The saved value is one real operation, but the earlier
        // flattened operations have not been independently reconstructed
        // into an interpreter StackFrame. Count this missing prefix
        // instead of silently re-executing it.
        ++audit.mapped;
        audit.destinations.push_back(std::move(destination));
    }
    std::map<std::string, std::vector<uint32_t>> knownStatementOps;
    for (const auto& dest : audit.destinations)
        knownStatementOps[dest.containingStatement].push_back(
            dest.statementOperationIndex);
    for (auto& [statement, indices] : knownStatementOps) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()),
                      indices.end());
        uint32_t prefix = 0;
        for (const uint32_t pos : indices) {
            if (pos != prefix) break;
            ++prefix;
        }
        audit.contiguousReportedPrefix += prefix;
        if (prefix != indices.size()) {
            ++audit.statementsWithMissingPrefix;
            audit.incompletePrefix += uint32_t(indices.size() - prefix);
        }
        // Report every flattened operation before a captured reporter
        // which is missing from the detached SSA source snapshots.
        const BlockCached* cached = blocks.getCachedExecute(statement);
        if (!cached) return std::nullopt;
        for (const uint32_t position : indices) {
            for (uint32_t prior = 0; prior < position; ++prior) {
                if (std::binary_search(indices.begin(), indices.end(),
                                       prior))
                    continue;
                const auto* op = cached->ops[prior];
                if (!op) return std::nullopt;
                const auto duplicate = std::find_if(
                    audit.missingPrefixOps.begin(),
                    audit.missingPrefixOps.end(),
                    [&](const auto& item) {
                        return item.statementBlock == statement &&
                            item.operationIndex == prior;
                    });
                if (duplicate == audit.missingPrefixOps.end())
                    audit.missingPrefixOps.push_back(
                        {statement, op->id, op->opcode, prior});
            }
        }
    }
    return audit;
}

} // namespace xyo::ir
