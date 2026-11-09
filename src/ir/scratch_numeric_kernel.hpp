// Provably exact Scratch integer-only straight-line kernel -> executable CFG.
//
// This is deliberately NOT a generic Scratch compiler. It only accepts
// scripts whose variable values and arithmetic results are statically proven
// integral and within the range of exactly representable IEEE-754 integers.
// Every rejected opcode/operation falls back to existing runtime codegen.
// I64 slots are private to the kernel, never arbitrary Scratch Value storage.
#pragma once

#include "scratch_effect.hpp"
#include "executable.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xyo::ir {

struct NumericProcedureArguments {
    std::unordered_map<std::u16string, ValueId> numbers;
    std::unordered_map<std::u16string, ValueId> booleans;
    bool operator==(const NumericProcedureArguments&) const = default;
};

struct ScratchNumericKernel {
    // A Scratch call block can be inlined more than once through its callers.
    // Keep SSA argument bindings for every expansion, in CFG lowering order.
    std::unordered_map<std::string, std::vector<NumericProcedureArguments>> procedureArguments;
    Executable executable;
    std::vector<std::string> slotIds;
    std::string resultVariable;
    // True when this kernel contains a Scratch loop that needs a scheduler
    // checkpoint. Only resumable backends may run it in normal execution;
    // the unscheduled lowering option remains test-only.
    bool requiresLoopScheduler = false;
};

