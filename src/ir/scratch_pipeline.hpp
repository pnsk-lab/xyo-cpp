// Shared, lossless frontend for native and WebAssembly JITs.
// This intentionally preserves opaque effects: only a backend may decide
// whether it can compile a node; unsupported operations must never disappear.
#pragma once

#include "scratch_effect.hpp"
#include "scratch_transfer.hpp"

namespace xyo::ir {

struct ScratchCompilationUnit {
    // Unique revision of the Blocks instance from which this immutable
    // JIT snapshot was captured. Unlike a fresh live Blocks::revision()
    // query during codegen, this provenance also covers async compilation.
    uint64_t sourceRevision = 0;
    ScratchScript script;
    ScratchEffects effects;
    ScratchTransferGraph transfers;
    // Resolve calls while capturing the original Blocks snapshot, never
    // by consulting mutable Scratch source during native/WASM codegen.
    std::unordered_map<std::string, std::string> procedureDefinitions;

    bool valid() const {
        return sourceRevision != 0 &&
               script.valid() && effects.valid && transfers.valid;
    }

    static ScratchCompilationUnit capture(Blocks& blocks, const Block* top) {
        ScratchCompilationUnit unit;
        const uint64_t startRevision = blocks.revision();
        unit.script = ScratchScript::capture(blocks, top);
        if (!unit.script.valid()) return unit;

        // Reachable procedure definitions are part of the source graph for
        // both backends. Appended nodes preserve existing stable block IDs.
        for (size_t i = 0; i < unit.script.nodes.size(); ++i) {
            const Block& node = unit.script.nodes[i];
            if (node.opcode != "procedures_call" || !node.mutation)
                continue;
            const std::string proccode = node.mutation->attr("proccode");
            if (proccode.empty()) {
                unit.script.error = ScratchIrError::MissingBlock;
                unit.script.errorBlock = node.id;
                return unit;
            }
            const std::string definition = blocks.getProcedureDefinition(proccode);
            // Scratch permits unresolved calls. The existing native JIT
            // and interpreter execute the surrounding statements normally.
            // Retain an unresolved Call site in the shared transfer IR;
            // never reject an otherwise valid script at capture time.
            if (definition.empty()) continue;
            unit.procedureDefinitions.emplace(proccode, definition);
            if (unit.script.getBlock(definition)) continue;
            ScratchScript callee = ScratchScript::capture(
                blocks, blocks.getBlock(definition));
            if (!callee.valid() ||
                unit.script.nodes.size() + callee.nodes.size() > ScratchScript::kMaxNodes) {
                unit.script.error = callee.valid()
                    ? ScratchIrError::TooManyBlocks : callee.error;
                unit.script.errorBlock = definition;
                return unit;
            }
            for (const Block& block : callee.nodes) {
                if (unit.script.byId.count(block.id)) continue;
                unit.script.byId.emplace(block.id, unit.script.nodes.size());
                unit.script.nodes.push_back(block);
            }
        }
        unit.effects = ScratchEffects::lower(unit.script);
        if (unit.effects.valid)
            unit.transfers = ScratchTransferGraph::lower(
                unit.script, unit.effects, unit.procedureDefinitions);
        // Fail closed on any in-flight edit, including source mutation while
        // traversing asynchronously captured procedure definitions.
        if (blocks.revision() == startRevision)
            unit.sourceRevision = startRevision;
        return unit;
    }
};

} // namespace xyo::ir
