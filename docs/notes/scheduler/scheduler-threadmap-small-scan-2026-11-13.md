# Thread IDマップ最適化調査（2026-11-13）

共有作業ツリーで未コミット。既定設定の動作は変更しない。

## 背景

`XYO_SCHED_CLEANUP_PROFILE=1`での短命Thread負荷1回の結果:
removed=2,141,329、map_hits=2,141,329。
map_size_0=0、map_size_1=1、map_size_2=831,228、map_size_3_4=26,202、map_size_5plus=1,283,898。
削除の約39%は2要素マップが対象。ほかの約60%は5要素以上。

## 小型mapのポインタ走査

`src/engine/sequencer.cpp`で`XYO_SCHED_THREADMAP_SMALL_SCAN=1`を追加。既定OFF。
mapサイズ4以下の場合だけ`unordered_map`をiteratorで走査し、
**Thread*と完全なScript IDの両方**が一致する場合だけerase(iterator)を使う。
5要素以上では従来と同じhash lookupを継続する。
同じIDの新旧Threadを誤削除しないガードは維持する。

### 独立C++マイクロベンチマーク

同一キーのinsert/eraseを40万回、6組、CPU16固定。
マップ内要素数2: hash 22.7〜24.2ms、pointer scan 19.1〜20.2ms。
要素数3: hash約27.5ms、scan約23.6ms。
要素数4: hash約31ms、scan約27ms。

実験ソース: `build-scheduler-rewrite-20261111/compact-suffix/bench-small-threadmap.cpp`。

### ScratchBenchとの比較

同一CLI・CPU16固定・12組の交互測定。スコアは高いほどよい。

| 対象 | OFF中央値 | ON中央値 | 対応ペアON/OFF | ON勝利 |
|---|---:|---:|---:|---:|
| sched_clonechurn | 367.742 | 361.464 | 0.9856倍 | 4/12 |
| sched_threads | 12062.087 | 12133.063 | 0.9996倍 | 6/12 |

結果: `build-scheduler-rewrite-20261111/compact-suffix/bench-small-map-scan/results.json`。
実際の処理では改善せず、**既定OFF**を維持する。

## 回帰

`tests/unit/test_scheduler.cpp`に4件/5件のThreadマップ、
生存・完了Threadの順序、ID一致、別名キーへの誤削除防止を追加。
専用compactionテスト: small scan ON/OFFとも **28/28 PASS**。

## 新規Threadの計測

`src/engine/runtime.cpp`の既存`XYO_PROF`プロファイラには
`mapNs`欄があるが値が蓄積されていなかった。
Thread生成+スロット登録、threadMap登録、JITコンパイル/初期化を
別々にナノ秒で積算するよう修正した。
既存Releaseビルドでは`XYO_DISABLE_RT_PROF=ON`なので計測コードは含まれない。
既存ビルドの設定を変更せず、`build-scheduler-prof-20261113`を
`-DXYO_DISABLE_RT_PROF=OFF`で別途構成している。

## Thread生成時のJIT・StackFrame最適化

Releaseプロファイルを別ビルドで実行した。
build-scheduler-prof-20261113 は XYO_DISABLE_RT_PROF=OFF。
元の build-scheduler-rewrite-20261111 のCMakeフラグは変更していない。
src/engine/runtime.cpp の既存XYO_PROFに未計測だったmapNs加算を追加、
さらにtryCompile()とinitializeThreadRoot()を独立した計時区間とした。

約154万回のpushThread()、XYO_JIT_MODE=eager、clonechurn選択SB3:
- alloc=182.8ms
- map insert=114.5ms
- tryCompile=339.8ms
- root initialization=99.5ms
- native shortSync=0件
- native non-short、mayYield=false、mainThread=false=1,238,745件
- その他native=301,117件
- インタプリタ=0件

src/engine/thread.cpp に XYO_JIT_READY_EAGER_FAST=1（既定OFF）を追加。
Eager JITかつJITキャッシュがすでに有効なら、通常の
decideCompile/compile二重マップ照会を省く。
Edge-activated hat、numeric common CFG、Fiberlessなsuspendable code、
AOTソースrevision等のガードを維持。HotSpotには適用しない。
テスト: キャッシュ再利用とedge-activated拒否の集中テストが4/4 PASS。

src/engine/runtime.cpp に XYO_LAZY_ROOT_NATIVE_WIDE=1（既定OFF）を追加。
compiled->entryがあり、mayYield=false、mainThread=false、
native procedures/lazy/WASM/coroがない場合、initial StackFrameを省いて
初期root IDだけをpushする。
Thread::peekStackFrame()/pushStack()/popStack()の遅延生成に従う。
12命令の非short同期nativeを使う検査はON/OFF両方5/5 PASS。

### ScratchBench A/B

同一通常Releaseバイナリ、Eager、CPU16固定、12組の交互測定。
スコアは大きいほどよい。いずれも明確な速度改善なし。

| 実験フラグ | 項目 | OFF中央値 | ON中央値 | ペアON/OFF | ON勝利 |
|---|---|---:|---:|---:|---:|
| READY_EAGER_FAST | sched_clonechurn | 385.838 | 385.608 | 1.0032倍 | 7/12 |
| READY_EAGER_FAST | sched_threads | 12363.430 | 12349.007 | 0.9965倍 | 5/12 |
| LAZY_ROOT_NATIVE_WIDE | sched_clonechurn | 400.000 | 400.380 | 0.9983倍 | 7/12 |
| LAZY_ROOT_NATIVE_WIDE | sched_threads | 12212.205 | 12248.133 | 1.0056倍 | 7/12 |
| LAZY_ROOT_NATIVE_WIDE | sched_pingpong | 448.961 | 460.052 | 1.0248倍 | 7/12 |

結果:
- build-scheduler-rewrite-20261111/compact-suffix/bench-eager-ready-fast/results.json
- build-scheduler-rewrite-20261111/compact-suffix/bench-wide-native-root/results.json

いずれも計測では可能性が見えたが、実ベンチでは安定した改善に
結びつかなかった。全て既定OFFの実験フラグとして残す。

## 全体回帰

同一Releaseバイナリ、JIT=eager、Mega一括・if/else・branch pair、compaction suffix、small-map scan、Eager ready fast、広域lazy-rootを同時にON:
- Scheduler **628/628 PASS**
- その他の全テスト群も0失敗、終了コード0
- build-scheduler-rewrite-20261111/compact-suffix/full-extended-on.out
- 待機通知レジストリONのcompaction集中テストも **28/28 PASS**
- git diff --check PASS

既存Releaseの構成を変更せず、profiler用CMakeは独立ビルドのみ。

他エージェントの変更や既存バイナリはreset/上書き/commitしていない。
