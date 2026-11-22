// Source-certified beta for a multi-script, stackless switch(pc) executor.
// Strictly reject any Scratch control flow that cannot be resumed from a
// simple native PC, an integer repeat count, and a Thread-owned join list.
#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>
#include <vector>

#include "../core/cast.hpp"
#include "../core/sstr.hpp"
#include "../model/blocks.hpp"
#include "mega_bench_end_source.hpp"
#include <cstdio>

namespace xyo::mega_yield {

// Only linear, source-proven argumentless call chains are currently safe.
// The runtime return stack has exactly this fixed maximum capacity.
inline constexpr size_t kMaxProcedureDepth = 4;

struct ReturnSuffix {
    std::string variableId;
    std::u16string variableName;
    double delta = 0;
};

struct Source {
    struct BenchVar {
        std::string id;
        std::u16string name;
    };
    struct BenchRound {
        BenchVar milliseconds, endTime, startTime, round;
        BenchVar total, count, chunks, bestRate;
    };
    struct OuterSet {
        std::string variableId;
        std::u16string variableName;
        bool clock = false;
        bool readVariable = false;
        std::string readVariableId;
        std::u16string readVariableName;
        double literal = 0;
    };
    enum class Kind { Scalar, Repeat, BroadcastWait, BroadcastRepeat,
                      BroadcastUntil };
    Kind kind = Kind::Repeat;
    std::u16string broadcastName;
    std::string variableId;
    std::u16string variableName;
    uint64_t repetitions = 0;
    double delta = 0;
    // An optional second in-order scalar statement in the same Scratch
    // turn. The strict source proof stops after at most two statements.
    bool hasSecondChange = false;
    std::string secondVariableId;
    std::u16string secondVariableName;
    double secondDelta = 0;
    // Source-certified if/else: compare one Scratch scalar variable to a
    // finite numeric literal, then execute exactly one constant addition.
    // This never coexists with hasSecondChange or a procedure return chain.
    enum class Comparison : uint8_t { Greater, Less, Equal };
    bool hasConditional = false;
    bool hasElseBranch = false;
    // Ordered second write in each conditional arm. These are independent
    // from the two-write straight-line scalar source.
    bool hasThenSecondChange = false;
    std::string thenSecondVariableId;
    std::u16string thenSecondVariableName;
    double thenSecondDelta = 0;
    bool hasElseSecondChange = false;
    std::string elseSecondVariableId;
    std::u16string elseSecondVariableName;
    double elseSecondDelta = 0;
    Comparison comparison = Comparison::Greater;
    std::string conditionVariableId;
    std::u16string conditionVariableName;
    double conditionLiteral = 0;
    // Preserve Scratch operand order when the constant is on the left.
    // For numbers LLVM uses the same order; helper fallback must NOT
    // silently swap gt/lt when a nonnumeric string is encountered.
    bool conditionLiteralLeft = false;
    // A source-proven repeat-until condition may be operator_not
    // wrapping one numeric comparison; negate only the final Scratch
    // boolean, never the numeric reporter or its operand order.
    bool untilConditionNegated = false;
    // The only permitted non-variable dynamic reporter in this beta.
    // Re-evaluated through the exact synchronous Scratch JIT helper
    // after each completed joined broadcast; never cached across a yield.
    bool untilClockReporter = false;
    // For the strict clock-vs-variable form, the other operand is
    // reloaded on every comparison, not captured at the first yield.
    bool untilCompareVariable = false;
    // Strict two-variable until comparison, opt-in for fixed-work
    // scheduler benchmarks. Both operands are reloaded at every join.
    bool untilVariablePair = false;
    // Exact ScratchBench-style elapsed milliseconds reporter:
    // (daysSince2000() - t0) * 86400000. Both arithmetic operations
    // must preserve Scratch's operand order and tagged-value behavior.
    bool untilClockElapsedMs = false;
    // Source-proven bounded outer repeat. Each iteration executes exactly
    // two Scratch setters (a literal and the current wall clock), then the
    // inner repeat-until broadcast loop. The outer repeat counter lives in
    // Thread::megaRemaining throughout joins, never on the LLVM stack.
    bool hasOuterUntil = false;
    OuterSet outerFirstSet;
    OuterSet outerSecondSet;
    // Optional source-certified measurements immediately AFTER the
    // dynamic until has become true, and BEFORE the numeric inner suffix.
    // Both setters commit once per outer iteration, even for zero joins.
    bool hasInnerAfterSets = false;
    OuterSet innerAfterFirstSet;
    OuterSet innerAfterSecondSet;
    // A SOURCE-VALIDATED argumentless bench_round body, executed
    // synchronously after the post-until snapshots; no omitted calls.
    bool hasBenchRound = false;
    BenchRound benchRound;
    ReturnSuffix outerSuffix;
    // Exact ScratchBench bench_begin five-set prelude (once per call).
    std::vector<OuterSet> beginSets;
    // Alternative final setter, executed once after the outer repeat.
    bool hasOuterFinalSet = false;
    OuterSet outerFinalSet;
    bool hasBenchEnd = false;
    std::string untilStartVariableId;
    std::u16string untilStartVariableName;
    std::string untilOtherVariableId;
    std::u16string untilOtherVariableName;
    std::string elseVariableId;
    std::u16string elseVariableName;
    double elseDelta = 0;
    // A source-proven, argument-free, non-warp procedure call can be
    // lowered into the same per-Thread continuation.
    bool tailProcedure = false;
    // Ordered OUTERMOST -> INNERMOST. The native dispatcher pushes these
    // fixed return PCs into Thread storage, and pops on procedure return.
    // Tail procedure calls contribute no return instruction.
    std::vector<ReturnSuffix> returnSuffixes;
};

inline bool literalNumber(const Blocks& blocks, const BlockInput* input,
                          double& result) {
    if (!input || input->block.empty() || input->block != input->shadow)
        return false;
    const Block* b = blocks.getBlock(input->block);
    if (!b || !b->shadow || !b->next.empty() ||
        (b->opcode != "math_number" &&
         b->opcode != "math_integer" &&
         b->opcode != "math_whole_number" &&
         b->opcode != "math_positive_number")) return false;
    const auto it = b->fields.find("NUM");
    if (it == b->fields.end()) return false;
    result = cast::toNumber(it->second.value);
    return std::isfinite(result);
}

// Accept only a fixed ASCII menu literal. Dynamic broadcast-name reporters,
// textual expressions and Unicode mapping are not this beta's contract.
inline bool literalBroadcast(const Blocks& blocks, const Block& send,
                             std::u16string& message,
                             bool allowNext = false) {
    if (send.opcode != "event_broadcastandwait" ||
        (!allowNext && !send.next.empty()))
        return false;
    const BlockInput* input = send.inputs.find("BROADCAST_INPUT");
    if (!input || input->block.empty()) return false;
    const Block* literal = blocks.getBlock(input->block);
    if (!literal || !literal->shadow ||
        literal->opcode != "event_broadcast_menu" ||
        !literal->next.empty()) return false;
    const auto field = literal->fields.find("BROADCAST_OPTION");
    if (field == literal->fields.end() || !field->second.value.isString())
        return false;
    const SStr* value = field->second.value.asStrPtr();
    if (!value) return false;
    const auto name = value->view();
    if (name.empty() || name.size() > 128) return false;
    for (char16_t ch : name)
        if (ch == 0 || ch >= 128) return false;
    message.assign(name);
    return true;
}

inline bool scalarChange(const Blocks& blocks, const Block* change,
                         Source& result, bool allowNext = false) {
    if (!change || change->opcode != "data_changevariableby" ||
        (!allowNext && !change->next.empty())) return false;
    double delta = 0;
    if (!literalNumber(blocks, change->inputs.find("VALUE"), delta))
        return false;
    const auto field = change->fields.find("VARIABLE");
    if (field == change->fields.end() || field->second.id.empty() ||
        !field->second.value.isString()) return false;
    result.variableId = field->second.id;
    result.delta = delta;
    if (auto* name = field->second.value.asStrPtr())
        result.variableName.assign(name->view());
    else
        result.variableName.clear();
    return true;
}

// A source-certified sequential pair. The second statement has NO next
// link; this is deliberately not a general-purpose Scratch basic block.
inline bool scalarChangeSequence(const Blocks& blocks, const Block* first,
                                 Source& result) {
    if (!scalarChange(blocks, first, result, true)) return false;
    if (first->next.empty()) return true;
    Source second;
    if (!scalarChange(blocks, blocks.getBlock(first->next), second))
        return false;
    result.hasSecondChange = true;
    result.secondVariableId = std::move(second.variableId);
    result.secondVariableName = std::move(second.variableName);
    result.secondDelta = second.delta;
    return true;
}

inline bool conditionalEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("XYO_JIT_MEGA_IF_ELSE_BATCH");
        return env && env[0] == '1';
    }();
    return enabled;
}

