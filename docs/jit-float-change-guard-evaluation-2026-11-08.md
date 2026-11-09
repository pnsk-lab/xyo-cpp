# MATH-015 — f64 change-by 加算の NaN 判定省略実験（2026-11-08）

## 前提

MATH-013/014 の浮動小数点数値SSAは、値が NaN になった反復を巻き戻すため、式内の NaN 判定と `data_changevariableby` の加算後の `FCmpORD` を実施する。

加算量が有限な数値リテラルである場合、加算先が非NaNであれば ±Infinity と加えても新たな NaN は生じないため、条件付きで `change-by` 直後の NaN 判定を省ける。その証明を `src/jit/numeric_repeat_float_ssa.inc` に一時実装し、比較のため `XYO_JIT_FLOAT_CONST_CHANGE_FACTS` を設けた。

## 現在の共有ビルドへの統合

23:42時点で `warp_clock.cpp` のビルドエラーは解消され、最新共有の `build/xyo` に MATH-014 が含まれていることを実行ファイル内のフラグ文字列で確認。`build/math015-head-xyo` として固定した最新共有バイナリで Scratch-bench parity を実行すると、HotSpot、Eager、Interpreter、AOT すべて **2123/2123**。ログ: `build/math015-head-parity.log`。

MATH-015変更後の共有ビルドも通常の `cmake --build build --target xyo-cli -j 3` が成功。バイナリ `build/math015-change-xyo` を固定した。

## 正確性

`tests/differential/jit_float_change_const_nan_ssa.py` を新規追加。10種類の初期値（0、負の0、±1、±1e308、tiny、±Infinity、NaN）と7種類の有限定数変化量（-1e308、-1、-0、0、0.1、1、1e308）から70独立Scratchスクリプトを生成。さらに非定数 `change-by` による既存チェックも保持。warp/non-warp、interpreter、Eager旧/新、HotSpot新の計6構成で、各350観測値が完全一致。ログ: `build/math015-change-differential.log`。

Scratch-bench parity はMATH-015変更後も、HotSpot/Eager/Interpreter/AOTの4方式すべて **2123/2123**。ログ: `build/math015-parity.log`。

## LLVM IR と交互速度測定

丸めと三角関数のScratch-bench独立プロジェクトを `XYO_JIT_DUMP2=1` で比較。
- Rounding: `rep.float.head` の `fcmp ord` 数は旧/新どちらも **2個**。
- Trigonometry: 旧/新いずれも **3個**。
- Exponential & sqrt: 旧/新IRは完全一致。
- N-body: 旧/新IRは完全一致。

LLVM はチェックと演算を別の形式に最適化しており、実効的なチェック数削減は確認できなかった。
同一バイナリ・CPU12固定・8組のABBA交互測定（すべて安定条件8%以下）では:
- Rounding: 最適化ON/OFFの速度比中央値 **0.9796278倍**（約2.04%遅い）
- Trigonometry: **0.9905311倍**（約0.95%遅い）

すべてのチェックサムは旧/新で一致。生データ `build/math015-round-trig-abba.json`、ログ `build/math015-round-trig-abba.log`。

## 採否

**不採用・実装撤回**。`src/jit/numeric_repeat_float_ssa.inc` からMATH-015の変更と環境変数を削除し、`tools/bench_math_abba.py` の診断スイッチも元へ戻した。MATH-014の最適化までを残す。失敗した実験を再調査できるよう、独立テストと計測ログは保存。

他エージェントのメインcodegen、RT/IR、レンダラー、スケジューラ変更は維持。未コミット・未プッシュ。
