// Guarded numeric subset of Scratch -> backend-neutral executable f64 CFG.
//
// Input/output scalar Values live in caller-owned 64-bit NaN-boxed snapshots.
// Only numeric inputs are permitted; a host guard and successful commit are
// mandatory. Unknown effects, yield points and procedures are rejected.
#pragma once

#include "scratch_numeric_kernel.hpp"
#include "scratch_pipeline.hpp"
#include "execution_abi.hpp"
#include "../core/cast.hpp"

#include <bit>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace xyo::ir {

inline bool isNumericStaticHat(const std::string& opcode) {
    return opcode == "event_whenflagclicked" ||
        opcode == "event_whenbroadcastreceived" ||
        opcode == "event_whenkeypressed" ||
        opcode == "event_whenthisspriteclicked" ||
        opcode == "event_whenstageclicked" ||
        opcode == "event_whenbackdropswitchesto" ||
        opcode == "control_start_as_clone";
}

struct NumericF64LoweringOptions {
    // Test/analysis-only path: emit a bounded CFG loop without a scheduler
    // checkpoint. Never enable for regular Scratch execution. A warp flag
    // alone is NOT sufficient; even warp loops periodically poll deadlines.
    bool allowUnscheduledLoops = false;
    // Enable a checkpointed compiler ABI. The caller must use a persistent
    // continuation frame and commit only at a complete yield boundary.
    bool enableResumableLoops = false;
};

