# 一般スクリプトのスタックレス継続（general coro）

2026-11-19 / Mega プリセットに含む（既定 ON）/ uncommitted

## 目的

Mega の同期バッチは yield しないスクリプトしか扱えない。一般の
Scratch スクリプトは非 warp ループで毎反復 yield するため、Fiber に
フォールバックしていた。本変更は **direct yield だけを持つ任意の
スクリプト** を LLVM コルーチンへ変換し、Fiber なしで継続実行する。

## 実装

- `stackless_coro_lower.hpp`:
  - `generalEnabled()`（`XYO_JIT_CORO_GENERAL=1`）と
    `generalCandidate()`（到達可能な手続きグラフに warp 定義が無いことを
    要求する判定）を追加。
  - `lower()` は既存の一般機構（direct `xyo_yield`/`xyo_yield_fast` を
    `llvm.coro.suspend` に変換、不透明サスペンドと `xyo_proc_` を拒否）。
  - ヒープフレームの base/size を `JitCtx::coroFrame` / `coroFrameSize` に
    公開。
- `codegen.cpp`: `stacklessNormalLoop` を narrow proof だけでなく
  `generalCandidate` でも立てる。非 warp ループの yield 点は既存の
  `emitLoopYield` が direct `xyo_yield_fast` を出す。
  - `control_wait` は `xyo_wait_begin` → `xyo_wait_poll` ループ +
    `xyo_yield` に展開（Fiber / coro 共通の意味論）。
  - `control_wait_until` は `xyo_wait_should_yield` 分岐 + `xyo_yield` に
    展開。coro 候補では event 依存の高速経路（`xyo_wait_event_yield`）を
    スキップするため、純変数条件でも coro 化できる。
  - `sensing_askandwait` は `xyo_sns_askbegin` + `xyo_yield` に展開。
    回答は `Runtime::emitAnswer` → `resolveWait` が coro を再開する。
  - `motion_glidesecstoxy` / `motion_glideto` は `xyo_mot_glidebegin_*`
    + `xyo_mot_glidestep`（0=完了 / 1=yield / 2=warp busy）の poll ループ。
  - `looks_sayforsecs` / `looks_thinkforsecs` / `sound_playuntildone` は
    begin（待機登録） + 1 回の suspend。
  - `looks_switchbackdroptoandwait` は `xyo_looks_backdropbegin` +
    `xyo_looks_backdropstep`（`waitThreads` と同じ判定）の poll ループ。
  - 到達可能な yield を持つスクリプトの `stop this script` は
    `xyo_stop_script_begin`（DONE 遷移のみ）+ return。`stop all` は
    `xyo_stop_all_begin`（stopAll + DONE）+ return。`retireThread` は
    `Sequencer::activeThread` の JitCtx を解放しないので、stopAll が
    呼び出し元自身を retire してもフレームは post-entry まで生存する。
  - `event_broadcastandwait` は `xyo_bcastwait_begin_v/lit` で受信者を
    起動し、`xyo_bcastwait_step`（0=完了 / 1=yield / 2=warp busy）を
    poll するループへ展開。join リストはヒープ所有で `JitCtx::joinRefs`
    から GC に可視。**ヘルパ名に `xyo_rt_` を付けない** — codegen は
    `xyo_rt_*` を mainThread 扱いにして Mega バッチの対象から外す。
- `backend_llvm.cpp`: `eligible` または `generalCandidate` で `lower()` を
  適用。手続き呼出しがある場合は先に `mega_goto::fuse`（budget 8192）で
  非 warp 手続きグラフを entry へ融合する。融合できない（再帰・過大）
  場合は `lower()` を諦め、通常の Fiber 経路へフォールバックする。
  一般コルーチンは `mainThread=false`（アクセス集合解析が worker-safe と
  判断する）。
- `sequencer.cpp`: コルーチン初回 entry でヒープフレームを GC root
  プロバイダに登録し、`scanRange` で保守的に走査。`releaseJitCtx` と
  `~Thread` で解除。`Gc::addRootProvider/removeRootProvider` と mark 中の
  走査は mutex で保護（worker からの登録に対応）。
