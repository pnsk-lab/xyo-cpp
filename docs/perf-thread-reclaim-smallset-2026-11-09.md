# PERF-001: Thread回収の小集合最適化 — 2026-11-09

## 対象

ScratchBench全47項目で既存5競合の最良記録を超えることが最終目標。本稿では `sched_clonechurn` を中心に `Runtime::reclaimThreads` の負荷を測定した。スクラッチベンチのプロジェクトは変更していない。

### 原因調査

`Runtime::reclaimThreads` は停止済みの多数のThreadから、実行スレッド、broadcast-and-waitの結合参照、待機キュー、未完了のJIT/Fiber、フレーム、停止スクリプトへの参照を辿り、未参照Threadだけを最大2,048個のプールに回収する。

一時的な `XYO_RECLAIM_DIAGNOSTIC=1` により、1回の回収時の `ownedThreads` と実際に保持すべき参照数を調査（診断コードは削除済み）。

| 元の診断プロジェクト | 回収直前の所有Thread数の代表値 | 参照が必要なThread数 |
|---|---:|---:|
| `clonechurn.sb3` | 約35,000 | 1 |
| `fanout.sb3` | 約84,000 | 1 |
| `fanwait.sb3` | 約83,000 | 102 |
| `pingpong.sb3` | 約49,000 | 1〜2 |

### 不採用となった比較

1. 既存の `XYO_THREAD_RECLAIM_STAMP=1`: 各Threadに回収世代を記録してハッシュ表の重複登録を除去。同一Releaseバイナリ・CPU14固定・24組の交互A/Bで `clonechurn` は従来1,068.15→ON1,086.73 ops/ms（ペア幾何平均+2.46%、20/24勝利）。しかし別の16組A/Bで `fanout` 約−1.70%、`fanwait` 約−2.01%、`pingpong` 約−0.79%で悪化したため、既定OFFのまま維持。詳細 `build/perf-clone-stamp-ab.json` と `build/perf-reclaim-stamp-cross-ab.json`。
2. `reusableThreads` のプール上限2,048を8,192〜65,536へ変更した試験。12周の交互5候補比較で、中央値は約+0.9%〜+1.4%程度、ペアでの勝率は9/12以下。保持メモリ増加もあるため、変更を**完全に撤回**。上限は従来の2,048個を維持。データ `build/perf-thread-pool-cap-matrix.{json,log}`。

## 保持した実験実装

`src/engine/runtime.cpp::Runtime::reclaimThreads` の重複判定に、`XYO_RECLAIM_SMALLSET=1` という実験フラグを追加。

* **1個の参照**だけ残る場合、`pending` ベクタ上で重複判定し、ハッシュ表へのノード・バケットの割り当てを省略する。
* **2個目の異なる参照**が見つかった時点で、それまでの`pending`の2ポインタを`std::unordered_set`へ移し、以降は従来と同じ重複判定に戻す。
* 最終の所有権コンパクションも、参照が1個だけのときは`pending`を参照する。ハッシュ表を必ず参照してしまう初期実装のバグで、未参照と誤判定して正常結果を生成できなかった問題を試験段階で検出し修正。解放・再利用・結合参照のテストで回帰防止する。
* `XYO_THREAD_RECLAIM_STAMP=1` の既存経路は変更せず、両方を有効化した場合は世代番号を優先。
* 既定値はまだOFF。性能・GUI parityなど検証完了後に判断。

先に7ポインタまで小ベクタを使う版を検証し、`clonechurn` は24組中20勝（ペア幾何平均+1.53%）したが、`fanwait` 16組でペア幾何平均約−0.90%、4/16勝利だった。そこで小ベクタを1要素だけに制限した。

**CPU14、同一凍結実行ファイル、24組交互A/B、元のプロジェクト:**

| 項目 | OFF中央値 | ON中央値 | ONのペア勝ち数 | ON/OFF ペア幾何平均 |
|---|---:|---:|---:|---:|
| `sched_clonechurn` | 1,069.94 ops/ms | **1,091.57 ops/ms** | **22/24** | **1.02078** |
| `sched_fanwait` | 2,335.07 ops/ms | 2,357.82 ops/ms | 14/24 | **0.99945** |

参照1個の`clonechurn`は+2.08%の再現可能な改善。参照102個の`fanwait`ではペア幾何平均はほぼ1.0で、速度改善・低下ともに確認できない。

生データ: `build/perf-reclaim-single-ab.{json,log}`。
凍結したネイティブRelease実行ファイル: `build/perf-reclaim-single-frozen-xyo`（SHA256 `15dca05ce8d7264d56a93e0a8775186da464546bf81df9c1a2c37078a6b98dc1`）。

## 正しさ

`tests/unit/test_scheduler.cpp::testThreadReclamation` に `XYO_THREAD_RECLAIM_STAMP=1` の世代番号オーバーフローの前後で参照の閉包（12個）と所有順序が保存されるテストを追加。既存テストでは以下も確認される。

* 12個の多重参照を辿り、途中でハッシュ集合へ移行する場合。
* 1個の生存Threadだけを保持する場合。
* startedThreads / compatibility frame / trash frame / wait queue / active / detached scheduler pass / JIT join ref。
* すべての参照を消した後で、完了Threadが解放され、再利用時に古いGC参照・実行状態が残らないこと。

回帰テストは7件までの小ベクタ版で `xyo-tests` **PASS（42.64秒）**。最終1要素版のテストON/OFF・元の47ケース・GUI parityの記録は、後続の結果確認後に追記する。

検証は共有ツリーから構成した独立 `build-perf-clone-single-index-20261109`、`XYO_SDL2=OFF` を使用した。試験中に別担当が投入した独立DLSS単体テストがメイン`xyo-tests`に二重リンクされる問題は、後に`tests/CMakeLists.txt`の別担当修正で解消した。他エージェントのファイル変更、コードのリセット、コミット、pushは行わない。

**最終47/47達成には至っていない。**
