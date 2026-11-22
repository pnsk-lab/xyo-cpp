# xyo-cpp: 通常LLVMループの直接Fiber Yield実験（2026-11-14）

対象：通常の非warp Scratchループが呼ぶ `xyo_yield_fast(Fiber*)`。Fiber連鎖は明示的に無効化した条件のみ。共有dirty worktreeで未コミット、他エージェントの変更はリセットしない。

## 実装

- `src/engine/fiber.cpp`: `XYO_FIBER_DIRECT_YIELD=1` かつ `XYO_FIBER_CHAIN=0` の場合だけ、`xyo_yield_fast()` が `Fiber::yield()` の連鎖先検査を飛ばして `xyo_fiber_switch(f->regs(), &detail::gSchedulerRegs)` を直接呼ぶ。
- 同じフラグ条件で `Fiber::resumeChain()` も直列の `resume()` を使用し、プログラムの別の呼出し経路が誤ってFiber連鎖を成立させないようにした。
- `XYO_ASAN` 下では直接経路を利用せず従来の `Fiber::yield()` を使用。FiberのGCレジスタ/スタックの所有権、スクラッチループのYield位置、終了処理は変更しない。
- `XYO_FIBER_DIRECT_YIELD` は既定OFF。`XYO_FIBER_CHAIN` が未設定または`1`なら新経路は無効。

## ネイティブ単体の16組交互測定

同一バイナリ、CPU16固定、200,000生成完了（0 Yield/1 Yield）および10,000×100 Yield。

| ケース | OFF中央値 | ON中央値 | ペア速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| 0 Yield | 5.8654ms | 5.7953ms | 1.0591倍 | 12/16 |
| 1 Yield | 10.6437ms | 10.6007ms | 1.0154倍 | 10/16 |
| 100 Yield | 16.7185ms | 16.3172ms | 1.0667倍 | 14/16 |

0 Yieldの対照条件にも約6%の見かけの改善があり、測定時のCPUクロック・環境負荷などのバイアスが疑われる。100 Yieldの1.067倍を純粋な切替高速化と主張しない。

実装・データ：
- `build-scheduler-rewrite-20261111/fiber-20261113/bench-direct-yield.cc`
- `build-scheduler-rewrite-20261111/fiber-20261113/bench-direct-yield-16pairs.json`

## ScratchBenchの12組交互比較

同じReleaseバイナリ、CPU16固定、Eager、両条件とも `XYO_FIBER_CHAIN=0`。
ON/OFFは `XYO_FIBER_DIRECT_YIELD` の1フラグのみ。スコアは大きいほどよい。

| 項目 | OFF中央値 | ON中央値 | ペアON/OFF速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| sched_pingpong | 414.000 | 411.8205 | 0.9944倍 | 6/12 |
| sched_clonechurn | 396.6575 | 400.6205 | 1.0110倍 | 8/12 |
| sched_threads | 6788.7315 | 6861.9695 | 1.0117倍 | 7/12 |
| sched_fanwait | 700.5200 | 722.5045 | 1.0222倍 | 10/12 |

原データ：`build-scheduler-rewrite-20261111/fiber-20261113/bench-direct-yield-sched/results.json`。

## 正しさ検証

- `tests/unit/test_scheduler.cpp` に、2本のネイティブFiberを各3回Yieldさせ、API `Fiber::resumeChain` のシリアルフォールバックで順序を照合するテストを追加。
- `XYO_TEST_ONLY_STACKLESS_CORO=1 XYO_TEST_ONLY_DIRECT_YIELD=1 XYO_FIBER_CHAIN=0 XYO_FIBER_DIRECT_YIELD=1 XYO_JIT_MODE=eager`: **4/4 PASS**。ログを含め、2本が完了しFiberPoolの貸出数が戻ることを検証。
- `xyo-fiber-smoke`, `xyo-rt001-native-gc-deferral-smoke`, `xyo-rt001-native-gc-multifiber-barrier-smoke`: **3/3 PASS**。
- 通常Releaseのビルド: xyo-tests / xyo-cli / xyo-fiber-smoke PASS。`git diff --check` PASS。

### 最終回帰・安全性確認

- Eager JIT・XYO_FIBER_CHAIN=0 XYO_FIBER_DIRECT_YIELD=1：全体テスト Scheduler **455/455件PASS**、他のテスト群0失敗、終了コード0。
- 全体テストログ：build-scheduler-rewrite-20261111/fiber-20261113/direct-yield-full-on.out。
- Fiber連鎖を禁止しない構成（XYO_FIBER_DIRECT_YIELD=1かつXYO_FIBER_CHAIN未指定）でも、既存のgeneric connected component chainテストは **4/4件PASS**。
- git diff --check PASS。変更は未コミットで、他エージェントの変更を保持する。

## 判断

ScratchBenchの改善は一貫せず、前提として既存の有用なFiber連鎖を全域で無効にする必要がある。**既定ONは不採用**。なお、JITが生成する通常非warpループのYield自体は削除していないため、Scratchのパス意味論は維持する。

## 実験コマンド

```sh
XYO_JIT_MODE=eager \
XYO_FIBER_CHAIN=0 \
XYO_FIBER_DIRECT_YIELD=1 \
./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 0
```