- 並列実行: 一般コルーチンは `parallelEligible` の対象になる。
  `XYO_DEPGRAPH=-` で `(batch 2)` が観測できる。

## 対象になるもの / ならないもの

| スクリプト | 一般 coro |
|---|---|
| 緑旗・broadcast 受信・clone 開始ハット | 対象 |
| 非 warp ループ（repeat/forever/until/while/for_each） | 対象 |
| `join` などの文字列生成（yield を跨いで生存） | 対象（フレームを GC root として走査） |
| 自作ブロック呼出し（非 warp） | 対象（融合してから coro 化） |
| warp 手続き・再帰手続き | **対象外**（warp は candidate が false、再帰は融合失敗で Fiber） |
| `wait` / `wait until` | 対象（poll ループ + yield へ展開。event 最適化は coro 時にスキップ） |
| `ask and wait` | 対象（begin + 1 回の suspend） |
| `glide` | 対象（begin + step の poll ループ） |
| `switch backdrop and wait` | 対象（begin + step の poll ループ） |
| `stop this script`（到達可能な yield あり） | 対象（begin + return） |
| `stop all` | 対象（stopAll + DONE + return。手続き内は呼出し後の unwind） |
| `delete this clone`（トップレベル） | 対象（begin + 条件付き return） |
| sound set/change effect/volume | 対象（begin + miscLimits 時のみ suspend） |
| `say/think for secs` / `play sound until done` | 対象（begin + 1 回の suspend） |
| `broadcast and wait` | 対象（begin + step の poll ループ） |
| warp 手続き内ループ | **対象外**（手続き呼出しがあるため） |
| `--block-count` / `XYO_PROF` / OSR / AOT / WASM | 対象外 |

## 検証

`tests/mega/run_mega_suite.py` の `coro` 層（interpreter と完全一致 +
`lowered=1` を要求）:

- `general_loop` — 2 書き込み/反復のループ
- `general_join` — 文字列連結ループ
- `general_broadcast` — broadcast 受信ハットのループ
- `general_clone` — clone 開始ハットのループ
- `general_join_stress` — 5000 回の文字列成長（`XYO_GC_THRESHOLD=1` でも
  クラッシュ・不一致なし）
- `general_wait` — ループ内 `wait`。`lowered=1` で一致
- `general_wait_until` — `wait until` ループ。`lowered=1` で一致
- `general_procedure_wait` — 非 warp 手続き内の `wait`。codegen が既に
  inline 済みでも `lowered=1` で一致
- `general_bcastwait` — sender / receiver とも `lowered=1` で一致
- `general_bcastwait_none` — 受信者 0 でもハングせず `lowered=1`
- `general_bcastwait_loop` — ループ内 broadcast-and-wait。sender が
  `lowered=1` で一致
- `general_wait_until_var` — 純変数条件の `wait until`（通常は event
  依存最適化）。coro では poll 経路で `lowered=1`・一致
- 単体 `testCoroAskAndWait` — ask が Fiber なしで suspend し、
  `emitAnswer` で再開して回答を変数へ書く
- `general_glide` — Sprite の glide 0.02s。`lowered=1` で x 位置 100 が一致
- `general_saysecs` — say 0.01s 後の継続。`lowered=1` で一致
- `general_backdropwait` — 受信者なしの backdrop-and-wait。`lowered=1` で一致
- `general_stop_after_loop` — ループの後で `stop this script`。`lowered=1` で一致
- `general_stop` — 全 yield が到達不能な stop。`lowered=0`（通常 JIT）で一致
- `general_stop_all` — 直後の `stop all`。`lowered=0` で一致
- `general_procedure_warp_loop` — warp 手続き内の 1000 回ループ
  （a=1001）。warp ループが毎反復 suspend しないことを確認
- `general_procedure_warp_empty` — 空本体の warp repeat 5000 回。
  `lowered=1` で一致
