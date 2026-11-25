# MATH-017 — 整数SSAの遅延ボックス化実験（2026-11-09）

## 仮説
`src/jit/numeric_repeat_ssa.inc` の `numericFromInteger` は整数の加減乗除剰余の途中結果ごとに `uitofp` と `bitcast` のIRを生成する。整数だけで続く演算ではそれらのf64影は不要であり、必要時だけ生成するとJITコンパイルコストが下がる可能性がある。

## 試作と正確性
`NativeNumericValue` の整数専用値では `bits` と `number` をnullとし、浮動小数点を必要とする式と64-tickチャンクの出口でだけ生成する試作を実装。従来方式とのA/Bフラグ `XYO_JIT_NUMERIC_REPEAT_LAZY_BOX=0` を用意した。評価用バイナリ `build/math017-lazy-xyo`。

単独 `vars_many` のLLVM `rep.numeric.head`:
- IR長 4057 → 2063 文字。
- `uitofp` 24 → 0。
- `bitcast double` 24 → 0。
- `math_arith` は元々LLVMが削減しておりIR長1298→1298、`uitofp` 2→2で変化なし。

Scratch-bench parity HotSpot/Eager/Interpreter/AOT各2123/2123。独立 `jit_numeric_repeat_u32_mod_ssa.py`、`jit_numeric_repeat_ssa.py`、`jit_numeric_until_ssa.py` は全構成でInterpreterと一致。ログ `build/math017-parity.log`、`build/math017-u32-parity.log`、`build/math017-legacy-int-parity.log`、`build/math017-until-parity.log`。

## 性能

ランタイム8組ABBA、固定CPU12、同一バイナリ：
- Arithmetic 6/8安定組、ON/OFF速度比中央値1.002715倍（ほぼ変化なし）。
- vars_many 4/8安定組、中央値1.004147倍（ほぼ変化なし）。
- Primes 3/8安定組、著しいドリフトで信頼できない。
`build/math017-run-abba.json`、`build/math017-run-abba.log`。

さらに `tools/bench_math_lazy_box_cost.py` で24個の独立warp手続き×8整数変数を生成し、同一バイナリ・CPU12・8組ABBAでプロセス全体のCPU時間・ピークRSS・出力digestを測定。CPU時間の新/旧比中央値 **1.0285573**（約2.86%悪化）、改善した組は1/8。RSS差中央値+0.003MiBで実質変化なし。全32回のdigest一致。生データ `build/math017-lazy-cost.json` と `build/math017-lazy-cost.log`。

この実験は、コンパイル単体ではなく起動と実行を含むCPU時間を評価している。ホスト負荷変動は大きいものの、メリットが観測されていないため採用しない。

## 結論
**不採用・撤回**。試作した遅延ボックス化および切替環境変数は共有 `src/jit/numeric_repeat_ssa.inc` から削除し、MATH-016の最終的なinteger SSAに戻した。 `tools/bench_math_abba.py` の追加フラグも撤回。独立の計測器 `tools/bench_math_lazy_box_cost.py` は残し、将来のコンパイル負荷調査で再利用可能とする。共有RT/IRやcodegen本体の他エージェント変更は保持。commit・pushは行わない。
