// Lossless, backend-neutral Scratch control-transfer/effect sites.
//
// This graph is a semantic manifest, not executable machine code: procedure
// calls, yield points and observable effects MUST survive lowering even if
// LLVM/WASM cannot compile them. The scheduler remains authoritative.
// All identifiers are stable Scratch block IDs, never live Block* pointers.
#pragma once

#include "scratch_effect.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xyo::ir {

enum class ScratchTransferKind : uint8_t {
    ProcedureEntry, Call, Return, Yield, Effect
};

struct ScratchTransferSite {
    ScratchTransferKind kind = ScratchTransferKind::Effect;
    std::string sourceBlockId;
    // Sequential continuation after the operation, empty at end of stack.
    std::string continuationBlockId;
    // Call: resolved procedures_definition ID.
    // Entry/Return: the owning procedures_definition ID.
    std::string procedureDefinitionId;
    ScratchEffect effects = ScratchEffect::Unknown;
};

struct ScratchTransferGraph {
    std::vector<ScratchTransferSite> sites;
    bool valid = false;

    // Resolutions are captured with the source in ScratchCompilationUnit.
    // No runtime Blocks lookup is permitted inside backend codegen.
    static ScratchTransferGraph lower(
        const ScratchScript& script, const ScratchEffects& effects,
        const std::unordered_map<std::string, std::string>& procedures) {
        ScratchTransferGraph result;
        if (!script.valid() || !effects.valid ||
            !script.getBlock(script.topId)) return result;
        result.sites.reserve(script.nodes.size());
        for (const Block& block : script.nodes) {
            const ScratchNode* node = effects.getBlock(block.id);
            if (!node) return {};
            const auto emit = [&](ScratchTransferKind kind,
                                  const std::string& definition = {}) {
                result.sites.push_back({
                    kind, block.id, block.next, definition, node->effects
                });
            };
            if (block.opcode == "procedures_definition") {
                emit(ScratchTransferKind::ProcedureEntry, block.id);
                // A semantic exit of this procedure, not an unconditional
                // branch from its definition. A future structured CFG must
                // connect every reachable stack end to this return point.
                emit(ScratchTransferKind::Return, block.id);
            } else if (block.opcode == "procedures_call") {
                if (!block.mutation) return {};
                const std::string code = block.mutation->attr("proccode");
                const auto it = procedures.find(code);
                if (it == procedures.end()) {
                    // Unresolved is a first-class, effectful call site,
                    // not a malformed snapshot. Backend execution must
                    // preserve Scratch's undefined-procedure semantics.
                    emit(ScratchTransferKind::Call);
                } else {
                    const Block* definition = script.getBlock(it->second);
                    if (!definition || definition->opcode !=
                        "procedures_definition")
                        return {};
                    emit(ScratchTransferKind::Call, it->second);
                }
            } else if (node->kind == ScratchNodeKind::Yield ||
                       (hasEffect(node->effects, ScratchEffect::MayYield) &&
                        node->kind == ScratchNodeKind::Control)) {
                emit(ScratchTransferKind::Yield);
            } else if (node->opaque ||
                       node->kind == ScratchNodeKind::WriteList ||
                       node->kind == ScratchNodeKind::Effect) {
                // Opaque code may BOTH produce observable effects AND yield.
                // One transfer site cannot encode both behaviors. Preserve
                // both source checkpoints so future schedulers cannot treat
                // an unknown extension as a replayable synchronous effect.
                emit(ScratchTransferKind::Effect);
                if (hasEffect(node->effects, ScratchEffect::MayYield))
                    emit(ScratchTransferKind::Yield);
            }
        }
        result.valid = result.validate(script);
        return result;
    }

    bool validate(const ScratchScript& script) const {
        if (!script.valid()) return false;
        for (const auto& site : sites) {
            const Block* source = script.getBlock(site.sourceBlockId);
            if (!source ||
                (!site.continuationBlockId.empty() &&
                 !script.getBlock(site.continuationBlockId)))
                return false;
            switch (site.kind) {
            case ScratchTransferKind::ProcedureEntry:
            case ScratchTransferKind::Return:
                if (source->opcode != "procedures_definition" ||
                    site.procedureDefinitionId != site.sourceBlockId)
                    return false;
                break;
            case ScratchTransferKind::Call: {
                const Block* callee =
                    script.getBlock(site.procedureDefinitionId);
                if (source->opcode != "procedures_call" ||
                    !source->mutation ||
                    (!site.procedureDefinitionId.empty() &&
                     (!callee || callee->opcode !=
                         "procedures_definition")))
                    return false;
                break;
            }
            case ScratchTransferKind::Yield:
                if (!hasEffect(site.effects, ScratchEffect::MayYield))
                    return false;
                break;
            case ScratchTransferKind::Effect:
                if (site.effects == ScratchEffect::None)
                    return false;
                break;
            }
        }
        return true;
    }

    // These sites cannot be silently represented as pure numeric SSA
    // instructions. A backend either implements the relevant host ABI and
    // scheduler semantics or explicitly keeps interpreter fallback.
    bool requiresRuntime() const {
        return !sites.empty();
    }
};

} // namespace xyo::ir