- `general_sound_effects` — ループ内の音量/エフェクト設定（miscLimits の
  1 tick 待ちを含む）。`lowered=1` で一致
- `general_deleteclone` — クローン開始 → 5 回ループ → delete this clone。
  clone hat が `lowered=1` で一致（a=5）
- `general_deleteclone_in_proc` — 手続き内 delete this clone。
  `lowered=1` で一致（a=5、呼出し後の tail は走らない）
- `general_stop_all_in_proc` — 手続き内 stop all。`lowered=0` で一致（a=1）
- `general_stop_all_nested_proc` — 2 段ネストした手続き内 stop all。unwind が
  両方の呼出し元を伝播することを確認（`lowered=0`、a=1）
- `general_procedure_warp_bcastwait` — warp 手続きのループ内 broadcast-and-wait。
  `lowered=1` で一致（a=103, b=3）
- `general_clone_stopscript` — クローンの `stop this script`（clone は生存）。
  clone hat が `lowered=1` で一致（a=6）
- `general_stop_all_after_loop` — ループ後の `stop all`。`lowered=1` で一致
- `general_procedure_plain` — 非 warp 手続き（ループ入り）を 2 回呼ぶ。
  `fused=1` → `lowered=1`、interpreter と一致
- `general_procedure_warp` — warp 手続き。`candidate=0` / `lowered=0` で
  interpreter と一致
- `general_parallel` — 独立 2 スプライトのループ。`XYO_PAR=1` で
  depgraph の `(batch 2)` を確認し、最終状態は interpreter と一致

## 既定 ON と優先順位

`XYO_JIT_CORO_GENERAL=1` は Mega プリセットに含めており、素の
`xyo run` / `xyo test` では既定で有効。

- **source-certified バッチが優先**: `XYO_JIT_MEGA_BATCH_YIELD` が有効で
  `mega_yield::prove` が成功する形状は、従来の switch(pc) バッチが担当し、
  一般 coro は適用しない（`generalCandidate` が false を返す）。これに
  より、yield / multi-PC / if-else / until の既存最適化はそのまま残る。
- Fiber チェーン最適化を直接検査する 2 テストはコルーチン有効時に
  skip し、receiver 再起動テストは「Fiber またはコルーチン」を許容する
  よう更新した。
- プリセットの scheduler 単体テストは **899 checks / 0 failures**。

## 次の段

1. 再帰手続きの融合。sync バッチは「1 関数」不変条件のため、再帰
   スクリプトは現状バッチに入れない（`XYO_JIT_MEGA_BATCH_TRACE=1` の
   `[mega-skip] ... may-yield` / `[mega-proc] ... reason=recursive` で
   理由を確認できる）。
2. `wait until` の event 依存を保ったまま coro 化する（arm/poll の状態機械）。
3. HotSpot / renderer 接続時の Mega 経路。


## 検証の拡張（2026-11-19 追記）

coro 層は各 fixture を **4 経路**（interp / coroutine / Fiber JIT / HotSpot）で
実行し、Stage の完全一致を要求する。新しい begin/poll サスペンド点は
コルーチンだけでなく Fiber JIT でも使われるため、Fiber 経路の回帰も
ここで検出する。`lowered` の期待値は coroutine 経路の trace で判定する。
`XYO_JIT_CORO_TRACE=1` は `lowered=0` の理由（`[coro-reject]`）も出力する。


## 再帰 warp 手続きの同期バッチ（2026-11-19 追記）

`mayYield` の循環判定を精密化した。`elideLeafProcFrames` が「warp かつ
フレームを elide できる再帰」を `safeRecursiveProcs` として公開し、
到達解析の再入ではその関数だけ `false` を返す。非 warp の循環は
`fnDirect=true` のまま（`xyo_proc_push` が `mayRecurse=1` で yield し得る）。

`compileMegaDispatch` は融合失敗（`reason=recursive` など）でも、
`compiled.mayYield == false` なら手続き関数を **out-of-line のまま**
モジュールに残してバッチを設置する。バッチの worker は Fiber を持たないが、
残した手続きは yield 点を持たない（フレーム elide 済みの warp 再帰、
ループなし)。`[mega-batch] definitions=2` は dispatcher + 手続き関数。

