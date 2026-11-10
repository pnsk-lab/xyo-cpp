# LLVM JIT 起動遅延の内訳（独立計測）

**対象:** `work/jit-startup-profile-v123`。親コミット `a5f15b7`
（OSR-010〜016 / CI #81 全件 PASS）。

これは計測と原因分析であり、製品の JIT 最適化方針は変更していない。
`src/jit/backend_llvm.cpp` には、`XYO_JIT_OBJECT_TIMING=1` のときだけ
ORC オブジェクト変換層の境界時刻を測る診断コードを追加した。
フラグが無い通常実行では、新たなORC変換処理は登録されない。

## 計測の定義と再現条件

- ScratchBench はプロセス起動から最初の `output` リスト出力までを
  `startup_ms` とする。LLVM コンパイルのみの時間ではない。
- `tests/bench/jit_compile_phase_bench.py` を新規追加した。
  fresh process / CPU 固定 / 同一ファイルをランダム化した順序で交互実行し、
  最初の stdout 受信、プロセス完了、既存の
  `XYO_STARTUP_TIMING` / `XYO_JIT_TIMING` / `XYO_JIT_ASYNC_TIMING`
  / `XYO_JIT_OBJECT_TIMING`
  を分離して記録する。LLVM IR 関数数と命令数、SB3 の先頭スクリプトの
  直接手続き呼び出し数も診断する。
- `[startup] first-step` と `greenflag-spawn` は
  `runLoaded()` 開始を原点とする。これらと `cmdRun()` の
  `read-sb3` / `runtime-init` / `sb3-parse` の絶対値を
  同一原点として足し合わせない。
- LLVM 23.1.4, Release / SDL2 ON / RSVG OFF,
  binary SHA256
  `e86f7ed1d8f39602883fd2cdd87de7c47847be2b621f30e227340f365458d116`。
- **CPU 0 固定**、最初の4フレーム、`--fps 0`、`--print-list output`。
  まず遅延と最適化レベルを独立比較するため
  **`XYO_PARFOR=0`** に固定した。これはCIの既定構成とは異なる。
  テストプロジェクトを測定前に作業ツリーへコピーした。
- `performance.sb3` SHA256
  `216bf3184555341c5397aa30fcc30556936937e78e970d6a43b4179f3ce64ebb`。
  `parity.sb3` SHA256
  `63baccef6c7288cfb13778ef9a6015a6ee0e310af78d5378ce6bd3346c12f13b`。
  後者は**2,656件に増えた拡張版**で、既存CIの固定2,123件とは異なる。
- CPU共有・スケジューリングノイズがあるため、コンパイル最適化レベルの
  比較だけで正式な性能向上とは認定しない。

## performance.sb3: 3回交互測定

初回 stdout 到達時間の中央値、単位 ms。

| 設定 | 最初の出力 | JIT合計 | LLVM opt | ORC lookup |
|---|---:|---:|---:|---:|
| Interpreter | 122.1 | 0 | 0 | 0 |
| HotSpot / auto lazy | 363.4 | 166.0 | 0.4 | 138.5 |
| HotSpot / lazy off | 2,831.7 | 2,696.1 | 907.8 | 1,727.6 |
| Eager / O3 | 2,797.2 | 2,668.9 | 946.9 | 1,697.0 |
| Eager / O2 | 2,592.8 | 2,463.0 | 905.6 | 1,533.0 |
| Eager / O3, codegen less | 2,680.3 | 2,554.6 | 919.6 | 1,627.8 |
| Eager / O2, codegen less | 2,607.6 | 2,479.9 | 855.6 | 1,598.1 |
| Eager / O1 | 2,438.7 | 2,305.4 | 703.6 | 1,553.0 |

主要スクリプト `~Mv`: LLVM関数76個、IR約93,008命令、
先頭から `next` だけをたどった直接 `procedures_call` は10箇所。
`10 × 4 <= 76` なので、**par-for 無効の場合は**
自動遅延コンパイルが採用された。
O1 / O2 の起動時間は短いが**実行時性能とParityを検証していない**ため
既定O3を変更しない。

## PARFOR による遅延コンパイルの安全性ガード

`XYO_PARFOR` を通常の有効状態に戻して比較すると、
`performance.sb3` は関数80個／IR 93,547命令になり、
`xyo_par_for` コールバックが関数アドレスをネイティブ側へ渡す。
`backend_llvm.cpp` の `hasParForCallback` ガードは、未マテリアライズ
関数ポインターを渡したときに起こるSIGSEGVを回避するため、
自動遅延を拒否する。呼び出し密度ガードとは独立した条件。

