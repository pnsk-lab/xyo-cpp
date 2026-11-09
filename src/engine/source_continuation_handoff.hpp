// Scheduler-owned, source-versioned native CallFrame handoff.
//
// This is a transfer of GC-rooted arguments and source control-flow symbols,
// not an executable LLVM continuation. A future deoptimizer must separately
// provide its SSA temporaries and committed host-effect positions.
#pragma once

#include "../jit/procedure_frame_state.hpp"
#include "thread.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace xyo {

// Temporary, only valid while its originating handoff roots remain alive.
// An executable continuation is deliberately NOT inferred from an arbitrary
// exported native CallFrame: source PC alone does not capture LLVM SSA/effects.
struct SimpleNativeInterpreterResume {
    struct Control {
        std::string blockId;
        bool isLoop = false;
        std::optional<double> remaining; // only counted control_repeat
    };
    struct Procedure {
        std::string callBlockId;
        std::unordered_map<std::u16string, Value> arguments;
        // Source lexical stack from THIS procedure to its suspended
        // child call (or, in the leaf, the committed loop checkpoint).
        // IF and LOOP frames may alternate in either nesting order.
        std::vector<Control> activeControls;
    };
    // Lexical scopes in the root green-flag script before the first call.
    // Ordered root source lexical scopes. Counted loops are restored from
    // the root CallFrame's typed live-counter slots, not by evaluating TIMES
    // again or deriving it from Scratch's mutable variable state.
    std::vector<Control> rootControls;
    std::vector<Procedure> calls; // outermost to innermost
    // Nonempty only for a proven, already-returned callee whose next
    // source statement must execute exactly once. The original call is
    // NEVER re-evaluated as a fresh interpreter procedure invocation.
    std::string postCalleeNextBlock;
    // A completed chain can return all the way out of the root hat.
    // This is a proven end-of-script, not a request to replay any call.
    bool postCalleeScriptExit = false;
};

struct NativeSourceHandoff {
    std::unique_ptr<ir::SelfContainedSourceContinuation> image;
    const Blocks* sourceBlocks = nullptr; // borrowed, only dereference via Thread
    const Target* sourceTarget = nullptr;  // borrowed, identity only
    uint64_t sourceRevision = 0;
    uint64_t sourceYieldCount = 0;
    std::string topBlock;

    // A source/target match alone is not enough: Scratch may restart or
    // recycle a Thread without editing its Blocks. Verify that this is the
    // same native procedure image at exactly the captured yield, not an
    // old snapshot from another logical invocation. The immutable symbol
    // table is shared with the image, so its pointer cannot be reused
    // while this handoff owns it (unlike a raw Thread/Fiber address).
    bool validFor(const Thread& thread) const {
        if (!image || !image->valid() ||
            !thread.blockContainer || thread.blockContainer != sourceBlocks ||
            thread.blockContainer->revision() != sourceRevision ||
            !thread.target || thread.target != sourceTarget ||
            thread.topBlock != topBlock || !thread.isCompiled ||
            !thread.fiber || thread.status != STATUS_RUNNING ||
            !thread.nativeProcedureFrames ||
            thread.nativeProcedureFrames->yields() != sourceYieldCount ||
            // This is the *compiled* immutable revision, not an epoch
            // invented when the scheduler happens to take a snapshot.
            !image->symbols ||
            image->symbols->sourceRevision != sourceRevision ||
            thread.nativeProcedureFrames->compiledRevision() != sourceRevision ||
            !thread.nativeProcedureFrames->sourceUnchanged() ||
            !thread.nativeProcedureFrames->matchesExport(*image->frames))
            return false;
        const auto current = thread.nativeProcedureFrames->exportSelfContained();
        return current && current->symbols == image->symbols;
    }