- `general_recursive_warp` — 引数付き warp 再帰 `sum n = n + sum(n-1)`。
  2 本の緑旗でバッチが成立し、interp / coroutine / Fiber / HotSpot /
  preset の 5 経路で a=15。`[mega-batch] installed=2 kind=sync`。
- `general_recursive` — 非 warp 再帰。5 経路一致だがバッチ外（Fiber）。


## 再帰の追加検証と stop all の修正（2026-11-19 追記）

- `general_recursive_mutual` — 相互再帰 `f`/`g`（warp・ループなし）。
  `safeRecursiveProcs` が 2 関数の循環を扱い、sync バッチに
  out-of-line の 2 手続きとして参加（`installed=2`、a=15）。
- `general_recursive_stopall` — 基底ケースの `stop all`。`stopAll` が
  `Thread::stack` を空にした後もネイティブ呼出しの unwind が
  `xyo_proc_pop` に到達するため、`Thread::popStack`（空スタックは no-op）と
  `xyo_proc_pop`（`stackFrames`/`stack` が空なら return）を防御した。
  これがないと preset/coro で SIGSEGV/ハングした。
- `general_recursive_stopscript` — 基底ケースの `stop this script`。手続き内
  なので呼出し元へ復帰し、tail が走る（a=103）。
- `stop all`/`stop this script` は `canElideWarpRecursiveFrames` の
  allowlist 外のため `mayYield=true`（Fiber）のまま。


## warp ループ付き再帰のバッチ参加（2026-11-19 追記）

warp 手続き内のループ yield は cooperative（watchdog の時間予算のみ）で、
Scratch の warp はそもそも yield しない。codegen は warp 定義内のループ
yield を `callCooperativeYield`（`fnDirect` のみ、`fnSemanticYield` なし）で
emit し、`CompiledScript::cooperativeOnly` を計算する。sync バッチの
グループ化は `!mayYield || cooperativeOnly` を使い、worker では
`xyo_yield` が `c->fiber == nullptr` を見て skip する（バッチ child は
cooperativeOnly のみ）。

- `general_recursive_warp_loop` — `sumloop n = repeat n { a++ }; sumloop(n-1)`。
  sync バッチに参加し（`installed=2`）、5 経路で a=15。
- 非 warp ループ（`general_loop` + 2 本目の緑旗）は
  `may-yield (sync group)` で従来通り Fiber（`cooperativeOnly=false`）。


## stop を含む再帰のバッチ参加（2026-11-19 追記）

`stop all` / `stop this script` は `xyo_stop_all_begin`（stopped フラグ + DONE）
または手続きからの early return で、どちらも yield しない。`control_stop` を
`canElideWarpRecursiveFrames` の allowlist に追加したことで、warp 再帰の
フレームが elide され `mayYield=false` になり、sync バッチに参加できる。

- `general_recursive_stopall` — 基底ケースで stop all。2 本の緑旗で
  `installed=2`、a=3。
- `general_recursive_stopscript` — 基底ケースで stop this script（呼出し元へ
  復帰）。`installed=2`、a=103。
- `stopAll` が `Thread::stack` を空にした場合に備え、`Thread::popStack` と
  `xyo_proc_pop` の空スタック no-op は維持（elide されない経路の防御）。


## 実レシーバ付き switch backdrop and wait（2026-11-19 追記）

`general_backdrop_receiver` — ステージに 2 つの背景（SVG アセット付き）を置き、
`event_whenbackdropswitchesto` のレシーバを実際に起動する。送信側の
`xyo_looks_backdropbegin/step` がレシーバの完了を待つ join を検証する
（a=1, b=1、coro lowered=1、5 経路一致）。


## scratch-vm との差分ハーネスによる Mega 検証（2026-11-19 追記）

`tests/differential/run_repros.py` は xyo を 3 経路（Mega preset / `--no-mega`
の HotSpot / `--jit off`）で実行し、scratch-vm（TurboWarp 2.1.46、コンパイラ
無効）の出力と比較する。19 repro すべてで 3 経路一致（2026-11-19）。