LLVM 23 / Release の同一計測バイナリにて:

| `performance.sb3` | PARFOR=0 | PARFOR=1 |
|---|---:|---:|
| 選択したJIT tier | lazy | eager |
| IR関数数 | 76 | 80 |
| JITコンパイル合計 | 123.1ms | 2,260.0ms |
| `greenflag-spawn` | 134.9ms | 2,269.9ms |
| 最初のステップ完了 | 301.6ms | 2,369.3ms |

`XYO_JIT_OBJECT_TIMING=1` による `PARFOR=1` のフェーズ内訳:

- LLVM native target + LLJIT + symbols 初期化: **0.731ms**
- IR生成: **22.465ms**
- LLVM IR最適化: **785.588ms**
- ORC `lookup` から機械語オブジェクト完成まで:
  **1,449.814ms**（単一 `756336` バイトのオブジェクト）
- オブジェクト完成から `lookup` 復帰まで: **1.384ms**

`pre-object` はコード生成以外のORC設定作業も含む。
`post-object` はリンク／再配置／シンボル解決に関連する時間で、
純粋なリンクだけの測定ではない。
複数オブジェクトが同時に生成された場合、またはオブジェクトの完成が
別スレッドから通知された場合は `split=unavailable` とし、
別のコンパイルの結果を混同しない。計測カウンタは一貫した
mutex保護スナップショットから読み出す。

したがって、このケースのLLVMオーバーヘッドはIR最適化と
ネイティブオブジェクト生成が支配的であり、
リンク処理自体が主なボトルネックではない。
CIの約3.7秒HotSpot起動がローカルの`XYO_PARFOR=0`計測より
ずっと遅い主因として、この安全性ガードが特定できた。
**コールバックのアドレス生存保証なしにガードを撤去しない。**

## parity.sb3: HotSpotがEagerに切り替わる理由

主要スクリプト `~1rf` はLLVM関数241個、IR約439,563命令、
直接手続き呼び出し246箇所。`246 × 4 > 241` により
`chooseLazyCompile()` の呼び出し密度ガードが発動し、
HotSpot / auto でもEagerへ切り替わる。

単回目視値（初回 stdout 到達）:
Interpreter **156.7 ms**、HotSpot auto **11,186.5 ms**、
Eager O3 **11,342.0 ms**、Eager O1 **10,890.9 ms**。

2回交互測定では:

| 設定 | 最初の出力中央値 | プロセス終了中央値 | LLVMコンパイル |
|---|---:|---:|---:|
| HotSpot auto | 11,250.9 ms | 11,296.8 ms | 同期 10,689.0 ms |
| HotSpot lazy on | 2,371.0 ms | 2,448.7 ms | 同期 872.5 ms、手続きは遅延 |
| HotSpot async | 840.0 ms | 10,927.6 ms | 非同期 10,079.6 ms |

`XYO_JIT_ASYNC=1` は最初の応答を速くするが、数秒規模のCPU仕事量
を減らさず、短命の CLI では終了時にワーカー完了を待つ。

`--jit-lazy on` は開始だけなら大幅に速いが、拡張版のGUI Parity
（SDL offscreen・実験用設定）を比較したところ、auto が
**2625/2656**、lazy on が **2623/2656**。後者で
`Motion: Glide Timing and Interpolation` が追加失敗した。
後続のABBA確認でもautoは2/2回 `2625/2656`、
強制lazyは2/2回 `2623/2656` と再現した。
旧CIの固定2123件の100%と混同しないこと。
強制遅延をデフォルトにしてはいけない。

## performance.sb3: 非同期の短命CLIへの影響

2回の交互測定、中央値。

| 設定 | 最初の出力 | プロセス終了 |
|---|---:|---:|
| Interpreter | 117.6 ms | 425.5 ms |
| HotSpot auto | 340.7 ms | 651.9 ms |
| HotSpot async | 156.0 ms | 2,612.1 ms |
| Eager O3 | 2,547.5 ms | 2,855.3 ms |

非同期の初回出力は約半分だが、プロセス終了は約4倍遅い。
初回応答性と全体実行時間を混同しない。

## 次段階

1. 新しいオブジェクト層の計測点で大部分がオブジェクト生成以前と
   確認した。さらに厳密なLLVMバックエンド内部の各パスとJITLinkを
   計測するなら、ORC/JITLink自体への追加計測が必要。
