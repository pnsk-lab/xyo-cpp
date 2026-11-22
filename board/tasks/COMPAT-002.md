---
id: COMPAT-002
title: WASM/Binaryen backend の未対応機能を解消
status: in_progress
owner: agent-binaryen-gpt6
priority: P1
updated: 2026-11-16
---

## Scope

WASM/Binaryen backend に残っている「Interpreter へ落ちる未対応機能」を、
モデル/スケジューラの意味論を変えずに順に解消する。結果の一致ではなく
**そのスクリプトが実際に WASM で実行されること**を増やすのが目的。
LLVM と同一の機能集合にするのではなく、正しさを検証できるものから広げる。

## Inventory (2026-11-16 時点)

- [x] 依存自己再帰（warp）: tail / prefix+if / 最小形、数値引数≤2、
  1024フレーム燃料。超過は `wasm return=1` で Interpreter へ。
- [x] 3-4戻り先 / 3-4引数の numeric warp 再帰
  （PC-stack emitter の上限を4へ。ホストは実引数数で frame buffer を確保）
- [x] ループ/待機を含まない非 warp 再帰（warp と同じ bounded 機構。yield を含む再帰は未対応）
- [x] 非数値引数の再帰は実行時タグ検査で bail（frame に文字列ポインタを残さない）
- [ ] 文字列/非数値引数を持つ再帰（GC root 付きフレーム）
- [x] warp 手続き内の数値入れ子ループ（nested WASM を既定で warp-only 有効化。`XYO_NESTED_WASM=0` で無効）
- [ ] 非 warp の入れ子ループ（nested WASM の ALL モードは opt-in のまま）
- [x] top-level ループ 128 個まで（旧 cap 32。129 個以上は hybrid で安全に解釈）
- [x] edge-activated ハット（`event_whengreaterthan` 等）: headless run が step を続けるよう修正
- [ ] motion / pen / sound / looks / sensing のホストブリッジ
- [ ] wait / その他 yield を含む区間のコンパイル
- [ ] OSR / 非同期コンパイル / 並列スケジューラ
- 対象外: ネイティブ AOT・ネイティブ機械語（ブラウザでは原理的に不可）

## Acceptance criteria

- [x] 未対応リストをコードと docs から棚卸しする
- [x] 依存自己再帰を WASM で実行できるようにする
- [ ] 各残項目を実装し、LLVM/Interpreter とのパリティを回帰に入れる

## Files / dependencies

src/wasm/wasm_compile.cpp/.hpp, src/jit/backend_wasm.cpp,
src/wasm/hot_jit.cpp, src/ir/execution_abi.hpp,
tests/differential/wasm_warp_procedure.py, docs/jit-backend-compatibility.md.
Dependencies: RT-001 / IR-001 for general yield continuation.

## 2026-11-16 progress: bounded warp recursion

- `Program::allowBoundedRecursion` を追加し、whole-script / hybrid の
  HotSpot emitter が `WarpTailRecurse` / `WarpReturnStack` /
  `WarpReturnPcStack` を 1024 フレーム燃料付きで放出できるようにした。
  `Until`/`While`/`ForEach` は従来どおり `allowBoundedConditionalLoop`
  必須のままで、yield 意味論は変えない。
- 最小形（本体が `if (n>0) call self(n-1)` のみ）も tail として認識する。
- 燃料超過は既存の transactional bail（`wasm return=1`）で Interpreter に
  復帰する。2000 段の再帰で全 staged 書き込みが破棄され、最終値が
  LLVM/Interpreter/Builtin/Binaryen で一致することを確認。
- 検証: `wasm_warp_procedure.py` 11/11（recursive_tail / recursive_deep /
  recursive_min を追加）、compat sweep 18/18（quick）、2123 チェック
  byte 一致（1899/2123、94 compiled）、effect fallback 8/8、native CTest 3/3。

## Next

- 3-4 引数/戻り先への拡張（frame buffer の `kMaxNumericArguments` を
  実際の引数数に合わせる）。
- edge-activated ハットの body コンパイル可否を確認して追加。
- 入れ子ループは nested WASM の既定化可否を性能込みで検討。

## Round 2 (2026-11-16): frame safety, 4-arg recursion, 128 loops

- **重要な修正**: whole-script の PC-stack 再帰は `invokeSegment` が
  `slotIds + 3` ワードしか確保しておらず、生成コードがフレームを
  その先へ書いていた。`HotCode::recursionFrameWords` を追加し、
  `AtomicWarpExecutionAbi::frameWords(実際の引数数)` を
  `slotIds + 3 + recursionFrameWords` として確保するようにした。
  hybrid region にも同じメタデータを設定。
- PC-stack の戻り先/引数上限を 2 → 4 に拡張。3引数の再帰
  (`recursive_three_args`) と suffix 付き (`recursive_suffix`)、
  2戻り先 (`recursive_two_sites`) を追加し、LLVM/Interpreter/
  Builtin/Binaryen の一致を確認。
- top-level ループ上限を 32 → 128 に拡張。40/128 ループは
  whole-script compile（`loops=128`, 63KB, 0.16s）、129 ループは
  hybrid で値一致。新規 `tests/differential/wasm_loop_limit.py`
  （境界 40/128/129）を sweep に追加。