inline bool branchPairEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("XYO_JIT_MEGA_BRANCH_PAIR");
        return env && env[0] == '1';
    }();
    return enabled;
}

// Narrow if or if/else proof: compare a scalar reporter to a finite
// literal. Each present arm has one or two ordered constant additions.
// The missing ELSE arm of control_if is a strict no-op.
inline bool numericIfConditional(const Blocks& blocks,
                                 const Block* statement, Source& result) {
    if (!conditionalEnabled() || !statement ||
        (statement->opcode != "control_if_else" &&
         statement->opcode != "control_if") ||
        !statement->next.empty()) return false;
    const bool hasElse = statement->opcode == "control_if_else";
    const BlockInput* condition = statement->inputs.find("CONDITION");
    const BlockInput* yes = statement->inputs.find("SUBSTACK");
    const BlockInput* no = statement->inputs.find("SUBSTACK2");
    if (!condition || condition->block.empty() ||
        !yes || yes->block.empty() ||
        (hasElse && (!no || no->block.empty())) ||
        (!hasElse && no)) return false;
    const Block* test = blocks.getBlock(condition->block);
    if (!test || !test->next.empty()) return false;
    if (test->opcode == "operator_gt")
        result.comparison = Source::Comparison::Greater;
    else if (test->opcode == "operator_lt")
        result.comparison = Source::Comparison::Less;
    else if (test->opcode == "operator_equals")
        result.comparison = Source::Comparison::Equal;
    else return false;
    const BlockInput* left = test->inputs.find("OPERAND1");
    const BlockInput* right = test->inputs.find("OPERAND2");
    if (!left || !right || left->block.empty() ||
        right->block.empty()) return false;
    const BlockInput* variableInput = left;
    if (literalNumber(blocks, right, result.conditionLiteral)) {
        result.conditionLiteralLeft = false;
    } else if (literalNumber(blocks, left,
                             result.conditionLiteral)) {
        result.conditionLiteralLeft = true;
        variableInput = right;
    } else return false;
    const Block* variable = blocks.getBlock(variableInput->block);
    if (!variable || variable->opcode != "data_variable" ||
        !variable->next.empty() || !variable->inputs.empty()) return false;
    const auto var = variable->fields.find("VARIABLE");
    if (var == variable->fields.end() || var->second.id.empty() ||
        !var->second.value.isString()) return false;
    result.conditionVariableId = var->second.id;
    if (auto* name = var->second.value.asStrPtr())
        result.conditionVariableName.assign(name->view());
    Source thenChange;
    if (!scalarChangeSequence(blocks, blocks.getBlock(yes->block),
                              thenChange) ||
        (thenChange.hasSecondChange && !branchPairEnabled()))
        return false;
    result.variableId = std::move(thenChange.variableId);
    result.variableName = std::move(thenChange.variableName);
    result.delta = thenChange.delta;
    result.hasThenSecondChange = thenChange.hasSecondChange;
    result.thenSecondVariableId = std::move(thenChange.secondVariableId);
    result.thenSecondVariableName = std::move(thenChange.secondVariableName);
    result.thenSecondDelta = thenChange.secondDelta;
    if (hasElse) {
        Source elseChange;
        if (!scalarChangeSequence(blocks, blocks.getBlock(no->block),
                                  elseChange) ||
            (elseChange.hasSecondChange && !branchPairEnabled()))
            return false;
        result.elseVariableId = std::move(elseChange.variableId);
        result.elseVariableName = std::move(elseChange.variableName);
        result.elseDelta = elseChange.delta;
        result.hasElseSecondChange = elseChange.hasSecondChange;
        result.elseSecondVariableId =
            std::move(elseChange.secondVariableId);
        result.elseSecondVariableName =
            std::move(elseChange.secondVariableName);
        result.elseSecondDelta = elseChange.secondDelta;
    }
    result.hasConditional = true;
    result.hasElseBranch = hasElse;
    return true;
}

