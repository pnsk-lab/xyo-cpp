# PROF-008: CFG型解析のコンパイルコスト（2026-11-09）

## 実装

`XYO_JIT_CFG_TYPE_STATS=1`でCFG型解析に限定したオプション計測を追加。`src/analysis/cfg_type_hint_telemetry.hpp`と`src/jit/codegen.cpp`の局所的なフックを使用する。カウンターは候補ループ数（eligible）、収益性による省略（skipped）、解析実行（analyses）、打ち切り（truncated）、数値影への昇格（promoted）、解析ノード（nodes）および`analyzeTypeFlow`呼び出し時間（analysis_ms）。計測OFF時は時計を読まない。CFGヒントOFF時は統計オブジェクトが構築されず、行が出ないことがある。

出力例：

```text
[jit-cfg-type] eligible=8 skipped=4 analyses=4 truncated=0 promoted=2 nodes=112 analysis_ms=0.337
```

この時間には収益性の事前判定、LLVM IR生成・最適化、機械語生成は含まれない。

## 全47項目の診断

同じRelease LLVMバイナリ`board/.prof-008-cmake/xyo`（SHA256 `fa0dc7189313881d46a0c975ab3e50878d80e1253f7cc0edee14bae6275be44c`）、CPU14固定、HotSpot JIT、`XYO_JIT_CFG_TYPE_STATS=1`と`XYO_JIT_COMPILE_STATS=1`で実測。順序はOFF、全適用、選択的、選択的、全適用、OFF。

全6回とも終了コード0、`DONE`、47結果行、決定論的チェックサム一致。

| モード | eligible | skipped | analyses | nodes | promoted |
| --- | ---: | ---: | ---: | ---: | ---: |
| 全適用 | 8 | 0 | 8 | 176 | 2 |
| 選択的 | 8 | 4 | 4 | 112 | 2 |

CFG解析のみの時間：全適用0.481/0.328ms、選択的0.448/0.337ms（各2回）。解析回数は半減したが、解析時間はサブミリ秒で有意な減少は確認できない。

JIT同期コンパイル合計：OFFは389.186/582.682ms、全適用470.359/387.872ms、選択的653.086/448.001ms。順序・環境の変動が数百msあり、解析省略によるコンパイル速度向上は測定できていない。診断ONの数値を通常時のスループットスコアと見なさない。

全適用と選択的の`XYO_JIT_VERBOSE=1`比較では、昇格対象は**両方とも`k_n`と`k_x`**。選択的フィルターが省いた4解析は、今回のベンチマークで昇格を生んでいなかった。機械語単位の同一性までは未証明だが、同じ最適化選択をしているため、前回の47項目にわたる一様な速度改善は測定ノイズの疑いが強い。

生データ：`board/prof-008/full/results.json`と各ログ、`board/prof-008/verbose-compare.json`。

## 個別ケース

`board/prof-008/isolated/results.json`に各方式2回、6ケースの結果を保存。`math_mandel`は両方式とも1解析・22ノード・1昇格、`str_cast`は全適用が1解析・8ノード・昇格0（約0.019ms）、選択的は省略。`app_ast`、`algo_ack`、`loops_repeat`、`sched_clonechurn`は、CFG解析の対象なし。したがってこれらの実行性能差を型解析省略の効果と解釈しない。

## 検証

- `tests/unit/jit_cfg_hint_telemetry_smoke.cpp`はGCC `-std=c++20 -Wall -Wextra -Werror`でコンパイル成功。計測ON/OFFそれぞれ正常終了。
- `tests/differential/jit_cfg_type_hints_selective.py`：12/12成功（warp/non-warp、eager/hotspot、Number/String混合）。
- Bunテスト：`tools/jit-cfg-selective-ab.test.ts`と`tools/jit-cfg-type-ab.test.ts`、6/6成功。
- Release LLVM全ビルド、`git diff --check`成功。ScratchBenchの47件完走は正式な全Parityテストとは別。

DLSS並行作業の未宣言型のため、専用の`board/.prof-008-cmake/build.ninja`の`sdl_renderer.cpp.o`だけに既存ヘッダーの`-include`を与えた。レンダラソースやOSRソースは変更していない。

## 再現

```sh
XYO_JIT_MODE=hotspot XYO_JIT_CFG_TYPE_HINTS=1 \
XYO_JIT_CFG_TYPE_HINTS_SELECTIVE=1 XYO_JIT_CFG_TYPE_STATS=1 \
XYO_JIT_COMPILE_STATS=1 board/.prof-008-cmake/xyo run \
  scratch-bench/.cache/build/performance.sb3 \
  --print-list output --fps 30 2>board/prof-008/repro.log
grep -E '^\[jit-(cfg-type|compile)\]' board/prof-008/repro.log
```

## 判断

選択的CFGヒントは実験的フラグのまま維持。解析時間の削減はわずかで、既定ONを正当化しない。今後は型解析の再利用キャッシュを安易に追加するより、LLVMの中間最適化や機械語生成などJIT全体で支配的な工程を別に計測してから収益性を判断する。変更は未コミット・未push。
