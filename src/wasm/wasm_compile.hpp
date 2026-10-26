// xyo-cpp: スクリプト → wasm コンパイラ（Phase C の本体）。
//
// 対象は **yield しないスクリプト**（warp ループ等）。wasm 関数は途中で
// 中断できないため、yield し得るスクリプトはコンパイルせずインタプリタへ
// フォールバックする（呼び出し側の責務）。
//
// ABI: `f(i32 slots) -> i32`
//   slots : i64[n] の Value ビット列（変数 i は slots + i*8）
//   返り値: 0 = 完走、1 = bail（非数値に遭遇 → 呼び出し側が interp へ戻す）
//
// 意味論:
//   - 数値は f64。算術入力の NaN は 0 に落とす（Scratch Cast.toNumber /
//     TW toNotNaN と同じ）。結果の NaN は保持し、代入時に kNan へ正規化する。
//   - 変数読みで「数値以外」のタグを見つけたら bail（文字列比較等は未対応）。
//   - repeat の回数は Math.round 相当（floor(x+0.5)）で 0 未満は 0。
//
// 未対応（bail 対象ではなく、コンパイル時に除外する）: 文字列・リスト・
// looks/motion/sensing/sound・手続き呼び出し・forever/while/until・
// procedures_*。これらは `Program` を作る段階で弾く。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xyo {
struct Block;
class Blocks;
} // namespace xyo

namespace xyo::wasm {

/// 式ノード。
enum class EK : uint8_t {
    ConstNum,   // num
    VarGet,     // var
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Round,
    Neg,
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
    SetVar,     // var = expr
    ChangeVar,  // var += expr
    Repeat,     // repeat(expr) { body }
};

struct Stmt {
    SK k = SK::SetVar;
    uint32_t var = 0;
    uint32_t expr = 0;
    std::vector<uint32_t> body;
};

struct Program {
    std::vector<Expr> exprs;
    std::vector<Stmt> stmts;
    uint32_t nSlots = 0;
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

} // namespace xyo::wasm
