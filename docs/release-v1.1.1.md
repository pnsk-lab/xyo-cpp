# xyo-cpp v1.1.1 — 汎用SSAループ最適化

## 概要

既存の未コミット開発成果から、ScratchBenchの特定の作品名や固定の入力に依存しないLLVM JIT最適化を抽出した。v1.1.0の最新mainを基に別の作業ツリーで移植し、型・範囲・副作用に関するガードを保ったままリリースする。

- `repeat` の整数加算・減算を、最大64反復のSSA還元ループへ変換する
- `for each` の独立した線形加算をSSA還元ループへ変換する
- 変数間コピーと整数増分のみからなる小規模な `repeat` をSSAで実行する
- `while` / `repeat until` の数値誘導変数に関する保守的な高速経路
- リスト要素同士の比較で、実行時に両値が非NaNのIEEE数値ならネイティブの浮動小数点比較を使い、その他は従来ヘルパへ戻す

特殊な数値・文字列・非warp・サイドエフェクト・動的範囲などに対しては、従来経路へフォールバックする。中断可能なwarpループは最大64反復ずつ処理し、元のスケジューラのyield境界を維持する。

## 候補バイナリによるA/B実測

CPU 12固定、同じRelease実行ファイルの環境変数でON/OFFを切り替え、ScratchBenchの各SB3についてランダム順に8ペア実行した。各ペアのops/ms速度比の中央値（1超で速い）。すべてのペアでチェックサム一致。

| ScratchBench | ON/OFF速度比 |
| --- | ---: |
| loops_repeat | 8.4315倍 |
| loops_foreach | 7.2605倍 |
| loops_nested | 5.8864倍 |
| vars_setchange | 6.1457倍 |
| algo_sort | 1.2117倍 |
| math_compare | 1.0014倍 |
| app_ast | 1.0128倍 |
| app_brainfuck | 1.0046倍 |

SHA256: `84ccd9c678b0d91991c1b123c48b273652ee0c19783dcac839a13f8e669c1ca4`

これは特定の最適化可能な処理が含まれるSB3の定常的なスループット差であり、一般の作品全体の加速率ではない。再現用の出力はビルドディレクトリ `build/v111-bench/results.json` に保存され、Gitには含まない。

## 回帰テスト

- `jit_repeat_ssa.py`: 数値文字列、負の増分、0/63/64/65/129回、非warp、非対応パターン。
- `jit_foreach_ssa.py`: 動的上限、インデックスの可視性、浮動小数点、型変化。
- `jit_repeat_copy_ssa.py`: 変数コピー順序、非数値、warp境界、整数上限。
- `jit_warp_induction.py`: 誘導変数最適化と従来経路の差分。
- `plain_numeric_cmp.py`: 混在リスト、NaN・Infinity・負のゼロ・真偽値・Unicode・途中の型変更。
- `plain_numeric_sort_algos.py`: bubble/quick/merge sortの全要素を比較。

上記6テストの単独実行、CTest全20/20（イベントスケジューラの長時間テストを含む）に成功。AOTは `loops_repeat`、`loops_foreach`、`loops_nested`、`vars_setchange`、`algo_sort` のコンパイル・実行とチェックサム照合に成功。

## 対象外

文字列・GC・Web版JITなどの別作業、実験的な `XYO_JIT_NUMERIC_LIST`、`XYO_JIT_EPOCH_GUARD`、`XYO_JIT_TRIG_HOIST` は今回のリリースへ混入させていない。特に数値リストの生配列アクセスでは型フラグ更新の保証がまだ不十分であり、無条件の有効化はしない。

## 制限

別スクリプトがwarpのちょうどyield境界で変数を書き換える状況について、現在の差分フィクスチャで網羅していない組合せは残る。守備的なランタイムガードと既存の同期動作を使い、未知のパターンには従来経路を選択する。
