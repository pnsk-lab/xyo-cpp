# インタプリタのネスト warp 状態の検証済みスナップショット（2026-11-08）

## 動機

Linux RISC-V の `RISCV.powerOn` は実行途中のスタックに、手続き呼び出し、`control_for_each`、`control_if_else` と `control_repeat` が重なっている。
実測例：6フレームの制御スタックの末端が `repeat`、残りカウンター849、呼び出し引数のあるフレームが3つ。
既存 `XYO_JIT_OSR=1` はトップレベルのループのみを対象とし、ネストした warp 手続きには移行できない。

## 今回追加したもの

`src/engine/interpreter_osr_snapshot.hpp` に、**実行中インタプリタスタックの読み取り専用** `InterpreterOsrSnapshot` を実装した。

- ソース revision、対象 Target、元 Thread、Scheduler epoch、top hat を保持。
- 全フレームのソース block ID、命令種別、warp、isLoop、手続き呼び出し実行済み、ループの残り counter、`for_each` の index を保持。
- 引数 Value を `GcValueHandles` で確実にルート化。ソースのunordered_mapに依存しないよう引数名順で保存。
- レポーター途中、タイマー、Promise/startedThreads、未検証の拡張命令、非整数・異常カウンター、深すぎるスタックなどは fail closed。
- `validFor(Thread, epoch)` は Source/Target/thread identity、revision、スタックブロック、loop/warp/引数の完全一致を再検証。次のスケジューラ epoch では使用不能。
- `sourcePath()` は NEXT/SUBSTACK と実際の `procedures_definition` だけを辿り、元の手続き呼び出し元、IF/ELSE の選択分岐、戻り先 block ID を静的に検証。走査には4096ノード上限。未知の関数呼出し経路には踏み込まない。
- `hasCountedLoopCheckpoint()` は正確なrepeat counterのあるループ末端候補だけを示す。**それだけでは LLVM のネイティブ実行エントリではない**。

`src/engine/runtime.cpp` の `Runtime::step` 後の Scheduler safe point に opt-in
`XYO_INTERP_OSR_SNAPSHOT=1` 診断を追加。既定は OFF。実行は一切切り替えず、stateをログ取得後に破棄する。

## 回帰確認

`tests/platform/interpreter_osr_snapshot_smoke.cpp` は本物の `Runtime`、`Blocks`、`Thread` を使って次を検証する。

- 3段のループ／分岐／手続きフレーム、GC 管理 UTF-16 引数、数値引数を保持。
- 手続き引数を元の Thread 側から除去しても、スナップショットの Root で `Gc::collect()` を跨いで文字列が生存。
- ループカウンター、reporter状態、timer、source revision の変更で失効。
- 捏造した手続き定義リンクは `sourcePath()` が必ず拒否。

`tests/differential/interpreter_osr_snapshot.py` は実 `.sb3` を動的生成し、
引数付き warp 手続きを2段、IF/ELSE、10億回上限repeatと数値変数更新を動かす。
SDL offscreen Interpreter で5フレーム／4チェックポイントの観測を確認。
4つすべて `depth=4 roots=2 counted_loop=1 valid=1 lexical=1`。
両テストを CTest に追加。

## 未対応・次に必要なこと

この snapshot は**実行済み副作用を巻き戻さないための読み取り専用検査**であり、LLVM を途中から起動するコードは含まない。
実際のネイティブ OSR には、保存済み `returnPc` とネイティブ `CallFrame` の対応、引数以外のSSA一時値、
warpの時間予算、CommitOnceEffects の確定位置、再開先 BasicBlock が同一 source revisionに属する証明が必要。

Real Project Linux RISC-V で、本診断が無効でも現行共有ビルドのHotSpotで
`corrupted double-linked list` 等が発生する場合があった。診断単体の原因と
混同せず、別途ビルド/allocator経路を調べる。Interpreter-onlyの実.sb3では
実行・snapshot取得とも正常に終了。

関連: IR-001（共通 CallFrame/Return PC）, RT-001（正確なdeopt後の復帰）, IR-002（効果journal）。
