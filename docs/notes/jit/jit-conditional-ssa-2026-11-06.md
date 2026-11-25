# LLVM SSA による条件分岐・空ループ高速化（2026-11-06）

## 変更

src/jit/conditional_ssa.inc と src/jit/codegen.cpp に、条件付き repeat の整数 SSA 経路を実装。
定数増分、set x = x +/- N や set x = y + N、ネストした if/if else、数値の加減算、正の定数剰余、比較、論理演算を静的に解析する。
未知のブロック・副作用・変数名と ID の別名が曖昧な場合は対象外。ループ本体内の外部呼び出しを削減し、LLVM の通常の O2/O3 最適化へ PHI を持つ整数ループを渡す。

既存の exact i32 repeat と warp が成立し、参照変数が全て plain f64 の厳密な整数で絶対値が 2^40 以下のときのみ、次の64反復期限確認まで計算する。Scratch 変数を期限確認前に公開し、その後は毎回再ガードする。非 warp、文字列・小数・未知の式は既存経路へ戻る。
環境変数 XYO_JIT_CONDITIONAL_SSA=1 で試験的に有効化。**既定 OFF**。独立した実作品コーパスで効果が確認されるまで既定経路には適用しない。

入れ子の空 repeat は従来 C++ の xyo_empty_repeat_warp 呼出しに戻っていた。新しい direct scheduler はグローバルの yieldCheckN を元のヘルパと同じ64境界で更新し、必要な warp 期限検査と実際の Fiber 切替だけ実施する。外側の private yield counter は触れない。XYO_JIT_INLINE_EMPTY_DIRECT=1 で試験的に有効化。**既定 OFF**（既存の非入れ子空ループ最適化はそのまま）。

## A/B 結果

同一バイナリの ON/OFF 比較。ScratchBench の実際の SB3 を使用。数値は ops/ms、高いほど高速。Eager JIT、CPU・GPU並列なし。

| 項目 | OFF | ON | 倍率 | チェックサム |
|---|---:|---:|---:|---|
| If / else chain（条件付きSSA） | 396,206 | 988,856 | 2.50× | 16001 |
| Empty loop（direct scheduler） | 3,510,116 | 93,469,744 | 26.63× | 40000 |
| Repeat（direct scheduler だけ切替） | 19,882,584 | 19,998,836 | 1.006× | 20000 |
| Nested loops（direct scheduler だけ切替） | 13,597,079 | 13,702,251 | 1.008× | 19600 |
| Math compare（両機能のON/OFF） | 251,813 | 339,479 | 1.348× | 5738 |

条件付き SSA は各5回、空 scheduler は CPU12 固定で各7回実行。元データは build/conditional-ssa-ab.json と build/conditional-ssa-empty-nested-ab.json（ビルド生成物、git管理外）。倍率は対象ループの数値であり、任意の Scratch 作品の倍率ではない。

## 差分テスト

tests/differential/jit_conditional_repeat_ssa.py は warp/nonwarp、Eager/HotSpot、LLVM O2、整数・小数・文字列、負数・負の剰余、ループ境界 0/1/63/64/65/127/128/129、ネスト条件を検証。
tests/differential/jit_empty_inline_scheduler.py にはネストの空 repeat、旧/新 scheduler、共有時計 ON/OFF を追加。
関連 CTest 登録済み。ビルド、差分テスト、ScratchBenchチェックサムを確認。

## 追加の横断検証（2026-11-06）

更新したJITライブラリを既存のGUIオブジェクトと手動リンクした検証用バイナリ（build/xyo-conditional-ssa）を使用。
他作業で進行中の GUI 側ソース変更（Launcher::button 宣言と定義の不一致）が通常の CLI 再ビルドを阻害しているため、GUI ソースには触れていない。

上記12項目のScratchBenchをCPU 12へ固定し、両機能 OFF/ON を各3回交互測定した。全項目でチェックサム一致。生データ: build/conditional-ssa-crossbench.json。

| 項目 | OFF (ops/ms) | ON (ops/ms) | 倍率 |
|---|---:|---:|---:|
| loops_empty | 3,315,762 | 89,092,657 | 26.869× |
| loops_ifelse | 378,808 | 956,186 | 2.524× |
| math_compare | 251,813 | 339,479 | 1.348× |
| loops_repeat | 18,930,976 | 18,964,075 | 1.002× |
| loops_until | 11,340,564 | 11,338,180 | 1.000× |
| loops_while | 11,310,134 | 11,337,345 | 1.002× |
| loops_foreach | 14,233,246 | 14,247,154 | 1.001× |
| loops_nested | 12,915,330 | 12,898,651 | 0.999× |
| vars_setchange | 12,282,452 | 12,299,471 | 1.001× |
| algo_sort | 354,701 | 355,653 | 1.003× |
| app_ast | 35,690 | 35,833 | 1.004× |
| app_brainfuck | 133,817 | 133,488 | 0.998× |

set系の拡張は独立生成SB3で8構成（warp/nonwarp、Eager/HotSpot、O2、OFFとの比較）PASS。
AOT オブジェクトを生成し、warpでは68変数、nonwarpでは66変数の値がインタプリタと一致。
最適化後のLLVM IRダンプでも、i64 PHIと定数剰余の srem i64 が生成されることを確認（build/conditional-ssa-ir-run.err）。


## 評価上の留意事項と既定値変更

2026-11-06 に評価方針を修正。上記の倍率は、特定の処理形状を認識する最適化に対して ScratchBench の対応項目を使った値であり、一般的な Scratch 作品の性能向上を代表しない。特に空ループの処理量（ops）は実用上の仕事量ではない。これらの数値を製品全体の高速化や SoTA の根拠として扱わない。

- 条件付き整数 SSA：既定 **OFF**、XYO_JIT_CONDITIONAL_SSA=1 で有効化。
- 入れ子空ループの直接期限確認：既定 **OFF**、XYO_JIT_INLINE_EMPTY_DIRECT=1 で有効化。
- 以前から存在する XYO_JIT_INLINE_EMPTY_SCHED の非入れ子最適化は変更しない。

既定 ON の再検討条件は、特定ベンチから独立した、事前に固定された実 SB3 コーパスでの比較である。プロジェクトごとの動作一致、フレーム時間の分布、JIT コンパイル時間、実行時メモリ、全体の中央値と幾何平均、最悪ケースの回帰を確認する。最適化を選ぶために使ったワークロードは最終評価用コーパスから除外する。

本来進めるべきことは、任意の Scratch ブロックに対する副作用解析と、yield/外部関数呼出しを跨がないスカラー SSA の構築、型ガード、明示的な状態の書戻し／再読込である。LLVM の GVN、SROA、LICM、LoopSimplify が通用する一般的な中間表現を用意し、コードサイズと JIT 時間の増加も評価する。
