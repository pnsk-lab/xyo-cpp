# MATH-013 — N-body の冗長な NaN ガード削減（2026-11-08）

## 対象
`src/jit/numeric_repeat_float_ssa.inc` の浮動小数点 warp repeat SSA で、演算ごとに積み上げる `FCmpORD(result,result)` の一部が、IEEE-754 の性質から不要である。

例: `x*x` は x が NaN でなければ Inf を含め必ず NaN にならず、`x*x+y*y` は非負で NaN にならず、その平方根も NaN にならない。JIT 入口では全キャリーが Numberかつ非NaNであることを確認済み。すでに前の文で NaN が出た場合は valid=false のため現在の反復を最後に確実に巻き戻す。

## 実装と切替
`FloatRepeatNaNFacts` と `floatRepeatNaNFacts` を追加して、条件付きで保証される非NaN・非負の事実を解析する。静的に保証できる場合のみ、`emitFloatRepeatExpr` 末尾の `FCmpORD` を削除する。

安全なケースは変数の値（valid が成立する限り）、数値リテラル、x*x、非負同士の加算、有限リテラルを片方に持つ加減算、非ゼロ有限リテラルによる乗除算、非負の平方根、abs/floor/ceil/round/atan/exp/10^、非負数の ln 等。不確定な演算は従来通り判定する。

既定 ON、`XYO_JIT_FLOAT_NAN_FACTS=0` で全ての追加証明を無効にできる。実行数学演算の値は変更せず、不要と証明できたチェックだけを削除する。

## IRと性能
独立した Scratch-bench N-body カーネルを抽出して、同一バイナリ Eager JIT の LLVM IR を比較した。

- `rep.float.head` の `fcmp ord`: **9個 → 5個**。
- 同ループ IRの長さ: **1948 → 1617文字**。
- 12組 ABBA/BAAB、同一バイナリ CPU12固定、`XYO_JIT_FLOAT_NAN_FACTS` ON/OFF 比較。**12/12組で改善**し、速度比中央値は **1.0128881x**（約1.29%増）。チェックサムは全48回 `51200`。
- 生データは `build/math013-nbody-abba.json`、`build/math013-nbody-abba.log`。生成IRは `build/math013-ir-{0,1}.err`。
- これは純粋な N-body カーネルの改善であり、全Scratchプロジェクトに同率の改善を約束するものではない。

## 回帰検証
- `tests/differential/jit_float_nan_facts_ssa.py`: 36ケース、180観測値の warp/non-warp、Eager OFF/ON、HotSpot ON と interpreter を比較。NaN、Infinity、負のゼロ、極大・極小値、0除算、符号反転、NaN再発生を含む。**全件一致**。
- 従来 `jit_float_repeat_ssa.py`（208観測値 + 7巻き戻し）、`jit_transcendental_repeat_ssa.py`（各129観測値）の回帰テストも PASS。
- Scratch-bench parity: **HotSpot/Eager/Interpreter/AOT 各2123/2123**。`build/math013-parity.log`。
- CTest `xyo-tests`: **1/1 PASS**（43.16秒）。`build/math013-ctest.log`。
- 全47項目性能テストの ON/OFF チェックサム比較: `build/math013-full-{off,on}.log`。

## 所有と状態
MATH-013 の修正対象は数学 SSA の include と、追加差分テスト、既存の計測スクリプト `tools/bench_math_abba.py`。他エージェント管理の JIT共通 codegen/RT、scheduler、renderer には介入していない。共有ツリーなのでコミット/プッシュは行っていない。

## Final full-suite and additional paired checks

The full unmodified 47-kernel ScratchBench performance project completed
with the same binary and facts OFF/ON. All non-scheduler checksums
matched exactly; no regression in observable values.
Logs: build/math013-full-off.log, build/math013-full-on.log.

The first sequential run showed that Math Rounding and Math Exponential
& sqrt might also benefit. Those results alone were not used to claim a
speedup: each kernel was then isolated and tested with the same
eight-group CPU12-pinned ABBA/BAAB checker as N-body.

| Kernel | Median ON/OFF rate | Stable groups | Checksum |
| --- | ---: | ---: | ---: |
| Math Rounding | **1.2048712x** (+20.49%) | 8/8 | 47241333 |
| Math Exponential & sqrt | **1.0435238x** (+4.35%) | 8/8 | 718102 |
| Math Two-body / N-body | **1.0128881x** (+1.29%) | 12/12 | 51200 |

Raw paired measurements:
- build/math013-round-exp-abba.json and .log
- build/math013-nbody-abba.json and .log

Unlike the earlier whole-suite sequential comparisons, every paired
group stayed inside the 8% drift threshold. This gain comes from
removing checks *proven redundant* within native JIT float SSA, not from
altering numerical results or reducing Scratch instruction counts.

The full-suite sequential rates for other Math categories changed by
small amounts; do not present those as a reliable estimate without
a paired comparison.
