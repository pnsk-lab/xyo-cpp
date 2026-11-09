// Backend-neutral, typed SSA graph for side-effect-free numeric operations.
// The callers perform Scratch number coercion BEFORE entering this graph.
// Deliberately no LLVM or WebAssembly dependencies.
#pragma once
#include <cstdint>
#include <array>
#include <cassert>
#include <vector>

namespace xyo::ir {

using ValueId = uint32_t;
enum class Type : uint8_t { F64, I1, I32, I64 };
enum class Op : uint8_t { Parameter, Add, Sub, Mul, Div, LessThan, GreaterThan };

struct Instruction {
    Op op;
    Type type = Type::F64;
    ValueId lhs = 0;
    ValueId rhs = 0;
};

class Function {
public:
    ValueId parameter() {
        return add({Op::Parameter});
    }
    ValueId binary(Op op, ValueId lhs, ValueId rhs) {
        assert(op != Op::Parameter && lhs < instructions_.size() &&
               rhs < instructions_.size() &&
               instructions_[lhs].type == Type::F64 &&
               instructions_[rhs].type == Type::F64);
        return add({op, op == Op::LessThan || op == Op::GreaterThan
                             ? Type::I1 : Type::F64, lhs, rhs});
    }
    const std::vector<Instruction>& instructions() const {
        return instructions_;
    }
private:
    ValueId add(Instruction instruction) {
        const auto id = static_cast<ValueId>(instructions_.size());
        instructions_.push_back(instruction);
        return id;
    }
    std::vector<Instruction> instructions_;
};

// The backend provides parameter(index) and binary(op, lhs, rhs).
// Every operand refers to an earlier SSA value. No CFG or implicit mutation.
template <typename Backend>
auto lower(const Function& function, Backend& backend, ValueId result) {
    using Value = decltype(backend.parameter(uint32_t{}));
    assert(result < function.instructions().size());
    // Most scalar graphs fit on the stack, avoiding heap allocations at JIT
    // compilation time. Larger graphs still use exactly one dynamic buffer.
    constexpr size_t kInlineCapacity = 16;
    std::array<Value, kInlineCapacity> inlineValues{};
    std::vector<Value> overflow;
    const size_t count = function.instructions().size();
    if (count > kInlineCapacity) overflow.resize(count);
    auto* values = count <= kInlineCapacity
        ? inlineValues.data() : overflow.data();
    uint32_t parameterIndex = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& inst = function.instructions()[i];
        if (inst.op == Op::Parameter)
            values[i] = backend.parameter(parameterIndex++);
        else
            values[i] = backend.binary(inst.op, values[inst.lhs],
                                       values[inst.rhs]);
    }
    return values[result];
}

inline Function makeBinaryFunction(Op op) {
    Function function;
    auto lhs = function.parameter();
    auto rhs = function.parameter();
    function.binary(op, lhs, rhs);
    return function;
}

inline const Function& binaryFunction(Op op) {
    static const Function add = makeBinaryFunction(Op::Add);
    static const Function sub = makeBinaryFunction(Op::Sub);
    static const Function mul = makeBinaryFunction(Op::Mul);
    static const Function div = makeBinaryFunction(Op::Div);
    static const Function lt = makeBinaryFunction(Op::LessThan);
    static const Function gt = makeBinaryFunction(Op::GreaterThan);
    switch (op) {
    case Op::Add: return add;
    case Op::Sub: return sub;
    case Op::Mul: return mul;
    case Op::Div: return div;
    case Op::LessThan: return lt;
    case Op::GreaterThan: return gt;
    default: return add;
    }
}

} // namespace xyo::ir
