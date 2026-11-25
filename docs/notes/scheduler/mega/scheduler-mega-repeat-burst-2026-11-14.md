# Mega Scheduler β — bounded LLVM Repeat Burst (2026-11-14)

状態：実験的・既定OFF・未コミット。他エージェントの共有worktree変更は保持。

## 目的

単独Threadの純粋な `repeat(N) { change [variable] by constant }` でも、
旧方式は反復1回ごとに `runSchedulerPass` / C++ガード /
`multiPcSingleEntry` を呼ぶため、300,000反復で約300,000回のLLVM呼び出しになる。

この呼び出し回数を削減する。ただし
**`runSchedulerPass()` の「1パス1ターン」契約は変更しない**。

## 実装

- `src/jit/jit.hpp` に `MegaDispatchCode::RepeatBurstEntry` を追加。
  ABIは `uint64_t (*)(Variable*, uint64_t* remaining, uint64_t maxIterations)`。
  ソースslotごとに関数ポインタ `repeatBurstEntries` を保持。
- `src/jit/backend_llvm.cpp` は `XYO_JIT_MEGA_REPEAT_BURST=1`
  かつ single specialization の場合、ソース証明済みの単一変数・有限リテラル
  repeatのみ、一つの純粋なLLVMループ関数を生成する。
  `FAdd` はfast-mathを付けず逐次実行する。ループ中の累積値は
  SSA `double` として保持し、関数の最後に一度だけScratch `Variable`
  に格納する。残反復回数を正確に減らし、実際の加算回数を返す。
- 毎回の加算後、値が正負無限大または非数になった場合はその加算を反映して
  即時返却。次の通常パスでScratchの型変換と継続を処理する。
- `src/engine/sequencer.cpp` に `tryMegaRepeatBurst()` を追加。
  **`stepThreads` 内の時間予算ループ**からだけ呼ぶ。
  1回の呼び出しで最大256反復。各呼び出しの合間に時間予算判定が働く。
  `runSchedulerPass` を直接呼ぶ場合は一切バーストしない。
- Thread 1本のみ、偶数開始PCではなく、有効な奇数resume PC、
  `remaining > 0`、pure repeat slot、`JitCtx/Fiber`不在、
  nativeコード適格、待機/join/OSR等不在、`Blocks::revision`一致、
  variable binding stamp一致、有限数値などをすべて検査。
  Stage変数世代はキャッシュ済みpointerを読む前に確認する。
- renderer / block count / wait registry / pending wait queue /
  ready index / event scheduler / instrumentation / parallel queue 等の場合は拒否。
  isolated event-wait dependency flag自体は既定ONだが、
  待機対象を持たないpure repeatはバーストできる。
- フラグOFFの通常パスに追加関数呼び出しの負担を与えないよう、
  `stepThreads` の入口で有効フラグをキャッシュする。
- バースト実行回数/反復数を
  `megaRepeatBurstCalls()` / `megaRepeatBurstTurns()` で検証。
  `XYO_JIT_MEGA_BULK_STATS=1` なら
  `[mega-repeat-burst] calls=... turns=...` を出力。