注: `tests/fixtures/scratch-parity-test.sb3` は 25 秒の自前タイムアウトを
持つ巨大パリティテストで、interpreter でも `timeout` で失敗する
（この環境の実行時間依存）。JIT 経路が終了しないのはこの fixture の
タイムアウト設計によるもので、Mega 固有の回帰ではない。


## ネスト手続き内の wait（2026-11-19 追記）

`general_procedure_wait_nested` — `outer` が 2 回 `inner wait` を呼び、内側の
手続きに `wait 0.001` がある。`mega_goto::fuse` が両方を entry へ融合し、
コルーチンが内側フレーム内で suspend する（lowered=1、a=2、5 経路一致）。


## マルチレシーバ / クローン同時継続（2026-11-19 追記）

- `general_bcast_storm` — 送信側 1 + レシーバ 3。各レシーバは
  `repeat 3 { wait 0.001; a++ }`。送信側の `broadcast and wait` が毎回
  3 スレッドすべての完了を待つ（a=45）。4 スレッドとも coro lowered=1。
- `general_clone_wait` — フラグが 3 クローンを生成し、各クローンは
  `repeat 5 { wait 0.001; a++ }`（a=15）。クローンごとの JitCtx/コルーチンが
  同時に待機する。clone hat は lowered=1、5 経路一致。


## ステータス・マトリクス（2026-11-19 最終監査）

| スクリプトの形 | 経路 | 検証 |
|---|---|---|
| yield なし（自作ブロック含む） | sync バッチ（融合） | 41 fixture / paired |
| warp 再帰（ループ・stop 含む）、相互再帰 | sync バッチ（out-of-line、yield-free） | fixture + batch trace |
| 一般ループ / wait / wait until | 一般コルーチン | fixture × 5 経路 |
| broadcast / broadcast-and-wait（複数レシーバ） | 一般コルーチン（join） | bcast_storm ほか |
| backdrop-and-wait（実レシーバ） | 一般コルーチン（join） | backdrop_receiver |
| glide / say / play sound / ask / stop all / stop script / delete clone / sound 設定 | 一般コルーチン（begin + poll/条件 yield） | fixture |
| clone（生成・待機・停止・削除） | 一般コルーチン（clone ごとの JitCtx） | clone_wait ほか |
| **非 warp 再帰** | Fiber（Scratch の再帰 yield のため） | general_recursive |
| async reporter（拡張） | Fiber | 未検証 |
| renderer 接続 / HotSpot | 対象外（別実行モード） | — |

`xyo test` の既定（Mega プリセット）と差分ハーネス 19 件（scratch-vm と
3 経路一致）で意味論を確認済み。既知の失敗は MLIR テスト 1 件のみで、
本作業とは独立（revert 実験で確認）。


## play sound until done のコルーチン検証（2026-11-19 追記）

`general_playuntildone` — 有効な WAV アセットを同梱した
`sound_playuntildone`。ヘッドレス（オーディオデバイス無し）では
`xyo_snd_playbegin` の後すぐ解決するが、コルーチンは一度 suspend して
継続する（lowered=1、a=1、5 経路一致）。


## 実プロジェクトでの検証と既知の JIT 差（2026-11-19）

`tests/mega/run_real_projects.py` は実プロジェクト（3DOcean /
voxelRenderer_fast / linux / 3D2 / tetrisai）で、まず interpreter を
2 回走らせて決定性を確認し、決定論的なものだけ Mega と比較する。

- 3DOcean / linux / tetrisai: `timer`・乱数・実時間依存で interpreter の
  2 回でもダンプが異なる → SKIP（有効なオラクルにならない）。
