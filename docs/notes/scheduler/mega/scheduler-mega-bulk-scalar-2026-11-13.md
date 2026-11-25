# Mega Scheduler β — 連続する受信スレッドのLLVM一括実行

日付: 2026-11-13
状態: opt-in、既定OFF、未コミット

## 背景

前段の`XYO_JIT_MEGA_DIRECT_SCALAR=1`では各受信Threadの定数加算を
LLVM直接storeに移したものの、まだC++ Sequencerの
`runSchedulerPass → dispatchReadyThread → stepThread → jitStepThread`
を1スレッドずつ通過していた。

今回の実装は、安全性を静的・動的に限定できる受信Threadを、
最大8本ずつ**1回のLLVM関数呼び出し**にまとめる。
Threadそのものや`broadcast and wait`のjoin identityは統合しない。

## 有効化

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_BCAST_RECEIVERS=1 \
XYO_JIT_MEGA_DIRECT_SCALAR=1 \
XYO_JIT_MEGA_SCALAR_BATCH=1 \
./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 30
```

任意の終了時集計は`XYO_JIT_MEGA_BULK_STATS=1`。
実行したRuntimeのみ`[mega-bulk] calls=N threads=M`をstderrに出力。

## 変更の概要

- `src/jit/backend_llvm.cpp`:
  バッチフラグONのときだけ、同じ定数`delta`を書き込む、手続き戻りsuffixを持たない
  `Source::Kind::Scalar`だけで構成されたバッチについて、
  メイン`switch(pc)`とは別の`symbol_scalar_batch` LLVM関数を作成。
  `Variable**`と要素数を受け取り、各要素の`Variable::value`を
  読み込み、浮動小数点加算を1回行い、順に書き戻す。
  LLVMの既存O1最適化を適用し、Scratch側への呼出しはしない。
- `src/jit/jit.hpp`:
  `MegaDispatchCode::scalarBatchEntry`を追加。利用できないバッチではnull。
- `src/engine/sequencer.hpp/.cpp`:
  連続する同じORCコード所有者の受信Threadを最大8本まで検査し、
  条件が成立する2本以上の連続区間をまとめて実行する。
  通常なら呼び出される`dispatchReadyThread`等を省略。
  LLVM終了後に各元Threadを個別に`retireThread`し、
  終了コンパクション・実際のThread順序を変更しない。
  呼出回数と処理Thread数を集計可能。
- `tests/unit/test_scheduler.cpp`:
  4クローンの1回目と2回目がそれぞれLLVM一括処理になること、
  文字列/真偽値への変更時は通常経路へ戻ること、
  数値に戻れば再び一括化されること、
  途中で変数マップのNodeを削除・再作成しても値とTarget間分離を保つことを検査。

## 一括実行の保守的な条件

- `XYO_JIT_MEGA_SCALAR_BATCH=1`、前段の3フラグも有効
- headless、ブロック数計測OFF、イベント全体再スケジューリングOFF、
  `XYO_WAIT_WAKE_REGISTRY=1`では必ず拒否
- 実際に依存待機を登録したThreadやPromise待機、
  過去のインタプリタ実行、Fiber/JitCtx保有、OSR中は拒否
- 先行する並列実行キューが空で、今回のスロットは非sparse巡回
- 同じ`MegaDispatchCode`、現在のBlocks revision一致
- 同じ定数加算1回のみ（手続きsuffixなし・broadcastなし・yieldなし）
- 存在するスカラーVariableを各ThreadのTargetから解決し、
  現在値は有限なIEEE754数値、差分`delta`も有限
- 事前検証からLLVMの書込みまでにhostコールバックが無い。
  数値の加算順を維持し、結合・並べ替えない

非適格Threadは**従来の1本ずつのJIT処理に戻す**。
隔離されたイベント待機機能は標準で有効だが、現在待機していない
Threadなら一括経路を適用できる。

この実験は任意のScratch制御フローやイベント受信者をまとめる一般解ではない。
Fiberの汎用置換、複数のScratch処理を同じPCで実行する方式、
任意の文字列/リスト/GC操作を許す方式には拡張していない。

## 検証

- `XYO_JIT_MEGA_SCALAR_BATCH=1`有効でxyo-tests
  `scheduler 514/514` PASS、全テストスイートPASS
- 1つのRuntimeで、4クローン×4回=16 Threadを4回のLLVM呼出しで処理
- ローカル変数の型変化、数値復帰、削除と同じIDでの再作成もPASS
- `XYO_WAIT_WAKE_REGISTRY=1`でバッチ関数を生成せず、通常経路になる。
  全体テストは**scheduler 509/509 PASS**、bulk呼出しゼロを確認

## ScratchBench `sched_fanwait`

計測はCPU16固定、同一バイナリ・SB3、交互A/B、8組。
スループット（大きいほど良い）。診断トレースはOFF。

| 比較 | OFF中央値 | ON中央値 | ON/OFF対応ペア幾何平均 | ON勝利 |
|---|---:|---:|---:|---:|
| Mega直接更新に対しLLVM一括を追加 | 815.278 | 912.0075 | **1.1047x** | 7/8 |
| 一括ORC生成をopt-in限定した最終ビルド | 867.8465 | 964.808 | **1.1021x** | 8/8 |
| 通常JITに対しMega直接更新＋一括 | 664.8435 | 968.5445 | **1.4490x** | 8/8 |

2行目は`sched_pingpong`も同じプロジェクトで実行したペア。
`sched_pingpong`はON/OFF=**1.0009x**、ほぼ不変（8組中4組勝利）。
2つの測定ケースの生スループットを無条件に混ぜず、
必ず同じrunの交互測定で比較する。

一括ONの測定ログでは、約5.2万〜5.8万回のbulk呼出しで
約40万〜45万Threadを処理したことを確認した。
これは全受信Threadが最適化対象という意味ではなく、非適格分は通常経路に残る。

測定元データ:
- `build-scheduler-rewrite-20261111/mega-bulk/sched-fanwait-bulk-ab/results.json`
- `build-scheduler-rewrite-20261111/mega-bulk/sched-fanwait-bulk-final/results.json`
- `build-scheduler-rewrite-20261111/mega-bulk/sched-normal-vs-bulk-ab/results.json`

## 未解決

ほかの`sched_*`ベンチ・複雑な実プロジェクト・GUIでの一般改善は未確認。
待機通知を維持した状態での一括化や、文字列を含む汎用スクリプトの
バッチ実行は未実装。途中のイベント・時計読み・GC・外部コールバックを
含む実行は従来JIT経路へ戻す。

既定はOFF。他エージェントの変更はリセット・コミットしていない。

## 最終回帰の追加

- 4クローンの最初の起動は**ちょうど1回**のLLVM batch callで4 Threadを処理
- 最終ソースでの全体テストはscheduler **514/514 PASS**、bulk統計`calls=4 threads=16`
- 同じ受信ハット再起動時も、ちょうど1 callで4 Thread
- 数値文字列と真偽値の混在時はbatch callは増えず、従来ヘルパーを使う
- 全変数が数値へ戻るとbatchは再開する
- 変数削除・再作成後も現TargetからVariableを再解決し、値が正しく更新される
- 一括フラグ未指定では追加のORC batch LLVM関数を生成しない
