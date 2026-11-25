# Mega Scheduler β — LLVM直接スカラー加算の実験と性能測定

日付: 2026-11-13
状態: 実装済み・検証中 / opt-in / 未コミット / 既定OFF

## 実装概要

`XYO_JIT_MEGA_DIRECT_SCALAR=1`で、source-certified Mega-Yieldの
`data_changevariableby`を、従来の`xyo_var_change`呼び出しから
LLVM内の load → fadd → store に変更する。

条件は変数の実体が正しく解決され、現在値が**通常のIEEE754数値**
（NaN以外）であること。文字列、真偽値、色、未定義、タグ付きNaN、
または変数が欠けている場合は元の`xyo_var_change`へ遷移する。

- `src/jit/backend_llvm.cpp`: 6引数の共有dispatch ABIを生成。
  数値のタグと順序性を確認し、数値ならLLVM内で加算して正準化し、
  Variable::valueを直接更新する。遅い条件は既存ヘルパーを呼ぶ。
- `src/jit/jit.hpp`: `MegaDispatchCode`に、スクリプトごとに
  本体と呼び出し元の戻り処理に使う変数IDを保存。
- `src/engine/thread.hpp` / `thread.cpp`: Thread所有の5本分の
  `Variable*`スロットと有効性フラグを末尾に追加。
- `src/engine/sequencer.cpp`: 各スレッド選択時に
  Target/Stage変数マップの世代番号を確認し、変化したら
  保存済みの`Variable*`をすべて失効させる。
  未解決変数が1つでもあれば**全スロットをnull**として
  元ヘルパーへ戻す。これは、同じLLVM sliceの途中でヘルパーが
  変数を作成しても、既存の直接ポインタを無効化しないため。
- `src/jit/jit.cpp`: 通常のgreen flag / broadcast receiver
  のインストール・キャッシュ命中・Thread再利用時にスロットをリセットする。

対応するsourceを持つスレッドとクローンはそれぞれ固有のJitCtxと
変数ポインタを保持する。ターゲットをまたいでポインタは共有しない。

待機通知`XYO_WAIT_WAKE_REGISTRY=1`が有効なとき、
直接ストアからも`xyo_wait_notify_var_store`を発行する。
無効時は通常JITと同様に不要な通知呼び出しを省略する。

LLVM最適化は引き続きO1。追加のGC所有値や文字列ポインタを
`MegaDispatchCode`に保存しない。変数ポインタはThread所有で
structural generation検査後の1スケジューラslice内に限り使用する。

## 検証

- `testMegaYieldSwitchDispatch`: 型付きNaN、数値文字列、
  真偽値、実行途中の文字列変更を経て通常の数値更新へ戻る
- 既存の変数削除→同じIDで再作成する休止中スレッドの回帰
- `testMegaBroadcastCloneSourceSharing`: 複数Targetの
  ローカル変数を正しく分離し、コードキャッシュを再利用
- `testMegaBroadcastReceiverFusion`: 単発Scalar＋repeat＋
  通常JITフォールバック混在・broadcast-and-wait join
- 直接更新ONのxyo-tests: **scheduler 508/508 PASS**、
  他のxyo-testsスイートも全件PASS
- 直接更新OFFのxyo-tests: **scheduler 502/502 PASS**
- `XYO_WAIT_WAKE_REGISTRY=1`を併用しても直接更新ONの全体テストは**scheduler 508/508 PASS**。

最初の直接更新ONで全体テストの後半に一度SIGSEGV (rc139) を観測。
同一バイナリ・同条件の再実行は終了コード0で全件PASSしたため、
この一度の原因は特定できていない。メモリ破壊が起きていないことを保証するものではない。

## ScratchBench 実測

`sched_fanwait`の元Scratchブロックを変更せず、該当ケース以外の
スイート呼び出しを除外。同一実行ファイル、CPU16固定、eager LLVM JIT、
8組交互測定。数値はスループット（大きいほど良い）。

| 条件 | 変数 | 中央値 | ペア幾何平均速度比 |
|---|---|---:|---:|
| Mega受信側統合・従来ヘルパー | `DIRECT_SCALAR=0` | 804.6185 | 基準 |
| Mega受信側統合・LLVM直接更新 | `DIRECT_SCALAR=1` | 885.3075 | **1.0988x** |
| 通常JIT | `MEGA_BCAST_RECEIVERS=0` | 857.9335 | 基準 |
| Mega受信側統合＋LLVM直接更新 | `MEGA_BCAST_RECEIVERS=1` | 890.4515 | **1.0365x** |

ONが勝ったペアはそれぞれ4/4、8/8。なお比較対象ごとに
実行した組数が違い、中央値を横断して一意の速度比にしていない。

測定データ:
- `build-scheduler-rewrite-20261111/mega-direct/sched-fanwait-direct-ab/results.json`
- `build-scheduler-rewrite-20261111/mega-direct/sched-fanwait-normal-ab/results.json`

実行例:

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_BCAST_RECEIVERS=1 \
XYO_JIT_MEGA_DIRECT_SCALAR=1 \
./build-scheduler-rewrite-20261111/tests/xyo-tests
```

ScratchBenchの全`sched_*`および実プロジェクトでの改善は未確認。
生成LLVM関数内の命令・動的フォールバック割合・C++ thread選択時間の
分解計測も未完了。上流の既定値は変更しない。

他エージェントの未コミット作業はリセット・コミットせず保持する。
