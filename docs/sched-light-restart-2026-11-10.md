# 短い broadcast 受信者の軽量再起動（SCHED-002）

## 範囲

短い受信者を broadcast 呼出し位置で即時実行するのではなく、既存の Thread と通常の実行待ち順を維持して初期化コストを削減する。

既存実装には shortSync の同期 native entry、JitCtx プール、初回／identity 置換再起動時の遅延 root StackFrame、到達可能性解析後の Thread ストレージプールが既にある。別の軽量 Thread 型やコードポインタキャッシュは導入しない。

## 変更

- `src/engine/runtime.cpp`: root 初期化を `initializeThreadRoot` に共通化。非yieldの native shortSync 受信者は root ID だけを保持し、StackFrame は既存のアクセス関数で必要時に生成する。従来この処理を使っていなかった fire-and-forget の in-place 再起動にも適用する。
- 未実行の短い ordinary receiver に `Thread::pendingShortBroadcast` を付ける。実行／retire／payload 解放時に失効する。同じ root ID・RUNNING・未materialize・Fiber/JitCtx無しのまま再 broadcast された場合だけ、既に pristine な状態の破棄・再初期化を省略する。受信イベントをその場で実行することはない。
- `XYO_LIGHT_BROADCAST_RESTART=0`: この省略を無効化し、in-place 再起動の root フレームを従来どおり即時確保する。既存 `XYO_LAZY_ROOT_FRAME=0` は新経路でも尊重する。
- Thread ストレージ取得を `acquireThreadStorage` に共通化。identity を置換する再起動も、到達可能性解析を通ったプールから取得できるようにする。古い Thread 自体をその場でプールへ返すことはない。新しい論理実行は destructor + construct_at によって完全に再構築する。
- `XYO_RESTART_THREAD_REUSE=1`: identity 置換再起動のプール利用を実験的に有効化する。**既定 OFF**。最初の性能比較では採用を支持する改善が無かったため、通常は従来の make_unique のまま。`XYO_THREAD_REUSE=0` は両方の起動経路でプール使用を禁止する。
- `tests/unit/test_scheduler.cpp`: 連続 pre-drain broadcast、inline 実行しないこと、root の遅延生成と materialize、join-exposed identity の置換、回収済みストレージの再起動利用、旧参照の GC 後の生存を検証。
- `tests/bench/algos/bench_scheduler_flags.py`: 同一バイナリの ON/OFF を交互測定するハーネス。既存の scheduler 分離関数を使用し、カーネル本体は変更しない。30fps / HotSpot、実行順・各値・SHA256・ペア幾何平均を保存する。

## 安全性

- `stack == [topBlock]` を維持する。空スタックを軽量エントリ表現に使わない。
- 受信者の実行順、既存スロットの位置、join の handle、active self-rebroadcast、停止と回収の参照閉包は変更しない。
- 一度でも開始／状態変更された受信者について、retireThread、releaseFinished、compatibility ExecContext の初期化は省略しない。未実行トークンは supported execution entry とスタック／reported-value 更新 API で失効する。公開メンバの任意の直接書換えを監視する仕組みではない。
- native コード・変数スロット・GC 参照を再利用する新しいキャッシュは持たない。
- JitCtx と Thread の既存フィールドのオフセットは変更しない。Thread 末尾に未実行マーカー1個を追加する。古い別ビルドの C++ オブジェクトを混在リンクせず、独立ビルドで検証する。

## 実測結果と採否

Release / LLVM / SDL無効の独立ビルド。固定した実行ファイルの SHA256 は `36c3ca3dd3d340bf8b142d1adcd87c0443041e02936d8ed163f8cc0efbdc7f74`。

### 対象経路の固定仕事量プローブ

[固定仕事量プローブ](../tests/bench/algos/bench_broadcast_restart.py) が生成する専用 Scratch プロジェクトで、受信者を12回ウォームアップした後、warp 内から短い同じ受信者に200,000回 broadcast する。計測中に受信者は実行せず、最後の待機付きイベントで1回だけ書込みが起きること（checksum=1）を毎回確認する。コンパイルは計測外。ScratchBench のスコアではない。

HotSpot / CPU8固定 / 同一バイナリ / 16交互ペア:

| 項目 | OFF | ON |
|---|---:|---:|
| 実行時間中央値 | 38.00 ms | **32.00 ms** |
| ペア速度比の幾何平均 | 1.0 | **1.2024倍** |
| ONが速かったペア | — | **16/16** |

[固定仕事量の生データ](../build-sched-light/coalesced-hotspot/results.json)。この差は未実行受信者の再初期化省略に限定した効果であり、fanout/fanwait全般が20%速くなるという意味ではない。

Eager は既存のコンパイル判定で shortSync 用プロフィール走査を省略するため、このテストには不適切だった。初期 Eager プローブは対象経路を踏まず、採否判断に使用していない。回帰テストも HotSpot でウォームアップして `shortSync && !mayYield` を明示的に確認するようにした。

### 元の ScratchBench scheduler 8項目

カーネル本体を変えず、HotSpot / 30fps / CPU8 / 各8回交互比較。[最終生データ](../build-sched-light/paired-pending/results.json)。**カテゴリ全体の改善は未確認**。fanout は8ペア中4勝、fanwait は3勝。変更と関係しないlooptick/waituntilにも大きな変動があり、集計値の上昇をこの変更の効果として主張しない。

前段の root 遅延化＋広い再起動プールだけの比較も、8項目のペア幾何平均0.99883倍で改善が無かった。[初期比較](../build-sched-light/paired/results.json)。このため `XYO_RESTART_THREAD_REUSE` は既定OFF。未実行状態を利用する狭い軽量経路のみ既定ONにする。

## 最終検証

- 完全な `xyo-tests`: **PASS 1/1、48.31秒**。[ログ](../build-sched-light-final-native-ctest.log)。全登録CTestを実行したという意味ではない。
- 同一ビルドの scheduler 単体を、通常／両最適化OFF／プールON／root遅延化OFFで実行: **各340 checks、0 failures**。
- `XYO_PAR=0 XYO_FIBER_CHAIN=0`: **332 checks、0 failures**。並列ディスパッチ専用assertionのスキップは想定どおり。
- 受信者の逆順ターゲット列挙、共有変数への書込み順、停止後の次イベント、join identity、reported値更新・materialize後の完全reset、native実行入口でのトークン失効を確認。
- `git diff --check`、追加Pythonツールの構文チェック: PASS。
- GUI parity、ASan、独立したJIT無効ビルドは今回再実行していない。

## 再現

```sh
python3 tests/bench/algos/bench_broadcast_restart.py \
  --binary build-sched-light/xyo-pending-frozen \
  --out build-sched-light/coalesced-repeat --runs 16 --cpu 8

python3 tests/bench/algos/bench_scheduler_flags.py \
  --binary build-sched-light/xyo-pending-frozen \
  --project scratch-bench/.cache/build/performance.sb3 \
  --out build-sched-light/scheduler-repeat \
  --flag XYO_LIGHT_BROADCAST_RESTART --runs 8 --cpu 8
```

次に fanout/fanwait を大きく改善するには、各イベントが実際に実行されるケースの新規Thread／変数束縛／完了処理を減らす必要がある。今回、汎用Threadを廃止する別キュー型までは導入していない。共有作業ツリーの他担当差分を保持し、commit/pushは行っていない。
