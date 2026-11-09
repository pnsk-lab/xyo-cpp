// Fail-closed selector for a single interpreter->native nested warp repeat.
// The selected entry executes ONLY the remainder of the loop. Its caller
// retains the interpreter procedure frames and resumes the original suffix.
#pragma once

#include "../model/blocks.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace xyo::jit {

struct NestedWarpLoop {
    const Block* loop = nullptr;
    std::string callId;
    std::string definitionId;
    // Optional outer caller for exactly three live interpreter frames:
    // root script -> outer warp -> inner warp -> selected counted repeat.
    std::string parentCallId;
    // Exact outer-to-inner Scratch callsite path, not native return PCs.
    std::vector<std::string> callPath;
    // Exact interpreter stack, excluding final repeat. Branch arms use
    // 1 (then), 2 (else), 3 (counted parent repeat), or 0 (call).
    std::vector<std::string> framePath;
    std::vector<uint8_t> branchArms;
    // Index of innermost procedure caller in framePath, for GC-rooted args.
    uint32_t argFrameIndex = 0;
};

inline bool safeNestedValue(const Blocks& blocks, std::string id,
                            std::unordered_set<std::string>& seen,
                            size_t& budget,
                            const std::vector<std::u16string>* paramNames = nullptr) {
    if (id.empty()) return true;
    if (!budget-- || !seen.insert(id).second) {
        if (std::getenv("XYO_JIT_OSR_NESTED_TRACE"))
            std::fprintf(stderr, "[nested-scan] value repeated/overbudget id=%s\n", id.c_str());
        return false;
    }
    const Block* b = blocks.getBlock(id);
    if (!b) {
        if (std::getenv("XYO_JIT_OSR_NESTED_TRACE"))
            std::fprintf(stderr, "[nested-scan] value unknown id=%s\n", id.c_str());
        return false;
    }
    const std::string& op = b->opcode;
    if (op == "argument_reporter_string_number" ||
        op == "argument_reporter_boolean") {
        // This reporter is pure ONLY if it names a real, validated
        // parameter. The native standalone loop's emitter delegates it to
        // the existing interpreter reporter trampoline, which reads from
        // the still-live and GC-rooted caller frame. An omitted/misspelled
        // name or an unexpected reporter must fail closed.
        if (!paramNames || !b->next.empty()) return false;
        const auto field = b->fields.find("VALUE");
        if (field == b->fields.end() || !field->second.value.isString() ||
            !field->second.value.asStrPtr()) return false;
        const std::u16string_view name =
            field->second.value.asStrPtr()->view();
        return std::find(paramNames->begin(), paramNames->end(), name) !=
               paramNames->end();
    }
    // Exactly the side-effect-free reporter subset supported by LLVM's
    // normal numeric/list lowering. Unknown extensions and time/random
    // reporters must not be evaluated by a continuation.
    const bool safe = op == "math_number" || op == "math_integer" ||
        op == "math_whole_number" || op == "math_positive_number" ||
        op == "math_angle" || op == "text" ||
        op == "data_variable" || op == "data_itemoflist" ||
        op == "data_lengthoflist" ||
        op == "operator_add" || op == "operator_subtract" ||
        op == "operator_multiply" || op == "operator_divide" ||
        op == "operator_mod" || op == "operator_round" ||
        op == "operator_mathop" ||
        (paramNames && op == "operator_join") ||
        op == "operator_equals" ||
        op == "operator_lt" || op == "operator_gt" ||
        op == "operator_and" || op == "operator_or" ||
        op == "operator_not";
    if (!safe || !b->next.empty()) {
        if (std::getenv("XYO_JIT_OSR_NESTED_TRACE"))
            std::fprintf(stderr, "[nested-scan] value unsafe id=%s op=%s next=%s\n",
                         id.c_str(), op.c_str(), b->next.c_str());
        return false;
    }
    for (const auto& [_, in] : b->inputs.items) {
        // LLVM evaluates a shadow whenever its concrete input is absent.
        // Validate that effective operand as well; otherwise a malformed
        // non-literal shadow could bypass the safe-reporter whitelist.
        const std::string& active = in.block.empty() ? in.shadow : in.block;
        if (!active.empty() &&
            !safeNestedValue(blocks, active, seen, budget, paramNames))
            return false;
    }
    return true;
}

