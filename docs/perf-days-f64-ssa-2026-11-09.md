# PERF-001: `daysSince2000` の同期時刻取得を維持したLLVM数値SSA高速化

日付: 2026-11-09。対象: xyo-cpp / ScratchBench 47項目。実行環境は i7-12700。

## 変更内容

`sensing_dayssince2000` は毎回 `rt::daysSince2000()` で現在のシステム時計を読み、Scratchと同じ Date.now()（整数ミリ秒）および `Date.UTC(2000,0,1)` の起点で計算する。

従来のLLVM JITは `uint64_t xyo_sns_dayssince2000(JitCtx*)` により`Value::num()`へ箱詰めしたビット列を受け取り、数式演算時に数値を再判定・再変換する。

- `src/jit/rt_blocks.cpp`: `double xyo_sns_dayssince2000_f64(JitCtx*)` を追加。`XYO_RT_PROF(kBlocks)`を維持して同じ `rt::daysSince2000()` を返す。
- `src/jit/backend_llvm.cpp`: LLVM JIT用にf64ヘルパーシンボルを解決。
- `src/jit/codegen.cpp`: `XYO_JIT_DAYS_F64` 既定 **ON**。共有サンプル時計 `XYO_JIT_DAYS_SHARED_CLOCK` が無効なとき、f64を取得して `emitCanonKnownNonNaN` で数値BoxとLLVM SSA型事実を生成。数値変換の重複を避ける。 `XYO_JIT_DAYS_F64=0` は従来のi64 boxed helperへ戻す。共有時計の実験経路には干渉しない。

非同期サンプリングや時刻量子化の変更、元の `scratch-bench` プロジェクト書き換えはなし。`rt::daysSince2000()` は通常時刻からの有限な倍精度値を返すため `emitCanonKnownNonNaN` の前提を満たす。

## CPU固定A/B（同一バイナリ、交互ON/OFF）

`build/perf-days-f64-frozen-xyo` (SHA256 `9542dacca09aaa8ffc854a49ceaf10b1360a75213fbbae7fbe10bd26474c5a26`) を使用。`build/scratchbench-cause-probes/projects/looptick.sb3`（元のScratchBench定義由来）のみを検査する時は、`XYO_JIT_DAYS_SHARED_CLOCK=0`を明示。

| 条件 | OFF中央 ops/ms | ON中央 ops/ms | ペア速度比幾何平均 | ON勝ち数 |
|---|---:|---:|---:|---:|
| CPU14 P-core 24組、高負荷時 | 5,792.04 | 6,136.46 | 1.02153 (+2.15%) | 13/24 |
| CPU16 E-core 40組、第1回 | 4,310.65 | **4,456.66** | **1.03378 (+3.38%)** | **39/40** |
| CPU16 E-core 40組、第2回 | 4,332.60 | **4,478.07** | **1.03114 (+3.11%)** | **38/40** |

CPU16は相対比較の再現性が高く、2回の独立した40組でほぼ同じ結果。CPU14は他エージェントの並行処理に起因するホスト負荷の変動が大きいため、単独の中央値差だけから判断しない。

同じ最初のCPU14バイナリで `sched_clonechurn` は24組ON17勝でペア幾何平均+1.16%、`sched_pingpong` は12/24勝で-0.22%。両方とも数値自体の分散が高く、機能の直接効果ではない。

生ログ: `build/perf-days-f64-ab.{log,json}`、`build/perf-days-f64-cpu16-ab.{log,json}`、`build/perf-days-f64-cpu16-ab2.{log,json}`。

## 元の47項目（同一SDLバイナリ）

`build/perf-days-f64-sdl-frozen-xyo` SHA256 `ddcec1f6c5848099b63a7cdd82c9cb2e1e82a45b37b675aa231aee225ccd36c3`。

元の47項目をON→OFF→OFF→ONで各1回測定。ランごとにCPU負荷・他エージェントが変動した。

- `sched_looptick` 結果（ops/ms）: ON1 **4,874.116**、OFF1 **6,261.699**、ON2 **7,016.9**、OFF2 **5,462.9**。ON/OFFの2回ずつの幾何平均比は **ほぼ1.000**。全体スイートでは局所的な+3%を有意に測定できていない。
- 47項目の個別速度比を2回ずつ幾何平均し、さらに47項目で幾何平均した比は **1.02185**。個別項目のON優位は **27/47**。無関係な算数・文字列・fanwait等にも大きな上下があるため、全体スコアの改善をこの変更に帰属させない。
- 比較データ: `scratch-bench/results/perf-days-f64-full-{on,off,off2,on2}-20261109/performance-xyo-cpp.json`、ログ `build/perf-days-f64-full-{on,off,off2,on2}.log`。

## 正しさ・ビルド

- Native Release: ヘッドレスとSDL版両方ビルドPASS。
- ScratchBench GUI parity: `XYO_JIT_DAYS_F64=1` **2123/2123**、`=0` **2123/2123**（100%）。`build/perf-days-f64-parity-{on,off}.log`。
- `xyo-tests`: ON **1/1 PASS（52.43s）**、OFF **1/1 PASS（50.74s）**。`build/perf-days-f64-ctest-{on,off}.log`。
- 共有 `tests/CMakeLists.txt` が他担当の単独 `main()` を持つ6個の `jit_*_smoke.cpp` を `xyo-tests` に自動追加してしまうため、検証専用ビルドではリンクコマンドから単独テスト6オブジェクト**だけ**除外した（テスト本体の全通常モジュールは残す）。リンク用補助スクリプト `build/perf-days-f64-link-tests.py`。共有ソース/CMakeは変更していない。
- SDLコンパイルは別エージェントが編集しているDLSSヘッダ問題を回避するため、検証用CMakeのみ `-DrealDlssEnabled_=false -include src/render/dlss_opaque_coverage.hpp`。描画ソースは変更していない。
- 既定ONの最終バイナリ: `build/perf-days-f64-default-frozen-xyo` SHA256 `36c752ad8da9d6e9163171b28e998ee778dfc1a254263aadd9af5fd092d3c884`。最終の既定設定CTest/GUI parityログは `build/perf-days-f64-default-{ctest,parity}.log`。

