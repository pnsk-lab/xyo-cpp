// Read-only, GC-rooted snapshot of an already-started Scratch interpreter.
//
// This is the first half of a deep native-OSR handoff, NOT an executable LLVM
// entry. A compiler must additionally prove a matching source PC, all live SSA
// temporaries, the original warp timer, and effect-commit position before it
// can adopt this snapshot. Never restart a compiled script from its hat.
#pragma once

#include "../ir/gc_value_handles.hpp"
#include "thread.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

namespace xyo::ir {

struct InterpreterOsrFrame {
    struct Argument {
        std::u16string name;
        RootedValueHandle value;
    };

    std::string blockId;
    std::string opcode;
    bool isLoop = false;
    bool warp = false;
    bool callExecuted = false;
    bool hasParams = false;
    bool hasExecutionContext = false;
    std::optional<double> loopCounter;
    std::optional<double> forEachIndex;
    std::vector<Argument> arguments;
};

// Owns the exact Value roots independent of the interpreter's call frames.
// Source/Thread/Target pointers below are identities only and MUST be tested
// against live objects before an attach. The epoch is a scheduler-frame stamp;
// snapshots are not valid across another Runtime::step().
class InterpreterOsrSnapshot {
public:
    static constexpr size_t kMaximumFrames = 64;
    static constexpr size_t kMaximumArguments = 256;

    static std::optional<InterpreterOsrSnapshot> capture(
        const Thread& thread, uint64_t schedulerEpoch) {
        if (!thread.interpreterStarted || thread.isCompiled ||
            thread.isKilled || thread.stackClick || thread.updateMonitor ||
            !thread.target || !thread.blockContainer ||
            thread.stack.empty() ||
            thread.stack.size() > kMaximumFrames ||
            thread.stackFrames.size() != thread.stack.size() ||
            (thread.status != STATUS_RUNNING &&
             thread.status != STATUS_YIELD_TICK &&
             thread.status != STATUS_YIELD))
            return std::nullopt;

        // Source ownership is checked by exact revision and topBlock ID.
        const Block* top = thread.blockContainer->getBlock(thread.topBlock);
        if (!top || (thread.topBlockPtr && top != thread.topBlockPtr) ||
            !top->topLevel) return std::nullopt;

        InterpreterOsrSnapshot image;
        image.origin_ = &thread;
        image.target_ = thread.target;
        image.blocks_ = thread.blockContainer;
        image.revision_ = thread.blockContainer->revision();
        image.epoch_ = schedulerEpoch;
        image.topBlock_ = thread.topBlock;
        image.roots_ = std::make_unique<GcValueHandles>();
        image.frames_.reserve(thread.stack.size());

        size_t totalArguments = 0;
        for (size_t i = 0; i < thread.stack.size(); ++i) {
            const std::string& id = thread.stack[i];
            const auto* block = thread.blockContainer->getBlock(id);
            const StackFrame* frame = thread.stackFrames[i].get();
            // A null sentinel, transient reporter, timer/promise, or
            // partially evaluated primitive has no source-level continuation.
            if (id.empty() || !block || !frame ||
                frame->waitingReporter || frame->hasReported ||
                frame->suspendLast || !frame->reporting.empty() ||
                !frame->reported.empty()) return std::nullopt;

            const std::string& op = block->opcode;
            const bool loop = op == "control_repeat" ||
                op == "control_repeat_until" || op == "control_while" ||
                op == "control_forever" || op == "control_for_each";
            const bool call = op == "procedures_call";
            const bool branch = op == "control_if" ||
                op == "control_if_else";
            // A live caller/branch/loop is a known source-control frame.
            // Procedure definition entries and unknown extension frames
            // cannot be reconstructed from their block ID alone.
            if (!loop && !call && !branch) return std::nullopt;
            if (frame->isLoop && !loop) return std::nullopt;
            if (frame->hasParams && !call) return std::nullopt;

            InterpreterOsrFrame state;
            state.blockId = id;
            state.opcode = op;
            state.isLoop = frame->isLoop;
            state.warp = frame->warpMode;
            state.hasParams = frame->hasParams;
            state.hasExecutionContext = frame->executionContext != nullptr;
            if (const ExecContext* ctx = frame->executionContext.get()) {
                if (ctx->timer || ctx->hasStartedThreads ||
                    !ctx->startedThreads.empty() || !ctx->vals.empty() ||
                    !ctx->returnValue.isUndefined() ||
                    (ctx->hasLoopCounter && op != "control_repeat") ||
                    (ctx->hasIndex && op != "control_for_each"))
                    return std::nullopt;
                state.callExecuted = ctx->executed;
                if (ctx->hasLoopCounter) {
                    if (!std::isfinite(ctx->loopCounter) ||
                        ctx->loopCounter < 0 ||
                        std::floor(ctx->loopCounter) != ctx->loopCounter ||
                        ctx->loopCounter > 1099511627776.0)
                        return std::nullopt;
                    state.loopCounter = ctx->loopCounter;
                }
                if (ctx->hasIndex) {
                    if (!std::isfinite(ctx->index) || ctx->index < 0 ||
                        std::floor(ctx->index) != ctx->index ||
                        ctx->index > 1099511627776.0)
                        return std::nullopt;
                    state.forEachIndex = ctx->index;
                }
            }
            if (frame->hasParams) {
                if (totalArguments + frame->params.size() > kMaximumArguments)
                    return std::nullopt;
                totalArguments += frame->params.size();
                std::vector<std::u16string> keys;
                keys.reserve(frame->params.size());
                for (const auto& [key, value] : frame->params)
                    keys.push_back(key);
                std::sort(keys.begin(), keys.end());
                state.arguments.reserve(keys.size());
                for (const auto& key : keys) {
                    const auto handle = image.roots_->retain(
                        frame->params.at(key));
                    if (!handle) return std::nullopt;
                    state.arguments.push_back({key, *handle});
                }
            }
            image.frames_.push_back(std::move(state));
        }
        return image;
    }

