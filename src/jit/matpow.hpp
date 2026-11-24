// xyo-cpp: 線形漸化式ループ（matrix-exponentiation ループ）の検出。
//
//   repeat <n>
//     set <v> to <affine 式>     （0 回以上・無条件）
//     change <v> by <affine 式>
//     ...
//
// 本体の全代入が「状態変数の affine（一次）関数」で書けるとき、1 反復は
// 拡大状態ベクトル [v; 1] への行列掛けなので、n 反復は **行列累乗** で
// O(log n) に置き換えられる。
//
//   例: set a to b; set b to a + b   →  [[0,1,0],[1,1,0],[0,0,1]] （fib）
//       change x by 3               →  [[1,3],[0,1]]
//
// f64 の bit 一致だけでは逐次ループと結果が異なり得るため、この特殊化は
// **整数環の厳密性**に限定する:
//  * 全係数・全定数は有限の整数（|c| ≤ 2^45）。`x/2` のような非整数係数は
//    コンパイル時に拒否（逐次の丸めを行列累乗では再現できない）。
//  * 実行時に (a) 全状態変数が数値で有限・整数・|v| < 4.5e15・-0 でない、
//    (b) ||A^(2^j)||∞ の積 P とコンパイル時の文ノルム積 H から
//        2·(k+1)·max|v|·P·H < 4.0e15 が示せる、ときだけ適用する。
//    この条件は「逐次評価の全中間値（部分和・式内の積・除算の被除数）が
//    2^53 未満の整数」＝「行列累乗の全中間行列要素が 2^53 未満」を保証し、
//    両者の結果が bit 完全一致になる。満たさなければ 0 を返して逐次へ。
//  * TIMES は逐次ループと同じ floor(n+0.5) 回として扱い、n<0.5 や非数値は
//    「適用・書き込み無し」（ret 2）で逐次と一致させる。
//
// 判定は保守側: 制御構文（内側 repeat のみ定数回数なら合成して畳む）・
// 手続き呼出し・リスト/センシング/乱数・非 affine 演算（mod/round/abs/
// 変数×変数 等）が 1 つでもあれば諦める。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xyo {

struct Block;
struct BlockField;
struct Blocks;

namespace jit {

/// 状態変数の上限（拡大行列は (k+1)x(k+1)）
inline constexpr int kMatPowMaxVars = 32;
/// 係数・定数・行列要素の上限（int64 合成で厳密に計算する範囲）
inline constexpr int64_t kMatPowCoeffLimit = int64_t(1) << 45;

struct MatPowPlan {
    const Block* loop = nullptr;
    /// 状態変数（VARIABLE フィールド）。読みだけの変数も含む。
    std::vector<const BlockField*> vars;
    /// 本体が実際に書き込む変数（この添字だけ書き戻す）。読み専用の
    /// 変数は値の計算に使うだけで、ループ後も変化しない。
    std::vector<char> written;
    /// 1 反復後の「型の出所」。値 w ≥ 0 は「初期値 w の型を引き継ぐ」
    /// （= 裸のコピー文だけを通った）、-1 は「算術結果で数値に確定」。
    /// xyo の JIT は `set x to 0` の数値シャドウを文字列 "0" のまま保持
    /// することがあるため、書き込み対象が 1 反復後に必ず数値になること
    /// を実行時にこの表で検証する（なれば逐次と型も一致する）。
    std::vector<int32_t> postSrc;
    /// 拡大行列（(k+1)x(k+1)、row-major、int64）。最終行は [0..0,1]。
    /// 添字 k の列が affine 定数列。
    std::vector<int64_t> mat;
    /// 逐次評価の文中間値を押さえる文ノルム積（保守的上界、cap 2^60）。
    int64_t stmtBound = 1;
    /// 内側 repeat の回数が状態変数依存（affine）のとき真。このとき回数が
    /// 負になり得ないことを実行時に確かめる（全状態変数 ≥ 0 を要求）。
    bool needsNonnegState = false;

    // --- 条件付きループ（repeat until / while）---------------------------
    /// 0 = repeat（TIMES 回）、1 = repeat until、2 = repeat while。
    int32_t condKind = 0;
    /// 0 = `<`、1 = `>`（条件は affine 同士の比較のみ）。
    int32_t cmpKind = 0;
    /// 条件の差 L-R（長さ dim、末尾が定数）。反復ごとに 1 行の affine で、
    /// 単調性（係数行列の性質から証明済み）で最初に真になる反復を二分探索する。
    std::vector<int64_t> diff;
};

/// `loop` が行列累乗できる `control_repeat` なら plan を返す。
/// 不可なら nullopt と理由（why 非 null のとき）。
std::optional<MatPowPlan> analyzeMatPow(Blocks& blocks, const Block* loop,
                                      std::string* why);

}  // namespace jit
}  // namespace xyo
