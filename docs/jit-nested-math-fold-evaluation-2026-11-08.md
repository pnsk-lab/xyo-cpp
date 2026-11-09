# MATH-012 — ネストした数学定数式の畳み込み検証（2026-11-08）

## 目的
既存の MATH-011 は `ln(2)`・`e ^ 1.5`・`10 ^ 0.05` など数値リテラルを直接入力とする数値 SSA の畳み込みを行う。本検証では `ln(1+2)`・`e ^ (3/2)`・`10 ^ ((1+2)/20)` など、演算式を入力とするケースにも再帰的な評価を追加し、実行時に関数呼び出しを削減できるか比較した。

## 試作
`src/jit/numeric_repeat_float_ssa.inc` に、純粋な定数式のみを再帰評価する試作を一時実装した。対象は有限・非サブノーマルの f64 に限定し、`+,-,*,/` と `jsnum::jsLog` / `v8math::exp` / `std::pow` の同一経路だけを採用した。NaN、Infinity、変数参照、剰余、Scratch round および AOT は除外した。切替用 `XYO_JIT_MATH_CONST_TREE` は試験専用だった。

## 測定
専用の `tests/differential/jit_constexpr_math_tree_ssa.py` を生成し、24種のネスト式・境界条件・変数混合を warp/nonwarp、interpreter/Eager/HotSpot と比較した。各6パターン120観測値はすべて一致した。`build/math012-tree-parity.log`。

`tools/bench_math_const_tree.py` で12独立スクリプト（各12演算）を同一バイナリ CPU12固定 ABBA方式で8組32回比較。CPU比中央値 `0.9963857`（約0.36%減）、8組中5組改善、全32回の結果digest一致。`build/math012-tree-abba.json`、`build/math012-tree-abba.log`。

さらに定数式の結果を毎反復で使用する `ln(1+2)+カウンタ` 等の生成IRも調査した（`build/math012-dyn-ir-*.err`）。LLVM は既存の式生成後に定数演算と数学呼び出しを既に畳み込んでおり、ON/OFFで `rep.float.head` の最終 IR が同一だった。ベンチマークの差はごく小さいため、積極的なメリットは見出せない。

## 採否
**不採用・撤回**。追加の再帰畳み込み実装と `XYO_JIT_MATH_CONST_TREE` は `src/jit/numeric_repeat_float_ssa.inc` から削除した。MATH-011 の既存の単一数値リテラル畳み込みと数学定数表はそのまま保持した。測定スクリプトと差分試験は調査記録として残した。

他のエージェントが編集している `src/jit/codegen.cpp`、runtime、renderer は変更せず、commit/pushもしていない。
