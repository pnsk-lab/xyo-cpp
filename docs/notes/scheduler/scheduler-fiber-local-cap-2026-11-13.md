# Fiberの取得・再開の計測とTLSプールの容量制御（2026-11-13）

対象: xyo-cpp。共有の未コミット作業ツリー。他エージェントの変更はreset/commitしない。

## 概要

JIT Fiberの切替経路を調査。既存の `XYO_FIBER_LOCAL_POOL=1` は、スレッドローカルキャッシュで賄える場合にFiberPool::acquire/releaseのMutexを回避する。
以前の実装ではOSスレッドごとに最大16本のアイドルFiberを保持した（通常1MiBスタック予約/本）。
新しい `XYO_FIBER_LOCAL_POOL_CAP=1..16` でローカルプールの上限を実行時に指定する。今回の測定を受け、既定をローカルプールON・ローカル上限1本に切り替えた。`XYO_FIBER_LOCAL_POOL=0` で共有プールのみへ切り戻せる。
1本でも溢れたFiberは従来の共有プールへ返す。共有プールの最大64本の予約やFiberの所有、GC unsafeCount、FiberRegsを変更しない。
0・不正な上限・未指定値は1に戻す。明示的な`XYO_FIBER_LOCAL_POOL_CAP=16`で旧ローカル容量を選択できる。プロファイル計測は `XYO_FIBER_POOL_STATS=1` で有効（`XYO_DISABLE_RT_PROF=OFF`のビルド限定）。通常のReleaseでは計測処理はコンパイル時に除去される。

## Microbench

独立したFiberPoolとx86-64 SysVアセンブリを-O3でビルドした。CPU16固定、14組の交互ON/OFF、同じベンチ実行ファイル。

| ケース | 共有プールOFF中央値 | TLSプールON中央値 | ペア速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| 200,000本の生成・実行・完了、Yieldなし | 7.994ms | 6.239ms | 1.2638倍 | 14/14 |
| 200,000本の生成・Yield1回・完了 | 13.613ms | 11.749ms | 1.1502倍 | 14/14 |
| 10,000本、各100回Yield・完了 | 17.928ms | 17.864ms | 1.0040倍 | 8/14 |

独立ベンチ: `build-scheduler-rewrite-20261111/fiber-20261113/bench-fiber-pool.cc`。
データ: `build-scheduler-rewrite-20261111/fiber-20261113/microbench-pairs.json`。
Yieldを繰り返すコストは取得・返却のコストより大きく、TLSプールによる切替の直接高速化は確認できない。

## ScratchBench: ローカル容量1 vs 共有のみ

同一Releaseバイナリ、CPU16固定、Eager JIT、12組の交互測定。
環境変数 `XYO_FIBER_LOCAL_POOL` のみON/OFF、 `XYO_FIBER_LOCAL_POOL_CAP=1` は共通。
スコアは大きいほどよい。各ペアでベンチ対象Scratchの最終結果を検証した。

| カーネル | OFF中央値 | ON中央値 | ペアON/OFF速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| sched_restart | 1039.260 | 1116.698 | 1.08195倍 | 11/12 |
| sched_clonechurn | 292.074 | 309.889 | 1.06499倍 | 12/12 |
| sched_fanwait | 511.573 | 537.616 | 1.01712倍 | 4/12 |

原データ: `build-scheduler-rewrite-20261111/fiber-20261113/bench-local-cap1-vs-off/results.json`。

## ScratchBench: 容量1 vs 容量16（TLS有効）

同一Releaseバイナリ、CPU16固定、Eager JIT、12組の交互測定。
ローカルプールを両方ON。**この計測時点のソースでは** `XYO_FIBER_LOCAL_POOL_CAP=0`は当時の既定16にフォールバック、`=1`が上限1だった。最終ソースは既定1なので再測定時は`=16`と`=1`を指定する。速度比は1/16。

| カーネル | 容量16中央値 | 容量1中央値 | ペア1/16速度比 | 1勝利 |
|---|---:|---:|---:|---:|
| sched_restart | 1406.439 | 1407.019 | 1.00287倍 | 7/12 |
| sched_clonechurn | 415.904 | 408.582 | 0.99425倍 | 3/12 |
| sched_fanwait | 772.198 | 768.481 | 0.99750倍 | 5/12 |

