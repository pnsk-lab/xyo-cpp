# Mega Scheduler β — 複数ThreadをScratchパス順で融合するRepeat Burst

2026-11-14 / 実験機能・既定OFF・未コミット。共有worktreeの他agent作業を保持。

## 背景と新設フラグ

単独Threadの `XYO_JIT_MEGA_REPEAT_BURST=1` は約300,000回の
LLVM呼び出しを約1,175回へ減らす実験機能。ただし2本以上のThreadが
稼働し続ける状況では毎パスごとにC++へ戻っていた。

新設：`XYO_JIT_MEGA_MULTI_REPEAT_BURST=1`。前提として既存の
単独Thread特化・コンパイル時source分類・Repeat Burstが有効なこと。
2〜8本のsource-certified純粋repeatをまとめる。

## 実装

- `src/jit/jit.hpp`: `MegaDispatchCode::MultiRepeatBurstEntry`。
  ABI `uint64_t (*)(MultiPcItem*, uint64_t count, uint64_t maxPasses)`。
- `src/jit/backend_llvm.cpp`: LLVM関数
  `*_multi_repeat_burst` を構築。内側は既存の
  `*_multi_pc_batch(items,count)` を1回呼び、各Threadを必ず
  スケジューラと同じ配列順に1回ずつ実行してから次パスへ移る。
  O1 LLVMパイプラインにより最終オブジェクトではbatchがインライン化される。
  共有Variable*同士のFAddをスレッドごとに並べ替えない。
  各パス終了時にすべての入力変数のIEEE754指数部を調べ、
  Inf/NaNを検出すると**そのパスを完了してから**停止する。
- `src/engine/sequencer.cpp`: `tryMegaMultiRepeatBurst()`。
  必ず `stepThreads()` の**外側の時間予算ループ**からのみ呼ぶ。
  `runSchedulerPass()` の1パス1Threadあたり1回という公開契約はそのまま。
  maxPassesは256および全Threadの`megaRemaining`の最小値。
  終了Threadが出たら通常schedulerに復帰し、retireと残りThreadの
  単独Burstへの移行を既存コードへ任せる。
- 成功時に `megaMultiBurstCalls()`、`megaMultiBurstTurns()`
  で生成呼び出し数と論理的Threadターン数を追跡。
  `XYO_JIT_MEGA_BULK_STATS=1` は
  `[mega-multi-burst] calls=... turns=...` を出力する。
- フラグOFF時には外側の静的ガードで新関数呼び出しを一切行わない。

## 安全条件

全ThreadがRUNNINGで、同じMega ORCソースコード・revisionを参照し、
LLVM native eligible、JitCtx/Fiber/OSR/Join/Waitがなく、
`megaPc`が奇数のrepeat再開PC、2以上の残反復数があり、
その残反復数がコンパイル時source上限内。

`singleFastSourceKinds[slot]==2` により単一変数・純粋repeatであることを確認。
renderer、blockCount、waitRegistry、indexedReady、profiler、
pending wait queue、parallel queue等では拒否。

保存済みの`Variable*`は、`variableBindingStamp()`の所有Target、
StageのID、Stage/localの構造世代が一致した場合に限り検査する。
値がscalar有限数値であることも確認。スクリプトの変数ポインタの
別名(alias)は許容し、**実行順序を完全保持**する。

ブロードキャスト、条件分岐、自作ブロックの未処理suffix、
型変換が必要な値、未知のsource形式は従来経路にフォールバックする。
内部パス粒度を最大256ターン分延ばすので、外部ホストからの
中間観測タイミング・時間予算の微小な超過は変わり得る。
既定OFFを維持。

