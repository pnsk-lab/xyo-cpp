# JIT warp共有時刻ポーリング（2026-11-04）

## 採用方式

通常のJIT warpループで、64反復に1度の時間判定をC++時計呼び出しから**共有時刻のAtomic Load**に変更した。切替を明示しない場合はこの方式が有効。

- `Sequencer::stepThreads()` の開始時に `Timer::startTime() + Runtime::warpTimeMs` を締切として設定。
- Sequencer単位のWatchdogスレッド（`src/engine/warp_clock.hpp/.cpp`）が `steady_clock` を約1ms間隔で読み、単調時計基準の整数マイクロ秒をAtomic変数へ書き込む。
- LLVM JITはwarpの64反復ごとに共有時刻・締切をAtomic Loadし、`nowUs > deadlineUs` なら従来通り `spillPromoted → xyo_yield → reloadPromoted` を実行する。カウンタと同期点の間隔は変更しない。
- 締切をJITコンテキストに値コピーせず、Watchdog側のAtomic変数として保持。Fiber再開時に新しいティックの締切を観測できる。Watchdogの世代番号とmutexで旧ティックの更新をキャンセルする。
- dangerモードは締切を`INT64_MAX`とし、Watchdogの定期更新を停止する。非warpループと待機系のyield動作は変更しない。
- 共有時計を使用できない場合（`Sequencer::stepThreads`以外からの直接JIT実行、カスタムの`Timer::NowFn`、無効化設定）は、従来の`xyo_warp_yield_due`を呼び出す。
- `XYO_JIT_WARP_CLOCK=0` で共有時刻方式を無効にし、元のポーリング方式へ戻せる。コンパイル時にJITの分岐形状を選択するため、実行時の追加設定判定はホットループに入らない。

## 構造

```text
Sequencer::stepThreads
  ├─ timer.start()
  └─ WarpClockWatchdog::arm(deadline, danger)
       └─ worker: sleep 〜1ms → steady_clock::now() → sampledUs.store()
JIT warp loop:
  ├─ 64反復ごと
  ├─ sampledUs.load(atomic)
  ├─ deadlineUs.load(atomic)
  └─ nowUs > deadlineUs ? spill / yield / reload : continue
```

時刻はJITが直接呼び出す`steady_clock::now()`ではなく、別スレッドで更新する。締切のキャッシュラインと頻繁に更新する時刻を分離する。

## 実験結果と制約

実験は`build/warpflag-experiment/REPORT.md`を参照。試験版での6回交互測定では、従来方式に比べ`algo_primes` +7.0%、`app_raycast` +5.3%、`loops_repeat` +42.3%、`vars_setchange` +56.4%。共有時刻方式は期限超過Flag方式と同等か、わずかに遅い。1msのWatchdog起床遅延や追加CPU消費は別途評価が必要。

**保証範囲**: Watchdogの起床はOSスケジューリングに依存するため、厳密に500ms以内のpreemptionは保証しない。JITが次の64反復境界に到達するまでの時間も必要である。これは性能改善のための時間確認キャッシュであり、厳密な実時間制御の仕組みではない。

新しいC++クラス`WarpClockWatchdog`の単体検証は`tests/unit/test_danger.cpp`に追加。元々のコードが持つカスタム時計・直接呼出しのテストには同期方式が残る。

## 本体へ統合後の測定

元の作業ツリーに共有時刻方式を直接導入した後、同じRelease実行バイナリ
`build/warpflag-experiment/production-xyo-20261104`で`XYO_JIT_WARP_CLOCK=0/1`を比較した。

| 項目 | 旧方式中央値 (ops/ms) | 共有時刻中央値 (ops/ms) | 速度比 |
|---|---:|---:|---:|
| algo_primes | 60,652 | 62,589 | 1.032 |
| app_raycast | 34,221 | 35,215 | 1.029 |
| vars_setchange | 1,408,318 | 2,088,689 | 1.483 |
| loops_repeat | 1,480,282 | 2,283,973 | 1.543 |
| loops_nested | 1,423,775 | 2,147,711 | 1.508 |
| app_ast | 7,761 | 7,722 | 0.995 |

各6回・順序を交互に入替・CPU 12固定。チェックサムは全72実行で一致。
生データ: `build/warpflag-experiment/production-ab-20261104.json`。

全ScratchBench 47項目は両設定で完走し、時間依存のscheduler8項目を除く
39項目のチェックサムがすべて一致。単発の全39項目幾何平均は1.088倍。
複数回測定のない項目まで速度向上が確定したものとは扱わない。

CTest 8/8成功。HotSpot parityおよびEager parityはどちらも2123/2123。
実行ログは `build/warpflag-experiment/production-*.log` を参照。
