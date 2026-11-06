# LLVM IR内でのwarpスケジューラ実行（2026-11-06）

## 対象と方針

Scratchのスケジューラ全体を関数内にコピーするのではなく、**副作用がないと証明された空のwarp `repeat`** のスケジューラ判定をJIT生成IRに直接表現する。Fiberの実際のコンテキスト切り替えは従来の関数を使う。

従来は `control_repeat` が空で、実行中がwarp、反復数が非負整数かつ `2^40` 以下の場合、`xyo_empty_repeat_warp(JitCtx*, double)` を呼んでいた。同関数は次の64反復のスケジューラ確認まで反復数をまとめ、必要なら `xyo_loop_yield` を呼ぶ。

新実装 `Codegen::emitInlineEmptyWarpRepeat()` は残り反復数をLLVMのローカルスロットに置き、現在の `yieldCheckN` から次の確認点までの反復を一括加算する。確認点で `yieldCheckN` を `counter + chunk - 1` に設定してから既存の `emitLoopYield()` を呼び、**従来と同じ64反復境界**で確認する。`emitLoopYield()` は共有時計と締切をatomic loadして比較するLLVM IRを既に生成している。実際のFiber切り替えだけが外部呼出しになる。

- コンパイル時に `XYO_JIT_INLINE_EMPTY_SCHED=0` なら従来ヘルパを使う。既定ON。
- 非warp、整数でない反復数、負の反復数、過大な回数は従来の `repeat` コードを使用する。
- 外側のループに個別の `curYieldCnt` がある場合、ネストされたカウンタの位相を乱さないため従来のヘルパを使用する。
- `XYO_PROF` を有効にした場合はプロファイラの意味を守るため従来ヘルパを使用する。
- 既存SSAループ（`repeat`・`for each`・変数コピー・帰納変数）にもwarpモード確認を省く実験を実施したが、`vars_setchange` で約8%悪化したため撤回。コードは残さなかった。

## A/B測定

**同一バイナリ**（SHA256 `57bfabc900a28136d1e0b5a8379423d82c0100ff4d62c3348200894ceff867a1`）でOFF/ONを環境変数のみ切り替え、CPU12に固定。各12ペアをランダム順に実行。Eager JIT、実行終了までのプロセスwall timeを測定（起動・JITコンパイル込み）。

| 独立生成Scratch作品 | 従来の時間の中央値 | LLVM IR版 | ペア速度比中央値 |
|---|---:|---:|---:|
| 1千万回の空warp `repeat` | 16.26 ms | 15.90 ms | 1.016倍 |
| 10億回の空warp `repeat` | 301.98 ms | 27.64 ms | **10.954倍** |

10億反復は12ペアすべてで改善。これは**空ループに限った特性の比較**であり、一般の作品や通常のループが10倍速くなるという意味ではない。

測定生データとスクリプトは `build/llvm-scheduler-study/empty-ab.json`、`build/llvm-scheduler-study/bench_empty.py`（ビルドディレクトリのためGit管理外）。

## 正しさとIR

`tests/differential/jit_empty_inline_scheduler.py` に非warp／warp別のSB3を生成。0、1、63、64、65、127、128、129、非整数、負数、文字列、動的回数、100001反復等を検証。インタプリタ対Eager JIT／HotSpotのON/OFF、共有時計有効／無効を比較。

LLVM IRダンプ `build/llvm-scheduler-study/empty-10m-0.ll` と `empty-10m-1.ll` を比較すると、ONでは `xyo_empty_repeat_warp` 呼出しが消え、`empty.scheduler.*` の制御フローと `xyo_yield` のcold pathが残る。

AOTでは2スクリプトをコンパイルし、`done=1` を確認。 さらに600億反復の空warp repeatを実行し、完了状態が一致することを確認した（yield発生回数そのものは未計測）。

## 今後

通常のScratchブロックを含むループで同様の処理を一般化するには、任意の状態変更・yieldを跨ぐ変数の再読み込み・GCルート・リスト再配置・例外的なScratch値の型変化を安全に保つ必要がある。現状でも汎用的なSSAループは64反復単位で計算をまとめられるが、全ループのスケジューラを無条件に削除・移植したわけではない。

## 最終検証

ビルド成功、CTest全25/25成功（イベントスケジューラの167秒回帰試験を含む）。最初のwarpモード判定省略案は性能悪化のため撤回し、空のwarp repeatのLLVM IRスケジューラ実装のみ採用。
