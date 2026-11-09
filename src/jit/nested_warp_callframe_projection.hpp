// Read-only source/CallFrame-ABI interoperability for bounded warp OSR.
//
// The interpreter owns ALL live StackFrames, root slots and GC roots. This
// witness converts a validated interpreter snapshot into IR-001 canonical
// numeric source PCs, immutable call-return sites and typed counted-loop
// Value bits. It is a proof for an isolated native inner repeat, NOT an
// executable deoptimization image or permission to resume arbitrary SSA PCs.
#pragma once

#include "../engine/interpreter_osr_snapshot.hpp"
#include "procedure_frame_state.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xyo::jit {

struct NestedWarpSourceWitness {
    uint64_t sourceRootPc = 0;
    uint64_t sourceRevision = 0;
    std::vector<ir::SourceReturnSite> calls;
    std::vector<ir::SourceConditionalArm> branches;
    std::vector<ir::SourceLoopResume> parentLoops;
    ir::SourceLoopResume innerLoop;
    // Typed procedure frames only. No synthetic live SSA state is claimed.
    std::vector<ir::CallFrameInfo> procedureFrames;
};

// The snapshot must have been checked with validFor(thread, epoch) first.
// Reject source aliases, missing lexical proof, forged ownership, wrong
// return PC, inconsistent live parent loop counts and wrong IF arm.
inline std::optional<NestedWarpSourceWitness>
projectNestedWarpSource(
    const ir::InterpreterOsrSnapshot& snapshot,
    const ir::SourceFrameSymbols& symbols,
    const std::string& rootId,
    const std::vector<std::string>& framePath,
    const std::vector<uint8_t>& frameKinds,
    const std::string& innerLoopId) {

    if (!symbols.verifiedSourceIdentity ||
        symbols.verifiedSourceIdentity->rootPc != symbols.sourceRootPc ||
        symbols.verifiedSourceIdentity->revision != symbols.sourceRevision ||
        symbols.verifiedSourceIdentity->digest != symbols.sourceDigest ||
        symbols.sourceRevision != snapshot.sourceRevision() ||
        !symbols.sourceDigest || framePath.empty() ||
        frameKinds.size() != framePath.size() ||
        snapshot.depth() != framePath.size() + 1 ||
        snapshot.frames().back().blockId != innerLoopId ||
        !snapshot.hasCountedLoopCheckpoint())
        return std::nullopt;

    const auto frozenRoot = symbols.block(symbols.sourceRootPc);
    if (!frozenRoot || *frozenRoot != rootId) return std::nullopt;

    const auto lexical = snapshot.sourcePath();
    if (!lexical || lexical->size() != snapshot.depth())
        return std::nullopt;

    const auto sourcePcFor = [&](const std::string& id)
        -> std::optional<uint64_t> {
        // The table is bounded by the original script source graph.
        // Validate the immutable PC->source block identity at each lookup.
        const auto it = std::find(
            symbols.blockIds.begin(), symbols.blockIds.end(), id);
        if (it == symbols.blockIds.end()) return std::nullopt;
        const uint64_t pc = uint64_t(it - symbols.blockIds.begin() + 1);
        const auto frozen = symbols.block(pc);
        return frozen && *frozen == id ? std::optional<uint64_t>(pc)
                                       : std::nullopt;
    };

    NestedWarpSourceWitness proof;
    proof.sourceRootPc = symbols.sourceRootPc;
    proof.sourceRevision = symbols.sourceRevision;
    uint64_t owner = ir::CallFrameAbi::kRootProcedure;
    std::vector<uint64_t> activeLoops;
    std::vector<ir::SourceConditionalArm> activeBranches;
    proof.procedureFrames.push_back({
        owner, ir::CallFrameAbi::kNoPc, symbols.sourceRootPc, 0, 0
    });

    for (size_t i = 0; i < framePath.size(); ++i) {
        const auto& state = snapshot.frames()[i];
        const auto& lexicalSite = (*lexical)[i];
        if (state.blockId != framePath[i] ||
            lexicalSite.sourceBlockId != framePath[i])
            return std::nullopt;
        const auto pc = sourcePcFor(framePath[i]);
        if (!pc) return std::nullopt;

        if (frameKinds[i] == 3) {
            // A parent repeat must already be executing in the real
            // interpreter. Do not touch/evaluate its original TIMES.
            if (state.opcode != "control_repeat" ||
                !state.isLoop || !state.loopCounter ||
                lexicalSite.branchArm != 0 ||
                !lexicalSite.returnBlockId.empty())
                return std::nullopt;
            const auto loop = std::find_if(
                symbols.loops.begin(), symbols.loops.end(),
                [&](const auto& entry) { return entry.sourcePc == *pc; });
            if (loop == symbols.loops.end() || !loop->counted ||
                loop->ownerProcedureId != owner ||
                loop->enclosingLoops != activeLoops)
                return std::nullopt;
            activeLoops.push_back(*pc);
            proof.parentLoops.push_back({
                *pc, Value::num(*state.loopCounter).bits
            });
            continue;
        }
        if (frameKinds[i] == 1 || frameKinds[i] == 2) {
            if ((state.opcode != "control_if" &&
                 state.opcode != "control_if_else") ||
                (frameKinds[i] == 2 &&
                 state.opcode != "control_if_else") ||
                lexicalSite.branchArm != frameKinds[i] ||
                !lexicalSite.returnBlockId.empty())
                return std::nullopt;
            ir::SourceConditionalArm arm{*pc, frameKinds[i]};
            activeBranches.push_back(arm);
            proof.branches.push_back(arm);
            continue;
        }
        if (frameKinds[i] != 0 ||
            state.opcode != "procedures_call" || !state.callExecuted)
            return std::nullopt;

        const ir::SourceReturnSite* site = nullptr;
        for (const auto& [returnPc, candidate] : symbols.returns) {
            if (candidate.callBlockPc == *pc &&
                candidate.returnPc == returnPc) {
                if (site) return std::nullopt;
                site = &candidate;
            }
        }
        if (!site || !symbols.validReturnTarget(*site) ||
            !symbols.returnSite(site->returnPc) ||
            site->callerProcedureId != owner ||
            site->callerLoopPath != activeLoops ||
            site->branchPath != activeBranches ||
            site->argumentCount != state.arguments.size())
            return std::nullopt;

        const auto expectedReturn = sourcePcFor(lexicalSite.returnBlockId);
        // An empty source next may return via AfterBranch, LoopLatch,
        // ProcedureExit or ScriptExit; the immutable return-site kind
        // and target is verified by IR-001, not guessed from NEXT.
        if ((!lexicalSite.returnBlockId.empty() &&
             (!expectedReturn ||
              site->kind != ir::SourceReturnKind::NextBlock ||
              site->targetPc != *expectedReturn)))
            return std::nullopt;
        proof.calls.push_back(*site);
        owner = site->calleeProcedureId;
        proof.procedureFrames.push_back({
            owner, site->returnPc, site->calleeEntryPc,
            site->argumentCount, 0
        });
        activeLoops.clear();
        activeBranches.clear();
    }

    const auto& leaf = snapshot.frames().back();
    const auto pc = sourcePcFor(innerLoopId);
    if (!pc || !leaf.isLoop || !leaf.loopCounter ||
        leaf.opcode != "control_repeat" ||
        (*lexical).back().sourceBlockId != innerLoopId)
        return std::nullopt;
    const auto loop = std::find_if(
        symbols.loops.begin(), symbols.loops.end(),
        [&](const auto& entry) { return entry.sourcePc == *pc; });
    if (loop == symbols.loops.end() || !loop->counted ||
        loop->ownerProcedureId != owner ||
        loop->enclosingLoops != activeLoops)
        return std::nullopt;

    proof.innerLoop = {*pc, Value::num(*leaf.loopCounter).bits};
    if (proof.calls.empty() ||
        proof.procedureFrames.size() != proof.calls.size() + 1 ||
        proof.procedureFrames.back().procedureId != owner)
        return std::nullopt;
    return proof;
}

} // namespace xyo::jit
