// IR-001: opt-in compiler-only provenance observation of Scratch
// reporter inputs actually emitted by the production LLVM codegen.
//
// Returned LLVM Value* identities are tracked weakly across inlining /
// deletion. These are observation records, NOT recovery locations:
// virtually all Scratch reporter values are NaN-boxed i64, not raw
// numeric SSA, and may contain GC-managed pointers.
#pragma once

#include "llvm_source_numeric_bindings.hpp"
#include "scratch_script.hpp"
#include "source_resume_phase.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace xyo::ir {

// Validate the EXACT source input edge in the separately captured
// ScratchCompilationUnit, never a mutable live Blocks map. A reporter
// may be a generated shadow fallback, but its ID must belong to the
// frozen source script and be referenced by this parent input.
inline bool frozenSourceReporterInputEdge(
    const ScratchScript& frozen, const std::string& parentId,
    const std::string& inputName, const std::string& childId) {
    if (!frozen.valid() || parentId.empty() ||
        inputName.empty() || childId.empty())
        return false;
    const Block* parent = frozen.getBlock(parentId);
    const Block* child = frozen.getBlock(childId);
    if (!parent || !child) return false;
    const BlockInput* input = parent->inputs.find(inputName);
    if (!input) return false;
    const std::string& selected = !input->block.empty()
        ? input->block : input->shadow;
    return selected == childId &&
           (child->parent.empty() || child->parent == parentId);
}

// Exact compiler source input slot, including fallback to a shadow.
// A slot is a lexical destination, NOT an executable interpreter frame.
struct FrozenReporterInputSlot {
    uint32_t ordinal = 0; // 1-based InputSet::items order
    bool selectedShadow = false;
};

inline std::optional<FrozenReporterInputSlot> frozenReporterInputSlot(
    const ScratchScript& script, const std::string& parentId,
    const std::string& inputName, const std::string& reporterId) {
    if (!frozenSourceReporterInputEdge(
            script, parentId, inputName, reporterId))
        return std::nullopt;
    const Block* parent = script.getBlock(parentId);
    if (!parent || parent->inputs.items.size() > UINT32_MAX)
        return std::nullopt;
    for (size_t i = 0; i < parent->inputs.items.size(); ++i) {
        const auto& [name, input] = parent->inputs.items[i];
        if (name != inputName) continue;
        return FrozenReporterInputSlot{
            uint32_t(i + 1), input.block.empty() &&
                             input.shadow == reporterId
        };
    }
    return std::nullopt;
}

// The OPCODE of the immutable Scratch block is a source-origin hint,
// never an SSA recovery certificate. A variable/list read may yield a
 // NaN-boxed heap reference even if LLVM stores it in a 64-bit integer.
enum class SourceReporterOrigin : uint8_t {
    Unknown,
    VariableRead,
    ListRead,
    ProcedureArgument,
    ComputedExpression,
    Literal,
    OtherReporter,
};

inline SourceReporterOrigin classifyFrozenReporter(
    const ScratchScript& frozen, const std::string& childId) {
    const Block* child = frozen.getBlock(childId);
    if (!frozen.valid() || !child) return SourceReporterOrigin::Unknown;
    const std::string& op = child->opcode;
    if (op == "data_variable") return SourceReporterOrigin::VariableRead;
    if (op == "data_itemoflist" || op == "data_itemnumoflist" ||
        op == "data_lengthoflist" || op == "data_listcontainsitem" ||
        op == "data_listcontents")
        return SourceReporterOrigin::ListRead;
    if (op == "argument_reporter_string_number" ||
        op == "argument_reporter_boolean")
        return SourceReporterOrigin::ProcedureArgument;
    if (op.starts_with("operator_"))
        return SourceReporterOrigin::ComputedExpression;
    if (op == "math_number" || op == "math_integer" ||
        op == "math_whole_number" || op == "math_positive_number" ||
        op == "math_angle" || op == "text" || op == "colour_picker")
        return SourceReporterOrigin::Literal;
    return SourceReporterOrigin::OtherReporter;
}

