// Scratch single-iteration numeric region construction. No source rewriting,
// backend dependencies, runtime loads/stores, or scheduler effects.
#pragma once

#include "../core/cast.hpp"
#include "../ir/numeric_region.hpp"
#include "../ir/numeric_region_float_facts.hpp"
#include "../ir/numeric_region_cost.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace xyo::analysis {

struct NumericRegion {
    ir::region::Graph graph;
    // Slot order is shared by fields, inputs, outputs, and written. Fields
    // belong to Blocks and remain valid only while its source is unchanged.
    std::vector<const BlockField*> fields;
    // Separate binding order for ListRead::slot; never scalar SSA state.
    // These are read-only lists, guarded/resampled by the backend per chunk.
    std::vector<const BlockField*> lists;
};

struct NumericRegionOptions {
    bool allowListReads = false;
};

namespace numeric_region_detail {

// The F64 payload of a String literal is sufficient for arithmetic, but NOT
// for copying that literal into a variable. In particular, "1" is not Number 1.
struct ValueRef {
    ir::ValueId id = 0;
    bool exactNumber = false;
    bool comparableNumeric = false;
};

class Lowerer {
    using Op = ir::region::Op;
    using State = std::map<uint32_t, ValueRef>;
    using Frame = std::map<std::u16string, ValueRef>;
    static constexpr size_t kMaxNodes = 2048;
    static constexpr size_t kMaxBlocks = 128;
    static constexpr size_t kMaxVariables = 32;
    static constexpr unsigned kMaxDepth = 64;
    static constexpr size_t kMaxCallDepth = 4;

    Blocks& blocks_;
    Target& target_;
    NumericRegionOptions options_;
    NumericRegion region_;
    State state_;
    std::vector<Variable*> variables_;
    std::map<std::string, uint32_t> slotsById_;
    std::vector<Variable*> listVariables_;
    std::map<std::string, uint32_t> listSlotsById_;
    std::vector<Frame> frames_;
    std::set<std::string> activeCalls_;
    std::set<const Block*> activeNodes_;
    ir::BlockId current_ = 0;
    size_t nodes_ = 0;
    bool failed_ = false;

    ir::region::Graph& graph() { return region_.graph; }

    bool visit(const Block* block) {
        if (!block || ++nodes_ > kMaxNodes) return false;
        return true;
    }

    ir::BlockId newBlock() {
        if (graph().instructions.size() >= kMaxBlocks) {
            failed_ = true;
            return 0;
        }
        const auto id = graph().ssa.cfg.createBlock();
        graph().instructions.emplace_back();
        return id;
    }

    ir::ValueId instruction(ir::BlockId block, Op op, ir::Type type,
                            ir::ValueId lhs = 0, ir::ValueId rhs = 0,
                            double literal = 0, uint32_t slot = 0) {
        if (graph().ssa.definitions.size() >= kMaxNodes) {
            failed_ = true;
            return 0;
        }
        const auto id = static_cast<ir::ValueId>(graph().ssa.definitions.size());
        auto& instructions = graph().instructions[block];
        const auto position = static_cast<uint32_t>(instructions.size() + 1);
        graph().ssa.definitions.push_back({block, type, position, false, 0});
        instructions.push_back({id, op, lhs, rhs, literal, slot});
        const bool unary = op == Op::Not || op == Op::ListRead;
        const bool binary = op != Op::Parameter && op != Op::Constant && !unary;
        if (unary || binary) {
            const auto operandType = (op == Op::And || op == Op::Or || op == Op::Not)
                ? ir::Type::I1 : ir::Type::F64;
            graph().ssa.uses.push_back({block, lhs, operandType, position});
            if (binary)
                graph().ssa.uses.push_back({block, rhs, operandType, position});
        }
        return id;
    }

    void jump(ir::BlockId from, ir::BlockId to) {
        auto& t = graph().ssa.cfg.block(from).terminator;
        t.kind = ir::TerminatorKind::Jump;
        t.trueBlock = to;
    }

    static const BlockField* field(const Block* block, const char* name) {
        const auto it = block->fields.find(name);
        return it == block->fields.end() ? nullptr : &it->second;
    }

    const Block* input(const Block* block, const std::string& name) {
        const auto* in = block->inputs.find(name);
        if (!in) return nullptr;
        // A dangling real input is malformed, not permission to evaluate its
        // shadow instead. Only an absent real connection selects the shadow.
        return blocks_.getBlock(in->block.empty() ? in->shadow : in->block);
    }

