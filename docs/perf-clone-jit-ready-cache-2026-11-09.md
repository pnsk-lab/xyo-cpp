# PERF-001: クローン開始ハットのJITキャッシュ直接再利用（2026-11-09）

## 計測条件・結論

ScratchBench の元の `sched_clonechurn`（変更なし）を最適化。約100個のクローンを繰り返し生成・停止し、`control_start_as_clone` の生成費用が主要ボトルネック。

**採用:** `src/engine/thread.cpp` の `Thread::tryCompile` が、クローン開始ハット用に共有済みのネイティブ `CompiledScript` があれば `JitEngine::readyScript(top)` から直接結び付ける。既定 **ON**、`XYO_JIT_READY_CLONE=0` で従来の `decideCompile` / `compile` 経路へ復帰。JIT未生成時、共通CFG実験 `XYO_JIT_COMMON_CFG=1`、正確なブロック数計測時は従来経路を維持。LLVM/AOT用の実行フレームABIは変更しない。

### 実際の処理時間の計測

`Target::makeClone` / `Target::dispose` に `XYO_CLONE_LIFECYCLE_PROF=1` の位相別計測を追加。v1.2.0の共有コミットがこの機能を採用したため、ソースはその確定版へ復元して保持。未指定では計測無効。生ログは `build/perf-clone-phase-{1,2,3}.err` と `build/perf-clone-deep-phase-{1,2}.err`。

* 約414,500回の生成: `makeClone` 合計 **173.4 ms**、そのうち `startHats` **135.6 ms**、新Target作成16.4 ms、変数コピー13.9 ms、設定7.5 ms。
* 約414,400回の削除: `dispose` 合計 **31.4 ms**、監視表示5.6 ms、スレッド停止7.0 ms、実行順削除8.5 ms。
* ネイティブJIT内部プロファイルを有効化した別ビルド（`XYO_DISABLE_RT_PROF=OFF`）の約285,200回生成では、`pushThread` の割り当て系 **41.6 ms**、コンパイル判定・ルートスタック初期化 **98.1 ms**。このうちJIT compileの既存計測点約64.9 ms。
* 過去の `stopForTarget` 索引化、初回ハット重複探索省略、空モニター通知省略、2–4変数 `wait until` 依存監視、待機サイトキャッシュは、有意な性能改善を示さず、すべて撤回または実験的既定OFF。詳細 `docs/clonechurn-thread-lifecycle-2026-11-09.md`。

### 同一バイナリ・CPU固定のA/B

ネイティブRelease/ヘッドレス `build/perf-clone-ready-frozen-xyo`
SHA256 `5b4053121d861281f78ed7b5734a83ba0dcf7944edb2083a6e92d474599df9ae`。
CPU14 P-core固定、24組、各組でON/OFF実行順を交互に変更。全て元の `build/scratchbench-cause-probes/projects/clonechurn.sb3` を使用。

| 指標 | OFF | ON |
|---|---:|---:|
| `sched_clonechurn` ops/ms 中央値 | 1,011.0555 | **1,083.646** |
| 中央値の改善 | — | **+7.18%** |
| ペアごとの速度比の幾何平均 | — | **1.06836**（+6.84%）|
| ON が勝った組 | — | **24/24** |

生データ `build/perf-clone-ready-ab.{json,log}`。計測中のソース・バイナリ不変。

### ScratchBench元の全47項目

同一SDL対応実行ファイル
`build/perf-clone-ready-sdl-frozen-xyo`
SHA256 `a609a84a0b02513f5fe2fb0a71dd02bff592dde5489f7336658b5d88efe9c1f0`、ソースハッシュ `e8bd5be3e7345088`。ON→OFF、逆順OFF→ONを各1回実行。

| 項目 | ON→OFF時のON | ON→OFF時のOFF | OFF→ON時のON | OFF→ON時のOFF |
|---|---:|---:|---:|---:|
| 全体スコア | 109,141 | 108,652 | 107,575 | 109,414 |
| `sched_clonechurn` ops/ms | 1,138.96 | 1,037.07 | 1,125.81 | 1,061.79 |

`sched_clonechurn` は2回ずつの幾何平均で **+7.91%** と再現した。一方、全47項目の無加重幾何平均速度比は **0.99468**（-0.53%）。変更した命令経路は `control_start_as_clone` に限定されるため、他項目のばらつきを原因と断言しない。単発ON報告と保存済みの異日5競合を比較した順位は **30/47勝・17敗**（現時点の暫定値）。`sched_clonechurn` 自体は過去の Scratch Everywhere! の1,907.96 ops/msに対して最大1,138.96 ops/msで、まだ大差がある。

