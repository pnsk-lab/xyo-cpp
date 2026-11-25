# Mega Scheduler β — broadcast-and-wait受信側の共有LLVM switch(pc)

日付: 2026-11-13
状態: opt-in / 既定OFF / 未コミット

## 変更

`XYO_JIT_MEGA_BATCH_YIELD=1` と `XYO_JIT_MEGA_BCAST_RECEIVERS=1` を
両方指定した場合だけ、`Runtime::startBroadcastAndWait()` が返した
実際の受信Thread集合のうち、同一Targetの複数の適格な受信ハットを
共有LLVM関数へ組み込む。

- `src/jit/mega_yield_source.hpp`: `event_whenbroadcastreceived`も
  source-certified（ハットの一致・受信Thread開始自体はRuntimeが管理）
- `src/engine/runtime.cpp`: 実際に受信者が起動・再起動された後でだけ
  `JitEngine::installMegaBroadcastReceivers`を呼ぶ。返すjoin vectorは変更しない
- `src/jit/jit.hpp`, `src/jit/jit.cpp`: Target別の適格性判定、2〜8 Thread毎の
  バッチ作成、既存LLVMコードキャッシュの利用。ソースが同一revisionで受信hat順序も
  同じ場合は直接キャッシュ命中し、2回目以降のAST再解析・ベクトル生成を省く
- `src/jit/backend_llvm.cpp`: 前回の共有 `switch(pc)` / Thread所有PC、
  repeat残数、戻り先スタック、broadcast joinをそのまま再利用

この経路は**送信者と受信者を1つのLLVM関数に統合しない**。
送信者がどのThreadを待機するかの正確なidentityは、従来どおりRuntimeが取得・保持する。
受信Threadの選択と継続実行もC++ Sequencerが担当する。
受信側だけが同じLLVM関数を共有し、C++スタック/Fiberは継続を保持しない。

### 安全性

- 既定OFF。eager LLVM JIT、headless、無計装、未開始のcompiled receiver限定
- Source revisionとTarget、Thread identity、PC初期状態を検査
- JitCtx/Fiber/OSR/実行開始済みThreadは拒否
- ネストした手続きは最大4階層、引数なし・非warp・非再帰の静的な呼び出し列
- 受信側の未対応命令は通常JITへ。送信者のjoined Thread*を置き換えない
- キャッシュ命中時も新しいThreadを検証し、ソースrevision変化は拒否
- 配送順序・送信者join・ソース編集・変数binding stampの既存安全処理は維持

## 検証

`testMegaBroadcastReceiverFusion`（tests/unit/test_scheduler.cpp）:
- 緑旗の送信者→実際に起動する4本の受信hat
- 3本の受信hatが同じLLVM関数に統合、1本の2命令受信hatは通常経路
- ThreadごとにPC=0,2,4。1回のstepでrepeat本体を1回だけ実行
- 受信者が残っている間は送信者の後続命令が動かない
- 最終変数値131、緑旗再実行後262
- 2回目はソースrevision一致のORC関数キャッシュを再利用
- 対象受信者はnative Fiberなし、全体完了後にリークなし

新規機能ONでxyo-tests全体PASS:
`scheduler: 489 checks, 0 failures`。
ログ例:
```text
[mega-yield] slots=3 definitions=1 states=6
[mega-receivers] installed=3 ... cache=miss
[mega-receivers] installed=3 ... cache=hit
```

## 性能測定

CPU16固定、同一バイナリとSB3、交互6組、eager LLVM O1、
`XYO_YIELD_QMODE=off`、プロセス全体の壁時計時間
（起動・JITコンパイルを含む）。Stageの全数値変数を照合。
送信者が `repeat(N) { broadcast PING and wait }` を実行し、
同じメッセージの受信hat3本が各2,3,4回の定数加算を行う。

| 送信rounds | OFF中央値 | ON中央値 | 対応ペア速度比 OFF/ON | ON勝利 |
|---|---:|---:|---:|---:|
| 250（初回実装）| 91.697ms | 94.704ms | 0.9547x | 0/6 |
| 250（直接キャッシュ命中追加） | 90.949ms | 95.034ms | 0.9504x | 0/6 |
| 2000（直接キャッシュ命中）| 136.561ms | 149.334ms | 0.9285x | 1/6 |

元データ:
- `build-scheduler-rewrite-20261111/mega-receivers/bench-250/results.json`
- `build-scheduler-rewrite-20261111/mega-receivers/bench-250-fastcache/results.json`
- `build-scheduler-rewrite-20261111/mega-receivers/bench-2000-fastcache/results.json`