    std::optional<uint32_t> slot(const Block* block) {
        const auto* f = field(block, "VARIABLE");
        if (!f || f->id.empty()) return std::nullopt;
        auto* variable = target_.lookupVariableById(f->id);
        if (!variable || variable->id != f->id ||
            variable->type != Variable::kScalarType || variable->isCloud)
            return std::nullopt;
        const auto existing = slotsById_.find(f->id);
        if (existing != slotsById_.end()) {
            if (variables_[existing->second] != variable) return std::nullopt;
            return existing->second;
        }
        // Different field IDs must not alias one mutable variable.
        for (auto* prior : variables_)
            if (prior == variable) return std::nullopt;
        if (variables_.size() >= kMaxVariables) return std::nullopt;
        const auto index = static_cast<uint32_t>(variables_.size());
        variables_.push_back(variable);
        slotsById_.emplace(f->id, index);
        region_.fields.push_back(f);
        // Entry is parameter-only. Late discovery cannot put a parameter
        // after its same-block use or invalidate recorded instruction positions.
        const auto id = instruction(graph().entry, Op::Parameter, ir::Type::F64,
                                    0, 0, 0, index);
        graph().inputs.push_back(id);
        graph().written.push_back(false);
        return index;
    }

    std::optional<uint32_t> listSlot(const Block* block) {
        const auto* f = field(block, "LIST");
        if (!f || f->id.empty() || !f->value.isString()) return std::nullopt;
        auto* variable = target_.lookupVariableById(f->id);
        if (!variable || variable->id != f->id ||
            variable->type != Variable::kListType || variable->isCloud)
            return std::nullopt;
        const auto existing = listSlotsById_.find(f->id);
        if (existing != listSlotsById_.end()) {
            const auto index = existing->second;
            if (listVariables_[index] != variable ||
                cast::toString(region_.lists[index]->value)->view() !=
                    cast::toString(f->value)->view()) return std::nullopt;
            return index;
        }
        // Keep alias/name resolution unambiguous even if a later invocation
        // must resolve a binding by name rather than the original ID.
        for (size_t i = 0; i < listVariables_.size(); ++i)
            if (listVariables_[i] == variable ||
                cast::toString(region_.lists[i]->value)->view() ==
                    cast::toString(f->value)->view()) return std::nullopt;
        if (listVariables_.size() >= kMaxVariables) return std::nullopt;
        const auto index = static_cast<uint32_t>(listVariables_.size());
        listVariables_.push_back(variable);
        listSlotsById_.emplace(f->id, index);
        region_.lists.push_back(f);
        graph().listSlots = static_cast<uint32_t>(listVariables_.size());
        return index;
    }

    ValueRef initial(uint32_t slot) { return {graph().inputs[slot], true, true}; }

    ValueRef value(const State& state, uint32_t slot) {
        const auto it = state.find(slot);
        return it == state.end() ? initial(slot) : it->second;
    }

    std::optional<ValueRef> literal(Value v) {
        if (!v.isNumber() && !v.isString()) return std::nullopt;
        // Number NaN is copied unchanged by set, unlike arithmetic coercion.
        // Decline nonfinite Number fields conservatively (String infinities
        // can still participate in arithmetic but never in comparisons).
        if (v.isNumber() && !std::isfinite(v.asNumberUnchecked())) return std::nullopt;
        const double arithmetic = cast::toNumber(v);
        // String comparison must not reuse arithmetic's invalid/whitespace -> 0
        // coercion. Only finite, genuinely numeric strings are representable.
        const bool comparable = v.isNumber() ||
            (!cast::isWhiteSpace(v) && std::isfinite(cast::toNumberOrNaN(v)));
        const auto id = instruction(current_, Op::Constant, ir::Type::F64,
                                    0, 0, arithmetic);
        return ValueRef{id, v.isNumber(), comparable};
    }