// Narrow repeat-until condition: one Scratch scalar compared to a finite
// numeric literal, optionally inside operator_not. Reevaluate after EACH
// completed broadcast join: the receiver may have changed the variable.
// No value, borrowed pointer or intermediate expression survives a yield.
inline bool numericUntilCondition(const Blocks& blocks,
                                  const Block* condition, Source& out) {
    if (!condition) return false;
    if (condition->opcode == "operator_not") {
        const BlockInput* input = condition->inputs.find("OPERAND");
        if (!input || input->block.empty() ||
            !condition->next.empty()) return false;
        out.untilConditionNegated = true;
        condition = blocks.getBlock(input->block);
        if (!condition) return false;
    }
    if (!condition->next.empty() || !condition->fields.empty())
        return false;
    if (condition->opcode == "operator_gt")
        out.comparison = Source::Comparison::Greater;
    else if (condition->opcode == "operator_lt")
        out.comparison = Source::Comparison::Less;
    else if (condition->opcode == "operator_equals")
        out.comparison = Source::Comparison::Equal;
    else return false;
    const BlockInput* lhs = condition->inputs.find("OPERAND1");
    const BlockInput* rhs = condition->inputs.find("OPERAND2");
    if (!lhs || !rhs || lhs->block.empty() || rhs->block.empty())
        return false;
    const BlockInput* variableInput = lhs;
    if (literalNumber(blocks, rhs, out.conditionLiteral)) {
        out.conditionLiteralLeft = false;
    } else if (literalNumber(blocks, lhs, out.conditionLiteral)) {
        out.conditionLiteralLeft = true;
        variableInput = rhs;
    } else {
        // Permit exactly ONE time reporter and ONE data-variable reader.
        // Arbitrary Scratch reporter expressions, lists, functions, time
        // arithmetic and two-variable tests remain on native Fiber.
        const Block* l = blocks.getBlock(lhs->block);
        const Block* r = blocks.getBlock(rhs->block);
        const auto isClock = [](const Block* node) {
            return node && node->opcode == "sensing_dayssince2000" &&
                !node->shadow && node->next.empty() &&
                node->inputs.empty() && node->fields.empty();
        };
        const auto variableField = [](const Block* node)
            -> const BlockField* {
            if (!node || node->opcode != "data_variable" ||
                !node->next.empty() || !node->inputs.empty())
                return nullptr;
            const auto field = node->fields.find("VARIABLE");
            if (field == node->fields.end() || field->second.id.empty() ||
                !field->second.value.isString() ||
                !field->second.value.asStrPtr()) return nullptr;
            return &field->second;
        };
        bool elapsedMs = false;
        const Block* elapsed = nullptr;
        const Block* other = nullptr;
        static const bool pairEnabled = [] {
            const char* flag = std::getenv(
                "XYO_JIT_MEGA_UNTIL_VAR_PAIR");
            return flag && flag[0] == '1' && flag[1] == '\0';
        }();
        if (pairEnabled && variableField(l) && variableField(r)) {
            const auto* firstVar = variableField(l);
            const auto* secondVar = variableField(r);
            if (l->shadow || r->shadow || l->fields.size() != 1 ||
                r->fields.size() != 1 || condition->inputs.size() != 2)
                return false;
            out.conditionVariableId = firstVar->id;
            out.conditionVariableName.assign(
                firstVar->value.asStrPtr()->view());
            out.untilOtherVariableId = secondVar->id;
            out.untilOtherVariableName.assign(
                secondVar->value.asStrPtr()->view());
            out.untilCompareVariable = true;
            out.untilVariablePair = true;
            out.conditionLiteralLeft = false;
            return true;
        }
        if (isClock(l) != isClock(r)) {
            other = isClock(l) ? r : l;
            out.conditionLiteralLeft = isClock(r);
        } else if (l && l->opcode == "operator_multiply" &&
                   r && variableField(r) &&
                   l->next.empty() && l->fields.empty()) {
            // Accept only the exact ScratchBench clock expression:
            // (daysSince2000() - t0) * 86400000 < b_budget.
            // Arbitrary expression trees (including swapped arithmetic
            // operands) must continue through the ordinary Fiber JIT.
            const BlockInput* left = l->inputs.find("NUM1");
            const BlockInput* right = l->inputs.find("NUM2");
            double multiplier = 0;
            if (!left || !right ||
                !literalNumber(blocks, right, multiplier) ||
                multiplier != 86400000.0) return false;
            const Block* sub = blocks.getBlock(left->block);
            if (!sub || sub->opcode != "operator_subtract" ||
                !sub->next.empty() || !sub->fields.empty())
                return false;
            const BlockInput* minuend = sub->inputs.find("NUM1");
            const BlockInput* subtrahend = sub->inputs.find("NUM2");
            const Block* clock = minuend
                ? blocks.getBlock(minuend->block) : nullptr;
            const Block* base = subtrahend
                ? blocks.getBlock(subtrahend->block) : nullptr;
            if (!isClock(clock)) return false;
            const auto* startField = variableField(base);
            if (!startField) return false;
            out.untilStartVariableId = startField->id;
            if (const auto* name = startField->value.asStrPtr())
                out.untilStartVariableName.assign(name->view());
            out.conditionLiteralLeft = false;
            other = r;
            elapsedMs = true;
        } else return false;
        const auto* field = variableField(other);
        if (!field) return false;
        out.untilClockReporter = true;
        out.untilCompareVariable = true;
        out.untilClockElapsedMs = elapsedMs;
        out.untilOtherVariableId = field->id;
        if (const auto* name = field->value.asStrPtr())
            out.untilOtherVariableName.assign(name->view());
        return true;
    }
    const Block* variable = blocks.getBlock(variableInput->block);
    if (!variable || !variable->next.empty() ||
        !variable->inputs.empty()) return false;
    if (variable->opcode == "sensing_dayssince2000") {
        // No Scratch arguments or external reporter subtree; each LLVM
        // entry samples precisely once at this comparison evaluation.
        if (variable->shadow || !variable->fields.empty()) return false;
        out.untilClockReporter = true;
        return true;
    }
    if (variable->opcode != "data_variable") return false;
    const auto field = variable->fields.find("VARIABLE");
    if (field == variable->fields.end() || field->second.id.empty() ||
        !field->second.value.isString()) return false;
    out.conditionVariableId = field->second.id;
    if (const auto* name = field->second.value.asStrPtr())
        out.conditionVariableName.assign(name->view());
    return true;
}

