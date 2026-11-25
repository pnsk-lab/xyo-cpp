# PROF-006: Mandelbrot の Typed SSA キャッシュミスと CFG 型推論 A/B（2026-11-09）

## 結論

既定設定で Mandelbrot の named LLVM block `typed.ssa.convert` に多数到達する原因は、`k_x = k_z` の代入で `k_x` が非数値書き込みの可能性ありと扱われ、mixed-type の lazy numeric shadow が作られること。`XYO_JIT_CFG_TYPE_HINTS=1` による CFG analysis が当該書き込みの Number-only を証明し、`xyo-jit: cfg numeric-shadow k_x` と出力した。すると数値影が eager になり、該当の `typed.ssa.convert` BB は生成されない。

これは安全性証明に依拠した収益性の選択であり、CFG 型推論自体がランタイムの Scratch 強制変換・比較適格ガードを削除するわけではない。

## PROF-004 の Guard 分類による再現

| 実行設定 | typed.ssa.convert hits | checksum |
| --- | ---: | ---: |
| default | 23,660,717 | 1957 |
| XYO_JIT_TYPE_FLOW=0 | 0（BB 未生成） | 1957 |
| XYO_JIT_CFG_TYPE_HINTS=1 | 0（BB 未生成） | 1957 |
| XYO_JIT_LAZY_TYPED_SSA=1 | 25,392,844 | 1957 |
| XYO_JIT_LOCAL_READ_FORWARD=1 | 25,547,271 | 1957 |
| XYO_JIT_DUPLICATE_SCALAR_NUM=1 | 25,602,411 | 1957 |

時間制限付き実行なので hits は処理量によって変化する。キャッシュヒット率や Deopt の回数ではなく、名前付き LLVM BB へのネイティブ実行到達数を示す。raw: `board/prof-006/flags.json`、CFG の型昇格ログ: `board/prof-006/mandel-cfg-verbose.stderr.log`。

## Instrumentation OFF の独立 A/B

固定した同一 Release LLVM バイナリ: `board/.prof-005-cmake/xyo`、SHA256 `991126e31c98f37c84775008463db7951c9575fd1d7945c53fd5de919ae70117`。`XYO_JIT_GUARD_STATS=0`、`XYO_JIT_MODE=hotspot`、CPU14 に固定。ソース変更なし。

- Mandelbrot 24回（冒頭4回をウォームアップとして除外、各群10回）: OFF 中央値 256,676 ops/ms、ON 中央値 274,818 ops/ms（比 1.0707）。全 checksum 1957。一方この測定は変動幅が広いため一般的な +7% を断定しない。raw: `board/prof-006/mandel-clean-ab.json`。
- 4ケース独立 8回ずつ（各設定4回、順序 OFF ON ON OFF ON OFF OFF ON）: Mandelbrot 比 1.0338、loops_repeat 比 1.0000、app_ast 比 0.9871、sched_clonechurn 比 0.9868。raw: `board/prof-006/isolated-ab/results.json`。独立した loops_repeat / app_ast / sched_clonechurn に全体実行と同程度の巨大な回帰は現れない。

## 全47項目 A/B（4回）

同一 `.sb3` (`scratch-bench/.cache/build/performance.sb3`) を OFF ON ON OFF で逐次実行。各回 47/47行、DONE、exit 0。確定的な39項目は checksum 一致。残りの scheduler 8項目は実行時間依存。公式の Scratch パリティスイート実施を意味しない。

| 設定・順序 | wall（ms） |
| --- | ---: |
| OFF #1 | 34,706 |
| ON #1 | 35,016 |
| ON #2 | 35,172 |
| OFF #2 | 34,702 |

一部の中央値 ON/OFF: math_mandel 1.0379、loops_repeat 0.7643、app_ast 0.8264、sched_clonechurn 0.4146。47項目の単純・非加重幾何平均は 0.9172、11項目で増加・36項目で減少。**公式 ScratchBench の重み付き overall スコアとは異なる**。raw: `board/prof-006/full-ab/results.json`。

全体の大幅な低下は、独立実行の 4回ずつでは再現しない。原因は未確定で、外部CPU負荷、ランタイムコンパイル、実行順や前項目のGC/スケジューラ状態などの寄与を分離する必要がある。2回/群の全体試験だけで回帰を断定しないこと。

## 安全性検査

- `tests/differential/jit_cfg_type_hints.py` を固定ビルドで実行、warp / non-warp、Number-only と混合 String|Number ガードの10チェック成功。
- `tests/differential/jit_type_flow_safety.py` を固定ビルドで実行、12構成成功。Interpreter / eager / hotspot、数値文字列・空白・Boolean・color・yieldを検証。
- `bun test tools/jit-cfg-type-ab.test.ts`: 3/3 成功。
- 47項目の checksum 一致は性能スイートの結果整合であり、GUI・全パリティ項目を保証しない。

## Reproduction

```sh
bun tools/jit-cfg-type-ab.ts --bin board/.prof-005-cmake/xyo --sb3 scratch-bench/.cache/build/performance.sb3 --out board/prof-006/full-ab --pairs 1 --cpu 14
python3 tests/differential/jit_cfg_type_hints.py board/.prof-005-cmake/xyo
python3 tests/differential/jit_type_flow_safety.py board/.prof-005-cmake/xyo
```

## 方針と今後の作業

1. `XYO_JIT_CFG_TYPE_HINTS` は既定OFFを維持。Mandelbrot では小さいが反復可能な改善がある一方、全体測定に大きい不確実性が残る。
2. 実行前JITコンパイル負荷と各実行中のスループットを分離し、クリーンなホスト環境でランダム化交互順と測定を増やす。
3. 一般的な損益基準で対象の dynamic write を絞る、または CFG解析結果をコード生成単位で再利用する実験を検討する。ベンチIDや変数名のハードコードは禁止。
4. 変更するなら数値文字列、Unicode、非数値代入、scheduler yield、同名変数・別ID、GC と OSR 復帰を含むパリティ検証を必須にする。

今回、共有の `src/jit/codegen.cpp`、描画側の並行作業、ScratchBench 原本は変更していない。測定・ツールは未コミット、未push。
