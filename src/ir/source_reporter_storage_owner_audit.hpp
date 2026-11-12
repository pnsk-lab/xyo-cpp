// IR-001: read back the CURRENT Scratch scalar/list storage OWNER of
// a frozen reporter. This is an ownership diagnostic, not a snapshot of
// the reporter's evaluated Value or a writable interpreter resume slot.
#pragma once

#include "llvm_source_reporter_trace.hpp"
#include "../model/target.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace xyo::ir {

enum class SourceReporterOwnerState : uint8_t {
    NonStorage, TypedScalar, TypedList, Missing, WrongType, Unresolved
};

struct SourceReporterOwnerObservation {
    uint64_t reporterPc = 0;
    SourceReporterStorageKind kind = SourceReporterStorageKind::Unresolved;
    SourceReporterOwnerState state = SourceReporterOwnerState::Unresolved;
    // The expected ID is from compiler-frozen source. NEVER a Value*,
    // pointer to a Variable, list-item reference, or GC-managed object.
    std::string sourceVariableId;
};

struct SourceReporterOwnerReadback {
    Target::VariableBindingStamp epoch;
    uint32_t typedScalarOwners = 0;
    uint32_t typedListOwners = 0;
    uint32_t missingOwners = 0;
    uint32_t wrongTypeOwners = 0;
    uint32_t unresolvedOrigins = 0;
    uint32_t nonStorageOrigins = 0;
    std::vector<SourceReporterOwnerObservation> observations;
    bool ownershipComplete() const {
        return missingOwners == 0 && wrongTypeOwners == 0 &&
               unresolvedOrigins == 0;
    }
    // The evaluated SSA Value may predate arbitrary Scratch changes.
    // No slot in the interpreter owns that historical expression result.
    bool sourceValueRestorable() const { return false; }
    bool executable() const { return false; }
};

inline bool validCompiledReporterStorage(
        const SourceReporterLiveSsaBinding& binding) {
    if (!binding.reporterPc || binding.reporterBlock.empty() ||
        binding.parentBlock.empty() || binding.inputName.empty())
        return false;
    const auto& s = binding.storage;
    switch (binding.origin) {
    case SourceReporterOrigin::VariableRead:
        return s.kind == SourceReporterStorageKind::ScalarVariable &&
               s.opcode == "data_variable" && !s.id.empty();
    case SourceReporterOrigin::ListRead:
        return s.kind == SourceReporterStorageKind::ListVariable &&
               !s.id.empty() &&
               (s.opcode == "data_itemoflist" ||
                s.opcode == "data_itemnumoflist" ||
                s.opcode == "data_lengthoflist" ||
                s.opcode == "data_listcontainsitem" ||
                s.opcode == "data_listcontents");
    case SourceReporterOrigin::ProcedureArgument:
        return s.kind == SourceReporterStorageKind::ProcedureArgument &&
               s.id.empty() &&
               (s.opcode == "argument_reporter_string_number" ||
                s.opcode == "argument_reporter_boolean");
    case SourceReporterOrigin::ComputedExpression:
        return s.kind == SourceReporterStorageKind::ExpressionTemporary &&
               s.id.empty() && s.opcode.starts_with("operator_");
    case SourceReporterOrigin::Literal:
        return s.kind == SourceReporterStorageKind::Literal &&
               s.id.empty() &&
               (s.opcode == "math_number" || s.opcode == "math_integer" ||
                s.opcode == "math_whole_number" ||
                s.opcode == "math_positive_number" ||
                s.opcode == "math_angle" || s.opcode == "text" ||
                s.opcode == "colour_picker");
    case SourceReporterOrigin::OtherReporter:
        return s.kind == SourceReporterStorageKind::OtherReporter &&
               s.id.empty() && !s.opcode.empty();
    case SourceReporterOrigin::Unknown:
        return s.kind == SourceReporterStorageKind::Unresolved &&
               s.id.empty();
    }
    return false;
}

// Called only with a currently active Scratch Target at a native RT
// checkpoint / scheduler safe point. No create-or-lookup-by-name fallback:
// by-id lookup has exactly Scratch's local-first / stage-second semantics.
// No Variable* is retained past the current generation check; no Value,
// SList element, or GC-managed SStr is read, dereferenced or rooted.
inline std::optional<SourceReporterOwnerReadback>
auditCompiledReporterStorageOwners(
        const std::vector<SourceReporterLiveSsaBinding>& bindings,
        Target& target) {
    SourceReporterOwnerReadback result;
    result.epoch = target.variableBindingStamp();
    result.observations.reserve(bindings.size());
    for (const auto& binding : bindings) {
        if (!validCompiledReporterStorage(binding)) return std::nullopt;
        SourceReporterOwnerObservation record;
        record.reporterPc = binding.reporterPc;
        record.kind = binding.storage.kind;
        record.sourceVariableId = binding.storage.id;
        switch (binding.storage.kind) {
        case SourceReporterStorageKind::ScalarVariable:
        case SourceReporterStorageKind::ListVariable: {
            Variable* current = target.lookupVariableById(
                binding.storage.id);
            if (!current) {
                record.state = SourceReporterOwnerState::Missing;
                ++result.missingOwners;
            } else {
                const bool wantList =
                    binding.storage.kind ==
                    SourceReporterStorageKind::ListVariable;
                const bool typed = current->id == binding.storage.id &&
                    current->type == (wantList ? Variable::kListType
                                               : Variable::kScalarType) &&
                    (!wantList || current->list != nullptr);
                if (!typed) {
                    record.state = SourceReporterOwnerState::WrongType;
                    ++result.wrongTypeOwners;
                } else if (wantList) {
                    record.state = SourceReporterOwnerState::TypedList;
                    ++result.typedListOwners;
                } else {
                    record.state = SourceReporterOwnerState::TypedScalar;
                    ++result.typedScalarOwners;
                }
            }
            break;
        }
        case SourceReporterStorageKind::Unresolved:
            record.state = SourceReporterOwnerState::Unresolved;
            ++result.unresolvedOrigins;
            break;
        default:
            record.state = SourceReporterOwnerState::NonStorage;
            ++result.nonStorageOrigins;
            break;
        }
        result.observations.push_back(std::move(record));
    }
    if (result.epoch != target.variableBindingStamp())
        return std::nullopt;
    return result;
}

} // namespace xyo::ir
