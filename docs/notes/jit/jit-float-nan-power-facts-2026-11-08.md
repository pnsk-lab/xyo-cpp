# MATH-014 — 同一変数の累乗と非ゼロ定数除算のNaN判定削減

Date: 2026-11-08. Owner: agent-math-gpt6.

## 実装

src/jit/numeric_repeat_float_ssa.inc の浮動小数点数値SSAに、floatRepeatSameVarProduct を追加。

- x*x*x や (x*x)*(x*x) など、すべての乗算葉が同じSSA変数を指す場合は、入力が非NaNである限り積も非NaNと証明できる。±0、±Infinity、アンダーフロー、オーバーフローでも成立する。偶数乗なら非負も保証する。
- 分子がコンパイル時に有限非ゼロ定数と分かり、分母が非NaNと証明できる場合、除算結果は非NaN。分母±0なら±Infinity、±Infinityなら±0になる。
- 0*Infinity、0/0、Infinity/Infinity、別変数同士の乗算などには絶対に適用せず、従来のNaN判定を残す。
- MATH-013の判定を残して本追加だけ戻すには XYO_JIT_FLOAT_NAN_POWER_FACTS=0。両方の証明を無効化するには XYO_JIT_FLOAT_NAN_FACTS=0。

## 数値改善（同一バイナリのABBA比較）

ScratchBench N-body の native rep.float.head で fcmp ord が5個から4個、LLVM IR文字数が1617から1554へ減少。12組のCPU12固定ABBA/BAAB測定のうち11組が8%以内のドリフト条件を満たし、その速度比中央値は **1.00735964倍（約0.74%改善）**。残り1組はホスト速度が大きく変動したため除外。48回すべてのチェックサムは51200。

測定: build/math014-nbody-abba.json、build/math014-nbody-abba.log。IR: build/math014-ir-0.err、build/math014-ir-1.err。

## 正確性

- 新規 tests/differential/jit_float_nan_power_ssa.py。16初期値×6式形状で96独立スクリプト、各構成384観測値。warp/nonwarp、インタプリタ対 Eager OFF/Eager ON/HotSpot ON の全6比較が一致。特殊値は±0、±Infinity、NaN、±1e308、サブノーマルなど。正当な同一変数累乗のほか、x*0、0/(x*x)、x/x の負例も含む。build/math014-power-differential.log。
- 既存のMATH-013 NaN境界テスト180観測値、Float SSAテスト208観測値とrollbackの差分もPASS。build/math014-oldfacts-parity.log、build/math014-float-parity.log。
- ScratchBench parity は HotSpot/Eager/Interpreter/AOTすべて2123/2123。build/math014-parity.log。
- 別リンクの xyo-tests は終了コード0、各テスト群失敗0件（NaN codegen 113368件、scheduler 257件など）。build/math014-xyo-tests.log。
- 無改変の47項目ScratchBench性能プロジェクトはOFF/ON双方完走。全非schedulerチェックサム一致。build/math014-full-off.log、build/math014-full-on.log。別項目の連続計測値はホスト負荷で大きく変動したため、速度改善率の根拠にはしない。

## ビルド状態と注意

共有ツリーを並列編集する他エージェントの src/engine/warp_clock.cpp で、一時的にms未宣言のコンパイルエラーが発生したため、数学オブジェクトのみCMakeで再コンパイルした。既存 libxyo.a の**コピー**へ同じbasenameの codegen.cpp.o を正しく差し替え、build/math014-power-xyo を独立リンクして全検証した。共有のlibxyo.aとbuild/xyoは上書きしていない。

最初の独立リンクには誤って別basenameのcodegenオブジェクトを追加してしまったが、これは差し替え前なので初期診断値は無効。正しく差し替えた後の実行ファイル内にフラグ文字列が存在することを確認して、上記IR/速度/パリティをすべて再測定した。

他エージェントのRT、レンダラー、スケジューラ、メインcodegen編集内容は変更していない。未commit、未push。後続の共有HEADへの完全統合は独立リンクの検証と区別する。
