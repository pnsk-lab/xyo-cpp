# MATH-016 — 整数SSAの32bit化と剰余選択（2026-11-09）

## 実装

対象: src/jit/numeric_repeat_ssa.inc

- 既存の数値SSAは、入口でNumber型、非NaN、非負整数、負のゼロでないこと、値が2^21以下であることをガードする。ループの64反復を整数の上界として解析し、必要なら通常経路へフォールバックする。
- この64反復の全整数中間演算（加算、乗算、剰余の被除数・除数、書き込み）がUINT32_MAX以下と証明できる場合だけ、整数状態PHIと計算をi32で統一する。証明できなければ従来のi64 SSAを維持。途中にtrunc i64を挟まないので剰余命令だけ狭めた試作より効率がよい。
- 全演算がINT32_MAX以下であれば符号付きsrem i32を使うことも可能。ただし8個の剰余を持つループでは符号付き除算展開が遅いため、ループ内の剰余が1個ならsigned i32、複数ならunsigned i32を自動選択。プロジェクト名による特例ではない。
- 変更はJITコード生成時に決まり、実行時のScratch動作・GC・描画・yield ABIには介入しない。

環境変数:
- XYO_JIT_NUMERIC_REPEAT_U32_MOD=0: 32bit SSAを無効化し、元のi64 SSAを使用。既定は自動有効。
- XYO_JIT_NUMERIC_REPEAT_SIGNED_MOD=0: unsigned i32のみを選択。=1: signedの範囲が証明できる場合にsigned i32を強制選択。未設定: 剰余1個ならsigned、複数ならunsignedを選択。

## 失敗した初期案

整数状態をi64で保持したまま剰余だけtrunc→urem i32→zextとする案は、同一バイナリ・CPU12固定・12組ABBAでMath Arithmeticが0.9965556倍（約0.34%遅い）。不採用。

## 32bit状態SSAの性能（同一バイナリ交互測定）

Math Arithmetic:
- unsigned i32 / i64: 12組ABBA中央値1.0108853倍、全12組改善。データ: build/math016-u32phi-abba.json。
- signed i32 / unsigned i32: 12組ABBA中央値1.0058529倍、全12組改善。データ: build/math016-signed-abba.json。
- IRで整数PHI2つがi32、剰余がsrem i32になることを確認。

Variablesスイート（vars_manyとvars_localに同じ算術ループが存在）:
- signed i32 / i64: vars_many 0.9186547倍、vars_local 0.9130804倍（4/4組で回帰）。データ: build/math016-vars-suite-abba.json。
- unsigned i32 / i64: vars_many 1.3335042倍、vars_local 1.3378178倍（4/4組で改善）。データ: build/math016-vars-suite-u32-abba.json。
- 単独vars_many: unsigned i32 / i64の8組ABBA中央値1.3493082倍、8/8組改善。algo_primesは0.9995411倍でほぼ変化なし。データ: build/math016-vars-primes-abba.json。
- 全てチェックサム一致。変数の数と剰余サイト数に応じて自動選択する方針を採用。

## 最終ビルドと回帰検証

検証用に build/math016-final-select-xyo を固定し、最新共有ビルドを確認した。

IR検証:
- math_arith: phi i32が2つ、srem i32が1つ、urem i64なし。
- vars_many: phi i32が8つ、urem i32が8つ、srem i32なし。
ログ: build/math016-default-ir-arith.err、build/math016-default-ir-vars_many.err。

パリティ検証:
- tests/differential/jit_numeric_repeat_u32_mod_ssa.py: 14初期値×11係数・除数形状=154独立スクリプト。各770観測値、Eager旧i64 / unsigned i32 / signed i32、HotSpot unsigned/signed、warp/nonwarpの10構成をInterpreterと比較。全件一致。負のゼロ、数字文字列、非数値文字列、NaN/Infinity、64反復境界、INT32_MAXとUINT32_MAXをまたぐ区間証明を含む。ログ: build/math016-select-diff.log。
- 既存のjit_float_repeat_ssa.py、jit_numeric_repeat_ssa.py、jit_conditional_repeat_ssa.py、jit_repeat_copy_ssa.pyもPASS。ログ: build/math016-select-float.log、build/math016-jit_numeric_repeat_ssa.log、build/math016-jit_conditional_repeat_ssa.log、build/math016-jit_repeat_copy_ssa.log。
- Scratch-bench parity: HotSpot / Eager / Interpreter / AOTでそれぞれ2123/2123。ログ: build/math016-select-parity.log。
- CTest xyo-tests: 1/1 PASS（65.91秒）。ログ: build/math016-select-ctest.log。
- 元の全47項目のperformanceを幅拡張OFFと既定選択で双方完走、非schedulerのチェックサム相違0。連続実行でMath Arithmetic 1.0166倍、vars_many 1.3468倍、vars_local 1.3517倍。ログ: build/math016-selected-full-off.log、build/math016-selected-full-default.log。速度の因果推定には上の交互測定を用いる。

## 変更ファイルと統合状況

src/jit/numeric_repeat_ssa.inc：区間解析、32bit整数キャリー、signed/unsigned選択。
tests/differential/jit_numeric_repeat_u32_mod_ssa.py：境界を含むScratch差分テスト。
tools/bench_math_abba.py：単独Arithmetic/Variables/Primes交互測定とフラグ制御。
tools/bench_math_vars_suite_abba.py：Variablesスイート全体の交互測定。
本ドキュメント。

他エージェント管理のメインcodegen.cpp、RT/IR、スケジューラ、描画は編集していない。共有ツリーの変更を保持し、commit/pushは実施していない。
