# Mega Scheduler β — 複数PCのLLVM一括継続実行

2026-11-13 / 実験的・既定OFF・共有作業ツリー内で未コミット

## 目的と実装

従来の`XYO_JIT_MEGA_SCALAR_BATCH=1`は、同じ定数加算を
1回だけ実行するスクリプトを最大8本まとめる経路だった。
今回は、一括実行するThreadごとに独立した
`pc`と`remaining`を持つ「多重PCバッチ」を追加した。

```text
Sequencer::runSchedulerPass
  └─ tryMegaScalarBatch (同じMegaDispatchCodeの連続するThreadを検証)
      └─ LLVM multiPcBatchEntry(MultiPcItem*, count)
          ├─ for each item in original Scratch order
          ├─ switch(*item.pc)
          │   ├─ Scalar：加算1回 → pc=UINT32_MAX
          │   └─ Repeat：
          │       ├─ start：remaining=静的反復回数
          │       ├─ remaining==0：pc=UINT32_MAX
          │       └─ remaining>0：加算1回、
          │           remaining--、pc=奇数resume PC
          └─ return to C++ after each Thread has advanced <=1 turn
  └─ C++ retires finished Thread identities individually
      and counts yielded Thread identities as active for next pass
```

新しいLLVM関数は`MegaDispatchCode::MultiPcItem`
（`Variable*`, `uint32_t* pc`, `uint64_t* remaining`）の
ポインタ配列を受け取る。LLVMが直接`Thread`のC++内部構造へ
依存しないABI。反復スクリプトごとに異なる定数加算値にも対応。

既存の`scalarBatchEntry`（全ソース同一の単発加算）を優先し、
それがない場合に`multiPcBatchEntry`を使う。
多重PCコードが存在しない、または検証条件に合わないThreadは
これまでどおり標準JIT継続経路へ戻る。

## 有効化

次の5フラグを全て有効にする。

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_BCAST_RECEIVERS=1 \
XYO_JIT_MEGA_DIRECT_SCALAR=1 \
XYO_JIT_MEGA_SCALAR_BATCH=1 \
XYO_JIT_MEGA_MULTI_PC_BATCH=1 \
./build-scheduler-rewrite-20261111/xyo run example.sb3 --fps 0
```

受信者バッチのみの利用では`XYO_JIT_MEGA_BCAST_RECEIVERS`が必要。
緑旗の反復スクリプトではそのフラグは不要。

任意の実行統計：`XYO_JIT_MEGA_BULK_STATS=1`。

## 動的ガード／適用対象

- LLVM JIT eager、rendererなし、ブロック数計測なし、診断プロファイルなし
- バッチ内ソースは`Scalar`か`Repeat`のみ、
  手続き戻りsuffix、動的broadcast、Scratchコールバックなし
- バッチは同じ`MegaDispatchCode`と現在の`Blocks::revision`に属する
- ループ再開PC（奇数）はRepeatのみ許可し、残り回数の上限も確認
- 全Threadが現在実行可能、独立したVariableが見つかり、
  値は有限なIEEE754数値、Fiber/JitCtx/OSRなし、
  待機登録やjoin中ではない
- バッチ開始前の並列実行キューが空、sparse巡回とイベント駆動の
  グローバルスケジューラを使用していない
- `XYO_WAIT_WAKE_REGISTRY=1`なら多重PC関数は生成しない
- 1回のバッチ内で同じScratch Threadを2回実行しない
- 最後の残った1本のThread等、バッチが2本未満になったら通常経路
- 変数値が文字列、真偽値、NaN/非有限値、未解決なら
  その時点でバッチを切り、元の型変換・書込ヘルパーを使う

## 回帰テスト

`testMegaMultiPcBatch`：
ブロードキャスト受信者5本を共有LLVMへコンパイルし、
`repeat(3) +1`、`repeat(2) +2`、`repeat(0) +50`、
単発`+5`、`repeat(1) +3`を混在させる。

4回のScheduler pass後の総和が15で一致した。
各passの実行後に`pc`、`remaining`、
稼働中Thread数を検証した。
最後の1本の通常JITへの切戻しと、
2回目のブロードキャストで同じLLVM ORCオブジェクトの再使用もPASS。

新フラグ有効の全体テスト：
**scheduler 525 checks, 0 failures**。他のテストスイートもPASS。
PC保存後に変数を文字列100へ書き換えるテストもPASSし、通常JITへのフォールバックが既に実行した部分を重複させず、最終値104を作ることを確認。

## 測定

`tests/bench/algos/build_mega_yield_fixture.py`で
8個の独立したスクリプトを生成し、それぞれ40,000回反復。
各スクリプトは独立した数値変数へ加算(1〜8)。
最終変数はそれぞれ40000,80000,...,320000で検証。
同一SB3、同一CLIバイナリ、
CPU16固定、LLVM eager O1、6組の交互A/B。
**実行プロセス全体の壁時計時間（小さいほどよい）**。

| 指標 | 従来Mega | 多重PC一括 |
|---|---:|---:|
| 中央値 | 288.338 ms | 222.691 ms |
| OFF/ON対応ペアの幾何平均速度比 | — | **1.2976倍** |
| ON勝利 | — | 6/6 |

ONのログでは、1回のプロセスで**40,001回のLLVM bulk呼出し・
320,008回のThreadスロット実行**を確認
（最後の0回反復確認を含むので反復本体実行回数とは一致しない）。
これは同一スレッドの処理を各パスでまとめた呼出回数であり、
Scratch Threadの生成やjoinそのものを完全に排除した数字ではない。

測定の正確なログ：
`build-scheduler-rewrite-20261111/mega-multi-pc/bench-40000x8/`
新しいペア測定器：
`tests/bench/algos/bench_mega_multi_pc.py`

既存ScratchBenchの`sched_pingpong` / `sched_fanwait`は
新機能の主対象ではない（特にfanwaitは従来の単発スカラー専用バッチを優先）。
6組の交互測定で、sched_pingpongの速度比は1.0060倍（4/6勝）、sched_fanwaitは0.9795倍（4/6勝）。新フラグによる安定した改善は確認できなかったが、小標本・測定値のばらつきがあり、確定的な回帰とも断定しない。
測定データ：build-scheduler-rewrite-20261111/mega-multi-pc/sched-fanwait-no-regression/。

## 限界・今後

この経路は`switch(pc)`を含む複数のScratchスクリプトを
1回のLLVM呼出しで進めるが、分岐、動的レポータ、
文字列・リスト操作、broadcast-and-waitの本体、
一般の自作ブロックCallFrameには未対応。
これらを一括実行するには、それぞれの再開可能な状態と
GCルート、通知バリア、スケジューラ順序を別途厳密に定義する必要がある。

既定OFF。他エージェントによる未コミット変更は保持し、
ソースのreset・全体commitはしない。

## 最終ON/OFF・待機通知の全体回帰

- 複数PC ON: Scheduler **525/525 PASS**（最終テスト追加後）
- 複数PC OFF、従来scalar bulk ON: Scheduler **514/514 PASS**
- 待機通知登録ONかつ複数PC要求ON: Scheduler **509/509 PASS**、バッチ呼出しゼロ
- git diff --check: PASS
- 他エージェントの未コミット編集には触れず、既定OFF