- 検証: warp suite 14/14、compat sweep 19/19 quick、2123 チェック
  byte 一致、effect fallback 8/8。
- edge-activated hat (`event_whengreaterthan`) は全エンジンで発火せず、
  ランタイム側の調査が必要（WASM 固有ではない）。

## Round 3 (2026-11-16): nested warp loops default-on

- `Sequencer` は `XYO_NESTED_WASM` 未設定なら nested JIT を **warp-only** で
  生成するようにした（`XYO_NESTED_WASM=0/off/false` で無効、
  `=1/on/true` は従来どおり全候補 + `XYO_NESTED_WASM_WARP_ONLY` 尊重）。
  warp ループは yield しないため、反復を1呼出しにまとめても
  Scratch のスケジューリングは変わらない。
- 実測（100k 反復の数値 warp 入れ子ループ）: WASM interpreter 0.099s、
  nested off 0.113s、nested on 0.087s。小さいループでは昇格オーバーヘッドが
  勝つ場合があるため opt-out を用意。
- 新規 `tests/differential/wasm_nested_default.py`: 既定で
  `warp_runs>0` かつ LLVM/Interpreter/Builtin/Binaryen 一致、
  `XYO_NESTED_WASM=0` で nested 統計が出ないことを検証。sweep に追加
  （20 スイート）。
- 検証: sweep 20/20 quick、2123 チェック byte 一致（1899/2123、94 compiled）、
  execute fixtures 115/116（既知の `control-stop-all-leaks.sb2` のみ）、
  effect fallback 8/8。
- 参考: `event_whengreaterthan` は全エンジンで発火せず、ランタイム側の
  edge-hat 評価が未実装（`updateEdgeActivatedValue` の呼出しがない）。
  WASM バックエンドの範囲外として残す。

## Round 4 (2026-11-16): headless edge-activated hats

- 原因: `runLoaded` はアクティブスレッドが無いと `Runtime::step()` を
  呼ばずに終了していた。edge hat は step 内で毎フレーム評価されるため、
  フラグハット等が無いプロジェクトでは一度も発火しなかった。
- 修正: `Runtime::hasEdgeActivatedHatScripts()` を追加し、該当スクリプトが
  ある場合はスレッド終了後も `--frames` 上限まで step を続ける。
  上限到達時にスレッドが無ければ `done`（rc=0）。
  `XYO_HAT_TRACE=1` で poll/spawn/遷移の診断ログ。
- 検証: timer 条件ハット単体（他スレッド無し）で native interp/LLVM と
  WASM interp/builtin/Binaryen が `x=42`、閾値未達は `x=0`。
  `tests/differential/edge_hat_activation.py` を追加し sweep 21 スイートへ。
- `xyo test` の fixture ループも同じ扱いに統一（スレッドが無くても edge hat
  スクリプトがあれば 30ms 間隔で再評価、25 秒の wall limit は維持）。
  native/WASM の execute fixtures は従来どおり 115/116（`control-stop-all-leaks.sb2`
  のみ、4エンジン共通）で、実行時間の退行なし。

## Round 5 (2026-11-16): non-warp synchronous recursion + numeric guard

- 非 warp 手続きでも、本体がループ/待機を含まない（yield しない）なら
  warp と同じ bounded recursion（tail / non-tail / PC stack）でコンパイル
  するようにした。prefix/suffix にループが入る形は既存の synchronous
  検査が拒否し、hybrid/Interpreter に残る。
- 再帰引数（tail の `argumentExprs`、PC stack の `nextArguments`）に
  実行時の数値タグ検査を追加。数値以外なら frame/local に格納する前に
  `bail()` して Interpreter に戻す。未 root の SStr ポインタが frame に
  残って GC で無効化される経路を塞ぐ。
- 新規 fixture: `nonwarp_recursive_tail`（コンパイル）、
  `nonwarp_recursive_suffix`（PC stack・コンパイル）、
  `nonwarp_recursive_loop`（yield するため hybrid/Interpreter）、
  `recursive_string_arg`（`join` 引数で guard が bail、`wasm return=1`）。
- 検証: warp suite 18/18、sweep quick、2123 チェック、native CTest。
- 参考: 無限再帰（`"ab" > 0` が文字列比較で真になる形）では LLVM JIT が
  SIGSEGV することを観察（Interpreter は frame limit まで走る）。
  ランタイム/LLVM 側の深さ制限は別途の課題。

## Round 6 (2026-11-16): compiled sensing_timer

- `EK::Timer` を追加し、`sensing_timer` を数値式として lowering。
  モジュールには Runtime が無いので `Runtime::step()` 先頭で
  `ioDevices.clock.projectTimer()` をスナップショットし、host import 27 が
  scratch へ f64 を書く（ステップ中 `currentMSecs` は一定なので interpreter と同値）。
- 新規 `tests/differential/wasm_timer_reporter.py`（22 スイート目）:
  `if timer > -1`=42、`if timer > 100000`=0、`repeat until timer > -1`=0 回、
  `7 + timer*0`=7 を 4 エンジンで比較し、コンパイルされたことも確認。
