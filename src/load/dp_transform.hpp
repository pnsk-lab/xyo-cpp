// xyo-cpp: DP（動的計画法）変換 — 純粋な再帰手続きの自動メモ化。
//
// Scratch のカスタムブロック（reporter 手続き）が次の条件を満たすとき、
// 結果をスプライトローカルの隠しリストへメモ化する（トップダウン DP）。
//
//   * warp 手続きで、引数がちょうど 1 個
//   * 本体が「純粋」: 変数・リスト・sensing/motion/looks/sound/pen・乱数・
//     broadcast・clone・wait/stop を一切使わず、数値演算と制御構文だけ
//   * 自分自身を呼ぶ（再帰）呼び出しがあり、値を返す return がある
//
// 純粋なら (引数 → 戻り値) は関数なので、同じ引数の再計算を省いても
// 結果は 1 bit も変わらない。指数時間の素朴再帰（fib など）が線形時間に
// なる。プロジェクトのファイルは書き換えず、ロード後の実行時ブロックだけを
// 書き換える（XYO_DP=0 で無効化）。
#pragma once

#include <string>

namespace xyo {

struct Runtime;
struct Target;

namespace dp {

struct Stats {
    int procedures = 0;   // 変換した手続き数
    int returns = 0;      // メモ化した return 数
    int rejected = 0;     // 対象外だった手続き数
};

/// target 内の手続きを走査してメモ化へ書き換える。
Stats applyToTarget(Target& target);

/// 全ターゲット（ステージ + スプライト）に適用する。
Stats applyToRuntime(Runtime& runtime);

}  // namespace dp
}  // namespace xyo