原データ: `build-scheduler-rewrite-20261111/fiber-20261113/bench-local-cap1-vs16/results.json`。
容量1の局所メモリ予約は、1 OSスレッドあたり最大1MiB分の遊休Fiberスタックになる（グローバルプールは別枠）。
このベンチでは容量1でも容量16の効果をほぼ維持している。全ワークロードへの一般化はできない。

## 回帰・追加検証

- Release `xyo-tests` 全体: `XYO_JIT_MODE=eager XYO_FIBER_LOCAL_POOL=1 XYO_FIBER_LOCAL_POOL_CAP=1` でScheduler 450/450、その他スイート失敗なし、終了コード0（`fiber-20261113/full-cap1.out`）。
- 追加プロファイルではローカル取得・共有取得・新規スタック生成、ローカル返却・共有返却・実体削除の件数を計測した。プロファイル専用コード以外は通常Releaseに残さない。

## 実行

```sh
XYO_FIBER_LOCAL_POOL=1 XYO_FIBER_LOCAL_POOL_CAP=1 \
  XYO_JIT_MODE=eager ./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 0
```

最終設定は既定ON・容量1本。`XYO_FIBER_LOCAL_POOL=0`で旧共有プールへ戻せる。共有プールの最大64本は別枠で、全OSスレッド合計の保持量が1 MiBになるわけではない。


## ScratchBench全体（4組、checksum照合）

変更していないperformance.sb3、CPU16固定、Eager JIT。両条件で
39項目の非スケジューラchecksumが全件一致。

| 項目 | 対応ペアON/OFF速度比 | ON勝利 |
|---|---:|---:|
| sched_looptick | 1.07387倍 | 3/4 |
| sched_pingpong | 1.07660倍 | 2/4 |
| sched_restart | 1.29392倍 | 4/4 |
| sched_clonechurn | 1.17574倍 | 4/4 |
| sched_threads | 1.07534倍 | 3/4 |
| sched_fanout | 1.11424倍 | 3/4 |
| sched_fanwait | 1.10158倍 | 3/4 |
| sched_waituntil | 1.08480倍 | 4/4 |

原データ: build-scheduler-rewrite-20261111/fiber-20261113/bench-full-cap1-vs-off/results.json

4組ではばらつきが大きく、特に1組はOFFの実行速度が大幅に低下していた。
対象を絞った12組のrestart=1.08195倍、clonechurn=1.06499倍を
効果の中心的根拠とし、全体4組の高倍率は一般化しない。

## プロファイル版での実際のFiberプール取得内訳

`XYO_FIBER_POOL_STATS=1`（`XYO_DISABLE_RT_PROF=OFF`でのみ生成）、
同じclonechurnプロジェクトを別プロセスで3条件実行。
ScratchBenchの反復量は試行ごとに変動するため件数の単純比較はしない。

| 設定 | ローカル取得 | 共有取得 | 新規stack | ローカル返却 |
|---|---:|---:|---:|---:|
| ローカルOFF | 0 | 1,250,473 | 435 | 0 |
| ローカル容量1 | 1,310,500 | 576 | 426 | 1,310,501 |
| ローカル容量16 | 1,291,692 | 576 | 291 | 1,291,708 |

容量1では取得合計1,311,502回中1,310,500回、約99.92%が
スレッドローカル経路。共有プールは溢れたFiberを引き続き管理する。

## 最終ビルド・回帰

- 最終ソースで再ビルド（xyo-tests/xyo-cli/Fiber smoke/Native GC smoke）成功。
- `XYO_JIT_MODE=eager`のみ指定、ローカルプール環境変数は**未設定**。
  既定ON・容量1でScheduler 450/450、全テスト群失敗ゼロ、終了コード0。
  build-scheduler-rewrite-20261111/fiber-20261113/full-default-local1.out
- 明示的に古い共有プールへ戻した`XYO_FIBER_LOCAL_POOL=0`でも、Fiber smokeと2本のGC smokeは3/3 PASS。
- ctestの xyo-fiber-smoke、
  xyo-rt001-native-gc-deferral-smoke、
  xyo-rt001-native-gc-multifiber-barrier-smoke は3/3 PASS。
- 元のポリシー: `XYO_FIBER_LOCAL_POOL=0`
- 旧ローカル容量: `XYO_FIBER_LOCAL_POOL_CAP=16`
- 容量1を明示する場合: `XYO_FIBER_LOCAL_POOL=1 XYO_FIBER_LOCAL_POOL_CAP=1`

実験差分は未コミット。GC所有権やネイティブstack switch ABIは変更しない。