// Narrow two-set prelude for the single outer repeat β. The helper
// accepts a literal numeric setter or the zero-input time reporter, but no
// other reporter subtree, variables-as-values, or multi-statement suffix.
// Both setters are run anew at the start of EVERY outer iteration.
inline bool outerUntilSetter(const Blocks& blocks, const Block* statement,
                             Source::OuterSet& out) {
    if (!statement || statement->opcode != "data_setvariableto")
        return false;
    const auto field = statement->fields.find("VARIABLE");
    if (field == statement->fields.end() || field->second.id.empty() ||
        !field->second.value.isString()) return false;
    out.variableId = field->second.id;
    if (const auto* name = field->second.value.asStrPtr())
        out.variableName.assign(name->view());
    const BlockInput* input = statement->inputs.find("VALUE");
    if (!input) return false;
    if (literalNumber(blocks, input, out.literal)) return true;
    const Block* reporter = blocks.getBlock(input->block);
    if (!reporter || reporter->opcode != "sensing_dayssince2000" ||
        reporter->shadow || !reporter->next.empty() ||
        !reporter->inputs.empty() || !reporter->fields.empty())
        return false;
    out.clock = true;
    return true;
}

// The source-bounded post-until measurement pair may snapshot either the
// current wall clock or a live Scratch scalar. Unlike initialization this
// accepts a data_variable value, but no arbitrary reporter expressions.
// A source revision change invalidates the entire compiled continuation.
inline bool innerAfterSetter(const Blocks& blocks, const Block* statement,
                             Source::OuterSet& out) {
    if (!outerUntilSetter(blocks, statement, out)) {
        if (!statement || statement->opcode != "data_setvariableto")
            return false;
        const auto field = statement->fields.find("VARIABLE");
        const auto value = statement->inputs.find("VALUE");
        const Block* reporter = value
            ? blocks.getBlock(value->block) : nullptr;
        if (field == statement->fields.end() || field->second.id.empty() ||
            !field->second.value.isString() ||
            !reporter || reporter->opcode != "data_variable" ||
            !reporter->next.empty() || !reporter->inputs.empty())
            return false;
        const auto inputField = reporter->fields.find("VARIABLE");
        if (inputField == reporter->fields.end() ||
            inputField->second.id.empty() ||
            !inputField->second.value.isString()) return false;
        out.variableId = field->second.id;
        if (const auto* name = field->second.value.asStrPtr())
            out.variableName.assign(name->view());
        out.readVariableId = inputField->second.id;
        if (const auto* name = inputField->second.value.asStrPtr())
            out.readVariableName.assign(name->view());
        out.readVariable = true;
    }
    return true;
}

