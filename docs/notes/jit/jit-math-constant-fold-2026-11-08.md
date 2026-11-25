# MATH-011 — JIT 数学定数表と定数畳み込み（2026-11-08）

## 実装

`src/jit/numeric_repeat_float_ssa.inc` の純粋数値 warp repeat SSA で、コンパイル時に数値リテラルと判定できる `ln`、`e ^`、`10 ^` を定数畳み込みする。Scratch 実行時と同じ `jsnum::jsLog`、`v8math::exp`、`std::pow(10.0, x)` を使用し、NaN ケースは従来 lowering に戻す。AOT は対象外。既定 ON、`XYO_JIT_MATH_CONST_FOLD=0` で無効化。

## 比較と採否

* 単一 warp 数値 SSA 内の12 ln reporterで、`XYO_JIT_LOG_SHARED_TABLE=1` と既定モジュール定数表を8組32回ABBA比較：プロセス CPU 時間比の組中央値0.97505（6/8組改善）、RSS差中央値 -0.15 MiB。出力32回完全一致。生データ `build/math011-shared-log-cost.json`。
* 24独立 warp script で同比較：IRログ約6.30 MBから約0.79 MB、CPU比中央値0.9660（5/8組改善）、RSS差中央値 +0.073 MiB。生データ `build/math011-multiscript-cost.json`。
* 8192件 `jsLog` の計算結果をプロセスにキャッシュし、LLVMモジュール内グローバル生成に再利用する案も実験したが、24 scriptでは中央値CPU比0.9923（4/8組改善）、64 scriptでは1.0154（1/6組改善）。効果が安定しないため**撤回**し、以前の挙動に戻した。
* 数値リテラルに対する数学定数畳み込み（12 script・8組ABBA、同一バイナリ・CPU12固定）は CPU比中央値0.96108（8/8組改善）、RSS差中央値 -0.22 MiB。生データ `build/math011-constfold-abba.json`。これは JIT起動＋実行を含む合算CPU時間で、実行ループ単体の改善率ではない。

## 正確性

* `tools/bench_math_const_fold.py` で生成した数値境界（負のゼロ、ln(-1), ln(0)、大きな正負の指数、アンダーフロー/オーバーフロー）で、fold ON/OFF, warp/non-warp, Eager/HotSpot/Interpreter の観測値ハッシュが一致。`build/math011-edge-*.json`。
* 従来数学演算の差分テスト、ln295観測値、exp241、pow10 241 の各テストは PASS。`build/math011-{math,log,exp,pow10}-diff.log`。
* `scratch-bench` parity: HotSpot / Eager / Interpreter / AOT が全て 2123/2123。`build/math011-parity.log`。
* 全47項目性能スイート fold ON/OFF 完走、非 scheduler チェックサム差分0。`build/math011-full-{on,off}.log`。無関係な項目の速度にもホスト負荷による大幅な変化があるため、連続実行倍率は因果的な測定値として採用しない。
* CTest `xyo-tests` 1/1 PASS、48.72秒。`build/math011-ctest.log`。

## 安全性と今後

* 共有の動的アドレスを LLVM IR に埋め込む `XYO_JIT_LOG_SHARED_TABLE=1` は以前の実験機能のまま **OFF が既定**。JITキャッシュの永続化やリモート実行に適さないため、積極的に推奨しない。
* AOT は既定の正確なランタイム関数へフォールバックする。
* `src/jit/codegen.cpp`、RT、IR、renderer の他エージェント管理差分は変更しない。共有作業ツリーは未commit/未push。