inline bool safeNestedLoopBody(
    const Blocks& blocks, const Block& loop,
    const std::vector<std::u16string>* paramNames = nullptr) {
    if (loop.opcode != "control_repeat") return false;
    size_t budget = 256;
    std::unordered_set<std::string> seen;
    const auto checkInput = [&](const BlockInput* in) {
        if (!in || (in->block.empty() && in->shadow.empty())) return false;
        if (!in->block.empty())
            return safeNestedValue(blocks, in->block, seen, budget, paramNames);
        return safeNestedValue(blocks, in->shadow, seen, budget, paramNames);
    };
    if (!checkInput(loop.inputs.find("TIMES"))) return false;
    const BlockInput* sub = loop.inputs.find("SUBSTACK");
    if (!sub || sub->block.empty()) return false;
    std::vector<std::string> pending{sub->block};
    while (!pending.empty()) {
        std::string id = std::move(pending.back());
        pending.pop_back();
        if (!budget-- || !seen.insert(id).second) return false;
        const Block* b = blocks.getBlock(id);
        if (!b) return false;
        const auto& op = b->opcode;
        const bool allowed = op == "data_changevariableby" ||
            op == "data_setvariableto" || op == "data_addtolist" ||
            op == "data_replaceitemoflist" || op == "control_repeat";
        if (!allowed) return false;
        if (op == "control_repeat" && !checkInput(b->inputs.find("TIMES")))
            return false;
        for (const auto& [name, in] : b->inputs.items) {
            if (name == "SUBSTACK") {
                if (in.block.empty()) return false;
                pending.push_back(in.block);
            } else if (name != "TIMES" || op != "control_repeat") {
                const std::string& active =
                    in.block.empty() ? in.shadow : in.block;
                if (!active.empty() &&
                    !safeNestedValue(blocks, active, seen, budget, paramNames))
                    return false;
            }
        }
        if (!b->next.empty()) pending.push_back(b->next);
    }
    return true;
}