inline std::optional<ScratchNumericKernel>
lowerExactIntegerScript(const ScratchScript& source) {
    if (!source.valid()) return std::nullopt;
    const ScratchEffects effects = ScratchEffects::lower(source);
    if (!effects.valid) return std::nullopt;
    const auto* top = effects.getBlock(effects.topId);
    if (!top || top->opcode != "event_whenflagclicked" ||
        top->nextId.empty()) return std::nullopt;

    ScratchNumericKernel kernel;
    auto& program = kernel.executable;
    program.ssa.cfg.createBlock();
    program.instructions.resize(1);
    std::unordered_map<std::string, uint32_t> slotForId;
    std::unordered_map<std::string, int64_t> known;
    std::string lastVariable;
    bool hasAssignment = false;
    constexpr int64_t kExactMax = INT64_C(9007199254740991); // 2^53-1
    constexpr int64_t kExactMin = -kExactMax;

    auto emit = [&](ExecOp op, ValueId lhs = 0, ValueId rhs = 0,
                    int64_t literal = 0, uint32_t slot = 0,
                    Type type = Type::I64) {
        const ValueId value = ValueId(program.ssa.definitions.size());
        program.ssa.definitions.push_back({
            0, type, uint32_t(program.instructions[0].size() + 1)
        });
        program.instructions[0].push_back({
            value, op, lhs, rhs, literal, slot
        });
        return value;
    };
    auto store = [&](ValueId value, uint32_t slot) {
        program.instructions[0].push_back({
            kNoValue, ExecOp::StoreVar, value, 0, 0, slot
        });
    };
    auto slotOf = [&](const std::string& id) -> uint32_t {
        const auto it = slotForId.find(id);
        if (it != slotForId.end()) return it->second;
        const uint32_t slot = uint32_t(kernel.slotIds.size());
        slotForId.emplace(id, slot);
        kernel.slotIds.push_back(id);
        program.variableSlots = uint32_t(kernel.slotIds.size());
        return slot;
    };
    // A lowered expression carries its exact value as an interval of one
    // element. It permits real arithmetic instructions, but rejects any
    // operation that would need Scratch floating-point rounding/coercion.
    struct Number {
        ValueId id;
        int64_t exact;
    };
    auto expression = [&](auto&& self, const std::string& id,
                          unsigned depth) -> std::optional<Number> {
        if (id.empty() || depth >= 128) return std::nullopt;
        const ScratchNode* node = effects.getBlock(id);
        const Block* original = source.getBlock(id);
        if (!node || !original || node->opaque) return std::nullopt;
        if (node->opcode == "math_number" ||
            node->opcode == "math_integer" ||
            node->opcode == "math_whole_number" ||
            node->opcode == "math_positive_number") {
            auto f = original->fields.find("NUM");
            if (f == original->fields.end() || !f->second.value.isNumber())
                return std::nullopt;
            const double number = f->second.value.asNumberUnchecked();
            if (!std::isfinite(number) || std::trunc(number) != number ||
                number < double(kExactMin) ||
                number > double(kExactMax)) return std::nullopt;
            const auto exact = int64_t(number);
            return Number{emit(ExecOp::ConstI64, 0, 0, exact), exact};
        }
        if (node->kind == ScratchNodeKind::ReadVariable) {
            const auto knownIt = known.find(node->variableId);
            const auto slotIt = slotForId.find(node->variableId);
            if (node->variableId.empty() || knownIt == known.end() ||
                slotIt == slotForId.end()) return std::nullopt;
            return Number{emit(ExecOp::LoadVar, 0, 0, 0,
                               slotIt->second), knownIt->second};
        }
        if (node->opcode == "operator_add" ||
            node->opcode == "operator_subtract") {
            const auto* a = effects.inputOf(id, "NUM1");
            const auto* b = effects.inputOf(id, "NUM2");
            if (!a || !b) return std::nullopt;
            const auto left = self(self, a->selectedId(), depth + 1);
            if (!left) return std::nullopt;
            const auto right = self(self, b->selectedId(), depth + 1);
            if (!right) return std::nullopt;
            const __int128 result = node->opcode == "operator_add"
                ? __int128(left->exact) + right->exact
                : __int128(left->exact) - right->exact;
            if (result < kExactMin || result > kExactMax)
                return std::nullopt;
            const ExecOp op = node->opcode == "operator_add"
                ? ExecOp::AddI64 : ExecOp::SubI64;
            return Number{emit(op, left->id, right->id), int64_t(result)};
        }
        return std::nullopt;
    };

    std::unordered_set<std::string> visited;
    for (std::string current = top->nextId; !current.empty();) {
        if (!visited.insert(current).second) return std::nullopt;
        const auto* node = effects.getBlock(current);
        if (!node || node->opaque ||
            (node->kind != ScratchNodeKind::WriteVariable &&
             node->kind != ScratchNodeKind::ChangeVariable) ||
            node->variableId.empty()) return std::nullopt;

        const bool change = node->kind == ScratchNodeKind::ChangeVariable;
        if (change && !known.count(node->variableId))
            return std::nullopt;
        const auto* input = effects.inputOf(current, "VALUE");
        if (!input) return std::nullopt;
        const auto rhs = expression(expression, input->selectedId(), 0);
        if (!rhs) return std::nullopt;
        int64_t resulting = rhs->exact;
        const uint32_t slot = slotOf(node->variableId);
        ValueId value = rhs->id;
        if (change) {
            const __int128 sum =
                __int128(known.at(node->variableId)) + rhs->exact;
            if (sum < kExactMin || sum > kExactMax)
                return std::nullopt;
            resulting = int64_t(sum);
            value = emit(ExecOp::ChangeVar, rhs->id, 0, 0, slot);
        }
        store(value, slot);
        known.insert_or_assign(node->variableId, resulting);
        lastVariable = node->variableId;
        hasAssignment = true;
        current = node->nextId;
    }
    if (!hasAssignment) return std::nullopt;
    // Return the last assigned variable through an actual slot read, so the
    // same dataflow is exercised in both LLVM and WASM backends.
    const ValueId returned = emit(ExecOp::LoadVar, 0, 0, 0,
                                  slotForId.at(lastVariable));
    program.ssa.cfg.block(0).terminator = {
        TerminatorKind::Return, 0, 0, 0, returned
    };
    kernel.resultVariable = lastVariable;
    if (!verifyExecutable(program)) return std::nullopt;
    return kernel;
}

} // namespace xyo::ir