constexpr const char* sourceReporterOriginName(SourceReporterOrigin origin) {
    switch (origin) {
    case SourceReporterOrigin::VariableRead: return "variable_read";
    case SourceReporterOrigin::ListRead: return "list_read";
    case SourceReporterOrigin::ProcedureArgument: return "procedure_argument";
    case SourceReporterOrigin::ComputedExpression: return "expression";
    case SourceReporterOrigin::Literal: return "literal";
    case SourceReporterOrigin::OtherReporter: return "other_reporter";
    case SourceReporterOrigin::Unknown: return "unknown";
    }
    return "unknown";
}

// Stable Scratch source owner identity, never a restored SSA value
// destination. A list read may be stale after its owner mutates.
enum class SourceReporterStorageKind : uint8_t {
    Unresolved, ScalarVariable, ListVariable, ProcedureArgument,
    ExpressionTemporary, Literal, OtherReporter,
};

constexpr const char* sourceReporterStorageName(
        SourceReporterStorageKind kind) {
    switch (kind) {
    case SourceReporterStorageKind::ScalarVariable: return "scalar_owner";
    case SourceReporterStorageKind::ListVariable: return "list_owner";
    case SourceReporterStorageKind::ProcedureArgument: return "procedure_arg";
    case SourceReporterStorageKind::ExpressionTemporary: return "temporary";
    case SourceReporterStorageKind::Literal: return "literal";
    case SourceReporterStorageKind::OtherReporter: return "other_reporter";
    case SourceReporterStorageKind::Unresolved: return "unresolved";
    }
    return "unresolved";
}

struct SourceReporterStorageSource {
    SourceReporterStorageKind kind = SourceReporterStorageKind::Unresolved;
    std::string id;
    std::string opcode;
    // The exact source reporter feeding a list's INDEX input.
    // Empty means absent/unproven. A runtime INDEX must never be
    // reconstructed by re-evaluating this Scratch child block.
    std::string indexBlock;
    bool operator==(const SourceReporterStorageSource&) const = default;
};

inline SourceReporterStorageSource frozenSourceReporterStorage(
        const ScratchScript& frozen, const std::string& blockId) {
    const Block* block = frozen.getBlock(blockId);
    if (!frozen.valid() || !block) return {};
    SourceReporterStorageSource owner;
    owner.opcode = block->opcode;
    const auto kind = classifyFrozenReporter(frozen, blockId);
    if (kind == SourceReporterOrigin::VariableRead ||
        kind == SourceReporterOrigin::ListRead) {
        const bool scalar = kind == SourceReporterOrigin::VariableRead;
        const auto it = block->fields.find(scalar ? "VARIABLE" : "LIST");
        if (it == block->fields.end() || it->second.id.empty())
            return owner;
        owner.kind = scalar ? SourceReporterStorageKind::ScalarVariable
                            : SourceReporterStorageKind::ListVariable;
        owner.id = it->second.id; // no GC-managed field Value
        if (!scalar && block->opcode == "data_itemoflist") {
            const BlockInput* index = block->inputs.find("INDEX");
            if (index) {
                const std::string& selected = index->block.empty()
                    ? index->shadow : index->block;
                if (!selected.empty() &&
                    frozenSourceReporterInputEdge(
                        frozen, blockId, "INDEX", selected))
                    owner.indexBlock = selected;
            }
        }
    } else if (kind == SourceReporterOrigin::ProcedureArgument) {
        owner.kind = SourceReporterStorageKind::ProcedureArgument;
    } else if (kind == SourceReporterOrigin::ComputedExpression) {
        owner.kind = SourceReporterStorageKind::ExpressionTemporary;
    } else if (kind == SourceReporterOrigin::Literal) {
        owner.kind = SourceReporterStorageKind::Literal;
    } else if (kind == SourceReporterOrigin::OtherReporter) {
        owner.kind = SourceReporterStorageKind::OtherReporter;
    }
    return owner;
}