**今回のソース限定実装では高速化が確認できていない**。
通常JITは変数加算をインライン化できる一方、
Mega-Yieldは`xyo_var_change`ヘルパー呼出しが残り、
再開時にThread状態検証を反復する。この差は回帰の候補だが、
プロファイル分析によって寄与率はまだ確定していない。
キャッシュ命中処理の削減だけでは性能改善しなかった。

したがって、受信側のbeta flagは既定ONにしない。
一般ScratchBenchの`sched_pingpong`, `sched_fanwait`の改善は未確認。
次の優先は数値変数のLLVM直接更新への移植と、
C++でThreadを選び直す回数の削減。

実験用fixture:
`tests/bench/algos/build_mega_receiver_fixture.py`
測定器:
`tests/bench/algos/bench_mega_receiver.py`

共有作業ツリーは未コミットのまま。他エージェントの変更は保持。

## 継続調査 — clone横断の受信者統合

ScratchBenchの実際の`sched_fanwait`は、100体以上のWorkerクローンが
`when broadcast sc_fan { sc_hits += 1 }`を実行する。各クローンの
Targetは異なるが同じBlocksを共有するため、Targetごとに分ける初期版では
単独受信者となり1本も統合できなかった。

改修:
- `mega_yield::Source::Kind::Scalar`（単発の変数定数加算）を追加
- 受信者のグループキーをTargetからBlocks所有者に変更
- コンパイル時の変数ID検査は各ThreadのTargetを使う
- LLVMは`JitCtx::target`からスカラーを解決し、クローンのローカル変数を混同しない
- 同一Blocks・同一ハットの大量クローンを8個単位でキャッシュ再利用し、
  2回目以降の受信者ごとのAST解析とハッシュグループ生成を省略

確認した生成ログ:
```text
[mega-yield] slots=8 definitions=1 states=16
[mega-receivers] installed=8 ... cache=miss
[mega-receivers] installed=8 ... cache=hit
```

`sched_fanwait`で実際に生成コードが適用されたことを確認。
C++スケジューラのThread巡回自体はまだ残る。

### ScratchBench実測値

実際の`performance.sb3`から該当`sched_fanwait`以外のスイート呼び出しを除外し、
ベンチマーク本体のScratchブロックは未変更。CPU16固定、eager JIT、
同一バイナリ、交互A/B。数値は**スループットで高いほど良い**。
診断トレースは性能を大きく歪めるため、下表では無効。

| 条件 | OFF中央値 | ON中央値 | ON/OFFの対応ペア幾何平均 | ON勝利 |
|---|---:|---:|---:|---:|
| 初期clone横断版（4組） | 815.008 | 608.756 | 0.7467x | 0/4 |
| 同一hat群の高速キャッシュ追加後（4組） | 866.465 | 804.486 | 0.9292x | 0/4 |

元データ:
- `build-scheduler-rewrite-20261111/mega-receivers/sched-clone-scalar-no-trace/`
- `build-scheduler-rewrite-20261111/mega-receivers/sched-clone-fast-no-trace/`

この段階で、**実ScratchBenchの動的受信側は融合できたが、約7%の回帰がある**。
診断ログONではさらに大きく減速した。したがってβの既定は引き続きOFF。
スレッド選択のC++→LLVM統合と、変数更新をLLVMの直接更新経路へ載せることが
実際の高速化に必要。イベント受信者が多い場合は現行の単純な共有LLVM
switchだけで十分な利益を得られない。

### 安全回帰の拡張

`testMegaBroadcastReceiverFusion` は受信者5本
（数値repeat3本・scalar1命令2本）と、通常経路へのフォールバック1本が
同時に存在する状態で送信者joinと2回目の実行を検証する。

`testMegaBroadcastCloneSourceSharing` は同じBlocksの元Sprite＋3クローン、
別々のローカル変数`v`への書込みを使用し、異なるTarget間で同じネイティブ
関数を使っても変数値が混ざらないことを検査する。

## 最終回帰テスト

- `XYO_JIT_MEGA_BATCH_YIELD=1 XYO_JIT_MEGA_BCAST_RECEIVERS=1`
  : xyo-tests **PASS, Scheduler 502 checks / 0 failures**
- `testMegaBroadcastCloneSourceSharing`:
  元Sprite＋3クローン（別Targetで同Blocks）の4本から
  `[mega-yield] slots=4 definitions=1 states=8`、
  受信者が`cache=miss`→`cache=hit`で再利用されることを検証。
  各クローンのローカル変数`v`は1回目1、2回目2で完全一致。
- `testMegaBroadcastReceiverFusion`:
  反復3本＋単発2本を1共有LLVM関数へ、非対応1本は通常経路。
  合計148→296、送信後続処理は全受信者の終了まで保留。
- `git diff --check`の結果は作業ツリー全体で別途検証。