2. par-forの関数アドレスをLLLazyJITで安全に扱うには、
   コールバックを分離して先に完全マテリアライズするなど、
   ABIと実行経路の設計が必要。安全性ガードを外すだけでは不可。
3. 呼び出し密度ガードを緩める前に、遅延版のグライド差分
   （時間依存/スケジューラ差/意味論差）を分離して全Parity再検証する。
4. CLIとGUIは起動時間の価値が異なる。GUIでは背景コンパイル、
   CLIでは短命プロジェクトの終了待ちも含めて評価する。
5. CIの `~3.7s` とローカル `~0.36s` には、
   上記 `XYO_PARFOR=1` と `0` の主要な条件差がある。
   残る差を検証するには同じSB3とビルド設定でGitHub
   ランナー上のフェーズを測る必要がある。

実測データ（コミット対象外）は
`build-startup-probes/performance-phases.json`,
`parity-async-matrix.json`,
`performance-async-matrix.json` に保存。

## 2026-11-11: 追加計測点の検証・確定

新規分離ブランチにコードを追加し、LLVM 23.1.4 Releaseで再ビルド。
5件の計測ログパーサー単体テストは全PASS。
CTest `xyo-jit-phase-profile-parser` を追加し **1/1 PASS**。
`xyo-tests`、`xyo-jit-lazy-integration`、
`xyo-interpreter-parity-regressions` は再ビルド後 **3/3 PASS**。

追加計測時のみ `ObjectTransformLayer` にフックを登録する。
`XYO_JIT_OBJECT_TIMING=1` は `XYO_JIT_TIMING` と独立して有効化でき、
いずれの診断フラグも指定しない場合は追加フックを登録しない。
単一オブジェクト・同一スレッドで観測した場合のみ、前後の時間を
分離する。複数オブジェクトや別スレッドでは安全側で `split=unavailable`。
`XYO_JIT_DUMP_OBJECT` の従来動作も維持する。実測で
756,336バイトのELF relocatable objectを正常に出力することを確認。
オブジェクトのファイル出力を同時に有効にすると、ファイルI/Oの時間が
`post-object` に計上されるため、純粋なリンク時間を求める測定では
併用しない。

3回の順序を変えたローカル測定、`performance.sb3`、
`XYO_PARFOR=1`、CPU 0固定、計測付きバイナリ
SHA256 `285c00f1c4670f3f29c992fd196a3454bd274bce9b560b18c471ba6d209079c7`。

| 設定 | 初回stdout中央値 | LLVM IR最適化 | オブジェクト完成前 | 完成後 |
|---|---:|---:|---:|---:|
| HotSpot / PARFOR=1 | 3,471.2ms | 1,279.0ms | 2,018.2ms | 1.4ms |
| Eager O3 / PARFOR=1 | 3,828.2ms | 1,456.5ms | 2,188.1ms | 2.5ms |

コールバックの安全性ガードによりHotSpotも同期Eagerを選択する。
3回の測定はホスト混雑によるばらつきが大きく、モード間の
約357ms差は速度改善の確定証拠ではない。
対象のORC `lookup` はオブジェクト生成前が大半で、
生成後の作業はこの計測では約1〜3ms。
全サンプルで `pre-object + post-object == lookup` を
0.1msの許容値で検証してPASSした。

フラグOFF / `XYO_JIT_OBJECT_TIMING=1` のみ /
`XYO_JIT_TIMING=1` と併用の3構成で、4フレームの標準出力が
バイト一致した（**PASS**）。診断フラグの有効時だけ
`[jit-object-timing]` が現れ、
`[jit-timing]` は従来どおり後者の併用時のみ出力される。

GUIの拡張版2656件Parityも、計測OFFとONの両方で
`2625/2656`（失敗グループも一致）。
これは拡張版の既存不一致31件があることを示しており、
本CIの固定2123件のPASSを意味するものではない。

計測を再実行する場合（SB3とバイナリは作業ツリー内で固定する）:

```sh
python3 tests/bench/jit_compile_phase_bench.py \
  --binary build-startup-jit/xyo \
  --project build-startup-probes/performance.sb3 \
  --configs hotspot_auto hotspot_parfor_on eager_o3_parfor_on \
  --object-timing --trials 3 --frames 4 \
  --output build-startup-probes/performance-object-matrix.json
```

このフェーズでは製品の最適化レベル、par-forガード、
JIT自動選択条件、非同期JIT既定値を**変更していない**。
GitHub Actions上のCIはこの独立した計測コミットに対して
**まだ実行していない**。