- 3D2: 決定論的、interpreter と Mega で一致。
- voxelRenderer_fast: interpreter は決定論的だが **JIT では 1 フレームあたりの
  進捗が大きい**（`chunk z` 19 → 約 500、20 フレーム）。JIT は yield した
  Fiber を同じフレーム内で再開するため、interpreter よりループが進む。
  `XYO_JIT_MODE=eager`（Mega 無効）でも再現する既存のスケジューラ差で、
  Mega 固有ではない（`--danger` でも interpreter は 19 のまま = warp 予算の
  問題ではない）。最終状態は同じでもフレーム数固定の比較には使えない。


## 非 warp 再帰の手続きコルーチン（2026-11-19）

`stackless::lowerRecursive` が再帰手続きグラフを「yield from」変換する。

- 再帰手続きの各関数を独立したコルーチンにし、呼出し元は
  `callee()`（最初の呼出しで停止するまで実行）→ `xyo_coro_top_frame` →
  `coro.done`/`coro.resume` のループ → まだ停止中なら呼出し元も
  `coro.suspend` で停止、で再開を伝播する。
- フレームは `malloc` され、`xyo_coro_push_frame/top/pop` で JitCtx の
  リンクリストに登録される。停止中は Sequencer の GC root provider が
  全フレームを `Gc::scanRange` で走査する。destroy は呼出し元が所有。
- 再帰呼び出しの yield は codegen が
  `xyo_proc_push_ex(..., mayRecurse=1, deferYield=1)` +
  `if (r && xyo_recursion_yield_due(ctx)) xyo_yield(ctx)` へ書き換え、
  コルーチン変換が `coro.suspend` にする（判定は `xyo_loop_yield` と同一）。
- **warp 手続きを含むグラフは対象外**（warp 再帰は sync バッチのまま。
  warp 手続きのコルーチン化は LLVM の CFG 破壊を招いたため明示的に除外）。
- 再帰 yield を明示形へ書き換えるのは `XYO_JIT_CORO_GENERAL=1` のときだけ
  （Fiber JIT は従来の `xyo_proc_push` を使い、MLIR frontend 経路に影響しない）。
- `XYO_JIT_CORO_RECURSION=0` で無効化。`XYO_JIT_CORO_TRACE=1` で
  `[coro-rec] ... lowered=1` を確認できる。

検証: `general_recursive`（自己再帰）、`general_recursive_mutual_nonwarp`
（相互）、`general_recursive_loop_nonwarp`（ループ付き）、
`general_recursive_strings`（3000 段の文字列成長 = ネストフレームの GC
root）、`general_recursive_stopscript_nonwarp`（stop this script）、
`general_recursive_bcastwait_nonwarp`（broadcast and wait）を 5 経路で完全一致。再帰 + `wait` も一致。warp 再帰
（`general_recursive_warp*`）は従来通り sync バッチ。

`general_recursive_stopall_nonwarp`（再帰から戻りながら `stop all`）も
5 経路で一致（a=1, n=0）。あわせて interpreter の `execute` は
stop all で retire された直後に残りのブロックを実行しないよう
`isKilled` を確認する（防御）。


## warp 手続きの再帰もコルーチン化（2026-11-22）

- warp 手続き内の**予算 yield**（64 反復ごとの `xyo_warp_yield_due` 経由）は
  cooperative な Fiber 切替なので、codegen が `!xyo.coop` メタデータを付け、
  再帰 lower では**停止点にしない**（warp ループだけの手続きは実 yield が
  無く、従来通り sync バッチ/Fiber）。
- warp 手続きに `wait` などの**実 yield** があれば、再帰グラフごとコルーチン
  化する（`general_warp_rec_wait` で lowered=1・5 経路一致）。
- `lowerRecursive` は**変換前の一括検証**を行う: 半分だけコルーチン化した
  状態で後から失敗すると不正 IR になるため、全関数の yield/return/内部
  yield ヘルパを先に検査し、失敗時はモジュールを変更しない
  （`general_recursive_warp_loop` の CFG クラッシュはこれで解消）。

追加 fixture: `general_recursive_stopscript_nonwarp`（stop this script）、
`general_recursive_bcastwait_nonwarp`（broadcast and wait）、
`general_warp_rec_wait`（warp 再帰 + wait、lowered=1）。
