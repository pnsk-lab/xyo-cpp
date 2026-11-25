# Mega Scheduler β — 最後の単独ThreadをC++側で早期ディスパッチ

2026-11-14 / 実験的・既定OFF・未コミット

## 変更内容

既存の `XYO_JIT_MEGA_SINGLE_DISPATCH=1` と
`XYO_JIT_MEGA_SINGLE_SPECIALIZE=1` を前提として、
`XYO_JIT_MEGA_SINGLE_FAST_SCAN=1` を追加した。

以前の `Sequencer::tryMegaScalarBatch` は1件だけ残るたび、
8件分の配列を準備し、複数スクリプト・分岐・複数変数の
一般バッチ走査を経てからLLVMへ渡していた。

新経路は `stepping->size()==1 && start==0` で、単独Threadに
専用 `multiPcSingleEntry` が存在し、1つの有限数値加算のみと
確認できる場合に、スタック上の `MultiPcItem` 1件を構築して
LLVMエントリへ渡す。バッチ配列3種類（Thread/Variable/Itemの各8件）
を準備せず、一般バッチ候補走査もスキップする。

既存の `megaScalarBatchCalls()` / `megaScalarBatchThreads()` は
通常バッチと同じ意味を維持。新しい `megaSingleFastCalls()` に
早期経路の選択数を記録して、テストで実際に経路を通ったことを検証する。

## 安全条件

先頭の一般バッチガード（renderer、計装、`XYO_WAIT_WAKE_REGISTRY`、
並列バッチ待機、source revision等）を変更していない。

さらに早期経路の中で、次をすべて検証する。

- Threadの継続状態：RUNNING、同じsource owner、未終了、
  未kill、非クリック/モニタ、空でないStack、非OSR、
  非interpreter-started、`JitCtx/Fiber`不在、待機・join不在
- 既存コンパイル済みnative entryの適格性（AOT/WASM等を拒否）
- PCがsource metadata内にあり、初回偶数PCまたは
  範囲内のrepeat再開PC、残反復数が一貫していること
- そのsourceが1個の変数を有限値で加算する単純形式のみであること。
  分岐、2番目の書込変数、呼出しsuffixを持つ形式は従来経路へ
- `Target::variableBindingStamp` のStage所有・ローカル/Stage世代番号が
  Threadのキャッシュと一致すること。**一致確認前に保存ポインタを読まない**
- 保存済み変数がscalarであり、値がIEEE754有限数値であること

1件でも外れた場合、従来の `tryMegaScalarBatch` を呼び切る経路へ
そのまま戻し、変数キャッシュ失効処理・Scratch型変換処理に任せる。

実行中のThread同一性・Scratch側のスケジューラ順序・各passに1反復・
GCルート・broadcast joinは変更しない。

## フラグ

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_DIRECT_SCALAR=1 \
XYO_JIT_MEGA_SCALAR_BATCH=1 \
XYO_JIT_MEGA_MULTI_PC_BATCH=1 \
XYO_JIT_MEGA_SINGLE_DISPATCH=1 \
XYO_JIT_MEGA_SINGLE_SPECIALIZE=1 \
XYO_JIT_MEGA_SINGLE_FAST_SCAN=1 \
./build-scheduler-rewrite-20261111/xyo run example.sb3 --fps 0
```

受信者バッチを含む場合 `XYO_JIT_MEGA_BCAST_RECEIVERS=1` も指定。

## ペア測定

`tests/bench/algos/bench_mega_single_dispatch.py --fast-scan` を使用。
同一凍結xyoバイナリ、同一SB3、CPU16固定、Eager O1、
初期化・JITコンパイル時間込みのプロセス壁時計。
2本の緑旗スクリプト：repeat(300000){+1}とrepeat(1){+2}。
片方が終了した後、最後の1本が300,000回近く継続する。
早期経路フラグのみをON/OFFに切り替え、20組交互A/B。
すべての実行で変数結果の厳密一致を検査。

| 項目 | 早期経路OFF | 早期経路ON |
|---|---:|---:|
| 実行時間中央値 | 199.543 ms | 193.715 ms |
| ペア幾何平均速度比 | — | **1.04160倍** |
| ON勝利数 | — | **19/20組** |
| LLVM batch呼出し | 300,001 | 300,001 |
| LLVM対象Thread slot | 300,003 | 300,003 |

生データ：
`build-scheduler-rewrite-20261111/mega-multi-pc/bench-single-fast-scan-300k/results.json`

約4.16%の改善は、この単純な合成入力に限った結果である。
複数スクリプトや実プロジェクトへの一般化は未確認。
両方式で同じLLVM関数を使っており、ScratchのYield回数を削減した
という意味ではない。既定OFFを維持する。

## テスト

- Release xyo-cli / xyo-testsビルド成功。
- 集中テスト：FAST_SCAN ON **27/27 PASS**、OFF **22/22 PASS**。
- LIVEな変数mapから変数sを削除・再作成：
  新世代への初回アクセスでは早期経路を拒否し、
  従来の一般経路で安全に再束縛。次の反復から早期経路を再開することを確認。
- Fiber / native GC deferral / multifiber GC barrier smoke：**3/3 PASS**。
- `git diff --check` とbenchmark Python構文チェックPASS。
- 全体テスト：
  `build-scheduler-rewrite-20261111/mega-multi-pc/single-fast-full-on.out`、
  `single-fast-full-off.out`（最終結果は下の追記を参照）。

## 状態

新フラグは既定OFF。他エージェントによる既存未コミット作業は
reset/commit/pushせず保持。

## 最終全体回帰

同一Releaseバイナリ、Eager JIT、Mega関連フラグ固定。
FAST_SCANのみON/OFF。

- FAST_SCAN=1: exit=0、Scheduler **588/588 PASS**、その他の群も失敗ゼロ。
- FAST_SCAN=0: exit=0、Scheduler **583/583 PASS**、その他の群も失敗ゼロ。
- 比較中に実行する追加の5件は早期経路ON時だけの安全性検査。
- WAIT_WAKE_REGISTRY=1でも正常実行、v0=300000、v1=2。

最終判断：限定合成入力では4.16%高速、19/20組で勝利。
他の多数のソース形式、実作品やGUIでの再現は未確認。
実験フラグの既定値はOFFに保つ。