    // Supported source subset: flag hat -> bounded nonwarp procedure
    // chain (including direct recursion) -> leaf repeat/while/until
    // AfterLoopBody checkpoint, or source-verified post-call NextBlock,
    // AfterBranch, LoopLatch, ProcedureExit chain, and limited root-only
    // ScriptExit. The final root call is never evaluated a second time.
    // Ancestor frames are AwaitingCallee;
    // their active loop paths and bounded ordered IF/IF-ELSE arms
    // are reconstructed exactly, without re-evaluating reporters.
    // Arbitrary SSA/effect PCs and unrecognized source paths are never
    // inferred. Reject uncertain cases and keep the LLVM Fiber intact.
    std::optional<SimpleNativeInterpreterResume>
    simpleInterpreterResume(const Thread& thread) const {
        // The common CallFrame arena supports larger recursion depths,
        // but reconstruct only a bounded number of interpreter frames.
        // All ancestor procedure, source path, GC root and loop checks
        // below remain mandatory regardless of depth.
        constexpr uint32_t kMaxRestoredCallDepth = 32;
        if (!validFor(thread) || !image->symbols ||
            image->frames->depth() == 0 ||
            image->frames->depth() > kMaxRestoredCallDepth)
            return std::nullopt;
        // Use the public, typed source-phase decoder. Never assume the
        // private native local layout or infer a phase from the loop PC.
        const auto frames = image->decodeFrames();
        if (!frames || frames->size() != image->frames->depth())
            return std::nullopt;
        const auto& leaf = frames->back();
        const auto& symbols = *image->symbols;
        // Each source IF contributes exactly one Scratch stack frame.
        // The path comes from an immutable, verified source graph, not
        // from executing a reporter a second time. Bound the maximum
        // reconstructed depth to avoid pathological/malicious projects.
        constexpr size_t kMaxConditionalFramesPerCall = 32;
        const bool afterLoop =
            leaf.phase == ir::SourceResumePhase::AfterLoopBody &&
            !leaf.loops.empty() &&
            leaf.frame.resumePc == leaf.loops.back().sourcePc;
        const bool afterCallee =
            leaf.phase == ir::SourceResumePhase::AfterCallee;
        // The detached image independently validates the post-call edge
        // against captured NEXT/SUBSTACK topology and exact caller owner.
        const bool rootOnlyPostCall = afterCallee && frames->size() == 1;
        const auto completedCall = afterCallee
            ? (rootOnlyPostCall ? image->rootPostCallSuccessor()
                                : image->postCallSuccessor())
            : std::nullopt;
        if (!afterLoop &&
            (!afterCallee || !completedCall ||
             (completedCall->kind != ir::SourceReturnKind::NextBlock &&
              completedCall->kind != ir::SourceReturnKind::AfterBranch &&
              completedCall->kind != ir::SourceReturnKind::LoopLatch &&
              !(rootOnlyPostCall &&
                completedCall->kind == ir::SourceReturnKind::ScriptExit) &&
              !(!rootOnlyPostCall &&
                completedCall->kind == ir::SourceReturnKind::ProcedureExit))))
            return std::nullopt;

        // Parent execution is suspended inside a child call. An active
        // loop path is valid only if its exact source blocks/counters can
        // be reconstructed below, before that child call frame.
        for (size_t i = 1; i + 1 < frames->size(); ++i)
            if ((*frames)[i].phase != ir::SourceResumePhase::AwaitingCallee)
                return std::nullopt;

        // Reject hidden loops outside the recovered path. The typed
        // decoder already resolves their lexical PCs and integer counter
        // slots, without relying on the private native frame word layout.
        size_t trackedLoops = 0;
        for (const auto& id : symbols.blockIds) {
            const auto* block = thread.blockContainer->getBlock(id);
            if (!block) return std::nullopt;
            if (ir::NativeProcedureFrameSession::isTrackedLoop(block->opcode))
                ++trackedLoops;
        }
        if (trackedLoops != symbols.loops.size()) return std::nullopt;

        // A linear prefix was executed and committed by the native Fiber.
        // Reconstructing an Interpreter frame AT the current source loop
        // must not evaluate those blocks again. Permit only known Scratch
        // statements, a fully completed sibling repeat or a completed
        // conditional whose successor is on the known source path.
        // Never skip another procedure, unknown effect or yield opcode.
        const auto reachedViaCommittedPrefix =
            [&](std::string id, std::string_view wanted,
                bool allowFinishedCalls) {
                for (size_t count = 0;
                     !id.empty() && count <= symbols.blockIds.size();
                     ++count) {
                    if (id == wanted) return true;
                    const auto* block = thread.blockContainer->getBlock(id);
                    if (!block) return false;
                    if (block->opcode == "procedures_call" &&
                        allowFinishedCalls) {
                        // A root AfterLoopBody checkpoint proves that the
                        // native execution passed the *entire* source prefix.
                        // Earlier calls are committed and must not replay.
                        // Never use this exception for an active callee.
                        if (!block->mutation ||
                            block->mutation->isReporterCall() ||
                            block->topLevel)
                            return false;
                    } else if (block->opcode != "data_changevariableby" &&
                               block->opcode != "data_setvariableto" &&
                               block->opcode != "control_repeat" &&
                               block->opcode != "control_while" &&
                               block->opcode != "control_repeat_until" &&
                               block->opcode != "control_if" &&
                               block->opcode != "control_if_else") {
                        return false;
                    }
                    id = block->next;
                }
                return false;
            };

        const auto* hat = thread.blockContainer->getBlock(topBlock);
        if (!hat || hat->opcode != "event_whenflagclicked")
            return std::nullopt;

        SimpleNativeInterpreterResume plan;
        plan.calls.reserve(frames->size() - 1);
        std::unordered_set<std::string> visitedDefinitions;

        // The interpreter stores one fewer remaining iteration than the
        // LLVM native pre-decrement latch. The IR-001 root CallFrame now
        // exports the SAME typed loop counters as procedure CallFrames.
        // A missing counter (including a legacy image) is never guessed.
        const auto restoreLoop = [&](const ir::SourceLoopResume& saved)
            -> std::optional<SimpleNativeInterpreterResume::Control> {
            const auto id = symbols.block(saved.sourcePc);
            if (!id) return std::nullopt;
            const auto* block = thread.blockContainer->getBlock(
                std::string(*id));
            if (!block) return std::nullopt;
            if (block->opcode == "control_repeat") {
                if (!Value{saved.counterBits}.isNumber())
                    return std::nullopt;
                const double count =
                    Value{saved.counterBits}.asNumberUnchecked() - 1.0;
                if (!std::isfinite(count) || count < 0 ||
                    std::trunc(count) != count || count > 1e6)
                    return std::nullopt;
                return SimpleNativeInterpreterResume::Control{
                    std::string(*id), true, count};
            }
            if ((block->opcode == "control_while" ||
                 block->opcode == "control_repeat_until") &&
                saved.counterBits == ir::CallFrameAbi::kNoLoopCounter)
                return SimpleNativeInterpreterResume::Control{
                    std::string(*id), true, std::nullopt};
            return std::nullopt;
        };

        // Rebuild the ordered source lexical stack from actual parent edges.
        // A previous NEXT block is already completed, not an active frame.
        // A SUBSTACK/SUBSTACK2 edge means the enclosing control block is
        // active and must stay on the interpreter stack when native exits.
        // This distinguishes repeat { if { call } } from
        // if { repeat { call } }, which need different frame ordering.
        const auto sourceControls = [&](
            const auto& snapshot, const Block& definition,
            std::string_view targetId,
            const std::vector<ir::SourceConditionalArm>* knownArms,
            bool atLoopCheckpoint, bool allowFinishedCalls = false)
            -> std::optional<std::vector<SimpleNativeInterpreterResume::Control>> {
            struct Ancestor {
                std::string id;
                bool loop;
                uint8_t arm;
            };
            std::vector<Ancestor> reversed;
            std::string current(targetId);
            for (size_t steps = 0;
                 current != definition.id && steps <= symbols.blockIds.size();
                 ++steps) {
                const auto* child = thread.blockContainer->getBlock(current);
                if (!child || child->parent.empty()) return std::nullopt;
                const auto* parent =
                    thread.blockContainer->getBlock(child->parent);
                if (!parent) return std::nullopt;
                if (parent->next == current) {
                    current = parent->id;
                    continue;
                }
                const auto& op = parent->opcode;
                const bool loop = op == "control_repeat" ||
                                  op == "control_while" ||
                                  op == "control_repeat_until";
                const bool branch = op == "control_if" ||
                                    op == "control_if_else";
                if (!loop && !branch) return std::nullopt;
                uint8_t arm = 0;
                if (thread.blockContainer->getBranch(parent->id, 1) == current)
                    arm = 1;
                else if (op == "control_if_else" &&
                         thread.blockContainer->getBranch(parent->id, 2) == current)
                    arm = 2;
                if (!arm || (loop && arm != 1)) return std::nullopt;
                reversed.push_back({parent->id, loop, arm});
                current = parent->id;
            }
            if (current != definition.id) return std::nullopt;
            std::reverse(reversed.begin(), reversed.end());

            std::vector<SimpleNativeInterpreterResume::Control> controls;
            controls.reserve(reversed.size() + (atLoopCheckpoint ? 1 : 0));
            size_t loopIndex = 0;
            size_t armIndex = 0;
            std::string start = definition.next;
            for (const auto& scope : reversed) {
                if (!reachedViaCommittedPrefix(
                        start, scope.id, allowFinishedCalls))
                    return std::nullopt;
                if (scope.loop) {
                    if (loopIndex >= snapshot.loops.size())
                        return std::nullopt;
                    auto loop = restoreLoop(snapshot.loops[loopIndex++]);
                    if (!loop || loop->blockId != scope.id)
                        return std::nullopt;
                    controls.push_back(std::move(*loop));
                } else {
                    if (knownArms) {
                        if (armIndex >= knownArms->size())
                            return std::nullopt;
                        const auto& wanted = (*knownArms)[armIndex];
                        const auto id = symbols.block(wanted.controlPc);
                        if (!id || *id != scope.id || wanted.arm != scope.arm)
                            return std::nullopt;
                    }
                    ++armIndex;
                    controls.push_back({scope.id, false, std::nullopt});
                }
                start = thread.blockContainer->getBranch(scope.id, scope.arm);
                if (start.empty()) return std::nullopt;
            }
            if (!reachedViaCommittedPrefix(
                    start, targetId, allowFinishedCalls))
                return std::nullopt;

            if (atLoopCheckpoint) {
                if (loopIndex >= snapshot.loops.size()) return std::nullopt;
                auto loop = restoreLoop(snapshot.loops[loopIndex++]);
                if (!loop || loop->blockId != targetId) return std::nullopt;
                controls.push_back(std::move(*loop));
            }
            if (loopIndex != snapshot.loops.size() ||
                (knownArms && armIndex != knownArms->size()) ||
                armIndex > kMaxConditionalFramesPerCall)
                return std::nullopt;
            return controls;
        };

        // A source-certified LoopLatch can be reached after completing
        // one or more terminal IF/IF-ELSE arms. None of their predicates
        // may be evaluated again: only the enclosing loop's saved latch
        // is still executable. Share the same check for root and nested
        // procedure frames (including a fully returned callee chain).
        const auto unwindTerminalConditionsToLatch = [&](
                std::vector<SimpleNativeInterpreterResume::Control>& active,
                uint64_t targetPc) -> bool {
            const auto latch = symbols.block(targetPc);
            if (!latch) return false;
            // A failed proof must leave the assembled plan untouched.
            size_t count = active.size();
            while (count && !active[count - 1].isLoop) {
                const auto* branch = thread.blockContainer->getBlock(
                    active[count - 1].blockId);
                if (!branch ||
                    (branch->opcode != "control_if" &&
                     branch->opcode != "control_if_else") ||
                    !branch->next.empty())
                    return false;
                --count;
            }
            if (!count || !active[count - 1].isLoop ||
                active[count - 1].blockId != *latch)
                return false;
            active.resize(count);
            return true;
        };

        if (afterLoop && frames->size() == 1) {
            // A root-only AfterLoopBody is a real, committed native loop
            // checkpoint. It cannot be adopted with an EMPTY plan: doing
            // so silently drops every remaining iteration and successor.
            // Recover the original root control stack from the exact live
            // source loop path and numeric counter bits. Earlier complete
            // nonwarp calls in the root prefix must not be replayed.
            const auto& root = frames->front();
            if (root.frame.procedureId !=
                    ir::CallFrameAbi::kRootProcedure ||
                root.loops.empty() ||
                root.frame.resumePc != root.loops.back().sourcePc)
                return std::nullopt;
            const auto loopId =
                symbols.block(root.loops.back().sourcePc);
            if (!loopId) return std::nullopt;
            auto controls = sourceControls(
                root, *hat, *loopId, nullptr, true, true);
            if (!controls || controls->empty() ||
                !controls->back().isLoop ||
                controls->back().blockId != *loopId)
                return std::nullopt;
            plan.rootControls = std::move(*controls);
            return plan;
        }

        if (rootOnlyPostCall) {
            // The final nonwarp callee has already returned: no procedure
            // parameters or LLVM temporaries remain live. Restore the
            // *actual* root lexical loop counters and previously taken IF
            // arms, then advance solely by the verified original return
            // edge. Never replay the completed procedures_call.
            const auto& root = frames->front();
            if (root.frame.procedureId !=
                    ir::CallFrameAbi::kRootProcedure ||
                !completedCall ||
                completedCall->callerProcedureId !=
                    ir::CallFrameAbi::kRootProcedure ||
                root.loops.size() != completedCall->callerLoopPath.size())
                return std::nullopt;
            for (size_t k = 0; k < root.loops.size(); ++k)
                if (root.loops[k].sourcePc !=
                    completedCall->callerLoopPath[k])
                    return std::nullopt;

            const auto callId =
                symbols.block(completedCall->callBlockPc);
            const auto* call = callId
                ? thread.blockContainer->getBlock(std::string(*callId))
                : nullptr;
            if (!call || call->opcode != "procedures_call" ||
                !call->mutation || call->mutation->isReporterCall() ||
                call->topLevel)
                return std::nullopt;
            const auto& procCode = call->mutation->attr("proccode");
            const auto* prototype =
                thread.blockContainer->getProcedurePrototype(procCode);
            if (!prototype || prototype->warp())
                return std::nullopt;

            auto controls = sourceControls(
                root, *hat, call->id, &completedCall->branchPath, false);
            if (!controls) return std::nullopt;
            plan.rootControls = std::move(*controls);
            switch (completedCall->kind) {
            case ir::SourceReturnKind::ScriptExit:
                // The source graph has proved that this call reaches the
                // end of the hat after unwinding its lexical ancestors.
                // A trailing IF/IF-ELSE with no NEXT is already complete:
                // do not re-evaluate its predicate or re-enter its arm.
                // A loop still needs its latch, so never treat a live loop
                // (or an unrecognized control) as a completed script.
                if (!call->next.empty()) return std::nullopt;
                for (const auto& control : plan.rootControls) {
                    const auto* branch =
                        thread.blockContainer->getBlock(control.blockId);
                    if (control.isLoop || !branch ||
                        (branch->opcode != "control_if" &&
                         branch->opcode != "control_if_else") ||
                        !branch->next.empty())
                        return std::nullopt;
                }
                // Every branch is proven terminal and has no pending
                // continuation or parameters. Avoid installing dead
                // interpreter StackFrames on a STATUS_DONE thread.
                plan.rootControls.clear();
                plan.postCalleeScriptExit = true;
                break;
            case ir::SourceReturnKind::NextBlock: {
                const auto next = symbols.block(completedCall->targetPc);
                if (!next || call->next != *next)
                    return std::nullopt;
                plan.postCalleeNextBlock = std::string(*next);
                break;
            }
            case ir::SourceReturnKind::AfterBranch: {
                const auto next = symbols.block(completedCall->targetPc);
                if (!next) return std::nullopt;
                // Unwind the completed IF arm without re-evaluating its
                // condition; outer repeat counters remain unchanged.
                size_t enclosing = plan.rootControls.size();
                for (size_t k = plan.rootControls.size(); k > 0; --k) {
                    if (plan.rootControls[k - 1].isLoop) continue;
                    const auto* branch = thread.blockContainer->getBlock(
                        plan.rootControls[k - 1].blockId);
                    if (branch && branch->next == *next) {
                        enclosing = k - 1;
                        break;
                    }
                }
                if (enclosing == plan.rootControls.size())
                    return std::nullopt;
                plan.rootControls.resize(enclosing);
                plan.postCalleeNextBlock = std::string(*next);
                break;
            }
            case ir::SourceReturnKind::LoopLatch:
                if (!unwindTerminalConditionsToLatch(
                        plan.rootControls, completedCall->targetPc))
                    return std::nullopt;
                break;
            default:
                return std::nullopt;
            }
            return plan;
        }

        for (size_t i = 1; i < frames->size(); ++i) {
            const auto& snapshot = (*frames)[i];
            const auto defId = symbols.procedure(snapshot.frame.procedureId);
            if (!defId || !snapshot.caller) return std::nullopt;
            const auto& site = *snapshot.caller;
            const bool rootCall = (i == 1);
            const bool reusedDefinition =
                !visitedDefinitions.emplace(*defId).second;
            if (reusedDefinition) {
                // The supported recursive shape is a direct self-call
                // inside a counted repeat. Every current/remaining
                // iteration of its parent is captured as a typed source
                // loop counter, and the exact LoopLatch return-site is
                // recorded. Nested IF-arm callsites with a verified
                // immutable ordered branch path are also reconstructible:
                // every reporter has already been evaluated by native code,
                // and each executed arm is materialized as an interpreter
                // branch frame rather than evaluated again.
                const auto& parentSnapshot = (*frames)[i - 1];
                const bool directLoop = site.kind ==
                    ir::SourceReturnKind::LoopLatch &&
                    !parentSnapshot.loops.empty() &&
                    Value{parentSnapshot.loops.back().counterBits}.isNumber();
                const bool directConditional =
                    !site.branchPath.empty() &&
                    site.branchPath.size() <= kMaxConditionalFramesPerCall &&
                    (site.kind == ir::SourceReturnKind::AfterBranch ||
                     site.kind == ir::SourceReturnKind::NextBlock ||
                     site.kind == ir::SourceReturnKind::ProcedureExit);
                if (rootCall || !(*frames)[i - 1].frame.procedureId ||
                    snapshot.frame.procedureId !=
                        (*frames)[i - 1].frame.procedureId ||
                    (!directLoop && !directConditional))
                    return std::nullopt;
            }
            if (site.callerProcedureId != (*frames)[i - 1].frame.procedureId ||
                site.calleeProcedureId != snapshot.frame.procedureId)
                return std::nullopt;

            if (rootCall
                  ? (site.kind != ir::SourceReturnKind::NextBlock &&
                     site.kind != ir::SourceReturnKind::AfterBranch &&
                     site.kind != ir::SourceReturnKind::LoopLatch &&
                     site.kind != ir::SourceReturnKind::ScriptExit)
                  : (site.kind != ir::SourceReturnKind::NextBlock &&
                     site.kind != ir::SourceReturnKind::ProcedureExit &&
                     site.kind != ir::SourceReturnKind::AfterBranch &&
                     !(site.kind == ir::SourceReturnKind::LoopLatch &&
                       !(*frames)[i - 1].loops.empty())))
                return std::nullopt;

            const auto callId = symbols.block(site.callBlockPc);
            if (!callId ||
                site.branchPath.size() > kMaxConditionalFramesPerCall)
                return std::nullopt;

            if (rootCall) {
                // The compiled source graph independently certifies the
                // exact root callsite's ordered lexical loops. Match those
                // PCs against the *actual* root CallFrame path and consume
                // its typed counters, not synthetic kNoLoopCounter values.
                const auto& root = (*frames)[0];
                if (root.loops.size() != site.callerLoopPath.size())
                    return std::nullopt;
                for (size_t k = 0; k < root.loops.size(); ++k)
                    if (root.loops[k].sourcePc != site.callerLoopPath[k])
                        return std::nullopt;
                auto controls = sourceControls(
                    root, *hat, *callId, &site.branchPath, false);
                if (!controls) return std::nullopt;
                plan.rootControls = std::move(*controls);
            } else {
                const auto parentDefId =
                    symbols.procedure((*frames)[i - 1].frame.procedureId);
                const auto* parentDefinition = parentDefId
                    ? thread.blockContainer->getBlock(std::string(*parentDefId))
                    : nullptr;
                if (!parentDefinition ||
                    parentDefinition->opcode != "procedures_definition")
                    return std::nullopt;
                auto controls = sourceControls(
                    (*frames)[i - 1], *parentDefinition,
                    *callId, &site.branchPath, false);
                if (!controls) return std::nullopt;
                // The parent remains suspended at this child call.
                // Materialize all parent control frames in source lexical
                // order before creating the child procedure frame.
                plan.calls.back().activeControls = std::move(*controls);
            }
            const auto* call = thread.blockContainer->getBlock(
                std::string(*callId));
            const auto* def = thread.blockContainer->getBlock(
                std::string(*defId));
            if (!call || call->opcode != "procedures_call" ||
                !call->mutation || call->mutation->isReporterCall() ||
                call->topLevel || !def ||
                def->opcode != "procedures_definition")
                return std::nullopt;

            const auto& code = call->mutation->attr("proccode");
            const auto actualDefinition =
                thread.blockContainer->getProcedureDefinition(code);
            const auto* prototype =
                thread.blockContainer->getProcedurePrototype(code);
            if (actualDefinition != *defId ||
                !prototype || prototype->warp()) return std::nullopt;
            prototype->parseArgs();
            call->mutation->parseArgs();
            const size_t count = snapshot.frame.arguments;
            if (count != prototype->argumentNames.size() ||
                count != prototype->argumentIds.size() ||
                count != call->mutation->argumentIds.size() ||
                count != site.argumentCount ||
                count > image->frames->frames().layout().argumentCapacity ||
                call->mutation->argumentIds != prototype->argumentIds)
                return std::nullopt;

            plan.calls.push_back({std::string(*callId), {}});
            auto& arguments = plan.calls.back().arguments;
            for (size_t arg = 0; arg < count; ++arg) {
                const auto value =
                    image->frames->argument(uint32_t(i), uint32_t(arg));
                if (!value || !arguments.emplace(
                        prototype->argumentNames[arg], *value).second)
                    return std::nullopt;
            }
            if (i + 1 == frames->size()) {
                if (afterLoop) {
                    // The leaf checkpoint has completed the previous
                    // loop body. Re-enter its latch, not its body prefix.
                    const auto loopId =
                        symbols.block(snapshot.loops.back().sourcePc);
                    if (!loopId) return std::nullopt;
                    auto controls = sourceControls(
                        snapshot, *def, *loopId, nullptr, true);
                    if (!controls) return std::nullopt;
                    plan.calls.back().activeControls = std::move(*controls);
                } else {
                    // This suspended native frame is the *caller* of an
                    // already completed child. Validate the child's exact
                    // original callsite and reconstruct only active parent
                    // controls. Do not revive the child CallFrame.
                    const auto callId =
                        symbols.block(completedCall->callBlockPc);
                    if (!callId ||
                        completedCall->callerProcedureId !=
                            snapshot.frame.procedureId)
                        return std::nullopt;
                    const auto* completed =
                        thread.blockContainer->getBlock(std::string(*callId));
                    if (!completed ||
                        completed->opcode != "procedures_call" ||
                        !completed->mutation ||
                        completed->mutation->isReporterCall())
                        return std::nullopt;
                    auto controls = sourceControls(
                        snapshot, *def, *callId,
                        &completedCall->branchPath, false);
                    if (!controls) return std::nullopt;
                    plan.calls.back().activeControls = std::move(*controls);
                    if (completedCall->kind ==
                        ir::SourceReturnKind::ProcedureExit) {
                        // The completed child was the final statement of
                        // this procedure. All its IF frames are finished
                        // too, and no loop may remain live in this leaf.
                        // Unwind *only* original source-authenticated
                        // terminal procedure calls. The next nonterminal
                        // caller determines the exact interpreter PC.
                        if (!snapshot.loops.empty())
                            return std::nullopt;
                        size_t exiting = i;
                        while (exiting > 0) {
                            const auto& finished = (*frames)[exiting];
                            if (!finished.caller || plan.calls.empty())
                                return std::nullopt;
                            const auto& parentSite = *finished.caller;
                            const auto parentCall =
                                symbols.block(parentSite.callBlockPc);
                            const auto* parentBlock = parentCall
                                ? thread.blockContainer->getBlock(
                                      std::string(*parentCall))
                                : nullptr;
                            if (!parentBlock ||
                                parentBlock->opcode != "procedures_call" ||
                                !symbols.validReturnTarget(parentSite))
                                return std::nullopt;
                            plan.calls.pop_back();
                            if (parentSite.kind ==
                                    ir::SourceReturnKind::ProcedureExit) {
                                // No child frame or active loop is needed
                                // once its parent procedure also exits.
                                if (exiting == 1) return std::nullopt;
                                --exiting;
                                continue;
                            }
                            if (parentSite.kind ==
                                    ir::SourceReturnKind::ScriptExit) {
                                if (exiting != 1 || !plan.calls.empty())
                                    return std::nullopt;
                                plan.postCalleeScriptExit = true;
                                break;
                            }
                            if (parentSite.kind ==
                                    ir::SourceReturnKind::NextBlock) {
                                const auto next =
                                    symbols.block(parentSite.targetPc);
                                if (!next || parentBlock->next != *next)
                                    return std::nullopt;
                                plan.postCalleeNextBlock =
                                    std::string(*next);
                                break;
                            }
                            if (parentSite.kind ==
                                    ir::SourceReturnKind::LoopLatch) {
                                auto& active = plan.calls.empty()
                                    ? plan.rootControls
                                    : plan.calls.back().activeControls;
                                if (!unwindTerminalConditionsToLatch(
                                        active, parentSite.targetPc))
                                    return std::nullopt;
                                break;
                            }
                            if (parentSite.kind ==
                                    ir::SourceReturnKind::AfterBranch) {
                                const auto next =
                                    symbols.block(parentSite.targetPc);
                                if (!next) return std::nullopt;
                                // The finished procedure was the last
                                // statement in the executing IF/ELSE arm.
                                // Drop the exact completed IF frame and its
                                // descendants, retaining any outer loop.
                                auto& active = plan.calls.empty()
                                    ? plan.rootControls
                                    : plan.calls.back().activeControls;
                                size_t enclosing = active.size();
                                for (size_t k = active.size(); k > 0; --k) {
                                    if (active[k - 1].isLoop) continue;
                                    const auto* branch =
                                        thread.blockContainer->getBlock(
                                            active[k - 1].blockId);
                                    if (branch && branch->next == *next) {
                                        enclosing = k - 1;
                                        break;
                                    }
                                }
                                if (enclosing == active.size())
                                    return std::nullopt;
                                active.resize(enclosing);
                                plan.postCalleeNextBlock =
                                    std::string(*next);
                                break;
                            }
                            // Unknown control/effect PCs remain native.
                            return std::nullopt;
                        }
                    } else if (completedCall->kind ==
                            ir::SourceReturnKind::NextBlock ||
                        completedCall->kind ==
                            ir::SourceReturnKind::AfterBranch) {
                        const auto next =
                            symbols.block(completedCall->targetPc);
                        if (!next) return std::nullopt;
                        if (completedCall->kind ==
                            ir::SourceReturnKind::NextBlock) {
                            if (completed->next != *next)
                                return std::nullopt;
                        } else {
                            // The callee returned from the final block
                            // in an IF arm. The nearest enclosing IF with
                            // this exact structural successor has already
                            // completed; drop that arm and its descendants,
                            // preserving any surrounding repeat frames.
                            auto& active =
                                plan.calls.back().activeControls;
                            size_t enclosing = active.size();
                            for (size_t k = active.size(); k > 0; --k) {
                                if (active[k - 1].isLoop) continue;
                                const auto* branch =
                                    thread.blockContainer->getBlock(
                                        active[k - 1].blockId);
                                if (branch && branch->next == *next) {
                                    enclosing = k - 1;
                                    break;
                                }
                            }
                            if (enclosing == active.size())
                                return std::nullopt;
                            active.resize(enclosing);
                        }
                        plan.postCalleeNextBlock = std::string(*next);
                    } else {
                        // The callee returned to the saved loop latch.
                        // Terminal IF arms are complete and must not be
                        // re-evaluated by the interpreter.
                        if (!unwindTerminalConditionsToLatch(
                                plan.calls.back().activeControls,
                                completedCall->targetPc))
                            return std::nullopt;
                    }
                }
            }
        }
        return plan;
    }

    uint32_t depth() const {
        return image && image->frames ? image->frames->depth() : 0;
    }
    size_t rootedArguments() const {
        return image && image->frames ? image->frames->rootedArguments() : 0;
    }
};

} // namespace xyo