    std::optional<ValueRef> expression(const Block* block, unsigned depth) {
        if (depth > kMaxDepth || !visit(block) ||
            !activeNodes_.insert(block).second) return std::nullopt;
        struct Leave {
            std::set<const Block*>& nodes;
            const Block* block;
            ~Leave() { nodes.erase(block); }
        } leave{activeNodes_, block};
        const auto& op = block->opcode;
        if (op == "math_number" || op == "math_integer" ||
            op == "math_whole_number" || op == "math_positive_number" ||
            op == "math_angle" || op == "text") {
            const auto* f = field(block, op == "text" ? "TEXT" : "NUM");
            return f ? literal(f->value) : std::nullopt;
        }
        if (op == "data_variable") {
            const auto index = slot(block);
            if (!index) return std::nullopt;
            return value(state_, *index);
        }
        if (op == "data_itemoflist") {
            if (!options_.allowListReads) return std::nullopt;
            const auto binding = listSlot(block);
            if (!binding) return std::nullopt;
            const auto index = expression(input(block, "INDEX"), depth + 1);
            // Do not implement Scratch's last/random/any/empty String index
            // semantics as numeric zero. Backend bounds guards still reject
            // zero, nonfinite, or out-of-range arithmetic-derived indexes.
            if (!index || !index->comparableNumeric) return std::nullopt;
            const auto id = instruction(current_, Op::ListRead, ir::Type::F64,
                                        index->id, 0, 0, *binding);
            // A ListRead is a guarded semantic Number, NOT a static fact about
            // arbitrary list contents. The backend must validate the raw item
            // tag and ORD before exact copy, Phi, comparison or later use. Any
            // String-coercion relaxation requires a separate arithmetic/index-
            // only use proof and must leave the original list item untouched.
            return ValueRef{id, true, true};
        }
        if (op == "argument_reporter_string_number") {
            const auto* f = field(block, "VALUE");
            if (!f || !f->value.isString() || frames_.empty()) return std::nullopt;
            const auto name = cast::toString(f->value)->view();
            const auto it = frames_.back().find(std::u16string(name));
            if (it == frames_.back().end()) return std::nullopt;
            return it->second;
        }
        Op arithmetic;
        if (op == "operator_add") arithmetic = Op::Add;
        else if (op == "operator_subtract") arithmetic = Op::Subtract;
        else if (op == "operator_multiply") arithmetic = Op::Multiply;
        else if (op == "operator_divide") arithmetic = Op::Divide;
        else if (op == "operator_mod") arithmetic = Op::Modulo;
        else return std::nullopt;
        const auto lhs = expression(input(block, "NUM1"), depth + 1);
        const auto rhs = expression(input(block, "NUM2"), depth + 1);
        if (!lhs || !rhs) return std::nullopt;
        const auto id = instruction(current_, arithmetic, ir::Type::F64,
                                    lhs->id, rhs->id);
        // Runtime guards reject Number NaN at entry AND immediately after
        // every arithmetic instruction, transactionally before external commit.
        // Therefore subsequent fast-path comparison operands cannot be NaN.
        return ValueRef{id, true, true};
    }

    std::optional<ir::ValueId> condition(const Block* block, unsigned depth) {
        if (depth > kMaxDepth || !visit(block) ||
            !activeNodes_.insert(block).second) return std::nullopt;
        struct Leave {
            std::set<const Block*>& nodes;
            const Block* block;
            ~Leave() { nodes.erase(block); }
        } leave{activeNodes_, block};
        const auto& op = block->opcode;
        if (op == "operator_not") {
            const auto operand = condition(input(block, "OPERAND"), depth + 1);
            if (!operand) return std::nullopt;
            return instruction(current_, Op::Not, ir::Type::I1, *operand);
        }
        if (op == "operator_and" || op == "operator_or") {
            const auto lhs = condition(input(block, "OPERAND1"), depth + 1);
            const auto rhs = condition(input(block, "OPERAND2"), depth + 1);
            if (!lhs || !rhs) return std::nullopt;
            return instruction(current_, op == "operator_and" ? Op::And : Op::Or,
                               ir::Type::I1, *lhs, *rhs);
        }
        Op compare;
        if (op == "operator_lt") compare = Op::Less;
        else if (op == "operator_gt") compare = Op::Greater;
        else if (op == "operator_equals") compare = Op::Equal;
        else return std::nullopt;
        const auto lhs = expression(input(block, "OPERAND1"), depth + 1);
        const auto rhs = expression(input(block, "OPERAND2"), depth + 1);
        if (!lhs || !rhs || !lhs->comparableNumeric || !rhs->comparableNumeric)
            return std::nullopt;
        return instruction(current_, compare, ir::Type::I1, lhs->id, rhs->id);
    }

