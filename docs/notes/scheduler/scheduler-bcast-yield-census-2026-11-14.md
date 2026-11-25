# xyo-cpp: broadcast-and-wait 中断回数の実測

2026-11-14 / shared dirty worktree / 未コミット

## 結論

実ScratchBenchの sched_pingpong / sched_fanwait で、ネイティブ送信側は
**broadcast and wait 1回につきFiberを正確に1回中断**していた。
2回目の再開時点では受信者が実行中のjoin集合から離れており、
追加のFiber中断は必要なかった。

これは実装に余分な再中断がないことを示す。待ち処理の検索や一時vector
操作を削ってもFiber切替回数は減らない。
中断回数を削減するには、Scratchのイベント起動順と待機境界を維持したまま、
**LLVMの明示PCによる送信側継続**（Fiber不要の状態機械）が必要。

## 計測コード

src/jit/rt.cpp に BroadcastJoinCensus を追加。
XYO_DISABLE_RT_PROF=OFF の診断用Releaseでのみコンパイルする。
通常Release（XYO_DISABLE_RT_PROF=ON）には計測コードも
文字列 [bcast-census] も存在しない。通常のThread処理には一切変更なし。

環境変数 XYO_BCAST_JOIN_CENSUS=1 を指定すると、
OS thread-localの計測器がプロセス終了時に以下をstderrへ表示する。

- calls: JITネイティブbroadcast-and-wait helperを実行した回数
- empty / single / multi: 開始したjoin receiver数が0 / 1 / 2以上の呼び出し
- join_checks: join集合を判定したパス数（**receiver個別の検査回数ではない**）
- yields: senderがxyo_yieldを呼んだ回数
- tick / active: STATUS_YIELD_TICK と STATUS_YIELD の各中断回数
- warp_busy: warpスクリプトでyieldせずにjoinを再検査した回数
- completed_0 / completed_1 / completed_many: 終了までに0 / 1 / 2回以上yieldした呼び出し数
- max_yields: 完了した1つのjoin当たり最大yield数

JITヘルパーを通過しないinterpreter区間は数えない。
マルチワーカの場合は各OS thread-localの行が出る。

## Eager JIT（プロファイル用Release）

同一xyoバイナリ、ScratchBenchの元カーネル本体を変更しない
scheduler-only SB3、30fps、XYO_PAR=0、XYO_YIELD_QMODE=off。
それぞれ単独実行。時間制限付きのベンチなので呼び出し数は
実行時間とCPU負荷によって変わる。

| 指標 | sched_pingpong | sched_fanwait | sched_restart |
| --- | ---: | ---: | ---: |
| native broadcast-and-wait calls | 441,747 | 6,971 | 6 |
| single receiver join | 441,746 | 0 | 5 |
| multi receiver join | 1 | 6,971 | 1 |
| join checks | 883,494 | 13,942 | 62 |
| sender yields | 441,747 | 6,971 | 56 |
| STATUS_YIELD (active) | 441,747 | 6,971 | 56 |
| STATUS_YIELD_TICK | 0 | 0 | 0 |
| completed_1 | 441,747 | 6,971 | 1 |
| completed_many | 0 | 0 | 5 |
| maximum yield per join | 1 | 1 | 11 |

※ sched_restartでは多数の再中断も正しく計数されており、
「1回」と決めつけるカウンタではないことを確認した。

## HotSpot JIT（同一プロファイル用Release）

| 指標 | sched_pingpong | sched_fanwait |
| --- | ---: | ---: |
| native calls | 503,485 | 9,045 |
| join checks | 1,006,970 | 18,090 |
| sender yields | 503,485 | 9,045 |
| completed_1 | 503,485 | 9,045 |
| completed_many | 0 | 0 |
| max_yields | 1 | 1 |

EagerとHotSpotの両方で「1 native join -> 1 sender yield」を確認。

ログは以下のとおり。
- build-scheduler-rewrite-20261111/bcast-census-20261114/sched_pingpong.err
- build-scheduler-rewrite-20261111/bcast-census-20261114/sched_fanwait.err
- build-scheduler-rewrite-20261111/bcast-census-20261114/sched_restart.err
- build-scheduler-rewrite-20261111/bcast-census-20261114/sched_pingpong-hotspot.err
- build-scheduler-rewrite-20261111/bcast-census-20261114/sched_fanwait-hotspot.err

## このラウンドで棄却した別経路

Runtime::step()の生存Thread配列のin-place compaction、
threadMapのidentical Thread*検証付き再構築省略を試した。
それぞれ単独の16組測定では fanwait が1.0157x（12/16勝）、
さらにstable-mapだけを追加した場合は1.0124x（13/16勝）だったが、
**両方を同時に有効にした20組A/Bでは fanwait 0.9966x（7/20勝）**。
pingpongも0.9981x（11/20勝）で改善なし。

実装2件と関連テストは安全に撤回した。
詳細とベンチ元データは scheduler-step-prep-fast-2026-11-14.md へ保存。
現在の本番Releaseに影響する変更はない。

## 今後の設計

優先すべきは、既定OFFのMega Scheduler βに既にある
Thread-owned PC / joinRefs / broadcastPending の継続表現を、
実ScratchBenchの自作ブロック内まで拡張することだ。

必要な条件は以下。

1. JITのソース改訂/手続き呼び出し経路/戻り位置を不変の情報で確認。
2. 送信時にだけRuntimeを経由して受信Thread集合を確定。
3. 送信者はFiberを持たずC++スケジューラへ1回帰るが、後続Scratch効果はまだ実行しない。
4. 受信者の実際のThread*をGC/reclamation rootとして保持する。
5. 完了が確認できた次のスケジューラ選択時にだけ再開PCへ進む。
6. 互換レポーター、引数、非静的broadcast名、ソース編集はフォールバック。
7. schedulerの公平性/順序、同一受信者restart/stopやネストjoinも差分テスト。
8. 受信者の実行コスト自体は別途最適化する。送信側Fiberだけ削っても全コストは消えない。

受信者を同一スレッド内で即時実行する強引な融合は、
Scratchのイベント実行順序を変えるため採用しない。

## 検証

本番Releaseビルド成功。プロファイル専用ビルド成功。
通常Releaseはstringsコマンドで [bcast-census] 不在を確認。
計測用Releaseでは [bcast-census] が存在し、上記の5実行で出力を確認。

最終Eager + Mega各既存オプトインONで
Scheduler 651/651 PASS、E2E 99/99 PASS、exit 0。
Fiber smoke / RT-001 GC deferred / multifiber barrier：3/3 PASS。
git diff --check PASS。変更は未コミット。