// Source-certified argumentless ScratchBench bench_begin prelude.
// Do not accept extensions, extra statements, reordered writes, or args.
inline bool proveBenchBegin(Blocks& blocks, const Block* call,
                            std::vector<Source::OuterSet>& sets) {
    if (!call || call->opcode != "procedures_call" ||
        !call->inputs.empty() || !call->fields.empty() ||
        !call->mutation || call->mutation->warp() ||
        call->mutation->isReporterCall() ||
        call->mutation->attr("proccode") != "bench_begin")
        return false;
    call->mutation->parseArgs();
    if (!call->mutation->argumentIds.empty()) return false;
    const Block* def = blocks.getBlock(
        blocks.getProcedureDefinition("bench_begin"));
    if (!def || def->opcode != "procedures_definition" ||
        !def->topLevel || def->inputs.size() != 1 ||
        !def->fields.empty()) return false;
    const auto* input = def->inputs.find("custom_block");
    const Block* proto = input ? blocks.getBlock(input->block) : nullptr;
    if (!proto || proto->opcode != "procedures_prototype" ||
        !proto->fields.empty() || !proto->inputs.empty() ||
        !proto->mutation || proto->mutation->warp() ||
        proto->mutation->isReporterCall() ||
        proto->mutation->attr("proccode") != "bench_begin")
        return false;
    proto->mutation->parseArgs();
    if (!proto->mutation->argumentIds.empty()) return false;

    static constexpr std::u16string_view expected[] = {
        u"b_round", u"b_total", u"b_chunks", u"b_bestrate", u"b_chk"
    };
    const Block* statement = blocks.getBlock(def->next);
    std::vector<Source::OuterSet> validated;
    for (auto name : expected) {
        Source::OuterSet set;
        if (!statement || statement->opcode != "data_setvariableto" ||
            statement->inputs.size() != 1 ||
            statement->fields.size() != 1 ||
            !outerUntilSetter(blocks, statement, set) ||
            set.clock || set.readVariable || set.literal != 0 ||
            set.variableName != name)
            return false;
        validated.push_back(std::move(set));
        statement = blocks.getBlock(statement->next);
    }
    if (statement) return false;
    sets = std::move(validated);
    return true;
}

// Strictly recognize the exact side-effect tree of ScratchBench
// bench_round, rather than trusting a procedure name or inlining an
// arbitrary reporter program. Reject all extra statements/arguments.
// Source revision and per-Thread variable bindings are checked by the
// caller; LLVM executes this expression tree in Scratch's original order.
inline bool benchField(const Block* node, std::string_view opcode,
                       Source::BenchVar& out) {
    if (!node || node->opcode != opcode ||
        node->fields.size() != 1) return false;
    if (opcode == "data_variable" &&
        (!node->next.empty() || !node->inputs.empty())) return false;
    if (opcode == "data_setvariableto" &&
        node->inputs.size() != 1) return false;
    if (opcode == "data_changevariableby" &&
        node->inputs.size() != 1) return false;
    const auto field = node->fields.find("VARIABLE");
    if (field == node->fields.end() || field->second.id.empty() ||
        !field->second.value.isString()) return false;
    out.id = field->second.id;
    if (const auto* name = field->second.value.asStrPtr())
        out.name.assign(name->view());
    return true;
}

inline bool benchRead(const Blocks& blocks, const Block* owner,
                      std::string_view key, Source::BenchVar& out) {
    if (!owner) return false;
    const BlockInput* input = owner->inputs.find(std::string(key));
    const Block* value = input ? blocks.getBlock(input->block) : nullptr;
    return benchField(value, "data_variable", out);
}

inline bool benchSame(const Source::BenchVar& a,
                      const Source::BenchVar& b) {
    return !a.id.empty() && a.id == b.id;
}

inline bool benchBinary(const Blocks& blocks, const Block* expr,
                        std::string_view opcode,
                        std::string_view lhsKey, std::string_view rhsKey,
                        Source::BenchVar& lhs,
                        Source::BenchVar& rhs) {
    return expr && expr->opcode == opcode &&
        expr->next.empty() && expr->fields.empty() &&
        expr->inputs.size() == 2 &&
        benchRead(blocks, expr, lhsKey, lhs) &&
        benchRead(blocks, expr, rhsKey, rhs);
}

inline bool benchDivision(const Blocks& blocks, const Block* division,
                          const Source::BenchRound& out) {
    Source::BenchVar num, den;
    return benchBinary(blocks, division, "operator_divide",
                       "NUM1", "NUM2", num, den) &&
        benchSame(num, out.count) &&
        benchSame(den, out.milliseconds);
}