## Opt-in

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
./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 0
```

## 性能測定（同一凍結Releaseバイナリ）

`tests/bench/algos/bench_mega_single_dispatch.py --burst`
で、CPU16固定、Eager O1、同じSB3、
起動時間/JITコンパイル時間込みでON/OFFを20組交互測定。
スクリプトは `repeat(300000) { change v0 by 1 }` と
`repeat(1) { change v1 by 2 }`。
両条件とも最後の変数状態とLLVM処理反復数が一致。

| 指標 | Burst OFF | Burst ON |
|---|---:|---:|
| 中央値 | 181.272ms | 64.766ms |
| 対応ペア幾何平均速度比 | 1.000 | **2.78065** |
| ON勝利 | — | **20/20** |
| Mega LLVM呼び出し | 300,001 | **1,175** |
| 論理的なScratch Threadターン数 | 300,003 | 300,003 |
| うちnative burst呼び出し | — | 1,172 |
| うちburstで処理したターン数 | — | 299,998 |

生データ：
`build-scheduler-rewrite-20261111/mega-multi-pc/bench-repeat-burst-300k/results.json`

起動・LLVMコンパイルを含め約2.78倍の速さ。
**一般のScratchBench・GUI・実作品での改善は未検証。**

## ORC生成機械語

`XYO_JIT_DUMP_OBJECT` で生成したJITオブジェクト
`build-scheduler-rewrite-20261111/mega-multi-pc/repeat-burst-object/trace-2.o` を
`llvm-nm -S` と `llvm-objdump -d` で検査した。

- `xyo_mega_resume_2_repeat_burst_0`: 0x54 = 84 bytes。
- `xyo_mega_resume_2_repeat_burst_1`: 0x54 = 84 bytes。
- 本体は逐次`vaddsd`によるループ1個。浮動小数点例外値の検査、
  残反復数の減算、最後の1回の変数書戻しを確認。
- 生成機械語のFAdd順序を変更するfast-mathは使用していない。

## 契約と制限

これは「すべてのThreadをまとめて実行する」実装ではない。
ソースが純粋な数値加算であること、同時実行するScratch Threadが1本であること、
headlessでrenderer等がないこと、という限定条件だけを対象とする。
バーストは通常の `runSchedulerPass` 呼び出し契約を変えないが、
`stepThreads` 中の内部チェック間隔を最大256反復分に延ばす。
したがって各フレームの進捗や外部ホストからの中間観測タイミングが
変わり得る。型・ソース・キャッシュが少しでも不適格なら従来経路を使う。
時間予算の判定は各バーストの終了後に実施する。

## テスト

- Release `xyo-tests` と `xyo-cli` ビルドPASS。
- `XYO_TEST_ONLY_STACKLESS_CORO=1 XYO_TEST_ONLY_MEGA_SINGLE=1`：
  Burst ON 45/45 PASS、OFF 31/31 PASS。
- 255回の非整数初期値逐次加算について、従来の単発LLVM
  255呼び出しとバースト1呼び出しの`Value.bits`厳密一致。
- `maxIterations=0` は無変更。
- `runSchedulerPass` 直接呼び出しの1スクリプト1反復を維持。
- 実際の `stepThreads()` で約8,192反復を実行し、
  ターン数と結果値が一致。
- 途中で変数を文字列に変更した場合の通常型変換フォールバック。
- 変数map削除・再作成後の安全な再束縛・バースト再開。
- `1e308 + 1e308` は1回加算したところで止まり、
  無限大のビット表現・残反復数が単発LLVMと一致。
- Fiber / GC deferral / multi-Fiber barrier smoke: 3/3 PASS。
- `XYO_WAIT_WAKE_REGISTRY=1`：従来経路を維持、
  v0=300000 / v1=2、exit=0、burst statsなし。
- Python構文チェックと`git diff --check` PASS。

全体回帰ログ：
`build-scheduler-rewrite-20261111/mega-multi-pc/repeat-burst-full-on.out`
および `repeat-burst-full-off.out`。

初期化途中で isolated-wait フラグ既定ONまで拒否する
過剰ガードがあり、集中テストで検出・修正済み。

既定OFF・未コミット。既存共有worktreeをリセット・コミット・pushしていない。

## 最終ビルドの再測定と全体回帰

フラグ未指定の場合に追加関数を呼ばないための外側ガードを加えた
最終Releaseバイナリに対し、同じ条件で20組を再測定：

| 指標 | Burst OFF | Burst ON |
|---|---:|---:|
| プロセス壁時計の中央値 | 181.355 ms | 65.554 ms |
| 対応ペア幾何平均速度比 | 1.000 | **2.75464倍** |
| ON勝利数 | — | **20/20組** |
| LLVM呼出し回数 | 300001 | **1175** |
| 処理した論理ターン | 300003 | 300003 |

最終ビルドの生データ：
`build-scheduler-rewrite-20261111/mega-multi-pc/bench-repeat-burst-final-300k/results.json`

同じ最終バイナリでの全体テスト：
- Burst ON: **Scheduler 606/606 PASS**、E2E 99/99 PASS、
  他の全テスト群で失敗0、exit=0。
- Burst OFF: **Scheduler 592/592 PASS**、E2E 99/99 PASS、
  他の全テスト群で失敗0、exit=0。
- 集中テスト: ON 45/45 PASS、OFF 31/31 PASS。
- 待機通知フラグON: 反復300000/2の最終状態一致、exit=0。
- `git diff --check` / Python構文チェックPASS。

最終全体回帰ログ：
`build-scheduler-rewrite-20261111/mega-multi-pc/repeat-burst-final-full-on.out` と
`repeat-burst-final-full-off.out`。

実験フラグの既定値は引き続きOFF。