    // A snapshot is valid only at the exact scheduler safe point where it was
    // taken. Re-check source revision, thread identity, frame PCs, payloads,
    // loop counters and warp modes. This prevents reuse after restart/yield.
    bool validFor(const Thread& thread, uint64_t schedulerEpoch) const {
        if (&thread != origin_ || schedulerEpoch != epoch_ ||
            !thread.interpreterStarted || thread.isCompiled ||
            thread.isKilled || thread.stackClick || thread.updateMonitor ||
            (thread.status != STATUS_RUNNING &&
             thread.status != STATUS_YIELD &&
             thread.status != STATUS_YIELD_TICK) ||
            thread.target != target_ ||
            thread.blockContainer != blocks_ ||
            !blocks_ || blocks_->revision() != revision_ ||
            thread.topBlock != topBlock_ ||
            thread.stack.size() != frames_.size() ||
            thread.stackFrames.size() != frames_.size() || !roots_)
            return false;

        for (size_t i = 0; i < frames_.size(); ++i) {
            const auto& captured = frames_[i];
            const StackFrame* frame = thread.stackFrames[i].get();
            const Block* block = blocks_->getBlock(captured.blockId);
            if (!frame || !block ||
                block->opcode != captured.opcode ||
                thread.stack[i] != captured.blockId ||
                frame->isLoop != captured.isLoop ||
                frame->warpMode != captured.warp ||
                frame->hasParams != captured.hasParams ||
                frame->waitingReporter || frame->hasReported ||
                frame->suspendLast || !frame->reporting.empty() ||
                !frame->reported.empty())
                return false;
            const ExecContext* ctx = frame->executionContext.get();
            if (bool(ctx) != captured.hasExecutionContext ||
                (ctx && (ctx->timer || ctx->hasStartedThreads ||
                         !ctx->startedThreads.empty() || !ctx->vals.empty() ||
                         !ctx->returnValue.isUndefined())) ||
                (ctx && ctx->executed) != captured.callExecuted ||
                (ctx && ctx->hasLoopCounter) !=
                    captured.loopCounter.has_value() ||
                (ctx && ctx->hasIndex) != captured.forEachIndex.has_value() ||
                (captured.loopCounter &&
                    (!ctx || ctx->loopCounter != *captured.loopCounter)) ||
                (captured.forEachIndex &&
                    (!ctx || ctx->index != *captured.forEachIndex)) ||
                frame->params.size() != captured.arguments.size())
                return false;
            for (const auto& argument : captured.arguments) {
                const auto it = frame->params.find(argument.name);
                const auto value = roots_->resolve(argument.value);
                if (it == frame->params.end() || !value ||
                    it->second.bits != value->bits)
                    return false;
            }
        }
        return true;
    }

    size_t depth() const { return frames_.size(); }
    const std::vector<InterpreterOsrFrame>& frames() const {
        return frames_;
    }
    size_t rootedArguments() const {
        return roots_ ? roots_->size() : 0;
    }
    std::optional<Value> argument(size_t frame, size_t index) const {
        if (!roots_ || frame >= frames_.size() ||
            index >= frames_[frame].arguments.size())
            return std::nullopt;
        return roots_->resolve(frames_[frame].arguments[index].value);
    }
    uint64_t sourceRevision() const { return revision_; }
    uint64_t schedulerEpoch() const { return epoch_; }