## 判断

**既定ONを採用**。変更は同期時刻値の伝達とLLVM数値SSAマークのみで、時刻の値と観測粒度を変更しない。`XYO_JIT_DAYS_F64=0` で切り戻せる。CPU16の独立した2×40組では対象ケースで+3.1〜3.4%を再現し、ScratchBench parityとユニットテストはON/OFFとも全件PASS。

全47項目のベンチマークで統計的な総合改善を立証できたわけではなく、保存済み5競合の最高記録に**47/47勝利は未達**。他担当と競合するパス（IR/OSR、DLSS）は変更せず、commit/pushもしない。

## 17:40 JST 時点の最新parityスイートとの差分切り分け

この作業の途中でParityソースが `aeaa86a89eca6afe`（2123件）から `d64a28bfd605ed0c`（2656件）に更新された。

**最新2656件の最終既定ONビルド**（SHA256 `36c752ad8da9d6e9163171b28e998ee778dfc1a254263aadd9af5fd092d3c884`）の検証では、`XYO_JIT_DAYS_F64` のデフォルトON実行も `=0` の従来経路も **2629/2656（99.0%）** だった。失敗27件の文字列を全件比較したところ **同じ27件が同じ結果で失敗しており、新規差分ゼロ**。音声待ち・エッジ反射・色/図形効果・`sin(-0)` / `tan(-0)` 等のテストで、`daysSince2000` とは無関係。記録: `build/perf-days-f64-default-parity.log`（ON）、`build/perf-days-f64-default-off-parity.log`（OFF）。

古い2123件ではON/OFF両方100%であったことはそのまま事実だが、現行2656件で100%とは言わない。最終版の `xyo-tests` は既定ONで **1/1 PASS（50.34秒）**、`build/perf-days-f64-default-ctest.log`。

## 暫定競合順位

元の全47項目の4回測定のうちON2の結果を、保存済み（異日）の5競合ベストと比較した。**26/47勝・21敗**。詳細 `scratch-bench/results/perf-days-f64-rivals-20261109.{md,json}`。`sched_looptick` ON2 7016.86 ops/ms 対 MistWarp 12200.54 ops/ms（**0.575倍**）。`sched_fanwait` は単発ON2 665.31 対 Scratch Everywhere! 3207.63 ops/msだが、同一バイナリ内の別の測定は1747.45など変動が極めて大きく、この順位を当該最適化の回帰と見なさない。

この段階での **47/47勝利は未達**。ソース編集はこのJIT経路に限り、commit/pushなし。他担当の共有コードは保全した。


## 18:15 JST 追加検証: 有限数SSA伝播（既定OFF）

`XYO_JIT_DAYS_FINITE_SSA=1` を実験的に追加した。適用条件は既存
`XYO_JIT_DAYS_F64=1` かつ `XYO_JIT_DAYS_SHARED_CLOCK=0` のLLVMパスに限定。
`rt::daysSince2000()` が `system_clock::now()` から整数msをdoubleへ変換し
UTC起点を差し引いて86400000で割る有限値であることを
`finiteArithmetic` に記録する。この情報を `emitArithNumeric` の既存の
非NaN転送規則に接続する。時刻取得回数、サンプリング精度、演算順序や
fast-math属性の変更はない。既定OFFなので従来経路に影響しない。

独立したヘッドレスReleaseバイナリ
`/home/kicky/.cache/xyo-perf-days-finite-20261109/xyo`
（SHA256 `be95c9f68539df56a0760750ca925f808d57ce3d271198c6ec46a3e28c637e5e`）
による `sched_looptick` CPU16固定24交互A/B組では
OFF中央値 **4505.232 ops/ms**、ON中央値 **4556.218 ops/ms**、
ペア幾何平均 **1.013033（+1.30%）**、**22/24組ON勝利**。
対数速度比に対する単純なt区間の95%参考範囲は
約 **+0.73%〜+1.88%**。ただしホスト負荷の非独立性もあり
全47項目の改善や他競合への優位は示していない。
生結果は
`/home/kicky/.cache/xyo-perf-days-finite-20261109/days-finite-ab.json`。

新しい差分回帰テスト `tests/differential/jit_days_finite_ssa.py` を追加。
元のScratchブロックから生成した診断SB3（Warp / 非Warp）で
時計値・Infinityを用いた算術、ゼロとの比較を含む15ケースを
Interpreter基準としてEager/HotSpotのON/OFF各4構成と比較した。
**120/120件一致**、全8構成完走。
時刻依存そのものの結果値は比較せず、確定する算術結果だけ比較。

SDL再ビルドは別担当の `src/engine/thread.cpp` で
`NativeSourceEffectTimelineRegistry` 未宣言により失敗。
この共有ソースは変更せず、GUI parityと元の47項目A/Bは
**未実施**。安全な切戻しと追加回帰測定を優先し、
`XYO_JIT_DAYS_FINITE_SSA` は **既定OFF** のまま。
本作業ではcommit/pushなし。PERF-001継続。
