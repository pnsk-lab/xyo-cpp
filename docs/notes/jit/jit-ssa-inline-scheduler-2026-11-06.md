# xyo-cpp: LLVM IR 統合型SSAチャンクスケジューラ（2026-11-06）

## 目的

通常のScratch命令を実行するループ全体をC++スケジューラへ移すのではなく、**副作用がないと解析で証明されたSSAループの計算区間をLLVM内で処理し、実際のyield境界のみ状態を同期する**。既存のLLVM-O3 `ScalarEvolution` がSSAの加算ループを最適化する機会を維持する。

## 実装

`src/jit/codegen.cpp` の `emitWarpDeadlineCheck(chkBB,done)` に既存の共有時計・締切のatomic load、時計が存在しない場合のフォールバック、実際のFiber切り替えを共通化した。元の `emitLoopYield()` から呼び出し、意味を変更しない。

新しい `emitSsaChunkScheduler(ticks,chunk,toPoll,preferDirect)` は、SSAチャンクの実行が完了した時点でカウンタを直接 `ticks+chunk` に更新する。確認点に到達したときだけ `emitWarpDeadlineCheck` へ進む。これにより、専用経路ではスケジューラのwarpモード再判定、カウンタの `chunk-1` 書き込み→1加算、`n&63` 比較を省略する。

- `repeat` の単一整数増分（`emitRepeatAffineChunk`）は既定で有効。
- `repeat` の複数変数コピー、`for each` の加算、`while/repeat until` の帰納変数は、互換性検証済みだが現状の性能を守るため既定では従来のIR生成を使用。
- `XYO_JIT_SSA_SCHEDULER=1` で4経路を強制有効化、`=0` ですべて旧経路。未指定なら上記の自動選択。
- これはC++のスケジューラ全体・Fiber切り替えをLLVMへ移植したものではない。非warpと型未証明のループは引き続き旧経路へフォールバックする。

## 固定バイナリのA/B測定（強制OFF対強制ON）

同一実行ファイル、CPU12固定、ScratchBenchから取得したSB3、各12ペア、HotSpot、チェックサム一致。ON/OFFはJITコード生成時の環境変数のみ切り替えた。

| 作品 | ON/OFF 速度比（中央値） |
| --- | ---: |
| `loops_repeat` | 1.0114倍 |
| `loops_foreach` | 0.9890倍 |
| `loops_nested` | 0.9949倍 |
| `vars_setchange` | 0.9744倍 |
| `loops_while` | 0.9944倍 |
| `algo_sort` | 1.0051倍 |
| `list_replace` | 0.9875倍 |
| `app_ast` | 0.9981倍 |
| `app_brainfuck` | 1.0010倍 |
| `math_compare` | 0.9987倍 |

初期版はSSAチャンクの確認点を通常ループと同じ「1/64の低頻度」とマークしていた。実際のSSAチャンクは通常、次の確認点ちょうどで終わるため、確認点を高頻度に訂正した。ただし別の8ペアの測定でも `loops_repeat` 1.0115倍以外のSSAループは横ばいまたは悪化した（`vars_setchange` 0.9633倍）。そこで構造別の保守的な自動選択を採用し、特定SB3名による分岐は実装していない。

生データ: `build/ssa-scheduler-eval/ssa-ab.json`、`weighted-ab.json`（git管理外）。これらは小さな速度差を測るもので、計測ノイズやCPU環境の影響を受ける。

## 互換性テスト

`tests/differential/jit_ssa_scheduler.py` は従来の5種類のSB3フィクスチャを再利用し、warp/非warp、eager/hotspot、共有時計ON/OFF、最適化OFF/ON/自動選択の120通りをインタプリタと比較する。型混在、0/63/64/65/129反復、非整数値、NaN、動的上限などを含む。

AOTは `repeat-warp` (24変数)、`copy-warp` (80変数)、`foreach-warp` (52変数)、`induction_extended-warp` (77変数)について、強制OFF/ON/自動選択の生成オブジェクトを実行し、すべての変数が一致。

既存の空warp `repeat` 用 `emitInlineEmptyWarpRepeat` は別の、より大幅な最適化。今回の拡張ではその動作を変更していない。

## 今後

通常のループに含まれるリストや任意のScratch命令の副作用を正しく認識するには、GC/リスト再配置、型変化、他スクリプトからの書換え、実行権の譲渡の解析を広げる必要がある。ループの観測可能な副作用を無視して、無条件にyieldやランタイム関数を削除することはしない。


## 自動選択の固定バイナリ最終測定

SHA256: 391e817e0e26b593b82f577e62bf978aff55700261dad757029b52ac662bd099。
CPU12固定、各12ペア、HotSpot、チェックサム全件一致。

| 作品 | 自動選択 / OFF （中央値） |
| --- | ---: |
| loops_repeat | **1.0094倍**（12/12改善） |
| loops_foreach | 0.9954倍 |
| loops_nested | 1.0052倍 |
| vars_setchange | 1.0035倍 |
| loops_while | 0.9990倍 |
| algo_sort | 0.9963倍 |

loops_repeat以外は計測ノイズがあり、確定的な性能改善と解釈しない。一般のループ全体がO(1)になったわけではない。生データ build/ssa-scheduler-eval/default-ab.json、測定スクリプト build/ssa-scheduler-eval/bench_default.py。


## 最終検証

- Release CLI / unit tests ビルド成功。
- CTest **26/26 PASS**。167秒のevent-driven scheduler長時間試験を含む。
- SSA用差分テストは5フィクスチャ×warp/非warp×共有時計ON/OFF×eager/hotspot×モードOFF/ON/autoの120組で全出力一致。
- AOTはrepeat/copy/foreach/induction_extendedの4作品でモードOFF/ON/autoの全変数一致（24/80/52/77変数）。
- LLVM最適化後のIRで ssa.scheduler.poll ラベルを確認。
- git diff --check PASS。
- commit/pushは実施していない。他の未コミット作業は保持。