    // Proven ancestry for a nested source continuation. The live
    // interpreter stack alone does NOT prove that a particular procedure
    // definition or IF arm contains its next frame: validate these edges
    // against the versioned Blocks graph without executing reporters.
    struct SourceSite {
        std::string sourceBlockId;
        std::string returnBlockId; // for suspended procedures_call only
        uint8_t branchArm = 0;     // 1/2 for IF(ELSE), 0 otherwise
    };
    std::optional<std::vector<SourceSite>> sourcePath() const {
        if (!blocks_ || frames_.empty()) return std::nullopt;

        // Follow only NEXT and executable SUBSTACK connections. Never
        // cross another procedure-call edge implicitly or inspect reporter
        // values while checking the lexical source path. Reject cycles,
        // opaque/malformed descendants and very large/mutable graphs.
        const auto reachable = [&](std::string start,
                                   const std::string& wanted) {
            std::vector<std::string> pending{std::move(start)};
            std::unordered_set<std::string> visited;
            while (!pending.empty() && visited.size() < 4096) {
                std::string id = std::move(pending.back());
                pending.pop_back();
                if (id.empty() || !visited.emplace(id).second)
                    continue;
                if (id == wanted) return true;
                const auto* block = blocks_->getBlock(id);
                if (!block) return false;
                if (!block->next.empty()) pending.push_back(block->next);
                if (block->opcode == "control_if" ||
                    block->opcode == "control_if_else" ||
                    block->opcode == "control_repeat" ||
                    block->opcode == "control_repeat_until" ||
                    block->opcode == "control_while" ||
                    block->opcode == "control_for_each" ||
                    block->opcode == "control_forever") {
                    const auto a = blocks_->getBranch(id, 1);
                    if (!a.empty()) pending.push_back(a);
                    if (block->opcode == "control_if_else") {
                        const auto b = blocks_->getBranch(id, 2);
                        if (!b.empty()) pending.push_back(b);
                    }
                }
            }
            return false;
        };

        const Block* root = blocks_->getBlock(topBlock_);
        if (!root || !reachable(root->next, frames_[0].blockId))
            return std::nullopt;

        std::vector<SourceSite> sites;
        sites.reserve(frames_.size());
        for (const auto& frame : frames_)
            sites.push_back({frame.blockId, {}, 0});

        for (size_t i = 1; i < frames_.size(); ++i) {
            const auto& ancestor = frames_[i - 1];
            const auto* block = blocks_->getBlock(ancestor.blockId);
            if (!block) return std::nullopt;
            const auto& childId = frames_[i].blockId;
            if (ancestor.opcode == "procedures_call") {
                if (!ancestor.callExecuted || !block->mutation)
                    return std::nullopt;
                const std::string definition = blocks_->getProcedureDefinition(
                    block->mutation->attr("proccode"));
                const Block* entry = blocks_->getBlock(definition);
                if (!entry || entry->opcode != "procedures_definition" ||
                    !reachable(entry->next, childId))
                    return std::nullopt;
                sites[i - 1].returnBlockId = block->next;
            } else if (ancestor.opcode == "control_if" ||
                       ancestor.opcode == "control_if_else") {
                const bool thenArm = reachable(
                    blocks_->getBranch(ancestor.blockId, 1), childId);
                const bool elseArm = ancestor.opcode == "control_if_else" &&
                    reachable(blocks_->getBranch(ancestor.blockId, 2),
                              childId);
                if (thenArm == elseArm) return std::nullopt;
                sites[i - 1].branchArm = thenArm ? 1 : 2;
            } else if (ancestor.isLoop) {
                if (!reachable(
                    blocks_->getBranch(ancestor.blockId, 1), childId))
                    return std::nullopt;
            } else {
                return std::nullopt;
            }
        }
        return sites;
    }

    // A complete source loop latch, with a captured numerical repeat counter,
    // is the most narrowly recoverable point. It is only a *candidate*:
    // no current native JIT entry can adopt a multi-frame interpreter image.
    bool hasCountedLoopCheckpoint() const {
        return !frames_.empty() &&
            frames_.back().opcode == "control_repeat" &&
            frames_.back().isLoop &&
            frames_.back().loopCounter.has_value();
    }

private:
    const Thread* origin_ = nullptr;
    const Target* target_ = nullptr;
    Blocks* blocks_ = nullptr;
    uint64_t revision_ = 0;
    uint64_t epoch_ = 0;
    std::string topBlock_;
    std::unique_ptr<GcValueHandles> roots_;
    std::vector<InterpreterOsrFrame> frames_;
};

} // namespace xyo::ir