- 検証: sweep 22/22 quick、2123 チェック byte 一致、native CTest、execute fixtures。

## Round 7 (2026-11-16): mouse x/y/down

- `Runtime::step()` が有効期間中だけ Runtime* を host に公開（終了時 nullptr）。
  host import 27-30 が timer / mouse x / mouse y / mouse down を読む。
- `sensing_mousedown` は数値文脈で 0/1 f64、タグ文脈では kTrue/kFalse に変換
  （join 等で boolean タグを保持）。型不整合は wasm validator で検出されたため、
  `emitTaggedExpr` に専用変換を追加した。
- 新規 `tests/differential/wasm_mouse_reporters.py`（23 スイート目）:
  `set x to mouse x`=0、`mouse x + 7`=7、`if mouse down ... else`=2、
  `if not mouse down`=42 を4エンジンで比較。
- 診断改善: instantiate 失敗時に Node でも validator メッセージを出す。
- 検証: sweep 23 スイート、2123 チェック、native CTest、execute fixtures。

## Round 8 (2026-11-16): loudness + nested sensors

- `sensing_loudness` を EK::Loudness / host import 31 でコンパイル。
  オーディオ無しは -1、有りは interpreter と同じキャッシュ
  (`cachedLoudness`/`loudnessTimer`/`currentStepTime`) を共有。
- nested 数値プログラムの `numericExpressionsOnly` に Timer/MouseX/MouseY/
  MouseDown/Loudness を追加。step 中は Runtime* が公開されているため、
  nested warp ループ内のセンサー条件も WASM で評価できる。
- 新規 `tests/differential/wasm_loudness.py`（24 スイート目）:
  `set x to loudness`=-1、`loudness+2`=1、`if loudness < 0`=42、
  `repeat until loudness < 0`=0回 を4エンジンで比較。
- 検証: sweep 24 スイート、2123 チェック、native CTest、execute fixtures。

## Round 9 (2026-11-16): wider recursion frames

- bounded recursion の引数上限を 4→8、戻り先サイト上限を 4→8 に拡張
  （tail は 8 引数、PC stack は 8 引数・8 サイト）。frame stride は
  `AtomicWarpExecutionAbi::frameWords(nArgs)` で動的なので、ホスト予約
  (`recursionFrameWordsFor`) もそのまま追随する。
- 新規 fixture: `recursive_five_args_tail`（5引数 tail, x=4）、
  `recursive_five_args_pc`（5引数 PC stack, x=y=4）、
  `recursive_five_sites`（5戻り先, x=32/y=160）。warp suite 21ケース。
- 検証: warp suite、sweep、2123 チェック、native CTest、execute fixtures。

## Round 10 (2026-11-16): commit-only terminal effects

- `SK::Effect` と host import 40 を追加し、region の最終文に限り
  `motion_setx/sety/changexby/changeyby` を WASM から適用。
  `looks_setsizeto/changesizeby` も同じ機構で有効化。
  `sound_setvolumeto/changevolumeby`（0..100 クランプ）も追加。
  motion の point in direction/turn right/turn left、
  pen の clear/down/up/stamp/set size/change size/color param set・change/
  color-to-color、
  looks の effect set/change/clear と show/hide も追加。
- 安全条件: (1) 効果は main の最終文のみ、(2) 効果を含む region は数値
  書き込みのみ（commit 後の数値検査で rollback → 再実行 → 二重適用を防ぐ）、
  (3) 効果の後に guard を置かない、(4) helper 失敗時のみ rollback。
- 効果オペランドは allocation-free な数値式に限定（文字列は interpreter）。
- 新規 `tests/differential/wasm_motion_effects.py`（25スイート目、18ケース）:
  set/change x/y、size set/change、volume set/change、NaN の size 変更、
  変数オペランド、
  文字列オペランドのフォールバック、非数値 prefix 後の change x が
  二重適用されないこと。
- 2123 パリティの "Size clamping and rounding" 差は、効果引数の NaN を
  primitive の `num()` と同じく 0 に正規化していなかったことが原因。
  helper で NaN→0 にして解消（compiled=106、全出力一致）。
- 検証: sweep 25 スイート、2123 チェック、native CTest、execute fixtures。

## Round 13 (2026-11-16): sensing_username + invoke-scoped Runtime

- `sensing_username` を EK::Username / host import 32 でコンパイル。
  空文字は kEmptyStr、非空は `Runtime::usernameRoot`（markRoots で root）が
  所有する SStr。比較は tagged compare、join は tagged join 経由。
- 発見: `greenFlag()` の即時ハット評価は `Runtime::step()` の外で module を
  呼ぶため、Runtime ポインタが null だった（timer/mouse/loudness の helper が
  静かに bail）。`invokeSegment` で `thread.target->runtime` を
  publish/restore するよう修正。
- 新規 `tests/differential/wasm_username.py`（26 スイート目）:
  username="" / ="abc" / 文字列順序 / join を4エンジンで比較。
- 検証: sweep 26 スイート、2123 チェック、native CTest、execute fixtures。