struct SourceReporterValueObservation {
    uint64_t sourcePc = 0; // frozen, sorted Scratch Block ID dictionary
    std::string sourceBlock;
    std::string parentBlock;
    std::string inputName;
    llvm::WeakTrackingVH value; // safe if LLVM deletes/RAUW the instruction
    SourceReporterOrigin origin = SourceReporterOrigin::Unknown;
    SourceReporterStorageSource storage;
    uint32_t inputOrdinal = 0;
    bool selectedShadow = false;
};

// Source loop ID, phase and the EXACT emitted LLVM checkpoint are
// tied together by Codegen while the original source is still frozen.
struct SourceReporterCheckpointObservation {
    uint64_t sourcePc = 0;
    std::string sourceLoopBlock;
    SourceResumePhase phase = SourceResumePhase::UnknownBody;
    llvm::WeakTrackingVH marker;
    // Actual path.size() at compiler emission. Not proof the runtime
    // CallFrame retained the loop counters or GC root set.
    uint32_t portableLoopPathDepth = 0;
    std::vector<std::string> sourceLoopPath;
};

// Compiler-only mapping to the existing RT-001 numeric SSA descriptor
// space (id + bit width). The runtime capture and rooted source location
// must be validated separately; this record is NOT a root or frame slot.
struct SourceReporterLiveSsaBinding {
    uint64_t reporterPc = 0;
    uint64_t descriptor = 0;
    SourceReporterOrigin origin = SourceReporterOrigin::Unknown;
    std::string reporterBlock;
    std::string parentBlock;
    std::string inputName;
    SourceReporterStorageSource storage;
    uint32_t inputOrdinal = 0;
    bool selectedShadow = false;
    bool operator==(const SourceReporterLiveSsaBinding&) const = default;
};

// Diagnostic obligation, never an executable source restoration plan.
struct SourceReporterCheckpointGap {
    uint64_t sourcePc = 0;
    // 1-based physical CallBase ordinal across the compiled module,
    // independent of source PC and of whether RT capture is possible.
    uint64_t physicalSiteId = 0;
    std::string sourceLoopBlock;
    SourceResumePhase phase = SourceResumePhase::UnknownBody;
    uint32_t liveReporterObservations = 0;
    uint32_t boxedWithoutGcProof = 0;
    uint32_t numericWithoutSourceSlot = 0;
    uint32_t unmappedSourceSlots = 0;
    uint32_t unsupportedLlvmValues = 0;
    uint32_t variableReads = 0;
    uint32_t listReads = 0;
    uint32_t procedureArguments = 0;
    uint32_t computedExpressions = 0;
    uint32_t literals = 0;
    uint32_t otherReporters = 0;
    uint32_t portableLoopPathDepth = 0;
    std::vector<std::string> sourceLoopPath;
    std::vector<SourceReporterLiveSsaBinding> reporterSsaBindings;
    bool loopPathRestorable() const { return false; }
    bool sourceEnvironmentComplete() const { return false; }
    bool executable() const { return false; }
};

struct SourceReporterValueSummary {
    std::vector<SourceReporterCheckpointGap> checkpointGaps;
    uint64_t attributedCheckpoints = 0;
    uint64_t unmatchedSourceSites = 0;
    uint64_t observed = 0;
    uint64_t remaining = 0;
    uint64_t discarded = 0;
    uint64_t constants = 0;
    uint64_t rawNumeric = 0;
    uint64_t boxedOrUnknown = 0;
    uint64_t liveAtCheckpoint = 0;
    uint64_t checkpointSites = 0;
    bool sourceEnvironmentComplete() const { return false; }
    bool executable() const { return false; }
};

