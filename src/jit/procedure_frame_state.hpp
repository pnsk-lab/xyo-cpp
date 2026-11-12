// Optional LLVM Fiber -> portable CallFrame ABI bridge.
//
// The native JIT already stores the exact machine instruction pointer in the
// Fiber. This mirror records the same numeric arguments and source return-PC
// for every *observable* native procedure call, preserving these frames
// across a real Fiber yield. It does not replace native execution/deopt.
//
// Opt-in: XYO_JIT_CALLFRAME=1. Heap-string argument Values are retained
// in a GC-rooted sidecar; the numeric frame arena never contains GC pointers.
#pragma once

#include "../ir/scratch_call_frame.hpp"
#include "../ir/gc_call_frame_values.hpp"
#include "../ir/source_resume_phase.hpp"
#include "../ir/source_ssa_snapshot.hpp"
#include "../ir/scratch_source_digest.hpp"
#include "../ir/scratch_pipeline.hpp"
#include "../core/value.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xyo::ir {

// A pointer-free description of the source-level location observed by
// native LLVM immediately before a Fiber suspension. Values needed for
// resumption outside loops still require a general SSA/deopt environment.
struct SourceLoopResume {
    uint64_t sourcePc = 0;
    uint64_t counterBits = CallFrameAbi::kNoLoopCounter;
    bool operator==(const SourceLoopResume&) const = default;
};
struct SourceProcedureCheckpoint {
    CallFrameInfo frame;
    std::vector<SourceLoopResume> loops;
    SourceResumePhase phase = SourceResumePhase::ProcedureEntry;
};

// A return PC names the point *after* a particular Scratch callsite,
// rather than guessing that block.next exists. In Scratch, the final
// statement of a SUBSTACK may instead return to a loop latch or to the
// enclosing procedure. Keeping this separate source phase also allows
// two distinct calls to the same procedure to have unique return PCs.
enum class SourceReturnKind : uint8_t {
    NextBlock,
    AfterBranch,
    LoopLatch,
    ProcedureExit,
    ScriptExit,
};
// The branch arm is not a speculative path guess: the native code can
// execute this particular callsite only from this lexical SUBSTACK arm.
// Conditional state after return remains an independent replay problem.
struct SourceConditionalArm {
    uint64_t controlPc = 0;
    uint8_t arm = 0; // 1 = SUBSTACK (then), 2 = SUBSTACK2 (else)
    bool operator==(const SourceConditionalArm&) const = default;
};
struct SourceReturnSite {
    uint64_t returnPc = 0;
    uint64_t callBlockPc = 0;
    uint64_t targetPc = 0; // 0 for a procedure/script exit
    SourceReturnKind kind = SourceReturnKind::NextBlock;
    uint64_t callerProcedureId = 0; // 0 = root script
    uint64_t calleeProcedureId = 0;
    uint64_t calleeEntryPc = 0;
    uint32_t argumentCount = 0;
    std::vector<SourceConditionalArm> branchPath; // outermost -> innermost
    // Immutable lexical loop scopes of the original source callsite.
    std::vector<uint64_t> callerLoopPath; // outermost -> innermost
};

// Immutable, backend-neutral symbols tied to the captured Scratch source,
// not to the native Fiber lifetime. All numeric PCs are local to this table.
struct SourceFrameSymbols {
    uint64_t sourceRootPc = 0;
    uint64_t sourceRevision = 0;
    ScratchSourceDigest sourceDigest; // originating compiled immutable Blocks
    struct Loop {
        uint64_t sourcePc = 0;
        uint32_t localSlot = 0;
        bool counted = false;
        uint64_t ownerProcedureId = 0; // 0 only for the root hat
        // Exact lexical ancestors, not previously visited loop sites.
        std::vector<uint64_t> enclosingLoops;
    };
    std::vector<Loop> loops;
    std::vector<std::string> blockIds;      // source PC = index + 1
    // Source PC -> owning procedure, 0 for the root script.
    // kNoPc means a reporter, prototype, definition or other
    // block with no verified executable statement ownership.
    std::vector<uint64_t> statementOwners;
    // Captured Scratch's physical control-flow links, independent of the
    // return-site table. This lets detached frames reject same-procedure
    // *wrong* successors, not only wrong-procedure targets.
    enum class SourceNodeKind : uint8_t {
        Other, Hat, ProcedureDefinition, TrackedLoop, Conditional,
        SubstackContainer
    };
    struct SourceNodeTopology {
        uint64_t parentPc = 0;
        uint64_t nextPc = 0;
        uint64_t substackPc = 0;
        uint64_t substack2Pc = 0;
        SourceNodeKind kind = SourceNodeKind::Other;
    };
    std::vector<SourceNodeTopology> sourceTopology;
    std::vector<std::string> procedureIds;  // procedure ID = index + 1
    std::unordered_map<uint64_t, SourceReturnSite> returns;

    // Independently frozen source-name tables: a detached diagnostic
    // copy may mutate its public blockIds/procedureIds vectors, but
    // it must not make the same numeric PC identify another statement
    // or make the same procedure ID identify a different definition.
    // Check only names actually dereferenced during a handoff (O(1)
    // per lookup), not every source block on every scheduler yield.
    std::shared_ptr<const std::vector<std::string>> verifiedBlockIds;
    std::shared_ptr<const std::vector<std::string>> verifiedProcedureIds;
    struct SourceIdentity {
        uint64_t rootPc = 0;
        uint64_t revision = 0;
        ScratchSourceDigest digest;
    };
    // A separate immutable copy prevents a cloned detached image from
    // treating a different source hat/revision/digest as its origin.
    std::shared_ptr<const SourceIdentity> verifiedSourceIdentity;

    std::optional<std::string_view> block(uint64_t pc) const {
        if (pc == 0 || pc > blockIds.size() || !verifiedBlockIds ||
            verifiedBlockIds->size() != blockIds.size() ||
            (*verifiedBlockIds)[size_t(pc - 1)] != blockIds[size_t(pc - 1)])
            return std::nullopt;
        return blockIds[size_t(pc - 1)];
    }
    std::optional<std::string_view> procedure(uint64_t id) const {
        if (id == 0 || id > procedureIds.size() || !verifiedProcedureIds ||
            verifiedProcedureIds->size() != procedureIds.size() ||
            (*verifiedProcedureIds)[size_t(id - 1)] !=
                procedureIds[size_t(id - 1)])
            return std::nullopt;
        return procedureIds[size_t(id - 1)];
    }
    std::optional<SourceReturnSite> returnSite(uint64_t pc) const {
        // The synthetic return PC is a source callsite identity, not
        // just a lookup hint. A detached dictionary copied for handoff
        // must not alias a genuine SourceReturnSite under another key:
        // immutable callsite proofs otherwise validate the authentic
        // payload while the numeric frame resumes at the forged key.
        const auto it = returns.find(pc);
        if (it == returns.end() || it->second.returnPc != pc)
            return std::nullopt;
        return it->second;
    }

