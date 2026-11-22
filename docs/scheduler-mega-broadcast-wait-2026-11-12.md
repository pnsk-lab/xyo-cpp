# Mega Scheduler β — broadcast and wait のスタックレス継続

2026-11-12 / xyo-cpp / **既定OFF、試験機能、未コミット**

## 実装

環境フラグ: XYO_JIT_MODE=eager XYO_JIT_MEGA_BATCH_YIELD=1
診断: XYO_JIT_MEGA_BATCH_TRACE=1
送信者joinの詳細診断: XYO_JIT_MEGA_BCAST_TRACE=1

共有LLVM実行関数のABIは引き続き

    void dispatch(JitCtx*, uint32_t* pc, uint64_t* remaining)

であり、複数のScratch緑旗スクリプトを同じLLVM Function内の
switch(pc)へ統合する。各Threadは独立した実行位置を保持する。

今回追加したソース認証済みパターン：

1. 緑旗 → 固定ASCIIメッセージのbroadcast and wait → 数値変数の定数加算
2. 緑旗 → repeat(非負の整数定数) {
       固定ASCIIメッセージのbroadcast and wait
   } → 数値変数の定数加算

以前の「緑旗 → repeat(定数) { 数値変数の定数加算 }」とも
同じLLVM関数内に共存できる。

### Thread-owned join

元のxyo_broadcast_wait_v は Fiber のC++スタックに
std::vector<Thread*> started を保持し、yield後も同じスタックに
戻ってから受信者の終了を確認していた。

新経路は、送信の初回PCから一度だけ
Runtime::startBroadcastAndWait(固定メッセージ) を呼び、
受信Threadのidentity一覧を Thread::megaJoinRefs に保存する。
さらに Thread::megaBroadcastPending で「まだ起動していない」と
「受信者を起動し終えて待機している」を区別する。

待機中は既存のRuntime::containsThread、STATUS_PROMISE_WAIT、
STATUS_YIELD_TICK、STATUS_YIELD に合わせて判定する。
全受信者が終了・離脱してから初めて同じLLVM関数の再開PCに入り、
後続の変数加算か、repeat残数の減算を行う。

繰り返しbroadcastの場合、1つのjoin終了後にremainingを減らし、
残数があれば同じ奇数の再開PCのままC++スケジューラへ戻る。
次回のスケジューラ選択時にだけ新しい受信者群を起動する。
残数が0になった時点で変数加算を1回実行し終了PCへ移動する。
0回repeatは受信者を一切起動せず、数値変数の末尾処理だけを実行。

ブロードキャスト受信Threadが終了後に再起動・回収されても
待機中のidentityがダングリングポインタにならないよう、
Runtime::reclaimThreads の参照保持閉包に Thread::megaJoinRefs を
接続した。終了・中止時には joinRefs と
JitCtx::joinRefs の対応を解除する。

送信側のNative Fiber確保は不要。受信側が従来Fiberを必要とする
スクリプトであれば、受信側にはFiberが残る。

### 適格性・フォールバック

厳密な解析対象は緑旗の2〜8個のスクリプト、eager LLVM JIT、
headless、固定ASCIIの event_broadcast_menu リテラル、
有限数値定数、明示された反復上限のみ。

変数名が動的なreporter、Unicodeメッセージ、任意の条件分岐、
引数付き自作ブロック、再帰、warp、AOT/WASM、複雑な副作用は
新β経路に変換せず、通常JIT/インタプリタに戻す。

ソースrevisionや変数binding generationが変化した場合は、
変数キャッシュ再バインドまたは保守的なThread終了を選択し、
実行済みScratch効果を最初から再生しない。
未実行時のソース変更では既存インタプリタからやり直す。

今回のLLVM関数は、送信者ごとのScriptFn呼出しではなく、
各senderの初回PCと再開PCを1つのswitchへ統合する。
ただし **複数Threadの選択と順序を制御するのは依然C++ Sequencer**。
プロジェクト全体を1回のLLVM呼出しで巡回する機能ではない。

## 正しさの検証結果

生成器:
- tests/bench/algos/build_mega_broadcast_fixture.py
- tests/bench/algos/build_mega_broadcast_repeat_fixture.py

単体テスト:
- testMegaBroadcastWaitContinuation
- testMegaBroadcastRepeatContinuation

具体的なSB3：
- 2メッセージを1回ずつ送信し、各受信者が3・2回反復
  → 受信側3・4、送信後加算10・20（OFF/ON一致）