## 起動方法

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_DIRECT_SCALAR=1 \
XYO_JIT_MEGA_SCALAR_BATCH=1 \
XYO_JIT_MEGA_MULTI_PC_BATCH=1 \
XYO_JIT_MEGA_SINGLE_DISPATCH=1 \
XYO_JIT_MEGA_SINGLE_SPECIALIZE=1 \
XYO_JIT_MEGA_SINGLE_FAST_SCAN=1 \
XYO_JIT_MEGA_SINGLE_PREPROVE=1 \
XYO_JIT_MEGA_REPEAT_BURST=1 \
XYO_JIT_MEGA_MULTI_REPEAT_BURST=1 \
./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 0
```

## 性能 (同一凍結Releaseバイナリ、CPU16)

`tests/bench/algos/bench_mega_multi_repeat_burst.py`。
2つの緑旗スクリプトがそれぞれ100,000回の非warp定数加算。
起動/JITコンパイル込みのプロセス壁時計、ON/OFFを16組交互測定。

| パターン | OFF中央値 | ON中央値 | 対応ペア幾何平均速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| 独立変数 (v0+=1, v1+=2) | 156.755 ms | 76.326 ms | **2.05599倍** | 16/16 |
| 共有変数 (v0+=0.1, v0+=0.2) | 144.057 ms | 64.202 ms | **2.24019倍** | 16/16 |

両パターン：
- Mega LLVM call：**100,001 -> 393回** (99.61%削減)。
- 論理Threadターン：両方 **200,002**。
- ON multi-burst呼び出し：391回、処理ターン数199,998。
- 完了時のStage variablesをON/OFFで一致確認。
  共有変数の書込み順を必要とする小数加算は、単体テストで
  `Value.bits`厳密比較済み。

生データ：
`build-scheduler-rewrite-20261111/mega-multi-pc/bench-multi-burst-independent-100k/results.json`
および
`build-scheduler-rewrite-20261111/mega-multi-pc/bench-multi-burst-shared-100k/results.json`。

## 単体・統合の検証

集中テスト (機能ON)：**64/64 PASS**。フラグOFF：45/45 PASS。

- 共有変数、異なる加算値、255パス：
  既存`multiPcBatchEntry`を255回直接呼ぶ結果と
  `MultiRepeatBurstEntry`を1回呼ぶ結果の`Value.bits`、
  PC・残反復数が完全一致。
- 独立変数、123パス：2変数ともbitwise一致。
- maxPasses=0：無変更。
- `runSchedulerPass()`直呼びは1ターンの契約維持。
- 8,192回×2Thread：実Runtimeで正しい最終値。
- 残反復数が異なる1000回と3回：
  融合先の短いThreadをretireした後に既存単独Burstへ移行。
- 共有変数を実行中に削除・再作成：
  世代キャッシュ無効化を検知した通常経路で安全に再束縛し、
  次パス以降で複数Thread融合を再開。
- 値が文字列に変わるケース：Scratch型変換にフォールバック。
- 2Thread間の1番目の加算でInfinityへ到達：
  2番目のThreadを現在パス内で実行した後に停止、
  値・残反復数が既存native batchと一致。
- 8本の同一Stage変数へ4,096回ずつ加算：
  合計32,768回・正しい結果と多数の融合ターンを確認。
- `XYO_WAIT_WAKE_REGISTRY=1`：最終v0=100000/v1=200000、
  exit=0、`[mega-multi-burst]`の出力なし。

## JIT機械語確認

`XYO_JIT_DUMP_OBJECT`からのORC object：
`build-scheduler-rewrite-20261111/mega-multi-pc/multi-burst-object/trace-2.o`。

- `xyo_mega_resume_2_multi_pc_batch`: 158 bytes。
- `xyo_mega_resume_2_multi_repeat_burst`: **268 bytes**。
- `llvm-objdump -d`: スクリプトPCからの分岐、
  `vaddsd`と書き戻し・残反復数更新を各Thread順に実施後、
  2要素以上の有限性チェックと最大反復数チェックを確認。
  LLVM O1により`multi_pc_batch`が外部Callではなく同関数へ
  インライン化されていることを確認。

全体回帰ログ：
`build-scheduler-rewrite-20261111/mega-multi-pc/multi-burst-full-on.out` と
`multi-burst-full-off.out`。

現時点でこれらは限定的なsource-certified合成入力での結果。
一般のScratchBenchやSDL GUIでの改善を示すものではない。
既存の未コミット変更はリセット・push・commitしていない。