    bool branch(const Block* block, unsigned depth) {
        const auto cond = condition(input(block, "CONDITION"), depth + 1);
        if (!cond) return false;
        const auto before = state_;
        const auto from = current_;
        const auto yes = newBlock();
        const auto no = newBlock();
        if (failed_) return false;
        auto& term = graph().ssa.cfg.block(from).terminator;
        term.kind = ir::TerminatorKind::Branch;
        term.condition = *cond;
        term.trueBlock = yes;
        term.falseBlock = no;
        current_ = yes;
        if (!chain(blocks_.getBranch(block->id, 1), depth + 1)) return false;
        const auto yesEnd = current_;
        const auto yesState = state_;
        current_ = no;
        state_ = before;
        if (block->opcode == "control_if_else" &&
            !chain(blocks_.getBranch(block->id, 2), depth + 1)) return false;
        const auto noEnd = current_;
        const auto noState = state_;
        const auto merge = newBlock();
        if (failed_) return false;
        jump(yesEnd, merge);
        jump(noEnd, merge);
        current_ = merge;
        state_.clear();
        // Variables discovered in either arm are initial Parameters in the
        // other arm. Iterate the slot union, not just the pre-branch state.
        for (uint32_t index = 0; index < variables_.size(); ++index) {
            auto a = value(yesState, index);
            auto b = value(noState, index);
            ValueRef joined{a.id, a.exactNumber && b.exactNumber,
                            a.comparableNumeric && b.comparableNumeric};
            if (a.id != b.id) {
                if (graph().ssa.definitions.size() >= kMaxNodes) return false;
                auto& phis = graph().ssa.cfg.block(merge).phis;
                const auto phiIndex = static_cast<uint32_t>(phis.size());
                joined.id = static_cast<ir::ValueId>(graph().ssa.definitions.size());
                phis.push_back({ir::Type::F64, {{yesEnd, a.id}, {noEnd, b.id}}});
                graph().ssa.definitions.push_back(
                    {merge, ir::Type::F64, 0, true, phiIndex});
            }
            state_.emplace(index, joined);
        }
        return true;
    }

    bool call(const Block* block, unsigned depth) {
        if (!block->mutation || block->mutation->isReporterCall() ||
            frames_.size() >= kMaxCallDepth) return false;
        const std::string code = block->mutation->attr("proccode");
        if (code.empty() || code.find("%b") != std::string::npos ||
            activeCalls_.count(code)) return false;
        std::vector<std::u16string> names;
        std::vector<std::string> ids;
        std::vector<Value> defaults;
        if (!blocks_.getProcedureParamNamesIdsAndDefaults(code, names, ids, defaults) ||
            names.size() != ids.size() || ids.size() > 8) return false;
        const auto* signature = blocks_.getProcedurePrototype(code);
        const auto definitionId = blocks_.getProcedureDefinition(code);
        const auto* definition = blocks_.getBlock(definitionId);
        if (!signature || !signature->warp() || !definition ||
            definition->opcode != "procedures_definition") return false;
        // Match emitProcedureCall's selected definition and custom_block real
        // connection; do not invent a different definition by scanning source.
        const auto* cb = definition->inputs.find("custom_block");
        const auto* proto = cb ? blocks_.getBlock(cb->block) : nullptr;
        if (!proto || proto->opcode != "procedures_prototype" || !proto->mutation ||
            !proto->mutation->warp() || proto->mutation->attr("proccode") != code)
            return false;
        proto->mutation->parseArgs();
        // A separately selected, incompatible prototype is ambiguous.
        if (proto->mutation->argumentNames != names ||
            proto->mutation->argumentIds != ids ||
            proto->mutation->argumentDefaults != defaults) return false;
        Frame frame;
        std::set<std::string> uniqueIds;
        for (size_t i = 0; i < names.size(); ++i) {
            if (ids[i].empty() || !uniqueIds.insert(ids[i]).second || frame.count(names[i]))
                return false;
            const auto* in = block->inputs.find(ids[i]);
            std::optional<ValueRef> arg;
            if (in && (!in->block.empty() || !in->shadow.empty()))
                arg = expression(input(block, ids[i]), depth + 1);
            else
                arg = literal(i < defaults.size() ? defaults[i] : Value::num(0));
            if (!arg) return false;
            frame.emplace(names[i], *arg);
        }
        // Arguments are immutable evaluated caller values. Push only after ALL
        // arguments are evaluated, before any callee assignment changes state.
        frames_.push_back(std::move(frame));
        activeCalls_.insert(code);
        const bool ok = chain(definition->next, depth + 1);
        activeCalls_.erase(code);
        frames_.pop_back();
        return ok;
    }