inline bool proveBenchRound(Blocks& blocks, const Block* call,
                            Source::BenchRound& out) {
    if (!call || call->opcode != "procedures_call" ||
        !call->inputs.empty() || !call->fields.empty() ||
        !call->next.empty() ||
        !call->mutation || call->mutation->warp() ||
        call->mutation->isReporterCall() ||
        call->mutation->attr("proccode") != "bench_round")
        return false;
    call->mutation->parseArgs();
    if (!call->mutation->argumentIds.empty()) return false;
    const Block* def = blocks.getBlock(
        blocks.getProcedureDefinition("bench_round"));
    if (!def || def->opcode != "procedures_definition" ||
        !def->topLevel || def->inputs.size() != 1 ||
        !def->fields.empty()) return false;
    const BlockInput* custom = def->inputs.find("custom_block");
    const Block* proto = custom
        ? blocks.getBlock(custom->block) : nullptr;
    if (!proto || proto->opcode != "procedures_prototype" ||
        !proto->inputs.empty() || !proto->fields.empty() ||
        !proto->mutation ||
        proto->mutation->attr("proccode") != "bench_round" ||
        proto->mutation->warp() || proto->mutation->isReporterCall())
        return false;
    proto->mutation->parseArgs();
    if (!proto->mutation->argumentIds.empty()) return false;

    // b_ms = (b_t1 - b_t0) * 86400000.
    const Block* millisecondsSet = blocks.getBlock(def->next);
    if (!benchField(millisecondsSet, "data_setvariableto",
                    out.milliseconds)) return false;
    const BlockInput* value = millisecondsSet->inputs.find("VALUE");
    const Block* mul = value
        ? blocks.getBlock(value->block) : nullptr;
    if (!mul || mul->opcode != "operator_multiply" ||
        !mul->next.empty() || !mul->fields.empty() ||
        mul->inputs.size() != 2) return false;
    const BlockInput* mulLhs = mul->inputs.find("NUM1");
    double factor = 0;
    if (!literalNumber(blocks, mul->inputs.find("NUM2"), factor) ||
        factor != 86400000.0 || !mulLhs) return false;
    const Block* subtract = blocks.getBlock(mulLhs->block);
    if (!benchBinary(blocks, subtract, "operator_subtract",
                     "NUM1", "NUM2", out.endTime, out.startTime))
        return false;

    // if b_round > 0:
    //   change b_total by b_ms
    //   change b_chunks by b_n
    //   if (b_n / b_ms) > b_bestrate:
    //     set b_bestrate to (b_n / b_ms)
    const Block* ifRound = blocks.getBlock(millisecondsSet->next);
    const Block* incrementRound = ifRound
        ? blocks.getBlock(ifRound->next) : nullptr;
    if (!ifRound || ifRound->opcode != "control_if" ||
        ifRound->inputs.size() != 2 || !ifRound->fields.empty() ||
        !incrementRound || incrementRound->opcode !=
            "data_changevariableby" ||
        !incrementRound->next.empty()) return false;
    const BlockInput* roundCond = ifRound->inputs.find("CONDITION");
    const BlockInput* branch = ifRound->inputs.find("SUBSTACK");
    const Block* gtZero = roundCond
        ? blocks.getBlock(roundCond->block) : nullptr;
    if (!gtZero || gtZero->opcode != "operator_gt" ||
        gtZero->inputs.size() != 2 ||
        !gtZero->next.empty() || !gtZero->fields.empty())
        return false;
    if (!benchRead(blocks, gtZero, "OPERAND1", out.round))
        return false;
    double zero = 1;
    if (!literalNumber(blocks, gtZero->inputs.find("OPERAND2"), zero) ||
        zero != 0) return false;

    Source::BenchVar roundDest;
    double one = 0;
    if (!benchField(incrementRound, "data_changevariableby", roundDest) ||
        !benchSame(roundDest, out.round) ||
        !literalNumber(blocks, incrementRound->inputs.find("VALUE"), one) ||
        one != 1) return false;

    const Block* totalChange = branch
        ? blocks.getBlock(branch->block) : nullptr;
    const Block* chunksChange = totalChange
        ? blocks.getBlock(totalChange->next) : nullptr;
    const Block* ifRate = chunksChange
        ? blocks.getBlock(chunksChange->next) : nullptr;
    Source::BenchVar totalDelta, chunksDelta;
    if (!benchField(totalChange, "data_changevariableby", out.total) ||
        !benchRead(blocks, totalChange, "VALUE", totalDelta) ||
        !benchSame(totalDelta, out.milliseconds) ||
        !benchField(chunksChange, "data_changevariableby", out.chunks) ||
        !benchRead(blocks, chunksChange, "VALUE", chunksDelta) ||
        !ifRate || ifRate->opcode != "control_if" ||
        ifRate->inputs.size() != 2 || !ifRate->fields.empty() ||
        !ifRate->next.empty()) return false;
    out.count = chunksDelta;
    const BlockInput* rateCond = ifRate->inputs.find("CONDITION");
    const BlockInput* rateBranch = ifRate->inputs.find("SUBSTACK");
    const Block* gtRate = rateCond
        ? blocks.getBlock(rateCond->block) : nullptr;
    const Block* setBest = rateBranch
        ? blocks.getBlock(rateBranch->block) : nullptr;
    if (!gtRate || gtRate->opcode != "operator_gt" ||
        gtRate->inputs.size() != 2 ||
        !gtRate->next.empty() || !gtRate->fields.empty() ||
        !benchField(setBest, "data_setvariableto", out.bestRate) ||
        !setBest->next.empty()) return false;
    const BlockInput* ratioLeft = gtRate->inputs.find("OPERAND1");
    const Block* ratio = ratioLeft
        ? blocks.getBlock(ratioLeft->block) : nullptr;
    Source::BenchVar currentBest;
    if (!benchDivision(blocks, ratio, out) ||
        !benchRead(blocks, gtRate, "OPERAND2", currentBest) ||
        !benchSame(currentBest, out.bestRate)) return false;
    const BlockInput* assignRatio = setBest->inputs.find("VALUE");
    const Block* assigned = assignRatio
        ? blocks.getBlock(assignRatio->block) : nullptr;
    return benchDivision(blocks, assigned, out);
}