- 受信者が1つだけ、0個、2個以上ある場合も一致
- 同一受信者が2つの送信で再起動する場合、旧identityを保持して
  重複効果・不正な参照なく一致
- 受信者が27・33回反復する場合、受信側27・66で一致
- repeat(send/broadcast and wait)を3・2回実行、受信側が2・3回反復
  → 受信側6・12、送信後加算10・20（OFF/ON一致）
- repeat送信0・1、8・9回、受信者なしも一致
- 数値repeatとbroadcast waitを1つのLLVM関数に混在させても一致
- 1回送信と繰り返し送信を混在させても一致
- 動的メッセージとUnicodeメッセージはbeta非適格となり、
  通常JITへフォールバック。OFF/ONで最終状態一致
- 緑旗の2回目は元のソースとPC順序が同じ限り共有LLVM関数を再利用
- 受信者待機中は送信者のFiberはNULL、後続の数値変数も不変
- 中断中の変数再作成やソースrevision検証は既存Mega-Yield unit
  テストで引き続き検証

初期のrepeat実装では、終了時に残反復数がuint64の最大値へ
アンダーフローすることを実テストで再現し、LLVM終端分岐を修正。
また最初の1回送信では再開後のdeltaの保存漏れを発見して修正。
修正後の全条件がPASSするまで性能評価に進まなかった。

最終xyo-tests:
- 新βをON: PASS 47.35秒（1回送信段階）
- repeat対応後: PASS 46.06秒
- 3つのMega機能をすべてON: PASS 46.09秒
- 3つともOFF: PASS 47.55秒

これらは全て同じビルドディレクトリの通常CTest。

## 交互A/B性能と起動の費用

CPU16固定、同一凍結バイナリ、同一SB3、eager JIT O1、
XYO_YIELD_QMODE=off、8組交互ON/OFF。
全ペアで受信側・送信側のScratch変数を検算。
計測値は起動・追加コンパイルを含むプロセス総実行時間。

| 条件 | OFF中央値 | ON中央値 | 対応ペア速度比 | ON勝利 |
| --- | ---: | ---: | ---: | ---: |
| 1回送信×2、受信側3000/4500回反復 | 77.339ms | 76.916ms | 0.9769x | 3/8 |
| 150/200回送信、受信側2/3回反復 | 70.968ms | 73.900ms | 0.9478x | 1/8 |

**この段階でbroadcast and waitは高速化できていない**。
約5%の冷間総実行時間回帰がある。既定ONにしない。

XYO_STARTUP_TIMING=1の4組で、baseline緑旗準備が約13.7〜14.0ms、
新β経路が約16.1〜17.8ms。
元の個別JITスクリプトを作った後、追加のLLVM関数をコンパイルする
約2〜3msの費用が目立つ。
今後はバッチ対象の個別JITを生成しない設計か、
通常起動を阻害しない遅延コンパイル/共有キャッシュを検討する。

生データ：
- build-scheduler-rewrite-20261111/mega-bcast/ab-long/results.json
- build-scheduler-rewrite-20261111/mega-bcast/ab-repeat/results.json
- build-scheduler-rewrite-20261111/mega-bcast/timing-*-*.err

## 次の改善

1. 二重コンパイル（個別ScriptFn+共有switch）の排除。
2. 受信者待機の再走査コスト削減、待機通知を再利用。
3. 完全なユーザー空間スレッドPC巡回をLLVM関数側へ移す。
4. ScratchBenchの実際のsched_pingpong/fanwaitには動的条件・
   自作ブロックを含むことがあるため、ソース証明を段階的に拡張。
5. Parity 100%とリスタート、ネストしたbroadcast join、GC等の
   安全性が揃うまでβの既定OFFを維持する。

変更はすべて未コミット。他エージェントの作業はリセットしない。

## ScratchBench sched_* 本体への適用範囲

scratch-bench/.cache/build/performance.sb3を実際に検査：
- 4ターゲット、2,310ブロック
- event_whenflagclicked: 1件
- event_broadcastandwait: 5件
- event_whenbroadcastreceived: 7件
- control_repeat: 119件
- procedures_call: 239件
- procedures_definition: 75件

新βフラグをONにしたeager JITで実際に8個のsched_*結果行を
出力できたが、[mega-yield]コンパイル件数は **0**。
したがって **この実装はScratchBench sched_*を高速化していない**。
緑旗1本のみ＋自作ブロック・動的イベントを含むソース形が現行の
単純2〜8スクリプト証明と異なることが主因。
次に必要なのはイベント受信hatや自作ブロック内をまたぐ
継続PCの明示ABIと、送信/受信者joinの正確な順序証明。