// Bounded, fail-closed lexical source ancestry. LLVM executes only the
// remaining counted repeat; the original interpreter preserves *all* call
// and control frames, GC roots and committed effects. BRANCH requires DEEP.
// Nested control IF/IF-ELSE statements are allowed, but NOT loops that
// invoke procedures, recursive definitions or arbitrary machine return PCs.
inline NestedWarpLoop selectNestedWarpLoop(Blocks& blocks, const Block* top) {
    if (!top) return {};
    const char* argFlag = std::getenv("XYO_JIT_OSR_NESTED_ARG_READS");
    const bool argReads = argFlag && argFlag[0] == '1';
    const char* deepFlag = std::getenv("XYO_JIT_OSR_NESTED_DEEP");
    const bool deep = deepFlag && deepFlag[0] == '1';
    const char* branchFlag = std::getenv("XYO_JIT_OSR_NESTED_BRANCH");
    const bool branches = deep && branchFlag && branchFlag[0] == '1';
    const char* parentRepeatFlag =
        std::getenv("XYO_JIT_OSR_NESTED_PARENT_REPEAT");
    const bool parentRepeats = deep && parentRepeatFlag &&
        parentRepeatFlag[0] == '1';

    constexpr size_t kMaxWarpCallers = 8;
    constexpr size_t kMaxBranches = 4;
    constexpr size_t kMaxParentRepeats = 2;
    constexpr size_t kMaxSourceFrames =
        kMaxWarpCallers + kMaxBranches + kMaxParentRepeats;
    struct SourceCursor {
        std::vector<std::string> frames;
        std::vector<uint8_t> arms;
        std::vector<std::string> calls;
        std::unordered_set<std::string> definitions;
        size_t nBranches = 0;
        size_t nParentRepeats = 0;
    };
    using Scanner = std::function<NestedWarpLoop(
        const Block*, const SourceCursor&)>;
    Scanner scanCall, scanStatements;
    scanCall = [&](const Block* call, const SourceCursor& source)
        -> NestedWarpLoop {
        if (!call || call->opcode != "procedures_call" ||
            !call->mutation || call->mutation->isReporterCall() ||
            source.calls.size() >= (deep ? kMaxWarpCallers : 1) ||
            source.frames.size() >= kMaxSourceFrames)
            return {};
        const std::string& code = call->mutation->attr("proccode");
        const Mutation* proto = blocks.getProcedurePrototype(code);
        if (!proto || !proto->warp() ||
            proto->argumentNames.size() != proto->argumentIds.size() ||
            proto->argumentIds.size() > 16)
            return {};
        const std::string definition = blocks.getProcedureDefinition(code);
        const Block* def = blocks.getBlock(definition);
        if (!def || def->opcode != "procedures_definition" ||
            source.definitions.contains(definition))
            return {};
        SourceCursor child = source;
        child.definitions.insert(definition);
        child.calls.push_back(call->id);
        child.frames.push_back(call->id);
        child.arms.push_back(0);
        // Parameter whitelisting must be scoped to the innermost procedure.
        return scanStatements(blocks.getBlock(def->next), child);
    };
    scanStatements = [&](const Block* first, const SourceCursor& source)
        -> NestedWarpLoop {
        std::unordered_set<std::string> visited;
        for (const Block* block = first;
             block && visited.size() < 128 &&
             visited.insert(block->id).second;
             block = blocks.getBlock(block->next)) {
            if (block->opcode == "control_repeat" &&
                !source.calls.empty()) {
                // Current procedure's definition was validated by scanCall.
                const Block* call =
                    blocks.getBlock(source.calls.back());
                const Mutation* proto = call && call->mutation
                    ? blocks.getProcedurePrototype(
                        call->mutation->attr("proccode")) : nullptr;
                if (proto && safeNestedLoopBody(
                    blocks, *block,
                    argReads ? &proto->argumentNames : nullptr)) {
                    auto path = source.frames;
                    auto arms = source.arms;
                    const auto position = std::find(
                        path.rbegin(), path.rend(), source.calls.back());
                    if (position == path.rend()) return {};
                    const uint32_t argIndex = uint32_t(
                        path.size() - 1 - (position - path.rbegin()));
                    return {block, source.calls.back(),
                            blocks.getProcedureDefinition(
                                call->mutation->attr("proccode")),
                            source.calls.size() > 1
                                ? source.calls[source.calls.size() - 2]
                                : std::string{},
                            source.calls, std::move(path), std::move(arms),
                            argIndex};
                }
            }
            // The root's first warp call remains eligible when DEEP is
            // disabled (the original two-frame OSR). Additional nested
            // procedure calls still require explicit DEEP opt-in.
            if ((deep || source.calls.empty()) &&
                block->opcode == "procedures_call") {
                auto child = scanCall(block, source);
                if (child.loop) return child;
            }
            if (parentRepeats &&
                block->opcode == "control_repeat" &&
                source.nParentRepeats < kMaxParentRepeats &&
                source.frames.size() < kMaxSourceFrames) {
                // The parent TIMES is evaluated by the interpreter.
                // Its *actual* live loop counter is checked at handoff;
                // LLVM must never repeat the parent evaluation or body.
                const BlockInput* times = block->inputs.find("TIMES");
                const BlockInput* sub = block->inputs.find("SUBSTACK");
                if (times && sub && !sub->block.empty()) {
                    const std::string& input =
                        times->block.empty() ? times->shadow
                                             : times->block;
                    std::unordered_set<std::string> seenTimes;
                    size_t timesBudget = 128;
                    if (!input.empty() &&
                        safeNestedValue(blocks, input, seenTimes,
                                        timesBudget)) {
                        SourceCursor nested = source;
                        nested.nParentRepeats++;
                        nested.frames.push_back(block->id);
                        nested.arms.push_back(3);
                        auto candidate = scanStatements(
                            blocks.getBlock(sub->block), nested);
                        if (candidate.loop) return candidate;
                    }
                }
            }
            if (branches && source.nBranches < kMaxBranches &&
                source.frames.size() < kMaxSourceFrames &&
                (block->opcode == "control_if" ||
                 block->opcode == "control_if_else")) {
                const int armCount =
                    block->opcode == "control_if_else" ? 2 : 1;
                for (int arm = 1; arm <= armCount; ++arm) {
                    const std::string firstBranch =
                        blocks.getBranch(block->id, arm);
                    if (firstBranch.empty()) continue;
                    SourceCursor branch = source;
                    branch.nBranches++;
                    branch.frames.push_back(block->id);
                    branch.arms.push_back(uint8_t(arm));
                    auto candidate = scanStatements(
                        blocks.getBlock(firstBranch), branch);
                    if (candidate.loop) return candidate;
                }
            }
        }
        return {};
    };

    // Include the root's linear statements so a root counted repeat may
    // enclose the selected procedure. scanStatements never returns a root
    // repeat as the native target: a validated warp call is mandatory.
    return scanStatements(blocks.getBlock(top->next), SourceCursor{});
}

} // namespace xyo::jit