    // Recover the structurally unique continuation of a callsite from
    // original immutable parent/NEXT/SUBSTACK edges. Do NOT consult the
    // user-provided SourceReturnSite (the object being validated).
    std::optional<std::pair<SourceReturnKind, uint64_t>>
    structuralReturnTarget(uint64_t callPc) const {
        if (callPc == 0 || callPc > sourceTopology.size())
            return std::nullopt;
        uint64_t current = callPc;
        if (sourceTopology[size_t(current - 1)].nextPc)
            return {{SourceReturnKind::NextBlock,
                     sourceTopology[size_t(current - 1)].nextPc}};
        // Bound even a malformed/mutated parent cycle in a detached image.
        for (size_t step = 0; step <= sourceTopology.size(); ++step) {
            const auto& node = sourceTopology[size_t(current - 1)];
            if (node.parentPc == 0) {
                if (node.kind == SourceNodeKind::ProcedureDefinition)
                    return {{SourceReturnKind::ProcedureExit, 0}};
                if (node.kind == SourceNodeKind::Hat)
                    return {{SourceReturnKind::ScriptExit, 0}};
                return std::nullopt;
            }
            if (node.parentPc > sourceTopology.size())
                return std::nullopt;
            const uint64_t parentPc = node.parentPc;
            const auto& parent = sourceTopology[size_t(parentPc - 1)];
            if (parent.kind == SourceNodeKind::ProcedureDefinition)
                return parent.nextPc == current
                           ? std::optional<std::pair<SourceReturnKind,
                                                   uint64_t>>{{
                                 SourceReturnKind::ProcedureExit, 0}}
                           : std::nullopt;
            if (parent.kind == SourceNodeKind::Hat)
                return parent.nextPc == current
                           ? std::optional<std::pair<SourceReturnKind,
                                                   uint64_t>>{{
                                 SourceReturnKind::ScriptExit, 0}}
                           : std::nullopt;
            if (parent.nextPc == current) {
                current = parentPc;
                continue;
            }
            if (parent.substackPc != current &&
                parent.substack2Pc != current)
                return std::nullopt;
            if (parent.kind == SourceNodeKind::TrackedLoop &&
                parent.substackPc == current)
                return {{SourceReturnKind::LoopLatch, parentPc}};
            if (parent.kind == SourceNodeKind::Conditional) {
                if (parent.nextPc)
                    return {{SourceReturnKind::AfterBranch, parent.nextPc}};
                current = parentPc;
                continue;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    // Derive the lexical caller and its *actual* control path from the
    // captured parent/NEXT/SUBSTACK topology. Never trust the path attached
    // to the SourceReturnSite being validated: a forged then/else arm or
    // omitted outer loop could otherwise appear self-consistent with a
    // similarly forged parent CallFrame's loop locals.
    struct StructuralCallerContext {
        uint64_t procedureId = 0;
        std::vector<SourceConditionalArm> branches;
        std::vector<uint64_t> loops;
    };
    // A source-verified callsite proof is derived from the independent
    // numeric source graph once, at compile/capture time. Reusing the
    // frozen proof avoids rewalking a potentially thousands-deep parent
    // chain for every frame on every scheduler handoff.
    struct VerifiedCallsite {
        SourceReturnKind kind = SourceReturnKind::ScriptExit;
        uint64_t targetPc = 0;
        StructuralCallerContext caller;
        // Bind the return address to the actual callsite and callee.
        // The source's callee identity/entry/arity must not be inferred
        // from the untrusted detached SourceReturnSite being checked.
        uint64_t returnPc = 0;
        uint64_t calleeProcedureId = 0;
        uint64_t calleeEntryPc = 0;
        uint32_t argumentCount = 0;
    };
    using VerifiedCallsiteMap =
        std::unordered_map<uint64_t, VerifiedCallsite>;
    // Shared separately from the mutable source symbol dictionary:
    // copying symbols for a diagnostic image cannot accidentally
    // rewrite the independent original source proof.
    std::shared_ptr<const VerifiedCallsiteMap> verifiedCallsites;

    std::optional<StructuralCallerContext>
    structuralCallerContext(uint64_t callPc) const {
        if (callPc == 0 || callPc > sourceTopology.size() ||
            statementOwners.size() != sourceTopology.size() ||
            statementOwners[size_t(callPc - 1)] == CallFrameAbi::kNoPc)
            return std::nullopt;
        StructuralCallerContext result;
        const uint64_t originalOwner = statementOwners[size_t(callPc - 1)];
        uint64_t current = callPc;
        for (size_t steps = 0; steps <= sourceTopology.size(); ++steps) {
            const auto& node = sourceTopology[size_t(current - 1)];
            const uint64_t parentPc = node.parentPc;
            if (parentPc == 0 || parentPc > sourceTopology.size())
                return std::nullopt;
            const auto& parent = sourceTopology[size_t(parentPc - 1)];
            if (parent.kind == SourceNodeKind::ProcedureDefinition) {
                if (parent.nextPc != current || originalOwner == 0 ||
                    !procedure(originalOwner) ||
                    procedure(originalOwner) != block(parentPc))
                    return std::nullopt;
                result.procedureId = originalOwner;
            } else if (parent.kind == SourceNodeKind::Hat) {
                if (parentPc != sourceRootPc ||
                    parent.nextPc != current || originalOwner != 0)
                    return std::nullopt;
                result.procedureId = 0;
            } else {
                if (parent.nextPc == current) {
                    current = parentPc;
                    continue;
                }
                if (parent.kind == SourceNodeKind::Conditional) {
                    uint8_t arm = 0;
                    if (parent.substackPc == current) arm = 1;
                    else if (parent.substack2Pc == current) arm = 2;
                    if (!arm) return std::nullopt;
                    result.branches.push_back({parentPc, arm});
                } else if (parent.kind == SourceNodeKind::TrackedLoop) {
                    if (parent.substackPc != current)
                        return std::nullopt;
                    result.loops.push_back(parentPc);
                } else if (parent.kind == SourceNodeKind::SubstackContainer) {
                    if (parent.substackPc != current)
                        return std::nullopt;
                } else {
                    return std::nullopt;
                }
                current = parentPc;
                continue;
            }
            std::reverse(result.branches.begin(), result.branches.end());
            std::reverse(result.loops.begin(), result.loops.end());
            return result;
        }
        return std::nullopt;
    }

    // The exact continuation and complete caller context must agree with
    // the original source topology, not merely valid source-PC membership.
    bool validReturnTarget(const SourceReturnSite& site) const {
        // The lookup key is the source call PC, never a mutable synthetic
        // return PC. Proofs are separately reconstructed from original
        // sourceTopology and frozen in SourceFrameSymbols before export.
        // A caller may supply a descriptor directly rather than obtaining it
        // through returnSite(). Require the canonical compiled return-PC key
        // to exist and identify this exact callsite before trusting its proof.
        // This is an O(1) lookup, not a source-graph traversal.
        const auto canonical = returnSite(site.returnPc);
        if (!verifiedCallsites || !canonical ||
            canonical->callBlockPc != site.callBlockPc)
            return false;
        const auto it = verifiedCallsites->find(site.callBlockPc);
        if (it == verifiedCallsites->end())
            return false;
        const auto& proof = it->second;
        if (proof.kind != site.kind ||
            proof.targetPc != site.targetPc ||
            proof.returnPc != site.returnPc ||
            proof.calleeProcedureId != site.calleeProcedureId ||
            proof.calleeEntryPc != site.calleeEntryPc ||
            proof.argumentCount != site.argumentCount ||
            proof.caller.procedureId != site.callerProcedureId ||
            proof.caller.branches != site.branchPath ||
            proof.caller.loops != site.callerLoopPath)
            return false;
        if (!block(site.callBlockPc) ||
            !procedure(site.calleeProcedureId) ||
            (site.callerProcedureId && !procedure(site.callerProcedureId)) ||
            (site.calleeEntryPc != CallFrameAbi::kNoPc - 1 &&
             !block(site.calleeEntryPc)) ||
            site.callBlockPc > statementOwners.size() ||
            statementOwners[size_t(site.callBlockPc - 1)] !=
                site.callerProcedureId)
            return false;
        // Caller control scopes are part of the source-level handoff:
        // their original IDs must also still resolve from the frozen PC
        // dictionary. Path equality by numeric PC alone is insufficient.
        for (const auto& branch : site.branchPath)
            if (!block(branch.controlPc)) return false;
        for (const uint64_t loopPc : site.callerLoopPath)
            if (!block(loopPc)) return false;
        switch (site.kind) {
        case SourceReturnKind::NextBlock:
        case SourceReturnKind::AfterBranch:
            return site.targetPc != 0 && block(site.targetPc) &&
                site.targetPc <= statementOwners.size() &&
                statementOwners[size_t(site.targetPc - 1)] ==
                    site.callerProcedureId;
        case SourceReturnKind::LoopLatch:
            return site.targetPc != 0 && block(site.targetPc) &&
                std::any_of(loops.begin(), loops.end(),
                    [&](const Loop& loop) {
                        return loop.sourcePc == site.targetPc &&
                            loop.ownerProcedureId == site.callerProcedureId;
                    });
        case SourceReturnKind::ProcedureExit:
            return site.targetPc == 0 && site.callerProcedureId != 0 &&
                procedure(site.callerProcedureId).has_value();
        case SourceReturnKind::ScriptExit:
            return site.targetPc == 0 && site.callerProcedureId == 0;
        }
        return false;
    }
};

// Portable handoff contains BOTH its independent GC-rooted frame arena and
// the immutable return-PC decoding contract. It can outlive the source
// NativeProcedureFrameSession, but it cannot execute arbitrary LLVM SSA.
struct SelfContainedSourceContinuation {
    struct DecodedFrame {
        CallFrameInfo frame;
        SourceResumePhase phase = SourceResumePhase::ProcedureEntry;
        std::optional<SourceReturnSite> caller;
        std::vector<SourceLoopResume> loops;
    };
    std::unique_ptr<RootedScratchCallStack> frames;
    std::shared_ptr<const SourceFrameSymbols> symbols;
    // Optional diagnostic sidecar. Existing native exports leave it empty;
    // it cannot authorize handoff without the runtime's effect proof.
    std::optional<SourceSsaSnapshot> diagnosticSsa;

    // A complete SSA recording must match a separately provided compiler
    // manifest AND the exact leaf frame/source identity. No implicit trust
    // is assigned to the manifest or to a snapshot copied from an image.
    bool matchesSsaManifest(const SourceSsaManifest& manifest) const {
        if (!diagnosticSsa || !diagnosticSsa->matches(manifest) ||
            !valid() || !symbols ||
            manifest.revision != symbols->sourceRevision ||
            manifest.digestLo != symbols->sourceDigest.lo ||
            manifest.digestHi != symbols->sourceDigest.hi)
            return false;
        const auto leaf = frames->frames().top();
        return leaf && leaf->procedureId == manifest.procedureId &&
            leaf->resumePc == manifest.resumePc;
    }

    // Copy raw numeric SSA words while their native backing storage is
    // still alive. The source manifest must come from the matching
    // compiler artifact, never be reconstructed from untrusted frame data.
    // A failed capture leaves any prior verified diagnostic unchanged.
    // This is diagnostic ownership transfer, NOT executable deopt.
    bool captureDiagnosticSsa(const SourceSsaManifest& manifest,
                              std::span<const uint64_t> words) {
        if (!valid() || !symbols || !frames ||
            manifest.revision != symbols->sourceRevision ||
            manifest.digestLo != symbols->sourceDigest.lo ||
            manifest.digestHi != symbols->sourceDigest.hi)
            return false;
        const auto leaf = frames->frames().top();
        if (!leaf || leaf->procedureId != manifest.procedureId ||
            leaf->resumePc != manifest.resumePc)
            return false;
        auto capture = SourceSsaSnapshot::capture(manifest, words);
        if (!capture) return false;
        diagnosticSsa = std::move(*capture);
        return true;
    }

    bool valid() const {
        if (!frames || !symbols || !symbols->sourceRevision ||
            !symbols->sourceDigest || !symbols->verifiedSourceIdentity ||
            symbols->verifiedSourceIdentity->rootPc !=
                symbols->sourceRootPc ||
            symbols->verifiedSourceIdentity->revision !=
                symbols->sourceRevision ||
            symbols->verifiedSourceIdentity->digest !=
                symbols->sourceDigest ||
            !frames->valid() || !frames->frames().validateImage())
            return false;
        const auto& stack = frames->frames();
        const auto* words = stack.words();
        const size_t stride = stack.layout().strideWords();
        const auto n = symbols->loops.size();
        if (symbols->statementOwners.size() != symbols->blockIds.size() ||
            symbols->sourceTopology.size() != symbols->blockIds.size())
            return false;
        if (n > 32 || stack.layout().localCapacity != 2 * n + 2)
            return false;
        for (size_t slot = 0; slot < n; ++slot) {
            if (symbols->loops[slot].localSlot != slot ||
                !symbols->block(symbols->loops[slot].sourcePc))
                return false;
        }
        for (uint32_t i = 0; i < stack.depth(); ++i) {
            const uint64_t* frame = words + size_t(i) * stride;
            const auto proc = frame[CallFrameAbi::kProcedureIdWord];
            const auto ret = frame[CallFrameAbi::kReturnPcWord];
            if (i == 0) {
                if (proc != CallFrameAbi::kRootProcedure ||
                    ret != CallFrameAbi::kNoPc ||
                    !symbols->block(symbols->sourceRootPc)) return false;
                const uint64_t rootLocalCount =
                    frame[CallFrameAbi::kLocalCountWord];
                if (rootLocalCount != 0 && rootLocalCount != 2 * n + 2)
                    return false;
                bool rootLoopResume = false;
                if (rootLocalCount) {
                    const auto* root = frame + CallFrameAbi::localWord(
                        stack.layout().argumentCapacity, 0);
                    const uint64_t depth = root[2 * n];
                    const uint64_t phase = root[2 * n + 1];
                    if (depth > n || !validSourceResumePhase(phase))
                        return false;
                    uint64_t seen = 0;
                    for (size_t j = 0; j < depth; ++j) {
                        const uint64_t pc = root[n + j];
                        const auto it = std::find_if(
                            symbols->loops.begin(), symbols->loops.end(),
                            [&](const auto& loop) {
                                return loop.sourcePc == pc;
                            });
                        if (it == symbols->loops.end() ||
                            it->ownerProcedureId != 0 ||
                            it->enclosingLoops.size() != j ||
                            (seen & (uint64_t{1} << it->localSlot)))
                            return false;
                        seen |= uint64_t{1} << it->localSlot;
                        for (size_t ancestor = 0; ancestor < j; ++ancestor)
                            if (root[n + ancestor] !=
                                it->enclosingLoops[ancestor])
                                return false;
                        const uint64_t bits = root[it->localSlot];
                        if (bits != CallFrameAbi::kNoLoopCounter &&
                            (!it->counted || !Value{bits}.isNumber()))
                            return false;
                    }
                    for (size_t j = depth; j < n; ++j)
                        if (root[n + j] != 0) return false;
                    // Dormant loop slots are deliberately reset on each
                    // checkpoint. Reject a detached image that smuggles
                    // a numeric stale counter for a different loop.
                    for (size_t slot = 0; slot < n; ++slot)
                        if (!(seen & (uint64_t{1} << slot)) &&
                            root[slot] != CallFrameAbi::kNoLoopCounter)
                            return false;
                    if (phase == uint64_t(SourceResumePhase::AfterLoopBody) &&
                        (depth == 0 ||
                         frame[CallFrameAbi::kResumePcWord] !=
                             root[n + depth - 1]))
                        return false;
                    // The root can itself yield at a completed source
                    // loop. Such a PC is a real loop site, not a synthetic
                    // post-call PC. UnknownBody may retain an old loop PC
                    // for observation only, never executable adoption.
                    rootLoopResume = depth > 0 &&
                        frame[CallFrameAbi::kResumePcWord] ==
                            root[n + depth - 1] &&
                        (phase == uint64_t(SourceResumePhase::InsideLoopBody) ||
                         phase == uint64_t(SourceResumePhase::AfterLoopBody) ||
                         phase == uint64_t(SourceResumePhase::UnknownBody));
                    if (phase == uint64_t(SourceResumePhase::AfterCallee)) {
                        const auto completed = symbols->returnSite(
                            frame[CallFrameAbi::kResumePcWord]);
                        if (!completed ||
                            completed->callerProcedureId != 0 ||
                            !symbols->validReturnTarget(*completed) ||
                            completed->callerLoopPath.size() != depth)
                            return false;
                        for (size_t j = 0; j < depth; ++j)
                            if (root[n + j] !=
                                completed->callerLoopPath[j])
                                return false;
                    }
                    if (stack.depth() > 1) {
                        const auto* child = words + stride;
                        const auto site = symbols->returnSite(
                            child[CallFrameAbi::kReturnPcWord]);
                        if (!site ||
                            phase != uint64_t(
                                SourceResumePhase::AwaitingCallee) ||
                            site->callerProcedureId != 0 ||
                            site->callerLoopPath.size() != depth)
                            return false;
                        for (size_t j = 0; j < depth; ++j)
                            if (root[n + j] != site->callerLoopPath[j])
                                return false;
                    } else if (phase == uint64_t(
                                   SourceResumePhase::AwaitingCallee)) {
                        return false;
                    }
                } else if (stack.depth() > 1) {
                    // Legacy observation-only images with no root locals
                    // cannot certify a root counted-repeat callsite.
                    const auto* child = words + stride;
                    const auto site = symbols->returnSite(
                        child[CallFrameAbi::kReturnPcWord]);
                    if (!site || !site->callerLoopPath.empty())
                        return false;
                }
                if (stack.depth() == 1) {
                    const uint64_t resume =
                        frame[CallFrameAbi::kResumePcWord];
                    // After the last native callee returns, the root may
                    // yield with its synthetic post-call PC still saved.
                    // It is a real, source-authenticated root continuation.
                    if (resume != symbols->sourceRootPc &&
                        !rootLoopResume) {
                        const auto site = symbols->returnSite(resume);
                        if (!site || site->callerProcedureId !=
                                         CallFrameAbi::kRootProcedure ||
                            !symbols->validReturnTarget(*site))
                            return false;
                    }
                }
                continue;
            }
            if (frame[CallFrameAbi::kLocalCountWord] != 2 * n + 2)
                return false;
            const auto* local = frame + CallFrameAbi::localWord(
                stack.layout().argumentCapacity, 0);
            const auto depth = local[2 * n];
            const auto phase = local[2 * n + 1];
            if (depth > n || !validSourceResumePhase(phase))
                return false;
            // The ordered active path cannot contain duplicate loops, and
            // nonrepeat loop sites must not carry numeric repeat counters.
            uint64_t seen = 0;
            for (uint32_t j = 0; j < depth; ++j) {
                const uint64_t pc = local[n + j];
                auto it = std::find_if(symbols->loops.begin(),
                    symbols->loops.end(), [&](const auto& l) {
                        return l.sourcePc == pc;
                    });
                if (it == symbols->loops.end()) return false;
                const uint32_t slot = it->localSlot;
                if (it->ownerProcedureId != proc ||
                    it->enclosingLoops.size() != j)
                    return false;
                for (uint32_t ancestor = 0; ancestor < j; ++ancestor)
                    if (local[n + ancestor] !=
                        it->enclosingLoops[ancestor])
                        return false;
                if ((seen & (uint64_t{1} << slot)) != 0) return false;
                seen |= uint64_t{1} << slot;
                const auto bits = local[slot];
                if (bits != CallFrameAbi::kNoLoopCounter &&
                    (!it->counted || !Value{bits}.isNumber()))
                    return false;
            }
            for (uint32_t j = depth; j < n; ++j) {
                if (local[n + j] != 0) return false;
            }
            for (size_t slot = 0; slot < n; ++slot)
                if (!(seen & (uint64_t{1} << slot)) &&
                    local[slot] != CallFrameAbi::kNoLoopCounter)
                    return false;
            // An AfterCallee leaf must point to a verified post-call PC
            // owned by this executing source procedure, with the same
            // active lexical loop path. No arbitrary SSA is reconstructed.
            if (static_cast<SourceResumePhase>(phase) ==
                    SourceResumePhase::AfterCallee) {
                const auto completed = symbols->returnSite(
                    frame[CallFrameAbi::kResumePcWord]);
                if (!completed ||
                    completed->callerProcedureId != proc ||
                    !symbols->validReturnTarget(*completed) ||
                    completed->callerLoopPath.size() != depth)
                    return false;
                for (size_t j = 0; j < depth; ++j)
                    if (local[n + j] != completed->callerLoopPath[j])
                        return false;
            }
            // A finished-loop checkpoint resumes at the recorded loop
            // site. AwaitingCallee instead uses a synthetic return PC.
            if (static_cast<SourceResumePhase>(phase) ==
                    SourceResumePhase::AfterLoopBody &&
                (depth == 0 || frame[CallFrameAbi::kResumePcWord] !=
                                   local[n + depth - 1]))
                return false;
            const auto site = symbols->returnSite(ret);
            if (!symbols->procedure(proc) || !site ||
                site->calleeProcedureId != proc ||
                site->callBlockPc == 0 ||
                site->callBlockPc > symbols->statementOwners.size() ||
                symbols->statementOwners[size_t(site->callBlockPc - 1)] !=
                    site->callerProcedureId ||
                site->callerProcedureId !=
                    words[(size_t(i) - 1) * stride +
                          CallFrameAbi::kProcedureIdWord] ||
                site->argumentCount !=
                    frame[CallFrameAbi::kArgumentCountWord] ||
                !symbols->block(site->callBlockPc) ||
                !symbols->validReturnTarget(*site) ||
                (site->calleeEntryPc != CallFrameAbi::kNoPc - 1 &&
                 !symbols->block(site->calleeEntryPc)) ||
                (site->callerProcedureId &&
                 !symbols->procedure(site->callerProcedureId)) ||
                (site->targetPc && !symbols->block(site->targetPc)) ||
                (site->kind != SourceReturnKind::ProcedureExit &&
                 site->kind != SourceReturnKind::ScriptExit &&
                 site->targetPc == 0) ||
                frame[CallFrameAbi::kReturnPcWord] !=
                    words[(size_t(i) - 1) * stride +
                          CallFrameAbi::kResumePcWord])
                return false;
            if (static_cast<SourceResumePhase>(phase) ==
                    SourceResumePhase::ProcedureEntry &&
                frame[CallFrameAbi::kResumePcWord] != site->calleeEntryPc)
                return false;
            // The parent is suspended at this exact callsite and must
            // report the same lexical loop path, not stale loop locals
            // belonging to a previous iteration or a different branch.
            if (i > 1) {
                const auto* parent = words + size_t(i - 1) * stride;
                const auto* parentLocal = parent + CallFrameAbi::localWord(
                    stack.layout().argumentCapacity, 0);
                if (parentLocal[2 * n + 1] !=
                        uint64_t(SourceResumePhase::AwaitingCallee) ||
                    parentLocal[2 * n] != site->callerLoopPath.size())
                    return false;
                for (size_t j = 0; j < site->callerLoopPath.size(); ++j)
                    if (parentLocal[n + j] != site->callerLoopPath[j])
                        return false;
            }
            for (uint64_t pc : site->callerLoopPath) {
                if (!symbols->block(pc) ||
                    std::none_of(symbols->loops.begin(),
                                 symbols->loops.end(),
                                 [&](const auto& loop) {
                                     return loop.sourcePc == pc;
                                 }))
                    return false;
            }
            for (const auto& branch : site->branchPath) {
                if (branch.arm < 1 || branch.arm > 2 ||
                    !symbols->block(branch.controlPc))
                    return false;
            }
        }
        return true;
    }

    // Classification only: never a grant to resume interpreter execution.
    // In particular, a valid UnknownBody image still lacks its live LLVM
    // SSA values and the precise intra-statement effect position.
    std::optional<SourceDeoptBoundary> deoptBoundary() const {
        const auto decoded = decodeFrames();
        if (!decoded || decoded->empty()) return std::nullopt;
        return sourceDeoptBoundary(decoded->back().phase);
    }

    // Required proofs are metadata, not proof values. Even a boundary
    // with only EffectContinuity remains ineligible for executable deopt
    // until the RT executor supplies and verifies that proof.
    std::optional<SourceDeoptRequirements> deoptRequirements() const {
        const auto boundary = deoptBoundary();
        if (!boundary) return std::nullopt;
        return sourceDeoptRequirements(*boundary);
    }

    // Optional source-structural successor after the nested native
    // callee returned. This is a *description* of the Scratch target,
    // not approval to deopt arbitrary LLVM SSA/effect positions.
    // Root-only returns remain intentionally excluded here: RT-001
    // currently uses this method as one of several executable-handoff
    // gates. See rootPostCallSuccessor() for source metadata only.
    std::optional<SourceReturnSite> postCallSuccessor() const {
        if (!valid() || frames->frames().depth() <= 1)
            return std::nullopt;
        const auto decoded = decodeFrames();
        if (!decoded || decoded->empty() ||
            decoded->back().phase != SourceResumePhase::AfterCallee)
            return std::nullopt;
        const auto site = symbols->returnSite(
            decoded->back().frame.resumePc);
        return site && site->callerProcedureId ==
                           decoded->back().frame.procedureId &&
                       symbols->validReturnTarget(*site)
                   ? site : std::nullopt;
    }

    // Detached root-only AfterCallee source metadata. Unlike the
    // legacy postCallSuccessor(), this reports the original Scratch
    // successor after the final native procedure frame has popped.
    //
    // This method NEVER authorizes the executable root-only deopt path:
    // reconstructing committed effects and control state is owned by the
    // scheduler/RT layer. This deliberately separate API keeps all
    // existing RT-001 call sites and their fail-closed policy unchanged.
    std::optional<SourceReturnSite> rootPostCallSuccessor() const {
        if (!valid() || frames->frames().depth() != 1)
            return std::nullopt;
        const auto decoded = decodeFrames();
        if (!decoded || decoded->size() != 1 ||
            decoded->front().frame.procedureId !=
                CallFrameAbi::kRootProcedure ||
            decoded->front().phase != SourceResumePhase::AfterCallee)
            return std::nullopt;
        const auto& root = decoded->front();
        const auto site = symbols->returnSite(root.frame.resumePc);
        if (!site || site->callerProcedureId !=
                         CallFrameAbi::kRootProcedure ||
            !symbols->validReturnTarget(*site) ||
            site->callerLoopPath.size() != root.loops.size())
            return std::nullopt;
        for (size_t i = 0; i < root.loops.size(); ++i)
            if (root.loops[i].sourcePc != site->callerLoopPath[i])
                return std::nullopt;
        return site;
    }

    // Build a purely numeric source-level deopt descriptor from the
    // self-contained handoff. String arguments remain in the rooted owner
    // and are borrowed on demand via frames->argument(frame, arg).
    std::optional<std::vector<DecodedFrame>> decodeFrames() const {
        if (!valid()) return std::nullopt;
        std::vector<DecodedFrame> result;
        const auto& stack = frames->frames();
        const auto* words = stack.words();
        const uint32_t n = uint32_t(symbols->loops.size());
        const size_t stride = stack.layout().strideWords();
        result.reserve(stack.depth());
        for (uint32_t i = 0; i < stack.depth(); ++i) {
            const auto* raw = words + size_t(i) * stride;
            DecodedFrame decoded;
            decoded.frame = {
                raw[CallFrameAbi::kProcedureIdWord],
                raw[CallFrameAbi::kReturnPcWord],
                raw[CallFrameAbi::kResumePcWord],
                uint32_t(raw[CallFrameAbi::kArgumentCountWord]),
                uint32_t(raw[CallFrameAbi::kLocalCountWord])
            };
            if (i ||
                decoded.frame.locals == stack.layout().localCapacity) {
                if (i)
                    decoded.caller = symbols->returnSite(decoded.frame.returnPc);
                const auto* local = raw + CallFrameAbi::localWord(
                    stack.layout().argumentCapacity, 0);
                decoded.phase = static_cast<SourceResumePhase>(local[2 * n + 1]);
                for (uint32_t j = 0; j < local[2 * n]; ++j) {
                    const uint64_t pc = local[n + j];
                    const auto it = std::find_if(symbols->loops.begin(),
                        symbols->loops.end(), [&](const auto& l) {
                            return l.sourcePc == pc;
                        });
                    decoded.loops.push_back({pc, local[it->localSlot]});
                }
            }
            result.push_back(std::move(decoded));
        }
        return result;
    }
};

class NativeProcedureFrameSession {
public:
    // Explicitly bounded: per-procedure frame storage is allocated for the
    // declared maximum depth, so indiscriminately reserving thousands of
    // lexical loop slots would waste considerable memory.
    static constexpr uint32_t kMaxTrackedLoops = 32;
    static bool isTrackedLoop(const std::string& opcode) {
        return opcode == "control_repeat" ||
               opcode == "control_while" ||
               opcode == "control_repeat_until" ||
               opcode == "control_forever" ||
               opcode == "control_for_each";
    }

    explicit NativeProcedureFrameSession(
        const ScratchCompilationUnit& source, const Blocks* sourceBlocks = nullptr,
        uint64_t compiledRevision = 0,
        ScratchSourceDigest compiledDigest = {})
        : procedures_(ScratchProcedureTable::capture(source.transfers)),
          stack_(chooseLayout(source)),
          sourceBlocks_(sourceBlocks),
          compiledRevision_(compiledRevision),
          compiledDigest_(compiledDigest),
          sourceTopId_(source.script.topId) {
        if (!source.valid() || !procedures_.valid() || !stack_.valid())
            return;
        // Source provenance is captured by the code generator, not
        // fabricated from the current Blocks revision at first native call.
        // A new source revision may have identical IDs but different opcodes
        // or argument defaults. Do not export such a mismatched frame.
        if (sourceBlocks_ && compiledRevision_ &&
            (source.sourceRevision != compiledRevision_ ||
             sourceBlocks_->revision() != compiledRevision_ ||
             (compiledDigest_ &&
              scratchSourceDigest(source) != compiledDigest_))) {
            disable("compiled-source-changed");
            return;
        }

        std::vector<std::string> blocks;
        blocks.reserve(source.script.nodes.size());
        for (const Block& block : source.script.nodes)
            blocks.push_back(block.id);
        std::sort(blocks.begin(), blocks.end());
        blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
        for (size_t i = 0; i < blocks.size(); ++i) {
            pcByBlock_.emplace(blocks[i], uint64_t(i + 1));
            const Block* node = source.script.getBlock(blocks[i]);
            if (node && isTrackedLoop(node->opcode)) {
                const uint32_t slot = uint32_t(loopSlots_.size());
                if (slot >= stack_.layout().localCapacity) return;
                loopSlots_.emplace(blocks[i], LoopSlot{
                    slot, node->opcode == "control_repeat", uint64_t(i + 1)});
            }
        }
        if (stack_.layout().localCapacity != loopSlots_.size() * 2 + 2)
            return;

        // Deterministic synthetic post-call PCs are disjoint from real
        // source-block PCs [1, nodes.size()]. Sorting makes the result
        // independent of graph traversal and hash-table iteration.
        std::vector<const ScratchTransferSite*> sites;
        for (const auto& transfer : source.transfers.sites)
            if (transfer.kind == ScratchTransferKind::Call &&
                !transfer.procedureDefinitionId.empty())
                sites.push_back(&transfer);
        std::sort(sites.begin(), sites.end(),
            [](const auto* a, const auto* b) {
                return a->sourceBlockId < b->sourceBlockId;
            });
        for (const auto* transfer : sites) {
            const auto procedure = procedures_.idFor(
                transfer->procedureDefinitionId);
            const Block* call = source.script.getBlock(transfer->sourceBlockId);
            const Block* definition =
                source.script.getBlock(transfer->procedureDefinitionId);
            if (!call || !definition || !call->mutation || !procedure)
                return;
            const auto entry = pcFor(definition->next);
            const auto callPc = pcFor(transfer->sourceBlockId);
            const auto successor = resolveReturnTarget(
                source.script, *call);
            const auto caller = resolveCallerContext(source.script, *call);
            if (!entry || !callPc || !successor || !caller) return;
            call->mutation->parseArgs();
            const auto& argIds = call->mutation->argumentIds;
            if (argIds.size() > stack_.layout().argumentCapacity)
                return;
            const uint64_t returnPc =
                uint64_t(blocks.size()) + 1 + uint64_t(returnSites_.size());
            if (returnPc == CallFrameAbi::kNoPc ||
                !returnSites_.emplace(returnPc, SourceReturnSite{
                    returnPc, *callPc, successor->second, successor->first,
                    caller->procedureId, *procedure, *entry,
                    uint32_t(argIds.size()), caller->branches,
                    caller->loops
                }).second)
                return;
            if (!calls_.emplace(transfer->sourceBlockId, CallTarget{
                    *procedure, returnPc, *entry,
                    call->mutation->attr("proccode"),
                    uint32_t(argIds.size())}).second)
                return;
        }

        // Construct the decoder once per captured source. An exported
        // continuation shares the immutable dictionary and can decode all
        // callsites after this session has been destroyed.
        auto symbols = std::make_shared<SourceFrameSymbols>();
        symbols->blockIds = std::move(blocks);
        symbols->sourceTopology.resize(symbols->blockIds.size());
        for (size_t i = 0; i < symbols->blockIds.size(); ++i) {
            const Block* node =
                source.script.getBlock(symbols->blockIds[i]);
            if (!node) return;
            auto& topology = symbols->sourceTopology[i];
            const auto edgePc = [&](const std::string& id) -> uint64_t {
                if (id.empty()) return 0;
                const auto pc = pcFor(id);
                return pc ? *pc : 0;
            };
            topology.parentPc = edgePc(node->parent);
            topology.nextPc = edgePc(node->next);
            if (const auto* input = node->inputs.find("SUBSTACK"))
                topology.substackPc = edgePc(input->block);
            if (const auto* input = node->inputs.find("SUBSTACK2"))
                topology.substack2Pc = edgePc(input->block);
            if (node->opcode == "procedures_definition")
                topology.kind =
                    SourceFrameSymbols::SourceNodeKind::ProcedureDefinition;
            else if (node->opcode.starts_with("event_") ||
                     node->opcode == "control_start_as_clone")
                topology.kind = SourceFrameSymbols::SourceNodeKind::Hat;
            else if (isTrackedLoop(node->opcode))
                topology.kind =
                    SourceFrameSymbols::SourceNodeKind::TrackedLoop;
            else if (node->opcode == "control_if" ||
                     node->opcode == "control_if_else")
                topology.kind =
                    SourceFrameSymbols::SourceNodeKind::Conditional;
            else if (node->opcode == "control_all_at_once")
                topology.kind =
                    SourceFrameSymbols::SourceNodeKind::SubstackContainer;
        }
        symbols->statementOwners.assign(
            symbols->blockIds.size(), CallFrameAbi::kNoPc);
        // Traverse only actual executable NEXT/SUBSTACK branches, seeded
        // from the root hat and the known procedure definitions. Reporter
        // inputs, prototypes and unrelated script nodes are never stamped.
        // Each compiled source block is visited at most once: O(nodes +
        // executable edges), not O(nodes * lexical parent-chain depth).
        std::vector<std::pair<const Block*, uint64_t>> pending;
        pending.reserve(source.script.nodes.size());
        const Block* hat = source.script.getBlock(source.script.topId);
        if (!hat) return;
        pending.emplace_back(hat, uint64_t{0});
        for (uint64_t proc = 1; proc <= procedures_.size(); ++proc) {
            const auto* sourceDefId = procedures_.sourceFor(proc);
            const Block* def = sourceDefId
                ? source.script.getBlock(*sourceDefId) : nullptr;
            const Block* first = def && !def->next.empty()
                ? source.script.getBlock(def->next) : nullptr;
            if (!def || !first || first->parent != def->id ||
                def->opcode != "procedures_definition") return;
            pending.emplace_back(first, proc);
        }
        for (size_t index = 0; index < pending.size(); ++index) {
            const auto [node, owner] = pending[index];
            const auto pc = pcFor(node->id);
            if (!pc || *pc > symbols->statementOwners.size()) return;
            auto& assigned = symbols->statementOwners[size_t(*pc - 1)];
            if (assigned != CallFrameAbi::kNoPc) {
                if (assigned != owner) return; // inconsistent source graph
                continue;
            }
            assigned = owner;
            const auto enqueue = [&](const std::string& next) {
                if (next.empty()) return;
                const auto* child = source.script.getBlock(next);
                if (child && child->parent == node->id)
                    pending.emplace_back(child, owner);
            };
            enqueue(node->next);
            if (node->opcode == "control_if" ||
                node->opcode == "control_if_else" ||
                isTrackedLoop(node->opcode) ||
                node->opcode == "control_all_at_once") {
                const auto* branch = node->inputs.find("SUBSTACK");
                if (branch) enqueue(branch->block);
                if (node->opcode == "control_if_else") {
                    const auto* alternate = node->inputs.find("SUBSTACK2");
                    if (alternate) enqueue(alternate->block);
                }
            }
        }
        const auto rootPc = pcFor(source.script.topId);
        if (!rootPc) return;
        symbols->sourceRootPc = *rootPc;
        symbols->sourceRevision = compiledRevision_ ? compiledRevision_
                                                    : source.sourceRevision;
        symbols->sourceDigest = compiledDigest_;
        symbols->returns = returnSites_;
        symbols->loops.resize(loopSlots_.size());
        for (const auto& [block, loop] : loopSlots_) {
            const Block* node = source.script.getBlock(block);
            if (!node) return;
            const auto context = resolveCallerContext(source.script, *node);
            if (!context) return;
            symbols->loops[loop.slot] = {
                loop.sourcePc, loop.slot, loop.counted,
                context->procedureId, context->loops
            };
        }
        for (uint64_t i = 1; i <= procedures_.size(); ++i) {
            const auto* id = procedures_.sourceFor(i);
            if (!id) return;
            symbols->procedureIds.push_back(*id);
        }
        symbols->verifiedBlockIds =
            std::make_shared<const std::vector<std::string>>(
                symbols->blockIds);
        symbols->verifiedProcedureIds =
            std::make_shared<const std::vector<std::string>>(
                symbols->procedureIds);
        symbols->verifiedSourceIdentity =
            std::make_shared<const SourceFrameSymbols::SourceIdentity>(
                SourceFrameSymbols::SourceIdentity{
                    symbols->sourceRootPc, symbols->sourceRevision,
                    symbols->sourceDigest});

        // Build source-independent verified callsite proofs ONCE, before
        // exposing the immutable symbols. This work must not depend on
        // the returnSites_ values being checked: reconstruct from the
        // original graph and then compare against those values.
        auto verified = std::make_shared<
            SourceFrameSymbols::VerifiedCallsiteMap>();
        verified->reserve(returnSites_.size());
        for (const auto& [returnPc, site] : returnSites_) {
            const auto sourceCallId = symbols->block(site.callBlockPc);
            const auto sourceCall = sourceCallId
                ? calls_.find(std::string(*sourceCallId)) : calls_.end();
            const auto target =
                symbols->structuralReturnTarget(site.callBlockPc);
            auto caller =
                symbols->structuralCallerContext(site.callBlockPc);
            if (!sourceCallId || sourceCall == calls_.end() ||
                sourceCall->second.returnPc != returnPc ||
                sourceCall->second.procedure != site.calleeProcedureId ||
                sourceCall->second.entryPc != site.calleeEntryPc ||
                sourceCall->second.argCount != site.argumentCount ||
                site.returnPc != returnPc ||
                !target || !caller ||
                target->first != site.kind ||
                target->second != site.targetPc ||
                caller->procedureId != site.callerProcedureId ||
                caller->branches != site.branchPath ||
                caller->loops != site.callerLoopPath ||
                !verified->emplace(
                    site.callBlockPc,
                    SourceFrameSymbols::VerifiedCallsite{
                        target->first, target->second, std::move(*caller),
                        sourceCall->second.returnPc,
                        sourceCall->second.procedure,
                        sourceCall->second.entryPc,
                        sourceCall->second.argCount}
                ).second)
                return;
        }
        symbols->verifiedCallsites = std::move(verified);
        symbols_ = std::move(symbols);

        // The root script has the same portable source-loop local layout
        // as a procedure. It has zero arguments and no GC sidecar.
        // Previously its locals were zero, so a counted repeat enclosing
        // the first call lost the live counter needed for safe handoff.
        if (!stack_.push(0, CallFrameAbi::kNoPc, *rootPc, {},
                         stack_.layout().localCapacity))
            return;
        for (uint32_t i = 0; i < loopSlots_.size(); ++i)
            if (!stack_.setLocal(i, CallFrameAbi::kNoLoopCounter))
                return;
        if (!stack_.setLocal(
                uint32_t(2 * loopSlots_.size() + 1),
                uint64_t(SourceResumePhase::UnknownBody)))
            return;
        enabled_ = true;
    }

    bool enabled() const { return enabled_; }
    uint64_t compiledRevision() const { return compiledRevision_; }
    bool sourceUnchanged() const {
        // Fast per-call/yield check; the deep source check is performed
        // only at an actual independent handoff.
        return enabled_ && sourceBlocks_ && compiledRevision_ &&
               sourceBlocks_->revision() == compiledRevision_;
    }
    bool sourceContentMatches() const {
        if (!sourceUnchanged() || !compiledDigest_) return false;
        const Block* top = sourceBlocks_->getBlock(sourceTopId_);
        if (!top) return false;
        const auto current = ScratchCompilationUnit::capture(
            *const_cast<Blocks*>(sourceBlocks_), top);
        return current.valid() &&
            current.sourceRevision == compiledRevision_ &&
            scratchSourceDigest(current) == compiledDigest_;
    }
    void invalidate(const char* reason) { if (enabled_) disable(reason); }
    uint32_t depth() const { return stack_.depth(); }
    uint64_t calls() const { return callsMade_; }
    uint64_t returns() const { return returnsMade_; }
    uint64_t yields() const { return yieldsMade_; }
    uint32_t maxDepth() const { return maxDepth_; }
    const ScratchCallStack& frames() const { return stack_; }
    std::size_t liveHeapRoots() const { return rootedArgs_.liveHeapRoots(); }

    // Return-PC decode is purely structural and is safe to use after
    // removing the original LLVM Fiber. Execution of a LoopLatch or
    // AfterBranch still requires a correct SSA/effect continuation.
    std::optional<SourceReturnSite> returnSite(uint64_t returnPc) const {
        if (!enabled_) return std::nullopt;
        const auto it = returnSites_.find(returnPc);
        if (it == returnSites_.end() || it->second.returnPc != returnPc)
            return std::nullopt;
        return it->second;
    }

    // Create an independently GC-rooted copy of the entire active procedure
    // chain, not just the leaf sourceCheckpoint(). RT-001 can take ownership
    // at a scheduler safe point without retaining any Fiber stack pointers.
    // The image is a typed CallFrame/argument transfer, NOT an executable
    // continuation: arbitrary SSA temporaries and branch/effect PCs are
    // still absent, so a caller must not resume a native frame from it yet.
    std::unique_ptr<RootedScratchCallStack> exportContinuation() const {
        // A legacy frame without embedded compile revision remains an
        // observation-only mirror; never mark it transferable to RT-001.
        if (!sourceContentMatches() || !stack_.validateImage() ||
            stack_.depth() != rootedArgs_.depth() + 1)
            return {};

        auto image = std::make_unique<RootedScratchCallStack>(
            stack_.layout());
        if (!image->valid()) return {};
        const auto* source = stack_.words();
        const size_t stride = stack_.layout().strideWords();
        const uint32_t argCapacity = stack_.layout().argumentCapacity;
        for (uint32_t index = 0; index < stack_.depth(); ++index) {
            const uint64_t* raw = source + size_t(index) * stride;
            const uint64_t procedureId = raw[CallFrameAbi::kProcedureIdWord];
            const uint64_t returnPc = raw[CallFrameAbi::kReturnPcWord];
            const uint64_t resumePc = raw[CallFrameAbi::kResumePcWord];
            const uint64_t argumentCount = raw[CallFrameAbi::kArgumentCountWord];
            const uint64_t localCount = raw[CallFrameAbi::kLocalCountWord];
            if (argumentCount > argCapacity ||
                localCount > stack_.layout().localCapacity ||
                (index == 0 && argumentCount != 0))
                return {};

            // The child's return site must be the parent frame's saved
            // continuation, even when multiple distinct callsites invoke
            // the same procedure or the call is recursive.
            if (index &&
                (source[(size_t(index) - 1) * stride +
                         CallFrameAbi::kResumePcWord] != returnPc ||
                 !returnSite(returnPc)))
                return {};

            std::vector<uint64_t> values;
            values.reserve(size_t(argumentCount));
            for (uint32_t arg = 0; arg < argumentCount; ++arg) {
                // The root frame intentionally has no sidecar.
                const auto borrowed = rootedArgs_.argument(index - 1, arg);
                if (!borrowed) return {};
                const uint64_t numericBits =
                    raw[CallFrameAbi::argumentWord(arg)];
                if (borrowed->isString() && !borrowed->isEmptyString()) {
                    if (numericBits != val::kUndefined) return {};
                } else if (numericBits != borrowed->bits) {
                    return {};
                }
                values.push_back(borrowed->bits);
            }
            if (!image->push(procedureId, returnPc, resumePc,
                             values, uint32_t(localCount)) ||
                !image->setResumePc(resumePc))
                return {};

            // All published locals are source loop counters, loop-PCs and
            // depth, not GC-bearing Value pointers. Preserve every word.
            for (uint32_t local = 0; local < localCount; ++local) {
                const uint64_t bits = raw[CallFrameAbi::localWord(
                    argCapacity, local)];
                if (!image->setNumericLocal(local, bits)) return {};
            }
        }
        if (!image->valid() || !image->frames().validateImage() ||
            image->depth() != stack_.depth())
            return {};
        return image;
    }

    // Carry the return-site/procedure/source-PC dictionary with the
    // independently rooted frame arena. No table entry owns Block* or
    // requires access to the native JIT or a mutable Blocks container.
    std::unique_ptr<SelfContainedSourceContinuation>
    exportSelfContained() const {
        if (!symbols_) return {};
        auto transfer = std::make_unique<SelfContainedSourceContinuation>();
        transfer->frames = exportContinuation();
        transfer->symbols = symbols_;
        if (!transfer->valid()) return {};
        // Diagnostic only: a matching lexical counter checkpoint is not
        // proof of complete LLVM SSA liveness or effect continuity.
        if (loopSsaDiagnostic_) {
            const auto leaf = stack_.top();
            if (leaf && loopSsaDiagnostic_->procedureId == leaf->procedureId &&
                loopSsaDiagnostic_->resumePc == leaf->resumePc)
                transfer->diagnosticSsa = loopSsaDiagnostic_;
        }
        return transfer;
    }

    // This is used by the explicit native export regression probe.
    // Images are independently rooted and may survive after their source
    // frames have popped; the probe only compares their original snapshot.
    bool matchesExport(const RootedScratchCallStack& image) const {
        if (!sourceContentMatches() || !image.valid() ||
            image.depth() != stack_.depth() ||
            image.frames().layout() != stack_.layout() ||
            !image.frames().validateImage())
            return false;

        const auto* source = stack_.words();
        const auto* copied = image.frames().words();
        const size_t stride = stack_.layout().strideWords();
        for (uint32_t index = 0; index < stack_.depth(); ++index) {
            const size_t base = size_t(index) * stride;
            const uint32_t n = uint32_t(
                source[base + CallFrameAbi::kArgumentCountWord]);
            if (n != copied[base + CallFrameAbi::kArgumentCountWord])
                return false;
            for (uint32_t word = 0; word < stack_.layout().strideWords(); ++word) {
                if (word >= CallFrameAbi::kHeaderWords &&
                    word < CallFrameAbi::kHeaderWords +
                           stack_.layout().argumentCapacity)
                    continue;
                if (source[base + word] != copied[base + word])
                    return false;
            }
            for (uint32_t arg = 0; arg < n; ++arg) {
                if (index == 0) return false;
                const auto original = rootedArgs_.argument(index - 1, arg);
                const auto exported = image.argument(index, arg);
                if (!original || !exported || *original != *exported)
                    return false;
            }
        }
        return true;
    }

    // The root frame has no arguments. Procedure frame N owns sidecar N-1.
    // Never use ScratchCallStack::argument() to decode heap-backed values:
    // those slots contain an intentionally non-pointer placeholder.
    std::optional<Value> argument(uint32_t frameIndex,
                                  uint32_t argumentIndex) const {
        if (!enabled_ || frameIndex == 0 ||
            stack_.depth() != rootedArgs_.depth() + 1 ||
            frameIndex >= stack_.depth()) return std::nullopt;
        return rootedArgs_.argument(frameIndex - 1, argumentIndex);
    }

    // Executed before xyo_proc_push, because that helper may suspend
    // immediately for recursive nonwarp Scratch calls.
    void enter(const char* blockId, const char* proccode,
               const uint64_t* args, uint32_t count) {
        loopSsaDiagnostic_.reset();
        if (!enabled_) return;
        if (sourceBlocks_ && compiledRevision_ && !sourceUnchanged()) {
            disable("compiled-source-changed");
            return;
        }
        if (!blockId || !proccode ||
            count > stack_.layout().argumentCapacity ||
            (count && !args)) {
            disable("invalid-arguments");
            return;
        }
        const auto it = calls_.find(blockId);
        if (it == calls_.end() || it->second.proccode != proccode ||
            it->second.argCount != count) {
            disable("callsite-mismatch");
            return;
        }
        // The portable frame is a pointer-free numeric arena. The sidecar
        // retains real Scratch Values as exact GC roots through every yield.
        // Reject unknown tagged values rather than mistaking them for SSA.
        std::vector<uint64_t> safeArgs;
        safeArgs.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const Value value{args[i]};
            if (!(value.isNumber() || value.isString() || value.isBool() ||
                  value.isColor() || value.isUndefined())) {
                disable("unsupported-argument-type");
                return;
            }
            safeArgs.push_back(value.isString() && !value.isEmptyString()
                                   ? val::kUndefined : args[i]);
        }
        const auto& call = it->second;
        const auto site = returnSite(call.returnPc);
        const auto caller = stack_.top();
        if (!site || !caller ||
            caller->procedureId != site->callerProcedureId) {
            disable("wrong-callsite-owner");
            return;
        }
        if (stack_.depth() >= stack_.layout().maximumDepth) {
            disable("stack-overflow");
            return;
        }
        if (stack_.depth() != rootedArgs_.depth() + 1) {
            disable("unbalanced-root-frames");
            return;
        }
        // A callsite outside all lexical loops must NOT inherit a
        // completed loop path from an earlier yield of the same procedure.
        // Calls inside a loop must agree with the source-scoped checkpoint
        // emitted immediately before xyo_proc_push.
        {
            const auto& required = returnSites_.at(call.returnPc).callerLoopPath;
            const uint32_t n = uint32_t(loopSlots_.size());
            if (required.empty()) {
                for (uint32_t i = 0; i < n; ++i) {
                    if (!stack_.setLocal(i, CallFrameAbi::kNoLoopCounter) ||
                        !stack_.setLocal(n + i, 0)) {
                        disable("failed-clear-completed-loop-path");
                        return;
                    }
                }
                if (!stack_.setLocal(2 * n, 0)) {
                    disable("failed-clear-completed-loop-depth");
                    return;
                }
            } else {
                const auto depth = stack_.local(2 * n);
                if (!depth || *depth != required.size()) {
                    disable("missing-callsite-loop-checkpoint");
                    return;
                }
                for (uint32_t i = 0; i < required.size(); ++i) {
                    const auto pc = stack_.local(n + i);
                    if (!pc || *pc != required[i]) {
                        disable("callsite-lexical-loop-mismatch");
                        return;
                    }
                }
            }
        }
        const uint32_t phaseSlot = stack_.layout().localCapacity - 1;
        if (!stack_.setResumePc(call.returnPc) ||
            !stack_.setLocal(phaseSlot,
                uint64_t(SourceResumePhase::AwaitingCallee))) {
            disable("failed-resume-pc");
            return;
        }
        if (!rootedArgs_.push(std::span<const uint64_t>(args, count))) {
            disable("failed-root-arguments");
            return;
        }
        if (!stack_.push(call.procedure, call.returnPc, call.entryPc,
                         safeArgs, stack_.layout().localCapacity)) {
            rootedArgs_.pop();
            disable("failed-push");
            return;
        }
        // Unknown state for each lexical loop until its own checkpoint.
        // Different nested loops, including recursive invocations of the
        // same code, never overwrite another loop's saved counter.
        for (uint32_t i = 0; i < stack_.layout().localCapacity; ++i) {
            const uint64_t initial = i < loopSlots_.size()
                ? CallFrameAbi::kNoLoopCounter : 0;
            if (!stack_.setLocal(i, initial)) {
                disable("failed-initialize-loop-slots");
                return;
            }
        }
        ++callsMade_;
        if (visitedReturnSites_.insert(call.returnPc).second) {
            const auto kind = returnSites_.at(call.returnPc).kind;
            switch (kind) {
            case SourceReturnKind::NextBlock: break;
            case SourceReturnKind::AfterBranch: ++branchReturnSites_; break;
            case SourceReturnKind::LoopLatch: ++loopReturnSites_; break;
            case SourceReturnKind::ProcedureExit: ++procedureReturnSites_; break;
            case SourceReturnKind::ScriptExit: ++scriptReturnSites_; break;
            }
        }
        maxDepth_ = std::max(maxDepth_, stack_.depth());
        maxHeapRoots_ = std::max(maxHeapRoots_, rootedArgs_.liveHeapRoots());
    }

    // Every generated Scratch stack statement starts with this opt-in
    // guard, BEFORE evaluating reporters or committing effects. A clean
    // loop latch or post-call phase applies to the exact source boundary
    // where it was published, not the remainder of the native function.
    // Without this transition, an unrelated later wait/yield could export
    // the *previous* safe loop PC and trigger duplicate side effects on
    // deoptimization. Keep the last PC as an observational provenance
    // marker, but prohibit executable recovery via UnknownBody.
    void statementStarted(const char* blockId) {
        loopSsaDiagnostic_.reset();
        if (!enabled_ || stack_.empty()) return;
        const auto pc = blockId ? pcFor(blockId) : std::nullopt;
        const auto executing = stack_.top();
        if (!pc || !executing || !symbols_ ||
            *pc > symbols_->statementOwners.size() ||
            symbols_->statementOwners[size_t(*pc - 1)] !=
                executing->procedureId) {
            // A valid source PC is not sufficient: it may belong to a
            // different Scratch procedure or an unexecutable reporter.
            // A false recovery must not be allowed to invent ownership.
            disable("unrecognized-or-foreign-statement");
            return;
        }
        const uint32_t phaseSlot = stack_.layout().localCapacity - 1;
        const auto phase = stack_.local(phaseSlot);
        if (!phase || !validSourceResumePhase(*phase) ||
            *phase == uint64_t(SourceResumePhase::AwaitingCallee)) {
            disable("invalid-statement-boundary-phase");
            return;
        }
        if (*phase != uint64_t(SourceResumePhase::UnknownBody) &&
            !stack_.setLocal(phaseSlot,
                             uint64_t(SourceResumePhase::UnknownBody)))
            disable("failed-statement-boundary-phase");
    }

    // The push helper can yield before the callee body starts. Once that
    // helper returns, any subsequent non-loop yield must NOT masquerade as
    // a clean procedure-entry checkpoint: arbitrary SSA/effects might have
    // executed and a machine-PC-free replay would duplicate them.
    void bodyStarted() {
        loopSsaDiagnostic_.reset();
        if (!enabled_ || stack_.depth() <= 1) return;
        const uint32_t phaseSlot = stack_.layout().localCapacity - 1;
        const auto old = stack_.local(phaseSlot);
        if (!old || *old != uint64_t(SourceResumePhase::ProcedureEntry) ||
            !stack_.setLocal(phaseSlot,
                             uint64_t(SourceResumePhase::UnknownBody)))
            disable("invalid-procedure-body-entry");
    }

    // A whole lexical loop path is captured at ONE handoff: outermost to
    // innermost. Previously only the innermost repeat counter was saved,
    // making recovery of a yield in nested loops impossible.
    //
    // Local ABI (per frame):
    //   [0,N):    each lexical loop's counter, or kNoLoopCounter
    //   [N,2*N):  ordered active source-loop PCs, 0 for unused entries
    //   [2*N]:    count of active lexical loops (raw unsigned integer)
    //   [2*N+1]:  SourceResumePhase (raw integer)
    // All numbers are pointer-free and can be decoded without a Fiber.
    void checkpointPath(const char* const* blockIds,
                        const uint64_t* counterBits, uint32_t count,
                        SourceResumePhase phase =
                            SourceResumePhase::AfterLoopBody) {
        loopSsaDiagnostic_.reset();
        if (!enabled_ || stack_.empty()) return;
        if (count == 0 || count > kMaxTrackedLoops ||
            count > loopSlots_.size() || !blockIds || !counterBits ||
            (phase != SourceResumePhase::InsideLoopBody &&
             phase != SourceResumePhase::AfterLoopBody)) {
            disable("invalid-loop-path");
            return;
        }
        std::array<uint32_t, kMaxTrackedLoops> slots{};
        std::array<uint64_t, kMaxTrackedLoops> pcs{};
        for (uint32_t i = 0; i < count; ++i) {
            if (!blockIds[i]) {
                disable("missing-path-block-id");
                return;
            }
            const auto pc = pcFor(blockIds[i]);
            const auto loop = loopSlots_.find(blockIds[i]);
            if (!pc || loop == loopSlots_.end() ||
                (counterBits[i] != CallFrameAbi::kNoLoopCounter &&
                 (!loop->second.counted ||
                  !Value{counterBits[i]}.isNumber()))) {
                disable("invalid-path-entry");
                return;
            }
            for (uint32_t j = 0; j < i; ++j) {
                if (slots[j] == loop->second.slot) {
                    disable("duplicate-path-loop");
                    return;
                }
            }
            // Source block IDs alone are insufficient: a corrupted
            // checkpoint could splice loops from other procedures or omit
            // an enclosing lexical loop while retaining valid PC numbers.
            const auto& lexical = symbols_->loops[loop->second.slot];
            const auto current = stack_.top();
            if (!current ||
                lexical.ownerProcedureId != current->procedureId ||
                lexical.enclosingLoops.size() != i) {
                disable("checkpoint-loop-lexical-owner");
                return;
            }
            for (uint32_t ancestor = 0; ancestor < i; ++ancestor) {
                if (lexical.enclosingLoops[ancestor] != pcs[ancestor]) {
                    disable("checkpoint-loop-lexical-path");
                    return;
                }
            }
            slots[i] = loop->second.slot;
            pcs[i] = *pc;
        }
        const uint32_t n = uint32_t(loopSlots_.size());
        // No scheduler handoff occurs while these locals are published.
        // Validate the whole path before mutating a frame (fail closed).
        for (uint32_t i = 0; i < n; ++i) {
            if (!stack_.setLocal(i, CallFrameAbi::kNoLoopCounter) ||
                !stack_.setLocal(n + i, i < count ? pcs[i] : 0)) {
                disable("failed-path-slot");
                return;
            }
        }
        uint32_t live = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (!stack_.setLocal(slots[i], counterBits[i])) {
                disable("failed-counter-slot");
                return;
            }
            if (counterBits[i] != CallFrameAbi::kNoLoopCounter) {
                ++live;
                if (i + 1 < count && stack_.depth() > 1) {
                    ++parentCounterSnapshots_;
                    const double parent = Value{counterBits[i]}.asNumberUnchecked();
                    if (std::isfinite(parent) && parent >= 0.0 &&
                        parent <= 1000000.0 && std::trunc(parent) == parent &&
                        parentCounterSum_ <= UINT64_MAX - uint64_t(parent))
                        parentCounterSum_ += uint64_t(parent);
                }
            }
        }
        if (!stack_.setLocal(2 * n, count) ||
            !stack_.setLocal(2 * n + 1, uint64_t(phase)) ||
            !stack_.setResumePc(pcs[count - 1])) {
            disable("failed-commit-path");
            return;
        }
        // Diagnostic-only readback of the compiled IDs and counters from
        // the actual CallFrame. This is NOT executable deopt proof.
        if (traceLoopPathReadback_) {
            const auto proof = verifyLoopPathReadback(
                blockIds, counterBits, count, phase);
            std::string ids;
            for (uint32_t i = 0; i < count; ++i) {
                if (!ids.empty()) ids += ',';
                ids += blockIds[i] ? blockIds[i] : "(null)";
            }
            std::fprintf(stderr,
                "[ir001-runtime-loop-path] block=%s phase=%u "
                "loop_path_depth=%u loop_path_ids=%s "
                "readback=%d counted=%u uncounted=%u "
                "gc_root_proven=0 source_complete=0 executable=0\n",
                blockIds[count - 1] ? blockIds[count - 1] : "(null)",
                unsigned(phase), count, ids.c_str(), int(bool(proof)),
                proof ? proof->counted : 0,
                proof ? proof->uncounted : 0);
            // Missing proof is a diagnostic gap, NOT a reason to stop
            // the native Fiber or revoke an otherwise valid mirror.
            // The generic interpreter deopt executor remains gated.
        }
        // Snapshot actual LLVM-emitted numeric counter words at the
        // checkpoint. IDs are lexical counter slots, not arbitrary SSA IDs.
        // Only enabled when explicitly requested to avoid hot-path overhead.
        if (loopSsaDiagnosticEnabled_ && symbols_ &&
            symbols_->sourceRevision && symbols_->sourceDigest) {
            const auto leaf = stack_.top();
            if (leaf) {
                SourceSsaManifest manifest{
                    symbols_->sourceRevision, symbols_->sourceDigest.lo,
                    symbols_->sourceDigest.hi, leaf->procedureId,
                    leaf->resumePc, {}};
                std::vector<uint64_t> values;
                for (uint32_t slot = 0; slot < n; ++slot) {
                    const auto value = stack_.local(slot);
                    if (value && *value != CallFrameAbi::kNoLoopCounter) {
                        manifest.slots.push_back({slot, 64});
                        values.push_back(*value);
                    }
                }
                loopSsaDiagnostic_ = SourceSsaSnapshot::capture(
                    manifest, values);
            }
        }
        // Preserve historical child-procedure mirror statistics. Root
        // loop counters are real ABI data, but they must not inflate the
        // nonroot checkpoint/yield metrics used by parity regressions.
        if (stack_.depth() == 1) return;
        maxPathDepth_ = std::max(maxPathDepth_, count);
        maxCounterSlots_ = std::max(maxCounterSlots_, live);
        const auto innermost = counterBits[count - 1];
        if (innermost != CallFrameAbi::kNoLoopCounter) {
            ++counterCheckpoints_;
            const double value = Value{innermost}.asNumberUnchecked();
            if (std::isfinite(value) && value >= 0.0 &&
                value <= 1000000.0 && std::trunc(value) == value &&
                counterSum_ <= UINT64_MAX - uint64_t(value))
                counterSum_ += uint64_t(value);
        }
        ++checkpoints_;
        lastCheckpoint_ = pcs[count - 1];
    }

    void checkpoint(const char* blockId, uint64_t counterBits) {
        const char* path[] = {blockId};
        checkpointPath(path, &counterBits, 1);
    }

    // Read-only, pointer-free resume path from the latest published frame.
    // Does not imply that arbitrary LLVM SSA values are reconstructible.
    std::optional<SourceProcedureCheckpoint> sourceCheckpoint() const {
        if (!enabled_ || stack_.empty() ||
            stack_.depth() != rootedArgs_.depth() + 1)
            return std::nullopt;
        const auto frame = stack_.top();
        const auto n = uint32_t(loopSlots_.size());
        const auto count = stack_.local(2 * n);
        const auto phase = stack_.local(2 * n + 1);
        if (!frame || frame->locals != 2 * n + 2 ||
            !count || *count > n || !phase ||
            !validSourceResumePhase(*phase)) return std::nullopt;

        SourceProcedureCheckpoint result{
            *frame, {}, static_cast<SourceResumePhase>(*phase)};
        result.loops.reserve(*count);
        for (uint32_t i = 0; i < *count; ++i) {
            const auto pc = stack_.local(n + i);
            if (!pc || !*pc) return std::nullopt;
            // The table is bounded by kMaxTrackedLoops (32), so this
            // short lookup is only done when a caller asks for a snapshot.
            const auto it = std::find_if(loopSlots_.begin(), loopSlots_.end(),
                [&](const auto& entry) { return entry.second.sourcePc == *pc; });
            if (it == loopSlots_.end()) return std::nullopt;
            const auto counter = stack_.local(it->second.slot);
            if (!counter) return std::nullopt;
            result.loops.push_back({*pc, *counter});
        }
        if (!result.loops.empty() &&
            (result.phase == SourceResumePhase::InsideLoopBody ||
             result.phase == SourceResumePhase::AfterLoopBody) &&
            result.loops.back().sourcePc != frame->resumePc)
            return std::nullopt;
        return result;
    }

    // IR-001: compare one LLVM checkpoint's path and exact numeric words
    // with the saved runtime CallFrame. This does NOT restore SSA or GC state.
    struct LoopPathReadback {
        uint32_t depth = 0;
        uint32_t counted = 0;
        uint32_t uncounted = 0;
    };
    std::optional<LoopPathReadback> verifyLoopPathReadback(
        const char* const* compiledIds, const uint64_t* compiledBits,
        uint32_t depth, SourceResumePhase phase) const {
        if (!enabled_ || !symbols_ || !symbols_->verifiedSourceIdentity ||
            symbols_->verifiedSourceIdentity->rootPc != symbols_->sourceRootPc ||
            symbols_->verifiedSourceIdentity->revision != symbols_->sourceRevision ||
            symbols_->verifiedSourceIdentity->digest != symbols_->sourceDigest ||
            !compiledIds || !compiledBits || depth == 0 ||
            depth > kMaxTrackedLoops ||
            (phase != SourceResumePhase::InsideLoopBody &&
             phase != SourceResumePhase::AfterLoopBody))
            return std::nullopt;
        const auto saved = sourceCheckpoint();
        if (!saved || saved->phase != phase ||
            saved->loops.size() != depth ||
            saved->frame.resumePc != saved->loops.back().sourcePc)
            return std::nullopt;
        LoopPathReadback result{depth, 0, 0};
        for (uint32_t i = 0; i < depth; ++i) {
            if (!compiledIds[i] || !compiledIds[i][0])
                return std::nullopt;
            const auto& stored = saved->loops[i];
            const auto name = symbols_->block(stored.sourcePc);
            const auto expectedPc = pcFor(compiledIds[i]);
            if (!name || *name != compiledIds[i] ||
                !expectedPc || stored.sourcePc != *expectedPc ||
                stored.counterBits != compiledBits[i])
                return std::nullopt;
            const auto it = std::find_if(symbols_->loops.begin(),
                symbols_->loops.end(), [&](const auto& loop) {
                    return loop.sourcePc == stored.sourcePc;
                });
            if (it == symbols_->loops.end() ||
                it->ownerProcedureId != saved->frame.procedureId ||
                it->enclosingLoops.size() != i)
                return std::nullopt;
            for (uint32_t j = 0; j < i; ++j)
                if (it->enclosingLoops[j] != saved->loops[j].sourcePc)
                    return std::nullopt;
            if (it->counted) {
                if (stored.counterBits == CallFrameAbi::kNoLoopCounter ||
                    !Value{stored.counterBits}.isNumber())
                    return std::nullopt;
                ++result.counted;
            } else {
                if (stored.counterBits != CallFrameAbi::kNoLoopCounter)
                    return std::nullopt;
                ++result.uncounted;
            }
        }
        return result;
    }

    std::optional<std::vector<uint64_t>> activeLoopPcs() const {
        const auto checkpoint = sourceCheckpoint();
        if (!checkpoint) return std::nullopt;
        std::vector<uint64_t> path;
        path.reserve(checkpoint->loops.size());
        for (const auto& loop : checkpoint->loops)
            path.push_back(loop.sourcePc);
        return path;
    }

    uint64_t checkpoints() const { return checkpoints_; }
    uint64_t lastCheckpoint() const { return lastCheckpoint_; }

    void leave() {
        loopSsaDiagnostic_.reset();
        if (!enabled_) return;
        if (stack_.depth() <= 1 ||
            stack_.depth() != rootedArgs_.depth() + 1 ||
            !stack_.pop()) {
            disable("failed-pop");
            return;
        }
        if (!rootedArgs_.pop()) {
            disable("failed-root-pop");
            return;
        }
        // The parent has completed the callee and now stands at its
        // synthetic post-call PC, not at the loop body checkpoint.
        if (!stack_.setLocal(stack_.layout().localCapacity - 1,
                             uint64_t(SourceResumePhase::AfterCallee))) {
            disable("failed-caller-return-phase");
            return;
        }
        ++returnsMade_;
    }

    // A suspended LLVM Fiber retains its machine PC and SSA registers.
    // The common CallFrame remains stable as observable procedure metadata.
    // It is not (yet) the instruction-level resumption mechanism.
    void beforeYield() {
        // The existing native mirror reports yields only while a
        // procedure activation is live. A root-only loop is retained in
        // the numeric root frame but is not an executable RT handoff yet.
        if (!enabled_ || stack_.depth() <= 1) return;
        if (sourceBlocks_ && compiledRevision_ && !sourceUnchanged()) {
            disable("compiled-source-changed");
            return;
        }
        if (stack_.depth() != rootedArgs_.depth() + 1) {
            disable("yield-root-depth-mismatch");
            return;
        }
        const auto leaf = sourceCheckpoint();
        if (!leaf) {
            disable("invalid-source-checkpoint");
            return;
        }
        if (probeExport_) {
            probeImage_ = exportSelfContained();
            if (!probeImage_ || !probeImage_->valid() ||
                !matchesExport(*probeImage_->frames)) {
                disable("failed-export-continuation");
                return;
            }
            const auto decoded = probeImage_->decodeFrames();
            if (!decoded || decoded->empty() ||
                decoded->back().phase != leaf->phase ||
                decoded->back().loops != leaf->loops) {
                disable("failed-decode-source-phase");
                return;
            }
            if (!decoded->front().loops.empty()) {
                ++exportedRootLoopFrames_;
                exportedRootMaxDepth_ = std::max(
                    exportedRootMaxDepth_,
                    uint32_t(decoded->front().loops.size()));
                bool counted = false;
                for (const auto& loop : decoded->front().loops) {
                    if (loop.counterBits == CallFrameAbi::kNoLoopCounter)
                        continue;
                    const Value value{loop.counterBits};
                    if (!value.isNumber()) {
                        disable("invalid-root-counter");
                        return;
                    }
                    counted = true;
                    const double number = value.asNumberUnchecked();
                    // The sum is for deterministic regression only;
                    // out-of-range values stay in the exact frame bits
                    // and are not guessed or rounded by this observer.
                    if (std::isfinite(number) && number >= 0.0 &&
                        number <= 1000000.0 &&
                        std::trunc(number) == number &&
                        exportedRootCounterSum_ <=
                            UINT64_MAX - uint64_t(number))
                        exportedRootCounterSum_ += uint64_t(number);
                }
                if (counted) ++exportedRootCountedFrames_;
            }
            for (size_t frame = 1; frame + 1 < decoded->size(); ++frame) {
                if ((*decoded)[frame].phase !=
                    SourceResumePhase::AwaitingCallee) {
                    disable("parent-not-awaiting-callee");
                    return;
                }
                ++exportedAwaitingFrames_;
                if (!(*decoded)[frame].loops.empty())
                    ++exportedAwaitingLoopFrames_;
            }
            switch (leaf->phase) {
            case SourceResumePhase::ProcedureEntry: ++exportedEntryYields_; break;
            case SourceResumePhase::AfterLoopBody: ++exportedAfterBodyYields_; break;
            case SourceResumePhase::AfterCallee: ++exportedAfterCalleeYields_; break;
            case SourceResumePhase::UnknownBody: ++exportedUnknownBodyYields_; break;
            case SourceResumePhase::InsideLoopBody:
            case SourceResumePhase::AwaitingCallee:
                disable("unsupported-leaf-yield-phase");
                return;
            }
            ++exportsMade_;
            maxExportHeapRoots_ = std::max(
                maxExportHeapRoots_, probeImage_->frames->rootedArguments());
            maxExportDepth_ = std::max(
                maxExportDepth_, probeImage_->frames->depth());
            // Check how many ancestors were captured inside their own
            // source loop. A nested recursive yield must preserve the
            // caller's repeat count, not merely the callee's loop state.
            const auto n = uint32_t(loopSlots_.size());
            const auto& imageFrames = probeImage_->frames->frames();
            const auto* words = imageFrames.words();
            const auto stride = imageFrames.layout().strideWords();
            for (uint32_t f = 1; f + 1 < probeImage_->frames->depth(); ++f) {
                const auto depthWord = CallFrameAbi::localWord(
                    imageFrames.layout().argumentCapacity, 2 * n);
                const uint64_t pathDepth =
                    words[size_t(f) * stride + depthWord];
                if (pathDepth > n) {
                    disable("invalid-ancestor-path");
                    return;
                }
                if (pathDepth) ++exportedAncestorLoopFrames_;
            }
            // Source branch paths describe the callsites that actually
            // reached this native yield, not statically visited branches.
            for (size_t f = 1; f < decoded->size(); ++f) {
                const auto& caller = (*decoded)[f].caller;
                if (!caller) {
                    disable("missing-source-caller");
                    return;
                }
                if (caller->branchPath.size() > 1)
                    ++exportedNestedBranches_;
                for (const auto& arm : caller->branchPath) {
                    if (arm.arm == 1) ++exportedThenBranches_;
                    else if (arm.arm == 2) ++exportedElseBranches_;
                }
            }
        }
        suspendedDepth_ = stack_.depth();
        suspendedRevision_ = stack_.revision();
        ++yieldsMade_;
    }
    void afterYield() {
        if (!enabled_ || !suspendedDepth_) return;
        if (stack_.depth() != suspendedDepth_ ||
            stack_.depth() != rootedArgs_.depth() + 1 ||
            stack_.revision() != suspendedRevision_ ||
            !stack_.validateImage() ||
            (probeExport_ && (!probeImage_ ||
                              !probeImage_->valid() ||
                              !matchesExport(*probeImage_->frames))))
            disable("yield-state-changed");
        probeImage_.reset();
        suspendedDepth_ = 0;
    }

    void report(const char* scriptId) const {
        std::fprintf(stderr,
            "[jit-callframe] script=%s calls=%llu returns=%llu "
            "yields=%llu max_depth=%u enabled=%d remaining=%u reason=%s "
            "checkpoints=%llu last_pc=%llu counters=%llu counter_sum=%llu "
            "max_counter_slots=%u heap_roots=%zu rooted_frames=%zu "
            "peak_heap_roots=%zu path_depth=%u "
            "parent_counters=%llu parent_counter_sum=%llu "
            "exports=%llu max_export_roots=%zu "
            "max_export_depth=%u ancestor_loop_frames=%llu "
            "return_sites=%zu loop_returns=%llu branch_returns=%llu "
            "proc_returns=%llu script_returns=%llu "
            "phase_entry=%llu phase_after_body=%llu "
            "phase_after_callee=%llu awaiting_frames=%llu "
            "awaiting_loop_frames=%llu phase_unknown=%llu "
            "branch_then=%llu branch_else=%llu nested_branch=%llu "
            "root_loop_frames=%llu root_counted_frames=%llu "
            "root_counter_sum=%llu root_max_depth=%u\n",
            scriptId ? scriptId : "",
            static_cast<unsigned long long>(callsMade_),
            static_cast<unsigned long long>(returnsMade_),
            static_cast<unsigned long long>(yieldsMade_),
            maxDepth_, int(enabled_), stack_.depth(), reason_,
            static_cast<unsigned long long>(checkpoints_),
            static_cast<unsigned long long>(lastCheckpoint_),
            static_cast<unsigned long long>(counterCheckpoints_),
            static_cast<unsigned long long>(counterSum_),
            maxCounterSlots_, rootedArgs_.liveHeapRoots(),
            rootedArgs_.depth(), maxHeapRoots_, maxPathDepth_,
            static_cast<unsigned long long>(parentCounterSnapshots_),
            static_cast<unsigned long long>(parentCounterSum_),
            static_cast<unsigned long long>(exportsMade_), maxExportHeapRoots_,
            maxExportDepth_,
            static_cast<unsigned long long>(exportedAncestorLoopFrames_),
            visitedReturnSites_.size(),
            static_cast<unsigned long long>(loopReturnSites_),
            static_cast<unsigned long long>(branchReturnSites_),
            static_cast<unsigned long long>(procedureReturnSites_),
            static_cast<unsigned long long>(scriptReturnSites_),
            static_cast<unsigned long long>(exportedEntryYields_),
            static_cast<unsigned long long>(exportedAfterBodyYields_),
            static_cast<unsigned long long>(exportedAfterCalleeYields_),
            static_cast<unsigned long long>(exportedAwaitingFrames_),
            static_cast<unsigned long long>(exportedAwaitingLoopFrames_),
            static_cast<unsigned long long>(exportedUnknownBodyYields_),
            static_cast<unsigned long long>(exportedThenBranches_),
            static_cast<unsigned long long>(exportedElseBranches_),
            static_cast<unsigned long long>(exportedNestedBranches_),
            static_cast<unsigned long long>(exportedRootLoopFrames_),
            static_cast<unsigned long long>(exportedRootCountedFrames_),
            static_cast<unsigned long long>(exportedRootCounterSum_),
            exportedRootMaxDepth_);
    }

private:
    struct CallerContext {
        uint64_t procedureId = 0;
        std::vector<SourceConditionalArm> branches;
        std::vector<uint64_t> loops;
    };

    // Ascend only verifiable NEXT/SUBSTACK edges to the enclosing
    // procedures_definition or the actual root hat. This rejects malformed
    // parent links and reporter inputs without inventing a call owner.
    // Each lexical IF/IF-ELSE path is saved as immutable source metadata,
    // not as transient LLVM branch SSA.
    std::optional<CallerContext> resolveCallerContext(
            const ScratchScript& script, const Block& call) const {
        CallerContext result;
        const Block* child = &call;
        for (size_t steps = 0; steps <= script.nodes.size(); ++steps) {
            if (child->parent.empty()) return std::nullopt;
            const Block* parent = script.getBlock(child->parent);
            if (!parent) return std::nullopt;
            if (parent->opcode == "procedures_definition") {
                if (parent->next != child->id) return std::nullopt;
                const auto owner = procedures_.idFor(parent->id);
                if (!owner) return std::nullopt;
                result.procedureId = *owner;
                std::reverse(result.branches.begin(), result.branches.end());
                std::reverse(result.loops.begin(), result.loops.end());
                return result;
            }
            if (parent->opcode.starts_with("event_") ||
                parent->opcode == "control_start_as_clone") {
                if (parent->next != child->id ||
                    parent->id != script.topId) return std::nullopt;
                result.procedureId = 0;
                std::reverse(result.branches.begin(), result.branches.end());
                std::reverse(result.loops.begin(), result.loops.end());
                return result;
            }
            if (parent->next == child->id) {
                child = parent;
                continue;
            }
            const auto* thenInput = parent->inputs.find("SUBSTACK");
            const auto* elseInput = parent->inputs.find("SUBSTACK2");
            if (parent->opcode == "control_if" ||
                parent->opcode == "control_if_else") {
                uint8_t arm = 0;
                if (thenInput && thenInput->block == child->id) arm = 1;
                else if (parent->opcode == "control_if_else" &&
                         elseInput && elseInput->block == child->id) arm = 2;
                if (!arm) return std::nullopt;
                const auto pc = pcFor(parent->id);
                if (!pc) return std::nullopt;
                result.branches.push_back({*pc, arm});
                child = parent;
                continue;
            }
            if (isTrackedLoop(parent->opcode) ||
                parent->opcode == "control_all_at_once") {
                if (!thenInput || thenInput->block != child->id)
                    return std::nullopt;
                if (isTrackedLoop(parent->opcode)) {
                    const auto pc = pcFor(parent->id);
                    if (!pc) return std::nullopt;
                    result.loops.push_back(*pc);
                }
                child = parent;
                continue;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    struct LoopSlot {
        uint32_t slot;
        bool counted;
        uint64_t sourcePc;
    };
    void disable(const char* reason) {
        enabled_ = false;
        reason_ = reason;
    }
    // Resolve the *structural* continuation of a call, including the
    // terminal statement of a conditional SUBSTACK or counted loop body.
    // A parent equal to the previous block in a linear stack is skipped.
    // Traversal is bounded by the immutable captured graph's size.
    std::optional<std::pair<SourceReturnKind, uint64_t>>
    resolveReturnTarget(const ScratchScript& script, const Block& call) const {
        // Only the original call's 'next' means a direct next statement.
        // Ancestors in a linear Scratch chain use 'next' to point *to*
        // the current child, not to its following continuation.
        if (!call.next.empty()) {
            const auto pc = pcFor(call.next);
            if (!pc) return std::nullopt;
            return {{SourceReturnKind::NextBlock, *pc}};
        }
        const Block* node = &call;
        for (size_t steps = 0; steps <= script.nodes.size(); ++steps) {
            if (node->parent.empty()) {
                if (node->opcode == "procedures_definition")
                    return {{SourceReturnKind::ProcedureExit, 0}};
                if (node->opcode.starts_with("event_"))
                    return {{SourceReturnKind::ScriptExit, 0}};
                return std::nullopt;
            }
            const Block* parent = script.getBlock(node->parent);
            if (!parent) return std::nullopt;
            if (parent->opcode == "procedures_definition")
                return {{SourceReturnKind::ProcedureExit, 0}};
            if (parent->opcode.starts_with("event_"))
                return {{SourceReturnKind::ScriptExit, 0}};
            // Linear sibling, not a structural parent: ignore the
            // previous statement's successor edge when climbing.
            if (parent->next == node->id) {
                node = parent;
                continue;
            }
            if (isTrackedLoop(parent->opcode)) {
                const auto pc = pcFor(parent->id);
                if (!pc) return std::nullopt;
                return {{SourceReturnKind::LoopLatch, *pc}};
            }
            if (parent->opcode == "control_if" ||
                parent->opcode == "control_if_else") {
                if (!parent->next.empty()) {
                    const auto pc = pcFor(parent->next);
                    if (!pc) return std::nullopt;
                    return {{SourceReturnKind::AfterBranch, *pc}};
                }
                node = parent;
                continue;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    struct CallTarget {
        uint64_t procedure;
        uint64_t returnPc;
        uint64_t entryPc;
        std::string proccode;
        uint32_t argCount;
    };

    static CallFrameLayout chooseLayout(
        const ScratchCompilationUnit& source) {
        if (!source.valid()) return {};
        size_t maxArguments = 0;
        for (const auto& site : source.transfers.sites) {
            if (site.kind != ScratchTransferKind::Call ||
                site.procedureDefinitionId.empty())
                continue;
            const Block* call = source.script.getBlock(site.sourceBlockId);
            if (!call || !call->mutation) return {};
            call->mutation->parseArgs();
            maxArguments = std::max(
                maxArguments, call->mutation->argumentIds.size());
            if (maxArguments > CallFrameAbi::kMaxArguments) return {};
        }
        size_t loopCount = 0;
        for (const auto& block : source.script.nodes)
            loopCount += isTrackedLoop(block.opcode);
        if (loopCount > kMaxTrackedLoops) return {};
        return {uint32_t(maxArguments), uint32_t(loopCount * 2 + 2),
                CallFrameAbi::kMaxDepth};
    }
    std::optional<uint64_t> pcFor(const std::string& sourceId) const {
        if (sourceId.empty()) return CallFrameAbi::kNoPc - 1;
        const auto it = pcByBlock_.find(sourceId);
        if (it == pcByBlock_.end()) return std::nullopt;
        return it->second;
    }

    ScratchProcedureTable procedures_;
    ScratchCallStack stack_;
    const Blocks* sourceBlocks_ = nullptr; // borrowed; same lifetime as Thread
    uint64_t compiledRevision_ = 0;
    ScratchSourceDigest compiledDigest_;
    std::string sourceTopId_;
    RootedCallFrameValues rootedArgs_;
    std::unordered_map<std::string, uint64_t> pcByBlock_;
    std::unordered_map<std::string, LoopSlot> loopSlots_;
    std::unordered_map<std::string, CallTarget> calls_;
    std::unordered_map<uint64_t, SourceReturnSite> returnSites_;
    std::shared_ptr<const SourceFrameSymbols> symbols_;
    const bool traceLoopPathReadback_ = [] {
        const char* flag = std::getenv("XYO_IR001_TRACE_SOURCE_VALUES");
        return flag && flag[0] == '1';
    }();
    const bool loopSsaDiagnosticEnabled_ = [] {
        const char* flag = std::getenv("XYO_JIT_CALLFRAME_LOOP_SSA_DIAGNOSTIC");
        return flag && flag[0] == '1';
    }();
    std::optional<SourceSsaSnapshot> loopSsaDiagnostic_;
    std::unordered_set<uint64_t> visitedReturnSites_;
    uint64_t loopReturnSites_ = 0;
    uint64_t branchReturnSites_ = 0;
    uint64_t procedureReturnSites_ = 0;
    uint64_t scriptReturnSites_ = 0;
    bool enabled_ = false;
    const char* reason_ = "ok";
    uint64_t callsMade_ = 0;
    uint64_t returnsMade_ = 0;
    uint64_t yieldsMade_ = 0;
    uint64_t checkpoints_ = 0;
    uint64_t lastCheckpoint_ = 0;
    uint64_t counterCheckpoints_ = 0;
    uint64_t counterSum_ = 0;
    uint64_t parentCounterSnapshots_ = 0;
    uint64_t parentCounterSum_ = 0;
    uint32_t maxPathDepth_ = 0;
    uint32_t maxCounterSlots_ = 0;
    uint32_t maxDepth_ = 0;
    std::size_t maxHeapRoots_ = 0;
    const bool probeExport_ = [] {
        const char* flag = std::getenv("XYO_JIT_CALLFRAME_EXPORT_PROBE");
        return flag && flag[0] == '1';
    }();
    std::unique_ptr<SelfContainedSourceContinuation> probeImage_;
    uint64_t exportsMade_ = 0;
    std::size_t maxExportHeapRoots_ = 0;
    uint32_t maxExportDepth_ = 0;
    uint64_t exportedAncestorLoopFrames_ = 0;
    uint64_t exportedEntryYields_ = 0;
    uint64_t exportedAfterBodyYields_ = 0;
    uint64_t exportedAfterCalleeYields_ = 0;
    uint64_t exportedUnknownBodyYields_ = 0;
    uint64_t exportedThenBranches_ = 0;
    uint64_t exportedElseBranches_ = 0;
    uint64_t exportedNestedBranches_ = 0;
    uint64_t exportedRootLoopFrames_ = 0;
    uint64_t exportedRootCountedFrames_ = 0;
    uint64_t exportedRootCounterSum_ = 0;
    uint32_t exportedRootMaxDepth_ = 0;
    uint64_t exportedAwaitingFrames_ = 0;
    uint64_t exportedAwaitingLoopFrames_ = 0;
    uint32_t suspendedDepth_ = 0;
    uint64_t suspendedRevision_ = 0;
};

} // namespace xyo::ir
