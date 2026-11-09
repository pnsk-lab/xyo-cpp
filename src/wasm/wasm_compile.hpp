// xyo-cpp: スクリプト → wasm コンパイラ（Phase C の本体）。
//
// 生成した WASM 関数単体は同期実行で yield できない。
// HotSpot はトップレベルの複数ループを prefix/body/suffix に分割し、
// 一般の yield 対象はインタプリタへフォールバックする。
//
// ABI: f(i32 slots) -> i32
//   slots : i64[n] の Scratch Value ビット列（変数 i は slots + i*8）
//   返り値: 0 = 完走、1 = bail (状態をコミットせず interp へ戻す)
//
// 対応: 数値演算/比較/条件分岐、Valueタグ付き文字列join/letter/contains/比較・文字列長、数値リストの一部操作、
// daysSince2000、非再帰warp手続きの制限付きインライン化、数値引数0～4個の末尾warp再帰。
// 非対応: 任意の副作用付き文字列レポータ、非末尾/非warp再帰・待機を含む手続き、一般のネストした
// yield/continuation、描画/音声/その他の副作用付き opcode。
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xyo::ir {
struct ScratchScript;
struct ScratchEffects;
}
namespace xyo {
struct Block;
class Blocks;
} // namespace xyo

namespace xyo::wasm {

/// 式ノード。
enum class EK : uint8_t {
    ConstNum,   // num
    ConstText,  // UTF-16 literal loaded from a bound host literal table
    StringJoin, // tagged Scratch join (host helper)
    StringLetter, // tagged character access (host helper)
    StringContains, // Scratch case-insensitive contains (host helper)
    StringLengthValue, // UTF-16 length of any tagged string expression
    VarGet,     // var
    ListGet,    // numeric list item (host-only, guarded)
    ListLength, // list length, including mixed-type lists
    ListContains, // numeric needle in a numeric-only list
    ListIndexOf,  // 1-based index (Scratch equality)
    ListContents, // original Scratch list formatting (tagged string)
    StringLength, // exact UTF-16 length of a rooted Scratch scalar
    DaysSince2000, // runtime host clock (millisecond precision)
    ProcedureArgument, // numeric argument captured once at call entry
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Round,
    MathOp,
    Neg,
    LessThan,
    GreaterThan,
    Equals,
    And,
    Or,
    Not,
};

struct Expr {
    EK k = EK::ConstNum;
    double num = 0;
    uint32_t a = 0;   // 子（単項）
    uint32_t b = 0;   // 子（二項）
    uint32_t var = 0;
};

/// 文ノード。
enum class SK : uint8_t {
    SetVar,     // var = numeric expression
    SetVarRaw,  // var = another scalar's exact tagged Value
    SetVarValue, // var = an arbitrary tagged expression (join/letter)
    SetVarBoolean, // var = boolean reporter, preserving the boolean tag
    ChangeVar,  // var += expr
    Repeat,     // repeat(expr) { body }
    Until,      // repeat until (expr) { body } (HotSpot segmented only)
    While,      // while (expr) { body } (HotSpot segmented only)
    Forever,    // forever { body } (HotSpot segmented only)
    ForEach,    // control_for_each: index in var, bound in expr (HotSpot only)
    If,         // if (expr) { body }
    IfElse,     // if (expr) { body } else { elseBody }
    ListAppend, // numeric-only host-staged list append
    ListReplace,// numeric-only host-staged indexed replacement
    ListDelete, // numeric-only host-staged indexed deletion
    ListDeleteAll, // host-staged list clear
    ListInsert, // numeric-only indexed insertion
    InlineProcedure, // guard-safe, non-yielding warp procedure
    WarpTailRecurse, // bounded numeric-parameter warp tail recursion
    WarpReturnStack, // numeric warp non-tail self recursion; LIFO return continuations
    WarpReturnPcStack, // multiple explicit return PCs and numeric argument frames
};

// A top-level, conditional self-call inside a bounded warp procedure.
// before runs at this continuation, condition decides whether to recurse.
// nextArguments are evaluated in the old frame before rebinding the callee.
struct WarpReturnSite {
    std::vector<uint32_t> before;
    uint32_t condition = 0;
    std::vector<uint32_t> nextArguments;
};

struct Stmt {
    SK k = SK::SetVar;
    uint32_t var = 0;
    uint32_t expr = 0;
    uint32_t extraExpr = 0; // list item for append/replace
    std::vector<uint32_t> body;
    std::vector<uint32_t> elseBody; // WarpReturnStack: deferred unwind suffix
    // For InlineProcedure only: argument expressions (caller scope) and
    // compiler-local argument register IDs (callee scope), in call order.
    std::vector<uint32_t> argumentExprs;
    std::vector<uint32_t> argumentSlots;
    // WarpReturnPcStack uses a bounded in-memory LIFO of return PCs and
    // tagged numeric arguments; elseBody is the final unwind suffix.
    std::vector<WarpReturnSite> returnSites;
    // A warp procedure's entry has no nonwarp-recursion scheduling guard.
    // Enables safe zero-argument, side-effect-free helper inlining inside
    // interpreter-owned fused warp loop trees.
    bool inlineWarp = false;
};

struct Program {
    std::vector<Expr> exprs;
    std::vector<Stmt> stmts;
    uint32_t nSlots = 0;
    // HotSpot-only: import env.h for exact Scratch runtime math helpers.
    // The standalone emitter can reject unsafe floating-point modulo.
    bool useHostMath = false;
    // Only a verified, interpreter-owned *warp* control statement may
    // execute a conditional loop in one transactional WASM invocation.
    // Non-warp loops must yield at Scratch's original scheduler boundary.
    bool allowBoundedConditionalLoop = false;
    // Slot order for host ABI binding. Populated by lowerScript().
    // Manually constructed Programs may leave this empty.
    std::vector<std::string> slotIds;
    // List IDs are independent of the scalar scratch slot namespace.
    std::vector<std::string> listIds;
    // Owned UTF-16 constant pool; never stores compiler-time GC pointers.
    std::vector<std::u16string> stringLiterals;
    std::vector<uint32_t> main;