    bool chain(std::string id, unsigned depth) {
        if (depth > kMaxDepth) return false;
        std::set<std::string> seen;
        while (!id.empty()) {
            const auto* block = blocks_.getBlock(id);
            if (!visit(block) || !seen.insert(id).second || activeNodes_.count(block))
                return false;
            activeNodes_.insert(block);
            bool ok = false;
            if (block->opcode == "data_setvariableto" ||
                block->opcode == "data_changevariableby") {
                const auto index = slot(block);
                const auto rhs = expression(input(block, "VALUE"), depth + 1);
                if (index && rhs) {
                    ValueRef assigned = *rhs;
                    if (block->opcode == "data_changevariableby") {
                        const auto lhs = value(state_, *index);
                        assigned = {instruction(current_, Op::Add, ir::Type::F64,
                                                lhs.id, rhs->id), true, true};
                    }
                    // A String must not become a Number merely because it
                    // passed through an argument frame or another SSA alias.
                    if (assigned.exactNumber) {
                        state_[*index] = assigned;
                        graph().written[*index] = true;
                        ok = true;
                    }
                }
            } else if (block->opcode == "control_if" || block->opcode == "control_if_else") {
                ok = branch(block, depth);
            } else if (block->opcode == "procedures_call") {
                ok = call(block, depth);
            }
            activeNodes_.erase(block);
            if (!ok || failed_) return false;
            id = block->next;
        }
        return true;
    }

public:
    Lowerer(Blocks& blocks, Target& target, NumericRegionOptions options)
        : blocks_(blocks), target_(target), options_(options) {}

    std::optional<NumericRegion> lower(const Block* loop) {
        if (!loop || (loop->opcode != "control_repeat" &&
                      loop->opcode != "control_forever" &&
                      loop->opcode != "control_repeat_until" &&
                      loop->opcode != "control_while")) return std::nullopt;
        graph().entry = newBlock();
        current_ = newBlock();
        jump(graph().entry, current_);
        activeNodes_.insert(loop);
        if (!chain(blocks_.getBranch(loop->id, 1), 0) || failed_) return std::nullopt;
        activeNodes_.erase(loop);
        graph().exit = current_;
        for (uint32_t index = 0; index < variables_.size(); ++index)
            graph().outputs.push_back(value(state_, index).id);
        // Return is structural only: consumers commit outputs atomically after
        // dynamic guards. Empty/no-write regions still have a valid F64 ID.
        const auto result = graph().outputs.empty()
            ? instruction(current_, Op::Constant, ir::Type::F64, 0, 0, 0)
            : graph().outputs.front();
        if (failed_) return std::nullopt;
        auto& ret = graph().ssa.cfg.block(current_).terminator;
        ret.kind = ir::TerminatorKind::Return;
        ret.result = result;
        for (ir::BlockId b = 0; b < graph().instructions.size(); ++b) {
            const auto& t = graph().ssa.cfg.block(b).terminator;
            const auto position = static_cast<uint32_t>(graph().instructions[b].size() + 1);
            if (t.kind == ir::TerminatorKind::Branch)
                graph().ssa.uses.push_back({b, t.condition, ir::Type::I1, position});
            else if (t.kind == ir::TerminatorKind::Return)
                graph().ssa.uses.push_back({b, t.result, ir::Type::F64, position});
        }
        return std::move(region_);
    }
};

} // namespace numeric_region_detail

// This API lowers only SUBSTACK, not loop counts, exit conditions, or yielding.
// The initial LLVM hook is counted-repeat only. The caller owns graph.verify
// and runtime entry/result-NaN guards. Opt-in list reads additionally require
// transactional index/item guards and chunk-safe read-only list bindings.
// Failure never modifies source variables or lists.
inline std::optional<NumericRegion> lowerNumericRegion(
    Blocks& blocks, Target& target, const Block* loop,
    NumericRegionOptions options = {}) {
    return numeric_region_detail::Lowerer(blocks, target, options).lower(loop);
}

} // namespace xyo::analysis