元データ `scratch-bench/results/perf-clone-ready-full-{on,off,on2,off2}-20261109/performance-xyo-cpp.json`、順位 `scratch-bench/results/perf-clone-ready-rivals-20261109.{md,json}`。

### 正しさ

* 直接結び付けるコードは `readyScript(top)` が実際に公開済みのネイティブコードを返したときだけ実行し、未準備なら既存のJIT判定を通す。
* `tests/unit/test_scheduler.cpp::testCloneMutationSchedulerAffinity` に、別のクローンが元スプライトのコンパイル済み `CompiledScript` を共有し、`mainThread` 属性が変化しないことの回帰テストを追加。
* `xyo-tests`（テスト追加版）: ON **1/1 PASS (41.52 s)**、OFF **1/1 PASS (41.57 s)**。ログ `build/perf-clone-ready-finalctest-{on,off}.log`。
* SDL GUI parity: ON **2123/2123**, OFF **2123/2123**（ともに100%）。ログ `build/perf-clone-ready-parity-{on,off}.log`。
* SDLビルド時、共有ツリーに別担当のDLSSコードのコンパイル問題があったため、検証専用CMake定義 `-DrealDlssEnabled_=false` を使用。他担当のソースは修正しない。
* 最終**既定ON**ネイティブReleaseビルド `cmake --build build-perf-clone-single-index-20261109 --target xyo xyo-tests -j4`: PASS。既定設定の `xyo-tests` も **1/1 PASS (54.30 s)**、`build/perf-clone-ready-default-ctest.log`。
* 最終**既定ON** SDL用独立ビルド `build/perf-clone-ready-default-sdl-frozen-xyo`（SHA256 `97e3d0b3405cdba0ad3da62f94069e149798de58d8da3ce3ba5df2f5fb5ae824`）のGUI parityも **2123/2123 (100%)**。ログ `build/perf-clone-ready-default-parity.log`。

他エージェントの変更に干渉せず、commit/pushなし。PERF-001の47/47達成には至っていない。

## JIT内部で削減できた処理の確認

追加のRelease計測ビルド `build-perf-clone-prof-full-20261109`（`-DXYO_DISABLE_RT_PROF=OFF`）で `XYO_PROF=1` を付け、同一バイナリ・CPU14固定、各設定を2回交互に測った。キャッシュONの場合はより多くのクローンが進行するため、**時間の累積値は総発生数が違う**ことに注意。

| 指標 | OFF 2回 | ON 2回 |
|---|---|---|
| `sched_clonechurn` ops/ms | 672.6 / 684.6 | **738.7 / 733.3** |
| JIT `compile` 経由の回数 | 327,597 / 328,797 | **3 / 3** |
| `pushThread` を含むJIT準備累積 | 116.1 / 109.5 ms | **87.6 / 90.4 ms** |
| `pushThread` allocの累積 | 50.5 / 48.2 ms | 50.5 / 52.9 ms |

`tryCompile` でコンパイル済みのスクリプトを直接拾えるため、クローンごとに繰り返していた `JitEngine::decideCompile / compile` 呼び出しをほぼすべて除去した。既存の生成・スレッド確保は引き続き実行される。ログ `build/perf-clone-ready-rtprof-{1,2}-{0,1}.{out,err}`。計測フラグによる負荷があるため、この数値を通常の性能スコアと混ぜない。

## 共有作業ツリーとの整合

作業中のv1.2.0リリースコミット `ddce625` が `src/model/target.cpp` のオプトイン計測機能を採用したため、削除途中の差分は撤回し、`src/model/target.cpp` を現在のHEADと完全一致するよう復元した。他エージェントの確定変更を保全した。PERF-001の自分の未コミット変更は `src/engine/thread.cpp` と `tests/unit/test_scheduler.cpp` のJIT直接接続および回帰テスト。

### 共有リリースコミットとの最終整合ビルド

`src/model/target.cpp` をリリースコミット `ddce625`（v1.2.0）と完全一致させた後で、専用のヘッドレスおよびSDLバイナリを再ビルドした。計測器は `XYO_CLONE_LIFECYCLE_PROF` 未指定では無効で、プロジェクト側の設定は維持される。

- 再ビルドのSDLバイナリ: `build/perf-clone-ready-release-frozen-xyo`、SHA256 `6ff91b4512b72b4e0fdc6d9eb19f45329741c7ab6bcd9e7c0ce4e0ed0efc685d`
- `xyo-tests`: **1/1 PASS (43.37 s)**、`build/perf-clone-ready-release-ctest.log`
- GUI parity: **2123/2123 (100%)**、`build/perf-clone-ready-release-parity.log`
- 変更を残した本体ソースは `src/engine/thread.cpp`、追加した回帰テストは `tests/unit/test_scheduler.cpp`。`git diff --check` はPASS。