inline bool prove(Blocks& blocks, const Block& top, Source& result) {
    // The event hat has already been matched/started by Runtime; it
    // produces no reporter value and the receiver body begins at top.next.
    // Source eligibility is otherwise identical to a green-flag script.
    if (top.opcode != "event_whenflagclicked" &&
        top.opcode != "event_whenbroadcastreceived") return false;
    const Block* first = blocks.getBlock(top.next);
    if (!first) return false;

    // Resolve at most four direct, nonrecursive argumentless procedures.
    // The only statements allowed after an individual call are a sole
    // constant variable addition (or no statement for a tail call).
    // No dynamic Scratch arguments or native CallFrame survive a yield.
    std::unordered_set<std::string> visited;
    for (size_t depth = 0; first->opcode == "procedures_call"; ++depth) {
        // bench_begin is a source-certified five-write prelude, not an
        // argumentless tail whose body can replace the outer dispatcher.
        if (first->mutation &&
            first->mutation->attr("proccode") == "bench_begin")
            break;
        if (depth >= kMaxProcedureDepth || !first->inputs.empty() ||
            !first->mutation || first->mutation->isReporterCall() ||
            first->mutation->warp()) return false;
        if (!first->next.empty()) {
            Source scalar;
            if (!scalarChange(blocks, blocks.getBlock(first->next), scalar))
                return false;
            result.returnSuffixes.push_back({
                std::move(scalar.variableId),
                std::move(scalar.variableName), scalar.delta});
        }
        first->mutation->parseArgs();
        if (!first->mutation->argumentIds.empty()) return false;
        const std::string code = first->mutation->attr("proccode");
        if (code.empty() || !visited.insert(code).second) return false;
        const std::string defId = blocks.getProcedureDefinition(code);
        const Block* definition = blocks.getBlock(defId);
        if (!definition || !definition->topLevel ||
            definition->opcode != "procedures_definition") return false;
        const BlockInput* protoInput = definition->inputs.find("custom_block");
        const Block* proto = protoInput
            ? blocks.getBlock(protoInput->block) : nullptr;
        if (!proto || proto->opcode != "procedures_prototype" ||
            !proto->mutation || proto->mutation->attr("proccode") != code ||
            proto->mutation->warp() ||
            proto->mutation->isReporterCall()) return false;
        proto->mutation->parseArgs();
        if (!proto->mutation->argumentIds.empty()) return false;
        first = blocks.getBlock(definition->next);
        if (!first) return false;
        result.tailProcedure = true;
    }

    // The real sched_pingpong begins with five synchronous resets.
    // Run them before the first outer iteration, including repeat(0).
    if (first->opcode == "procedures_call" &&
        first->mutation &&
        first->mutation->attr("proccode") == "bench_begin") {
        static const bool beginEnabled = [] {
            const char* flag = std::getenv(
                "XYO_JIT_MEGA_UNTIL_BENCH_BEGIN");
            return flag && flag[0] == '1' && flag[1] == '\0';
        }();
        if (!beginEnabled ||
            !proveBenchBegin(blocks, first, result.beginSets))
            return false;
        first = blocks.getBlock(first->next);
        if (!first || first->opcode != "control_repeat")
            return false;
    }

    // A broadcast receiver may consist of one synchronous numeric
    // update. It has no suspension PC; its Thread finishes in one entry.
    // This is important for ScratchBench's clone fanout handlers.
    if (first->opcode == "data_changevariableby" ||
        first->opcode == "control_if_else" ||
        first->opcode == "control_if") {
        if (first->opcode != "data_changevariableby") {
            // No procedure return suffix is allowed across a speculative
            // conditional source proof. The normal interpreter/JIT handles it.
            if (!result.returnSuffixes.empty() ||
                !numericIfConditional(blocks, first, result)) return false;
        } else if (!scalarChangeSequence(blocks, first, result)) return false;
        result.kind = Source::Kind::Scalar;
        return true;
    }

    if (first->opcode == "event_broadcastandwait") {
        // One broadcast/join followed by exactly one numeric suffix.
        if (!literalBroadcast(blocks, *first, result.broadcastName, true) ||
            !scalarChange(blocks, blocks.getBlock(first->next), result))
            return false;
        result.kind = Source::Kind::BroadcastWait;
        return true;
    }

    static const bool untilEnabled = [] {
        const char* flag = std::getenv("XYO_JIT_MEGA_UNTIL_BCAST");
        return flag && flag[0] == '1' && flag[1] == '\0';
    }();
    static const bool outerUntilEnabled = [] {
        const char* flag = std::getenv("XYO_JIT_MEGA_UNTIL_OUTER");
        return flag && flag[0] == '1' && flag[1] == '\0';
    }();

    // Do not intercept ordinary scalar repeats just because the new
    // feature is enabled: they must keep their existing Source::Repeat
    // fast path and correctness checks.
    const Block* outerFirst = nullptr;
    const Block* outerSecond = nullptr;
    const Block* outerInner = nullptr;
    if (first->opcode == "control_repeat" &&
        untilEnabled && outerUntilEnabled) {
        const BlockInput* sub = first->inputs.find("SUBSTACK");
        outerFirst = sub ? blocks.getBlock(sub->block) : nullptr;
        outerSecond = outerFirst
            ? blocks.getBlock(outerFirst->next) : nullptr;
        outerInner = outerSecond
            ? blocks.getBlock(outerSecond->next) : nullptr;
    }
    if (first->opcode == "control_repeat_until" ||
        (outerInner &&
         outerInner->opcode == "control_repeat_until")) {
        const Block* until = first;
        if (first->opcode == "control_repeat") {
            // Exactly two setters (literal counter reset then wall clock
            // baseline) followed by ONE dynamic joined-broadcast loop.
            // Cap the outer loop at 64: if the inner until is already true
            // all 64 iterations can run without a scheduler yield. Larger
            // repeats fall back to the regular bounded-tick Fiber JIT.
            // No other arbitrary Scratch effects may be skipped or moved.
            double repetitions = 0;
            if (!literalNumber(blocks, first->inputs.find("TIMES"),
                               repetitions) ||
                repetitions < 0 || repetitions > 64.0 ||
                std::floor(repetitions) != repetitions) return false;
            const Block* firstSet = outerFirst;
            const Block* secondSet = outerSecond;
            until = outerInner;
            if (!firstSet || !secondSet || !until ||
                until->opcode != "control_repeat_until" ||
                !outerUntilSetter(blocks, firstSet,
                                  result.outerFirstSet) ||
                result.outerFirstSet.clock ||
                !outerUntilSetter(blocks, secondSet,
                                  result.outerSecondSet) ||
                !result.outerSecondSet.clock ||
                first->next.empty()) return false;
            const Block* final = blocks.getBlock(first->next);
            if (!scalarChange(blocks, final, result)) {
                static const bool finalSetEnabled = [] {
                    const char* flag = std::getenv(
                        "XYO_JIT_MEGA_UNTIL_OUTER_POST_SET");
                    return flag && flag[0] == '1' && flag[1] == '\0';
                }();
                if (final && !final->next.empty()) {
                    // The exact original ScratchBench end call is a
                    // certified, synchronous helper. Unsupported source
                    // effects must remain on the ordinary Fiber path.
                    static const bool nativeEndEnabled = [] {
                        const char* flag = std::getenv(
                            "XYO_JIT_MEGA_UNTIL_BENCH_END");
                        return flag && flag[0] == '1' && flag[1] == '\0';
                    }();
                    const char* audit = std::getenv(
                        "XYO_JIT_MEGA_BENCH_END_AUDIT");
                    const bool auditEnabled =
                        audit && audit[0] == '1' && audit[1] == '\0';
                    const bool certified =
                        (nativeEndEnabled || auditEnabled) &&
                        mega_bench_end::certify(
                            blocks, blocks.getBlock(final->next));
                    if (auditEnabled)
                        std::fprintf(stderr,
                            "[mega-bench-end-proof] certified=%d fallback=%d\n",
                            int(certified), int(!nativeEndEnabled || !certified));
                    if (!nativeEndEnabled || !certified)
                        return false;
                    result.hasBenchEnd = true;
                }
                if (!finalSetEnabled || !final ||
                    !innerAfterSetter(blocks, final,
                                      result.outerFinalSet) ||
                    !result.outerFinalSet.readVariable ||
                    final->inputs.size() != 1 ||
                    final->fields.size() != 1)
                    return false;
                result.hasOuterFinalSet = true;
            }
            // The outer suffix is distinct from the inner one. Use a
            // temporary proof object so neither source operation is lost.
            if (!result.hasOuterFinalSet)
                result.outerSuffix = {
                    result.variableId, result.variableName, result.delta};
            result.repetitions = uint64_t(repetitions);
            result.hasOuterUntil = true;
        } else if (!untilEnabled) {
            return false;
        }
        const BlockInput* sub = until->inputs.find("SUBSTACK");
        const BlockInput* condition = until->inputs.find("CONDITION");
        const Block* body = sub ? blocks.getBlock(sub->block) : nullptr;
        const Block* test = condition
            ? blocks.getBlock(condition->block) : nullptr;
        if (!body || !test ||
            !numericUntilCondition(blocks, test, result) ||
            !literalBroadcast(blocks, *body, result.broadcastName))
            return false;
        const Block* after = blocks.getBlock(until->next);
        if (result.hasOuterUntil && after &&
            after->opcode == "data_setvariableto") {
            static const bool postEnabled = [] {
                const char* flag = std::getenv(
                    "XYO_JIT_MEGA_UNTIL_POST_SET");
                return flag && flag[0] == '1' && flag[1] == '\0';
            }();
            if (!postEnabled) return false;
            const Block* second = blocks.getBlock(after->next);
            const Block* suffix = second
                ? blocks.getBlock(second->next) : nullptr;
            if (!second || !suffix ||
                !innerAfterSetter(blocks, after,
                                  result.innerAfterFirstSet) ||
                !innerAfterSetter(blocks, second,
                                  result.innerAfterSecondSet)) return false;
            if (suffix->opcode == "procedures_call") {
                static const bool inlineBench = [] {
                    const char* flag = std::getenv(
                        "XYO_JIT_MEGA_UNTIL_BENCH_ROUND");
                    return flag && flag[0] == '1' && flag[1] == '\0';
                }();
                if (!inlineBench ||
                    !proveBenchRound(blocks, suffix,
                                     result.benchRound)) return false;
                result.hasBenchRound = true;
            } else if (!scalarChange(blocks, suffix, result)) return false;
            result.hasInnerAfterSets = true;
        } else if (!scalarChange(blocks, after, result)) {
            return false;
        }
        if (result.hasOuterUntil) {
            const bool originalClock =
                result.untilClockElapsedMs &&
                result.untilStartVariableId ==
                    result.outerSecondSet.variableId;
            // Fixed-work experiment: repeat until NOT(reset_counter <
            // live_budget). The count is reset in the outer first setter
            // and read afresh after every joined broadcast.
            const bool fixedWork =
                result.untilVariablePair &&
                result.comparison == Source::Comparison::Less &&
                result.untilConditionNegated &&
                !result.conditionLiteralLeft &&
                result.conditionVariableId ==
                    result.outerFirstSet.variableId;
            if (!originalClock && !fixedWork) return false;
        }
        result.kind = Source::Kind::BroadcastUntil;
        return true;
    }

    if (first->opcode != "control_repeat") return false;
    double count = 0;
    if (!literalNumber(blocks, first->inputs.find("TIMES"), count) ||
        count < 0 || count > 2147483647.0 ||
        std::floor(count) != count) return false;
    const BlockInput* sub = first->inputs.find("SUBSTACK");
    if (!sub || sub->block.empty()) return false;
    const Block* body = blocks.getBlock(sub->block);
    if (!body) return false;

    if (first->next.empty()) {
        // Existing beta: one native scalar write per repeat iteration.
        if (body->opcode == "control_if_else" ||
            body->opcode == "control_if") {
            if (!result.returnSuffixes.empty() ||
                !numericIfConditional(blocks, body, result)) return false;
        } else if (!scalarChangeSequence(blocks, body, result)) return false;
        result.kind = Source::Kind::Repeat;
    } else {
        // New beta: each repeat body launches the same literal broadcast
        // once, waits for its actual started recipients, then advances its
        // Thread-owned remaining count. The sole suffix executes once when
        // all repeat iterations have completed.
        if (!literalBroadcast(blocks, *body, result.broadcastName) ||
            !scalarChange(blocks, blocks.getBlock(first->next), result))
            return false;
        result.kind = Source::Kind::BroadcastRepeat;
    }
    result.repetitions = static_cast<uint64_t>(count);
    return true;
}

} // namespace xyo::mega_yield