inline std::optional<ScratchNumericKernel>
lowerNumericF64Script(
    const ScratchScript& source,
    NumericF64LoweringOptions options = {},
    const ScratchTransferGraph* transfers = nullptr) {
    if (!source.valid()) return std::nullopt;
    const ScratchEffects effects = ScratchEffects::lower(source);
    if (!effects.valid) return std::nullopt;
    const ScratchNode* top = effects.getBlock(effects.topId);
    if (!top || !isNumericStaticHat(top->opcode) ||
        top->nextId.empty()) return std::nullopt;

    ScratchNumericKernel kernel;
    auto& program = kernel.executable;
    program.externalValueSlots = true;
    program.resumable = options.enableResumableLoops;
    std::unordered_map<std::string, uint32_t> slots;
    BlockId current = program.ssa.cfg.createBlock();
    program.instructions.resize(1);
    auto newBlock = [&]() -> BlockId {
        const BlockId id = program.ssa.cfg.createBlock();
        program.instructions.emplace_back();
        return id;
    };
    auto emit = [&](ExecOp op, ValueId lhs = 0, ValueId rhs = 0,
                    int64_t literal = 0, uint32_t slot = 0,
                    Type type = Type::F64) -> ValueId {
        const ValueId id = ValueId(program.ssa.definitions.size());
        program.ssa.definitions.push_back({
            current, type, uint32_t(program.instructions[current].size() + 1)
        });
        program.instructions[current].push_back({
            id, op, lhs, rhs, literal, slot
        });
        return id;
    };
    auto store = [&](ValueId value, uint32_t slot) {
        program.instructions[current].push_back({
            kNoValue, ExecOp::StoreNumber, value, 0, 0, slot
        });
    };
    auto getSlot = [&](const std::string& id) -> uint32_t {
        const auto found = slots.find(id);
        if (found != slots.end()) return found->second;
        const uint32_t slot = uint32_t(kernel.slotIds.size());
        slots.emplace(id, slot);
        kernel.slotIds.push_back(id);
        program.variableSlots = uint32_t(kernel.slotIds.size());
        return slot;
    };

    // Each inline call has a lexical, immutable SSA argument frame.
    // Arguments are evaluated before the callee starts, not fetched
    // again from a mutable Scratch variable after the callee writes.
    std::vector<std::unordered_map<std::u16string, ValueId>> arguments;
    std::vector<std::unordered_map<std::u16string, ValueId>> booleanArguments;
    auto numberExpr = [&](auto&& self, const std::string& id,
                          unsigned depth) -> std::optional<ValueId> {
        if (id.empty() || depth >= 128) return std::nullopt;
        const ScratchNode* node = effects.getBlock(id);
        const Block* original = source.getBlock(id);
        if (!node || !original || node->opaque) return std::nullopt;
        const auto& op = node->opcode;
        if (op == "argument_reporter_string_number") {
            const auto field = original->fields.find("VALUE");
            if (arguments.empty() || field == original->fields.end() ||
                !field->second.value.isString() ||
                !field->second.value.asStrPtr())
                return std::nullopt;
            const std::u16string name(
                field->second.value.asStrPtr()->view());
            const auto found = arguments.back().find(name);
            return found == arguments.back().end()
                ? std::nullopt
                : std::optional<ValueId>{found->second};
        }
        if (op == "math_number" || op == "math_integer" ||
            op == "math_whole_number" || op == "math_positive_number" ||
            op == "math_angle") {
            const auto it = original->fields.find("NUM");
            if (it == original->fields.end() ||
                (!it->second.value.isNumber() &&
                 !it->second.value.isString())) return std::nullopt;
            // SB3 stores numeric shadow literals as text. Interpret them with
            // Scratch's own conversion, just like the regular JIT frontend.
            const double number = cast::toNumber(it->second.value);
            return emit(ExecOp::ConstF64, 0, 0,
                        std::bit_cast<int64_t>(number));
        }
        if (node->kind == ScratchNodeKind::ReadVariable) {
            if (node->variableId.empty()) return std::nullopt;
            return emit(ExecOp::LoadNumber, 0, 0, 0,
                        getSlot(node->variableId));
        }
        if (op == "operator_round") {
            const auto* input = effects.inputOf(id, "NUM");
            if (!input) return std::nullopt;
            const auto value = self(self, input->selectedId(), depth + 1);
            return value ? std::optional<ValueId>{emit(ExecOp::RoundF64, *value)}
                         : std::nullopt;
        }
        if (op == "operator_mathop") {
            const auto choice = original->fields.find("OPERATOR");
            if (choice == original->fields.end() ||
                !choice->second.value.isString() ||
                !choice->second.value.asStrPtr())
                return std::nullopt;
            const auto name = choice->second.value.asStrPtr()->view();
            ExecOp operation;
            if (name == u"abs") operation = ExecOp::AbsF64;
            else if (name == u"floor") operation = ExecOp::FloorF64;
            else if (name == u"ceiling") operation = ExecOp::CeilF64;
            else if (name == u"sqrt") operation = ExecOp::SqrtF64;
            else return std::nullopt;
            const auto* input = effects.inputOf(id, "NUM");
            if (!input) return std::nullopt;
            const auto value = self(self, input->selectedId(), depth + 1);
            return value ? std::optional<ValueId>{emit(operation, *value)}
                         : std::nullopt;
        }
        ExecOp operation;
        if (op == "operator_add") operation = ExecOp::AddF64;
        else if (op == "operator_subtract") operation = ExecOp::SubF64;
        else if (op == "operator_multiply") operation = ExecOp::MulF64;
        else if (op == "operator_divide") operation = ExecOp::DivF64;
        else return std::nullopt;
        const auto* a = effects.inputOf(id, "NUM1");
        const auto* b = effects.inputOf(id, "NUM2");
        if (!a || !b) return std::nullopt;
        const auto left = self(self, a->selectedId(), depth + 1);
        if (!left) return std::nullopt;
        const auto right = self(self, b->selectedId(), depth + 1);
        if (!right) return std::nullopt;
        return emit(operation, *left, *right);
    };
    // Only recursively lower pure numeric comparisons and Boolean logic.
    // Eager evaluation is safe here because reporters with effects are
    // rejected by numberExpr()/booleanExpr() before compilation.
    auto booleanExpr = [&](auto&& self, const std::string& id,
                           unsigned depth) -> std::optional<ValueId> {
        if (id.empty() || depth >= 64) return std::nullopt;
        const ScratchNode* node = effects.getBlock(id);
        if (!node || node->opaque) return std::nullopt;
        if (node->opcode == "argument_reporter_boolean" ||
            node->opcode == "argument_reporter_string_number") {
            const auto* original = source.getBlock(id);
            if (!original || arguments.empty()) return std::nullopt;
            const auto field = original->fields.find("VALUE");
            if (field == original->fields.end() ||
                !field->second.value.isString() ||
                !field->second.value.asStrPtr()) return std::nullopt;
            const std::u16string name(field->second.value.asStrPtr()->view());
            const auto boolean = booleanArguments.back().find(name);
            if (boolean != booleanArguments.back().end()) return boolean->second;
            const auto numeric = arguments.back().find(name);
            if (numeric == arguments.back().end()) return std::nullopt;
            const auto zero = emit(ExecOp::ConstF64, 0, 0,
                std::bit_cast<int64_t>(0.0));
            const auto equal = emit(ExecOp::EqualF64, numeric->second, zero,
                0, 0, Type::I1);
            return emit(ExecOp::NotI1, equal, 0, 0, 0, Type::I1);
        }
        if (node->opcode == "operator_not") {
            const auto* input = effects.inputOf(id, "OPERAND");
            if (!input) return std::nullopt;
            const auto value = self(self, input->selectedId(), depth + 1);
            return value ? std::optional<ValueId>{
                emit(ExecOp::NotI1, *value, 0, 0, 0, Type::I1)}
                         : std::nullopt;
        }
        const bool conjunction = node->opcode == "operator_and";
        const bool disjunction = node->opcode == "operator_or";
        const auto* a = effects.inputOf(id, "OPERAND1");
        const auto* b = effects.inputOf(id, "OPERAND2");
        if (!a || !b) return std::nullopt;
        if (conjunction || disjunction) {
            const auto left = self(self, a->selectedId(), depth + 1);
            if (!left) return std::nullopt;
            const auto right = self(self, b->selectedId(), depth + 1);
            if (!right) return std::nullopt;
            return emit(conjunction ? ExecOp::AndI1 : ExecOp::OrI1,
                        *left, *right, 0, 0, Type::I1);
        }
        ExecOp comparison;
        if (node->opcode == "operator_lt") comparison = ExecOp::LessF64;
        else if (node->opcode == "operator_gt")
            comparison = ExecOp::GreaterF64;
        else if (node->opcode == "operator_equals")
            comparison = ExecOp::EqualF64;
        else return std::nullopt;
        const auto left = numberExpr(numberExpr, a->selectedId(), 1);
        if (!left) return std::nullopt;
        const auto right = numberExpr(numberExpr, b->selectedId(), 1);
        if (!right) return std::nullopt;
        return emit(comparison, *left, *right, 0, 0, Type::I1);
    };

    std::unordered_set<std::string> visited;
    // Each inlined warp call must have a unique, resolvable call-site
    // continuation. Do not turn a nonwarp or recursive procedure into an
    // uncheckpointed CFG: the interpreter owns such scheduler semantics.
    std::unordered_set<std::string> activeProcedureDefinitions;
    unsigned activeWarpProcedures = 0;
    // Limit a statically bounded but nested repeat test: 1024^3 loops
    // would otherwise build a small CFG that runs for billions of ticks.
    constexpr uint64_t kMaxStaticWork = 100000;
    uint64_t staticWork = 0;
    uint64_t activeMultiplier = 1;
    std::string lastVariable;
    auto lowerChain = [&](auto&& self, const std::string& start,
                          unsigned depth) -> bool {
        if (depth >= 32) return false;
        for (std::string id = start; !id.empty();) {
            if (activeMultiplier > kMaxStaticWork - staticWork)
                return false;
            staticWork += activeMultiplier;
            if (!visited.insert(id).second) return false;
            const ScratchNode* node = effects.getBlock(id);
            if (!node || node->opaque) return false;
            if (node->opcode == "procedures_call") {
                if (!transfers) return false;
                const auto* call = source.getBlock(id);
                if (!call || !call->mutation ||
                    call->mutation->isReporterCall()) return false;
                const ScratchTransferSite* transfer = nullptr;
                for (const auto& site : transfers->sites)
                    if (site.kind == ScratchTransferKind::Call &&
                        site.sourceBlockId == id) {
                        transfer = &site;
                        break;
                    }
                if (!transfer || transfer->continuationBlockId != node->nextId)
                    return false;
                const Block* definition = source.getBlock(
                    transfer->procedureDefinitionId);
                if (!definition || definition->opcode !=
                    "procedures_definition" || definition->next.empty() ||
                    !activeProcedureDefinitions.insert(definition->id).second)
                    return false;
                const BlockInput* prototypeInput =
                    definition->inputs.find("custom_block");
                const Block* prototype = prototypeInput
                    ? source.getBlock(prototypeInput->block.empty()
                        ? prototypeInput->shadow : prototypeInput->block)
                    : nullptr;
                if (!prototype || !prototype->mutation ||
                    prototype->mutation->isReporterCall())
                    return false;
                prototype->mutation->parseArgs();
                call->mutation->parseArgs();
                const auto& ids = prototype->mutation->argumentIds;
                const auto& names = prototype->mutation->argumentNames;
                if (ids.size() > CallFrameAbi::kMaxArguments || ids.size() != names.size() ||
                    ids != call->mutation->argumentIds ||
                    call->inputs.items.size() != ids.size())
                    return false;
                // Read all arguments from the caller's existing SSA frame
                // before entering the callee, matching Scratch's eager,
                // left-to-right argument binding. Recursive calls and
                // argument expressions with effects are rejected.
                std::unordered_map<std::u16string, ValueId> frame, booleanFrame;
                std::unordered_set<std::u16string> argumentNames;
                for (size_t i = 0; i < ids.size(); ++i) {
                    if (!argumentNames.insert(names[i]).second) return false;
                    const auto* input = effects.inputOf(id, ids[i]);
                    if (!input) return false;
                    const auto value = numberExpr(
                        numberExpr, input->selectedId(), 0);
                    if (value) {
                        if (!frame.emplace(names[i], *value).second) return false;
                    } else {
                        const auto boolean = booleanExpr(
                            booleanExpr, input->selectedId(), 0);
                        if (!boolean ||
                            !booleanFrame.emplace(names[i], *boolean).second)
                            return false;
                    }
                }
                // Only straight-line numeric bodies and nested verified
                // synchronous calls are accepted. The active-procedure set rejects
                // direct/indirect recursion; control loops cannot evade
                // the shared scheduler deadline here.
                const bool yieldingBody = options.enableResumableLoops &&
                    !prototype->mutation->warp() &&
                    activeWarpProcedures == 0;
                std::unordered_set<std::string> bodySeen;
                auto synchronousBody = [&](auto&& check, std::string part,
                                           unsigned nesting) -> bool {
                    if (nesting >= 32) return false;
                    for (; !part.empty();) {
                        const Block* step = source.getBlock(part);
                        if (!step || !bodySeen.insert(part).second) return false;
                        const bool branch = step->opcode == "control_if" ||
                            step->opcode == "control_if_else";
                        const bool loop = yieldingBody &&
                            (step->opcode == "control_repeat" ||
                             step->opcode == "control_while" ||
                             step->opcode == "control_repeat_until" ||
                             step->opcode == "control_forever");
                        if (!branch && !loop && step->opcode != "data_setvariableto" &&
                            step->opcode != "data_changevariableby" &&
                            step->opcode != "procedures_call") return false;
                        if (branch || loop) {
                            for (const char* name : {"SUBSTACK", "SUBSTACK2"})
                                if (const auto* input = step->inputs.find(name))
                                    if (!check(check, input->block.empty()
                                            ? input->shadow : input->block,
                                            nesting + 1)) return false;
                        }
                        part = step->next;
                    }
                    return true;
                };
                if (!synchronousBody(synchronousBody, definition->next, 0))
                    return false;
                if (yieldingBody)
                    kernel.procedureArguments[id].push_back({frame, booleanFrame});
                arguments.push_back(std::move(frame));
                booleanArguments.push_back(std::move(booleanFrame));
                auto callerVisited = std::move(visited);
                visited.clear();
                const bool warpCall = prototype->mutation->warp();
                if (warpCall) ++activeWarpProcedures;
                const bool valid = self(self, definition->next, depth + 1);
                if (warpCall) --activeWarpProcedures;
                visited = std::move(callerVisited);
                arguments.pop_back();
                booleanArguments.pop_back();
                if (!valid) return false;
                activeProcedureDefinitions.erase(definition->id);
            } else if (node->opcode == "control_if" ||
                node->opcode == "control_if_else") {
                const auto* condition = effects.inputOf(id, "CONDITION");
                if (!condition) return false;
                const auto flag = booleanExpr(booleanExpr, condition->selectedId(), 0);
                if (!flag) return false;

                const BlockId yes = newBlock();
                const BlockId no = newBlock();
                const BlockId join = newBlock();
                program.ssa.cfg.block(current).terminator = {
                    TerminatorKind::Branch, yes, no, *flag
                };
                current = yes;
                if (const auto* input = effects.inputOf(id, "SUBSTACK")) {
                    if (!self(self, input->selectedId(), depth + 1))
                        return false;
                }
                program.ssa.cfg.block(current).terminator = {
                    TerminatorKind::Jump, join
                };
                current = no;
                if (node->opcode == "control_if_else") {
                    if (const auto* input = effects.inputOf(id, "SUBSTACK2")) {
                        if (!self(self, input->selectedId(), depth + 1))
                            return false;
                    }
                }
                program.ssa.cfg.block(current).terminator = {
                    TerminatorKind::Jump, join
                };
                current = join;
            } else if (node->opcode == "control_while" ||
                       node->opcode == "control_repeat_until" ||
                       node->opcode == "control_forever") {
                if (!options.enableResumableLoops) return false;
                const auto* input = effects.inputOf(id, "CONDITION");
                const bool forever = node->opcode == "control_forever";
                if (!input && !forever) return false;
                kernel.requiresLoopScheduler = true;
                const BlockId header = newBlock();
                const BlockId body = newBlock();
                const BlockId exit = newBlock();
                program.ssa.cfg.block(current).terminator = {
                    TerminatorKind::Jump, header};
                current = header;
                std::optional<ValueId> flag;
                if (forever) {
                    const auto zero = emit(ExecOp::ConstF64, 0, 0,
                        std::bit_cast<int64_t>(0.0));
                    const auto one = emit(ExecOp::ConstF64, 0, 0,
                        std::bit_cast<int64_t>(1.0));
                    flag = emit(ExecOp::LessF64, zero, one, 0, 0, Type::I1);
                } else {
                    flag = booleanExpr(booleanExpr, input->selectedId(), 0);
                }
                if (!flag) return false;
                const bool until = node->opcode == "control_repeat_until";
                program.ssa.cfg.block(current).terminator = {
                    TerminatorKind::Branch, until ? exit : body,
                    until ? body : exit, *flag};
                current = body;
                if (const auto* substack = effects.inputOf(id, "SUBSTACK"))
                    if (!self(self, substack->selectedId(), depth + 1))
                        return false;
                const BlockId latch = current;
                program.ssa.cfg.block(latch).terminator = {
                    TerminatorKind::Jump, header};
                program.yieldBoundaries.push_back({latch, header, id});
                current = exit;
            } else if (node->opcode == "control_repeat") {
                // Repeat in the interpreter evaluates the initial count,
                // rounds it and samples a yield after each body. Either the
                // explicit test-only unscheduled mode or a resumable
                // continuation ABI is required to preserve those yields.
                if (!options.allowUnscheduledLoops &&
                    !options.enableResumableLoops) return false;
                const auto* times = effects.inputOf(id, "TIMES");
                if (!times) return false;
                const Block* literal = source.getBlock(times->selectedId());
                const bool isLiteral = literal &&
                    (literal->opcode == "math_number" ||
                     literal->opcode == "math_integer" ||
                     literal->opcode == "math_whole_number" ||
                     literal->opcode == "math_positive_number");
                // Preserve the compile-time constant fast path. A dynamic
                // reporter is evaluated ONCE, at the start of its loop.
                const uint64_t kMaxRepeat = options.enableResumableLoops
                    ? kMaxScheduledNumericRepeat : 1024;
                uint64_t constantCount = 0;
                ValueId countExpr = kNoValue;
                if (isLiteral) {
                    const auto it = literal->fields.find("NUM");
                    if (it == literal->fields.end() ||
                        (!it->second.value.isNumber() &&
                         !it->second.value.isString())) return false;
                    const double repetitions =
                        cast::toNumber(it->second.value);
                    if (!std::isfinite(repetitions)) return false;
                    // The Scratch VM uses JS Math.round: ties go toward
                    // +Infinity, and any nonpositive result skips the body.
                    // Unlike the old integer-only restriction, fractional
                    // literals now follow the same rule as dynamic counts.
                    const double rounded =
                        repetitions > 0 && repetitions < 0.5 ? 0
                        : std::floor(repetitions + 0.5);
                    if (rounded > double(kMaxRepeat)) return false;
                    constantCount = rounded > 0 ? uint64_t(rounded) : 0;
                } else {
                    const auto number =
                        numberExpr(numberExpr, times->selectedId(), 0);
                    if (!number) return false;
                    countExpr = *number;
                }
                kernel.requiresLoopScheduler = true;
                const BlockId preheader = current;
                const ValueId limit = countExpr == kNoValue
                    ? emit(ExecOp::ConstI64, 0, 0,
                           int64_t(constantCount), 0, Type::I64)
                    : emit(ExecOp::RepeatCount, countExpr, 0, int64_t(kMaxRepeat), 0,
                           Type::I64);
                const ValueId zero = emit(
                    ExecOp::ConstI64, 0, 0, 0, 0, Type::I64);
                const ValueId one = emit(
                    ExecOp::ConstI64, 0, 0, 1, 0, Type::I64);
                const BlockId header = newBlock();
                const BlockId body = newBlock();
                const BlockId exit = newBlock();
                program.ssa.cfg.block(preheader).terminator = {
                    TerminatorKind::Jump, header
                };
                current = header;
                auto& cfgHeader = program.ssa.cfg.block(header);
                const uint32_t phiIndex = uint32_t(cfgHeader.phis.size());
                cfgHeader.phis.push_back({Type::I64, {}});
                const ValueId counter = ValueId(
                    program.ssa.definitions.size());
                program.ssa.definitions.push_back(
                    {header, Type::I64, 0, true, phiIndex});
                const ValueId condition = emit(
                    ExecOp::LessI64, counter, limit, 0, 0, Type::I1);
                program.ssa.cfg.block(header).terminator = {
                    TerminatorKind::Branch, body, exit, condition
                };
                current = body;
                const uint64_t oldMultiplier = activeMultiplier;
                // Only uncheckpointed execution needs a total-work cap.
                // Resumable loops return to the scheduler each iteration.
                const uint64_t count = options.enableResumableLoops ? 1 :
                    (countExpr == kNoValue ? constantCount : kMaxRepeat);
                if (count != 0 &&
                    activeMultiplier > kMaxStaticWork / count)
                    return false;
                activeMultiplier *= count;
                if (const auto* substack = effects.inputOf(id, "SUBSTACK")) {
                    if (!self(self, substack->selectedId(), depth + 1))
                        return false;
                }
                activeMultiplier = oldMultiplier;
                const ValueId advanced = emit(
                    ExecOp::AddI64, counter, one, 0, 0, Type::I64);
                const BlockId latch = current;
                program.ssa.cfg.block(latch).terminator = {
                    TerminatorKind::Jump, header
                };
                // The body may contain if/else or nested loops; the back
                // edge is the actual final CFG block, not necessarily body.
                program.ssa.cfg.block(header).phis[phiIndex].inputs = {
                    {preheader, zero}, {latch, advanced}
                };
                program.yieldBoundaries.push_back({
                    latch, header, id, counter, constantCount,
                    countExpr == kNoValue ? kNoValue : limit
                });
                current = exit;
            } else {
                if (node->variableId.empty() ||
                    (node->kind != ScratchNodeKind::WriteVariable &&
                     node->kind != ScratchNodeKind::ChangeVariable))
                    return false;
                const auto* input = effects.inputOf(id, "VALUE");
                if (!input) return false;
                const auto value = numberExpr(numberExpr,
                                              input->selectedId(), 0);
                if (!value) return false;
                const uint32_t slot = getSlot(node->variableId);
                if (node->kind == ScratchNodeKind::ChangeVariable) {
                    // Scratch change-by casts both operands to number.
                    const auto updated = emit(
                        ExecOp::ChangeNumber, *value, 0, 0, slot);
                    store(updated, slot);
                } else {
                    store(*value, slot);
                }
                lastVariable = node->variableId;
            }
            id = node->nextId;
        }
        return true;
    };
    if (!lowerChain(lowerChain, top->nextId, 0) ||
        lastVariable.empty()) return std::nullopt;
    const ValueId result = emit(ExecOp::LoadVar, 0, 0, 0,
                                slots.at(lastVariable), Type::I64);
    program.ssa.cfg.block(current).terminator = {
        TerminatorKind::Return, 0, 0, 0, result
    };
    kernel.resultVariable = lastVariable;
    if (!verifyExecutable(program)) return std::nullopt;
    return kernel;
}

// The JitEngine backend-neutral frontend passes an immutable source
// snapshot and the shared transfer graph. The same inlined numeric warp
// procedure body becomes one Executable for native LLVM and Binaryen.
inline std::optional<ScratchNumericKernel>
lowerNumericF64Script(
    const ScratchCompilationUnit& unit,
    NumericF64LoweringOptions options = {}) {
    if (!unit.valid()) return std::nullopt;
    return lowerNumericF64Script(unit.script, options, &unit.transfers);
}

} // namespace xyo::ir
