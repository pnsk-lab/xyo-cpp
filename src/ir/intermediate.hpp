// xyo-cpp: IR 中間ノード — intermediate.js の移植（design.md §6.2）。
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "enums.hpp"

namespace xyo::ir {

struct IrInput;
struct IrStackBlock;
struct IrStack;

/// inputs の値は「IrInput ツリー」「IrStack」「プリミティブ値」のいずれか。
/// 可変長フィールドを型安全に保つため variant を使う。
struct IrInputs {
    // name → 値。値の種別は各 opcode の仕様による。
    std::map<std::string, std::variant<
        std::unique_ptr<IrInput>,
        std::unique_ptr<IrStack>,
        double,
        bool,
        std::string,
        int64_t>> entries;
};

struct IrInput {
    InputOp opcode;
    Type type;
    IrInputs inputs;
    bool yields = false;

    bool isConstant(double v) const;
    bool isConstant(const char* v) const;
    bool isAlwaysType(Type t) const { return always(type, t); }
    bool isSometimesType(Type t) const { return sometimes(type, t); }
    /// target へキャスト（定数なら畳み込み、さもなくば CAST_* ノードを返す）。
    /// design.md §6.2 の toType 相当。
    std::unique_ptr<IrInput> toType(Type target) &&;
};

struct IrStack {
    std::vector<IrStackBlock> blocks;
};

class TypeState; // iroptimizer.hpp

struct IrStackBlock {
    StackOp opcode;
    IrInputs inputs;
    bool yields = false;
    bool ignoreState = false;
    TypeState* entryState = nullptr;
    TypeState* exitState = nullptr;
};

struct IrScript {
    std::string topBlockId;
    std::optional<IrStack> stack;
    bool isProcedure = false;
    std::string procedureVariant;
    std::string procedureCode;
    std::vector<std::string> arguments;
    bool isWarp = false;
    bool yields = true;
    bool warpTimer = false;
    std::vector<std::string> dependedProcedures;
    void* cachedCompileResult = nullptr;
    TypeState* cachedAnalysisEndState = nullptr;
    bool executableHat = false;
};

struct IrRep {
    IrScript entry;
    std::map<std::string, IrScript> procedures;

    IrScript* getProcedure(const std::string& proccode);
};

} // namespace xyo::ir