// Run AFTER the compiler's regular inlining and IR rewrites but BEFORE
// runtime native code emission. No LLVM IR mutation or runtime side effect.
inline SourceReporterValueSummary traceSourceReporterValues(
    llvm::Module& module,
    const std::vector<SourceReporterValueObservation>& observations,
    bool emitLog = false,
    const std::vector<SourceReporterCheckpointObservation>& sourceSites = {}) {
    SourceReporterValueSummary summary;
    std::vector<std::pair<const llvm::CallBase*, LlvmCheckpointLiveness>>
        checkpoints;
    for (llvm::Function& function : module) {
        if (function.isDeclaration()) continue;
        for (llvm::BasicBlock& block : function) {
            for (llvm::Instruction& instruction : block) {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call || !call->getCalledFunction() ||
                    call->getCalledFunction()->getName() !=
                        "xyo_proc_frame_checkpoint_path_phase")
                    continue;
                checkpoints.emplace_back(call,
                    analyzeLlvmCheckpointLiveness(function, *call));
                ++summary.checkpointSites;
            }
        }
    }

    std::unordered_set<const llvm::CallBase*> usedSourceSites;
    for (const auto& site : sourceSites) {
        llvm::Value* markerValue = site.marker;
        auto* call = llvm::dyn_cast_or_null<llvm::CallBase>(markerValue);
        const auto* emittedPhase = call && call->arg_size() == 5
            ? llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(4))
            : nullptr;
        const auto* emittedDepth = call && call->arg_size() == 5
            ? llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(3))
            : nullptr;
        if (!call || !site.sourcePc || site.sourceLoopBlock.empty() ||
            !validSourceResumePhase(uint64_t(site.phase)) ||
            !emittedPhase ||
            emittedPhase->getZExtValue() != uint64_t(site.phase) ||
            !emittedDepth || site.portableLoopPathDepth == 0 ||
            site.portableLoopPathDepth > 32 ||
            emittedDepth->getZExtValue() != site.portableLoopPathDepth ||
            site.sourceLoopPath.size() != site.portableLoopPathDepth ||
            site.sourceLoopPath.back() != site.sourceLoopBlock ||
            std::any_of(site.sourceLoopPath.begin(),
                        site.sourceLoopPath.end(),
                        [](const std::string& id) { return id.empty(); }) ||
            std::unordered_set<std::string>(
                site.sourceLoopPath.begin(),
                site.sourceLoopPath.end()).size() !=
                site.sourceLoopPath.size() ||
            !usedSourceSites.insert(call).second ||
            !call->getCalledFunction() ||
            call->getCalledFunction()->getName() !=
                "xyo_proc_frame_checkpoint_path_phase") {
            ++summary.unmatchedSourceSites;
            continue;
        }
        // A source PC belongs to the exact physical CallBase, not
        // merely another checkpoint in the same LLVM Function.
        const auto matched = std::find_if(
            checkpoints.begin(), checkpoints.end(),
            [&](const auto& checkpoint) { return checkpoint.first == call; });
        if (matched == checkpoints.end() || !matched->second.validCheckpoint) {
            ++summary.unmatchedSourceSites;
            continue;
        }

        SourceReporterCheckpointGap gap;
        gap.sourcePc = site.sourcePc;
        gap.physicalSiteId = uint64_t(
            std::distance(checkpoints.begin(), matched) + 1);
        gap.sourceLoopBlock = site.sourceLoopBlock;
        gap.phase = site.phase;
        gap.portableLoopPathDepth = site.portableLoopPathDepth;
        gap.sourceLoopPath = site.sourceLoopPath;
        gap.unsupportedLlvmValues = matched->second.unsupported;
        for (const auto& reporter : observations) {
            llvm::Value* value = reporter.value;
            const auto* instruction =
                llvm::dyn_cast_or_null<llvm::Instruction>(value);
            if (!instruction || instruction->getFunction() !=
                    call->getFunction())
                continue;
            const auto live = std::find_if(
                matched->second.numeric.begin(),
                matched->second.numeric.end(),
                [&](const LlvmLiveValue& entry) {
                    return entry.definition == instruction;
                });
            if (live == matched->second.numeric.end())
                continue;
            // This is the SAME numeric instruction-ID/width encoding used
            // by RT's existing source typed SSA manifest. No GC handle is
            // inferred from an i64 descriptor.
            const uint64_t descriptor =
                uint64_t(live->id) | (uint64_t(live->width) << 32);
            gap.reporterSsaBindings.push_back({
                reporter.sourcePc, descriptor, reporter.origin,
                reporter.sourceBlock, reporter.parentBlock,
                reporter.inputName, reporter.storage,
                reporter.inputOrdinal, reporter.selectedShadow});
            ++gap.liveReporterObservations;
            switch (reporter.origin) {
            case SourceReporterOrigin::VariableRead: ++gap.variableReads; break;
            case SourceReporterOrigin::ListRead: ++gap.listReads; break;
            case SourceReporterOrigin::ProcedureArgument:
                ++gap.procedureArguments; break;
            case SourceReporterOrigin::ComputedExpression:
                ++gap.computedExpressions; break;
            case SourceReporterOrigin::Literal: ++gap.literals; break;
            case SourceReporterOrigin::OtherReporter:
            case SourceReporterOrigin::Unknown:
                ++gap.otherReporters; break;
            }
            // Merely observing a live LLVM definition proves NEITHER
            // a Scratch resume destination nor a retained GC root.
            ++gap.unmappedSourceSlots;
            if (value->getType()->isIntegerTy(64))
                ++gap.boxedWithoutGcProof;
            else
                ++gap.numericWithoutSourceSlot;
            if (emitLog && gap.liveReporterObservations <= 24)
                std::fprintf(stderr,
                    "[ir001-source-resume-gap-item] checkpoint_pc=%llu "
                    "block=%s phase=%u reporter_pc=%llu reporter=%s "
                    "parent=%s input=%s source_kind=%s "
                    "gc_root_proven=0 source_slot_proven=0 executable=0 "
                    "ssa_id=%u ssa_width=%u "
                    "source_owner=%s storage_id=%s "
                    "input_ordinal=%u selected_shadow=%d\n",
                    static_cast<unsigned long long>(gap.sourcePc),
                    gap.sourceLoopBlock.c_str(), unsigned(gap.phase),
                    static_cast<unsigned long long>(reporter.sourcePc),
                    reporter.sourceBlock.c_str(),
                    reporter.parentBlock.c_str(),
                    reporter.inputName.c_str(),
                    sourceReporterOriginName(reporter.origin),
                    live->id, live->width,
                    sourceReporterStorageName(reporter.storage.kind),
                    reporter.storage.id.empty() ? "none" :
                        reporter.storage.id.c_str(),
                    reporter.inputOrdinal, int(reporter.selectedShadow));
        }
        if (emitLog) {
            std::string lexicalPath;
            for (const auto& blockId : gap.sourceLoopPath) {
                if (!lexicalPath.empty()) lexicalPath += ',';
                lexicalPath += blockId;
            }
            std::fprintf(stderr,
                "[ir001-source-resume-gap] pc=%llu block=%s phase=%u "
                "live_reporters=%u boxed_unrooted=%u "
                "numeric_unmapped=%u source_slot_missing=%u "
                "unsupported_llvm=%u variables=%u lists=%u "
                "arguments=%u expressions=%u literals=%u other=%u "
                "loop_path_depth=%u loop_path_ids=%s "
                "loop_path_restorable=0 source_complete=0 executable=0 "
                "physical_site=%llu\n",
                static_cast<unsigned long long>(gap.sourcePc),
                gap.sourceLoopBlock.c_str(), unsigned(gap.phase),
                gap.liveReporterObservations, gap.boxedWithoutGcProof,
                gap.numericWithoutSourceSlot, gap.unmappedSourceSlots,
                gap.unsupportedLlvmValues, gap.variableReads, gap.listReads,
                gap.procedureArguments, gap.computedExpressions,
                gap.literals, gap.otherReporters, gap.portableLoopPathDepth,
                lexicalPath.c_str(),
                static_cast<unsigned long long>(gap.physicalSiteId));
        }
        summary.checkpointGaps.push_back(std::move(gap));
        ++summary.attributedCheckpoints;
    }

    for (const auto& recorded : observations) {
        ++summary.observed;
        llvm::Value* value = recorded.value;
        if (!value) {
            ++summary.discarded;
            continue;
        }
        ++summary.remaining;
        const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
        if (!instruction) ++summary.constants;
        // The PRODUCTION emitInputBlock contract is a boxed Scratch
        // Value (normally i64), including LLVM ConstantInt literals.
        // A numeric-looking ConstantInt may encode an arbitrary NaN-
        // boxed GC reference. It must never become source-recovery
        // proof merely because integer arithmetic/provenance succeeds.
        // Only a genuinely non-boxed i1/i32 reporter return could
        // qualify as a raw numeric result (none are currently known).
        const bool raw =
            (value->getType()->isIntegerTy(1) ||
             value->getType()->isIntegerTy(32)) &&
            provenLlvmRawNumeric(value);
        if (raw) ++summary.rawNumeric;
        else ++summary.boxedOrUnknown;

        uint64_t live = 0;
        if (instruction) {
            for (const auto& checkpoint : checkpoints) {
                if (instruction->getFunction() !=
                    checkpoint.first->getFunction())
                    continue;
                if (std::any_of(
                    checkpoint.second.numeric.begin(),
                    checkpoint.second.numeric.end(),
                    [&](const LlvmLiveValue& candidate) {
                        return candidate.definition == instruction;
                    }))
                    ++live;
            }
        }
        if (live) ++summary.liveAtCheckpoint;
        if (emitLog && summary.observed <= 64) {
            // Exactly one compiler-origin value: never print raw
            // addresses, boxed GC bits or Scratch private field contents.
            std::fprintf(stderr,
                "[ir001-source-reporter] pc=%llu block=%s parent=%s "
                "input=%s raw_numeric=%d live_checkpoints=%llu "
                "source_kind=%s gc_root_proven=0 executable=0\n",
                static_cast<unsigned long long>(recorded.sourcePc),
                recorded.sourceBlock.c_str(),
                recorded.parentBlock.c_str(),
                recorded.inputName.c_str(), int(raw),
                static_cast<unsigned long long>(live),
                sourceReporterOriginName(recorded.origin));
        }
    }
    if (emitLog)
        std::fprintf(stderr,
            "[ir001-source-resume-summary] checkpoints=%llu "
            "attributed=%llu unowned_checkpoints=%llu "
            "invalid_source_observations=%llu "
            "source_complete=0 executable=0\n",
            static_cast<unsigned long long>(summary.checkpointSites),
            static_cast<unsigned long long>(summary.attributedCheckpoints),
            static_cast<unsigned long long>(
                summary.checkpointSites > summary.attributedCheckpoints
                    ? summary.checkpointSites - summary.attributedCheckpoints
                    : 0),
            static_cast<unsigned long long>(summary.unmatchedSourceSites));
    if (emitLog)
        std::fprintf(stderr,
            "[ir001-source-reporter-summary] observed=%llu "
            "remaining=%llu discarded=%llu constants=%llu "
            "raw_numeric=%llu boxed_or_unknown=%llu "
            "live_at_checkpoint=%llu checkpoint_sites=%llu "
            "source_complete=0 executable=0\n",
            static_cast<unsigned long long>(summary.observed),
            static_cast<unsigned long long>(summary.remaining),
            static_cast<unsigned long long>(summary.discarded),
            static_cast<unsigned long long>(summary.constants),
            static_cast<unsigned long long>(summary.rawNumeric),
            static_cast<unsigned long long>(summary.boxedOrUnknown),
            static_cast<unsigned long long>(summary.liveAtCheckpoint),
            static_cast<unsigned long long>(summary.checkpointSites));
    return summary;
}

} // namespace xyo::ir