    uint32_t expr(Expr e) {
        exprs.push_back(e);
        return uint32_t(exprs.size() - 1);
    }
    uint32_t exprNum(double d) {
        Expr e;
        e.k = EK::ConstNum;
        e.num = d;
        return expr(e);
    }
    uint32_t exprVar(uint32_t v) {
        Expr e;
        e.k = EK::VarGet;
        e.var = v;
        return expr(e);
    }
    uint32_t exprBin(EK k, uint32_t a, uint32_t b) {
        Expr e;
        e.k = k;
        e.a = a;
        e.b = b;
        return expr(e);
    }
    uint32_t exprUn(EK k, uint32_t a) {
        Expr e;
        e.k = k;
        e.a = a;
        return expr(e);
    }
    uint32_t stmt(Stmt s) {
        stmts.push_back(std::move(s));
        return uint32_t(stmts.size() - 1);
    }
};

/// wasm モジュールを生成する。生成に失敗したら空ベクタ。
std::vector<uint8_t> compileProgram(const Program& p);

/// ブロック木を `Program` へ落とす。未対応 opcode を含む場合は false
/// （呼び出し側はそのスクリプトをインタプリタへフォールバックする）。
/// `top` はハットブロック。本体は `top->next` のチェーン。
bool lowerScript(::xyo::Blocks& blocks, const ::xyo::Block* top,
                 Program& out);

// Lower an independently captured whole-script source IR. It preserves
// opaque instructions in the snapshot, returning false rather than
// miscompiling unsupported Scratch behavior.
bool lowerCapturedScript(const ::xyo::ir::ScratchScript& script, Program& out);

// Lower a top-level statement range [first, pastEnd) from the *same* captured
// source and effects graph. The tail and unsupported statements are retained
// by the caller for interpreter execution; no source blocks are mutated.
bool lowerCapturedRange(const ::xyo::ir::ScratchScript& script,
                        const ::xyo::ir::ScratchEffects& effects,
                        const std::string& first, const std::string& pastEnd,
                        Program& out);

// Lower one nested control statement without flattening it into a
// surrounding script chain. Used only at a verified warp loop checkpoint.
bool lowerCapturedStatement(const ::xyo::ir::ScratchScript& script,
                            const ::xyo::ir::ScratchEffects& effects,
                            const std::string& blockId, Program& out);

} // namespace xyo::wasm
