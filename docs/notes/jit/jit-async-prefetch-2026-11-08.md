# 非同期 JIT の broadcast 受信スクリプト事前コンパイル（2026-11-08）

## 背景

Linux RISC-V Real Project の RISCV.powerOn (I?) 受信スクリプトが warp 手続き 0.Reset RAM を実行する。内部には control_repeat と data_replaceitemoflist が3箇所ずつあり、既存 OSR は深い手続きスタックから移行できない。LLVM コンパイル完了後も開始済み Interpreter スレッドにコードは反映されない。

## 実装

JitEngine::prefetchWarpBroadcasts をグリーンフラグ開始時に呼ぶ。broadcast 受信ハットが warp 手続きと counted repeat＋リスト更新を含む場合だけ候補として順位付けする。静的な走査は最大256 source blockまで。対象は非同期2件／同期1件に制限。

- XYO_JIT_ASYNC_PREFETCH 未設定・0：既定 OFF。
- XYO_JIT_ASYNC_PREFETCH=1：最大2件を LLVM worker に予約。Scratch スレッドは遅延させない。
- XYO_JIT_ASYNC_PREFETCH=2：最上位の1件を開始前に同期コンパイル。Linux RISC-Vでは約123msの一時的なUI停止が発生した。プロジェクトタイマーはコンパイル完了後にリセット。
- XYO_JIT_ASYNC_PREFETCH_LOG=1：選択したブロックID・静的スコア・開始準備時間を出力。

Scratch命令はプリフェッチ中に実行しない。事前コンパイル後も既存のバージョン管理とキャッシュ失効規則を使用する。実行途中のCallFrame引継ぎや深いOSRではない。

## Linux RISC-V 実測

HotSpot async / SDL offscreen / 同一実行ファイル / 45フレーム / XYO_BLOCK_COUNT=1。

| モード | Scratch実行ブロック | 経過時間 |
|---|---:|---:|
| 0 | 223,650,405 | 22.03秒 |
| 1 | 278,696,165 | 21.82秒 |
| 2 | 669,885,770 | 9.45秒 |

モード1は I? を最優先で選択し、約232msで worker のコンパイルが完了。ただし受信スレッド起動より遅くなる場合には、開始済みInterpreterへ移行できない。

モード2は I? を開始前に約123msでコンパイルして、45フレーム中の進捗を増やした。ただし UI を止めるため既定 ON にしていない。3モードのフレーム数は同じだが実行されたScratchブロック数は異なるため、単純な「生成コードの速度比」ではない。

## 回帰テスト

- GUI Scratch Parity: モード1のHotSpotは2,123/2,123、モード2はInterpreter/HotSpot/Eagerすべて2,123/2,123。
- gui_async_prefetch_smoke.py: warp 手続き内で RAM に2,000個追加し、broadcast-and-wait で一度だけ実行、変数更新まで含めて Interpreter・モード0・1・2の全経路で一致。
- no-JIT ビルド用の空実装も追加。
- ログ: board/riscv-prefetch-*.log、board/riscv-prefetch-smoke-test.log、board/riscv-prefetch-sync-gui-parity.log。

本命の深い warp 手続き OSR は IR-001/RT-001 の共有 CallFrame・戻り先 PC・引数 GC ルート統合が必要。今回のプリフェッチは事前実行ではなくコンパイルのみで、継続状態の巻き戻しを行わない。
