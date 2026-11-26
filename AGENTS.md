# xyo-cpp

## マルチエージェント作業（最初に確認）

同時作業のタスク・担当・連絡は [BOARD.md](BOARD.md) を正本とする。作業開始前に `bun tools/board.ts list`、`bun tools/board.ts claim <TASK-ID> <agent-id>` を実行し、編集するファイルをタスクの投稿で宣言する。未担当の作業でも、他エージェントの未コミット差分は消さない。

Scratch/TurboWarp 完全互換の LLVM JIT 対応高速ランタイム（C++20）。
設計の正典は `docs/design.md`。参照実装の移植元は `refs/scratch-vm`（TurboWarp）。

## docs の構成

`docs/` 直下は設計書・ガイド・主要リファレンスのみ。分類ディレクトリは `bench/`（ベンチ・互換性）`core/`（文字列・リスト）`jit/`（JIT 設計メモ）`render/`（描画・ペン）`platform/`（OS 別）`extensions/` `wasm/` `scheduler/` `release/`。日付付き実験メモは `docs/notes/` 以下（`jit/` `llvm/` `rt/` `osr/` `scheduler/`（`mega/` 含む）`aot/` `perf/` `list/` `string/` `wasm/` `ir/` `interpreter/` `core/` `misc/`）に置く。新しい実験メモも該当する notes/ サブディレクトリへ入れること。

## 状態

- M0/M1 完了: 値・キャスト・数値変換（jsnum、node 差分 4 万件一致）・文字列・GC・リスト・ランタイムヘルパ。
- M2 完了: モデル層・SB3 ローダ・インタプリタ・コアブロック全実装。
- M3 完了: Runtime/Sequencer（hats/clone/warp/broadcast/stopAll detach）移植。
- SB2 ローダ完了（specmap 機械生成）。
- **sb3 minify（`xyo minify <in.sb3> [-o out.sb3]`）**: `src/load/sb3_minify.*` が project.json と zip を**直接**書き換える（`extensionStorage`/`customFonts` 等も保持）。既定は可逆で、zip deflate・ID 短縮（TW `tw-compress-sb3.js` 準拠）・未参照アセットと到達不能ブロックの削除を行う。`--aggressive` は未使用の変数/リスト/broadcast も削除（`analyzeScript` で集計）。オプション: `--strip-comments`/`--keep-ids`/`--keep-assets`/`--keep-orphans`/`--level N`/`--store`。検証 **71/71・assert 222/222**。
- **構造解析（`xyo analyze <project.sb3> [--dot g.dot] [--top N] [--scripts N]`）**: `src/analysis/project_analysis.*`。実行せずに最適化候補を出す（R/W は `engine/deps` の `analyzeScript` と共通、`AccessNote` が直列の理由を出す）。`total blocks` は孤立 shadow を別計上（3D2 は 2894/孤立 5326/warp 候補 1）。`--dot` は衝突グラフ DOT、`--complexity`（`complexity.*`）は静的 Big-O 見積り。テスト `tests/unit/test_analysis.cpp` / `test_complexity.cpp`（41 checks）。
- **par-for（並列 append ループの JIT 特殊化、`src/jit/parfor.*`）**: warp 中の `repeat` 本体が「1 カウンタ + 各リストへ append」だけなら、append を `base+iter` 書き込みへ privatize して反復を分割する（`xyo_par_for`）。不適格や trip<512（`XYO_PARFOR_MIN`、チャンク `XYO_PARFOR_CHUNK`）は逐次へ落とす。`XYO_PARFOR=0` で無効、`XYO_PARFOR_DEBUG=1` でログ、AOT では無効。実測 10M 反復 525ms → **182ms（2.9×）**。3D2 のホットループは不適格（`XYO_JIT_VERBOSE=1` に `parfor skip` と出る）。テスト `tests/unit/test_parfor.cpp`。
- **matpow（線形漸化式を行列累乗 O(log N) へ、`src/jit/matpow.*`）**: warp 内 `control_repeat`/`repeat_until`/`while` の本体が状態変数の affine 更新のみなら `xyo_matpow` で行列累乗に置き換える（条件付きループは単調なら二分探索で停止反復数を求める）。検出は保守的で（RHS は +−*/・リテラル・floor/ceiling/round、係数 |c|≤2^45）、f64 bit 一致を範囲証明する（|v|≤4.5e15、`stmtBound·max(W)<2^52`）。**warp 中のみ**、`XYO_MATPOW=0` で無効、`XYO_MATPOW_DEBUG=1` でログ。MLIR 経路も `xyo.matpow.try`。実測 `repeat(2e9){change x by 3}` 536→**37ms**。テスト `tests/unit/test_matpow.cpp`（115 checks）。
- **DP 変換（純粋な再帰手続きの自動メモ化、`src/load/dp_transform.*`）**: ロード直後に warp 定義・引数1個・純粋（変数/リスト読み書き・乱数・broadcast・wait 不使用、文字列生成 op 禁止）・自己再帰・値付き return の reporter 手続きを `__xyo_dp_*` リストによるトップダウン DP へ書き換える（len<8192 の線形照合）。`XYO_DP=0` で無効。fib(30) 159→**39ms**。テスト `tests/unit/test_dp.cpp`（34 checks）。
- **ループ不変 pen setter のホイスト（`codegen.cpp`）**: warp 手続き内ループの先頭に連続する `pen_setPenSizeTo`/`pen_setPenColorToColor`/`pen_setPenColorParamTo` の引数がループ不変なら、ガード（repeat は `n >= 0.5`、forever は無条件）付きでループ外へ 1 回だけ出す（until/while/for_each 未対応、`XYO_PENHOIST=0` で無効）。実測 10M 反復 223→200ms（約 10%）。3D2 には効かない。テスト `tests/unit/test_pen_hoist.cpp`。
- **sb3 の機械的書き換え（`xyo rewrite <in.sb3> [-o out.sb3] [--no-warp] [--no-inline-temps] [--warp-min N] [--report]`）**: `src/load/sb3_rewrite.*` が project.json を書き換える。(1) yield する op を含まないループ持ち手続きへ `warp="true"`（`--warp-min` 既定 40）。(2) `set v=<純粋式>` 直後の条件への一時変数インライン化。検証 `tests/unit/test_rewrite.cpp`（19 checks）。3D2 では 1 手続きだけ warp 化され、実行時間は変わらない。
- **3D2 の実測メモ（詳細は `docs/bench/`・`docs/render/`）**: draw ループは基準 2368ms に対し `pen down` 除去 −20% / `pen set color` 除去 −14% で、`[rtprof] pen` は 383ms・残り約 1.8s は JIT ループ本体。`xyo rewrite --minify --aggressive` は孤立 shadow 5326→0 にするが 90 フレームの時間は変わらず、効いたのは **AOT の −21%** だけ。`item # of` は 90 フレームで 123 calls / 2,066 要素のみで O(1) 索引は不要。ヘッドレスは緑旗が `wait until <mouse down>` で止まるので `XYO_INJECT="5:md:240,180"` + `XYO_EXIT_FRAMES=90` + `SDL_VIDEODRIVER=dummy` で回す（AOT で 4.27s → 3.38s）。
- M4 完了: LLVM ORC JIT（`src/jit/`）。execute フィクスチャ **116/116** が両モードでパス（assert: JIT 336/336、インタプリタ 339/339）。ユニット 231 + e2e JIT 85 / interp 80 + pen 31 + renderer JIT 879 / interp 878 パス。renderer の ASan 検証もパス（`detect_leaks=0`、後述の既知リークを除く）。
- JIT カバレッジ拡大（M4b）: operator/list/motion/looks/sensing/control/sound の全コア opcode を native helper 経由で lowering。ベンチ（warp 演算ループ）で interp 比 **3.4×**。
- 並列スケジューラ完了（design.md §4.6）: スレッド間メモリ依存を静的解析して衝突グラフを構築し、独立成分を worker pool で並列実行（`src/engine/deps.*`, `pool.hpp`, `sequencer.cpp`）。独立 8 スプライトの変数ループで **5.4×**。検証フィクスチャは `tests/fixtures/par/`（生成器: `gen_par_fixture.py`）。
- SDL2 ウィンドウレンダラ完了（design.md §10.4）: `xyo play <sb3>` でスプライト/ペン/モニタ/バブルを表示し、キー・マウス入力と green flag/stop UI を備える（`src/render/`、SDL2 + librsvg/Cairo/Pango）。**renderer 接続中も独立した計算スクリプトは並列実行する** — ターゲット更新・描画/ペン・可変IO・互換実行・共有GPU呼出しは mainThread/unknown/spawn として除外し、その直前で先行バッチを join する。
- 完全対応完了: 全7視覚エフェクト（`src/render/effects.*`、pick/タッチングと同一ビットマップ）、スライダモニタ・スプライトドラッグ、ask and wait バー、オーディオ（`src/audio/sdl_audio.*`、WAV/SBV/MP3、`playSoundAndWait` で `resolveWait`、pitch/pan、loudness）。バブルは `Target::setSay` に一元化。
- ペン拡張完了（`src/blocks/pen.cpp`）: scratch3_pen 全 opcode（色/サイズ/レガシー hue/shade）。`Target::penState` + `onTargetMoved` フック（penDown 中のみセット、clone に複製）。`RendererAPI` に `createPenSkin/penLine/penPoint/penClear/penStamp` を追加（`Runtime::penSkinId`）。
- **HotSpot JIT（既定の実行経路）**: インタプリタで走らせ、ホットになったら JIT へ昇格する（`tryCompile`→`decideCompile`、トップ `Block*` 単位）。しきい値は `XYO_HOT_INVOCATIONS`（3）/`XYO_HOT_BLOCKS`（4096）/`XYO_HOT_SIZE`（5000）、`XYO_HOT_LOOP=0` でループ初回 JIT を無効化。モードは `--jit=hotspot|eager|off`（`XYO_JIT_MODE`、`XYO_JIT_DISABLE=1`）で、`eager` はテスト/ベンチ用（`xyo test`/ctest 既定）。**AOT は `xyo aot` 生成 exe か `--aot <obj>` のみ**。2 段ティアは不採用（+5.5s の純損失）。
- **Mega スケジューラ一括プリセット（`--mega` / `XYO_JIT_MEGA=1`）**: `docs/notes/scheduler/mega/` の 39 本の実験を 1 スイッチで有効化する（`src/engine/mega_preset.*`）。`xyo run`/`xyo test` は JIT 未指定なら**既定でこのプリセット**（`--no-mega`/`XYO_JIT_MEGA=0`/`--jit`/`--block-count` で従来経路、`play`/`gui` は対象外）。yield しないスクリプトは `mega_goto::fuse(force=true)`、warp ループ yield は cooperative、`stop` 系もバッチ参加。検証 **905 checks / 0 failures** + `run_mega_suite.py` 9 fixture 一致 + `bench_mega_*` 全 PASS。未完了: グローバル goto・HotSpot・renderer 接続・AOT/WASM。詳細は `docs/notes/scheduler/mega/scheduler-mega-complete-2026-11-19.md`。
- **一般スクリプトのスタックレス継続（`XYO_JIT_CORO_GENERAL=1`、既定 ON）**: 非 warp ループで yield するスクリプトを `stackless::lower` で LLVM コルーチンへ（非 warp 毎反復 / warp 64 反復 `stacklessWarp`）。wait/glise/sayforsecs/broadcastandwait/stop 等は begin+step+yield 形へ展開し、フレームは `JitCtx::coroFrame` で GC が走査する。`mainThread=false` なら worker 並列可。非 warp 再帰は `lowerRecursive` がグラフ全体をコルーチン化（`XYO_JIT_CORO_RECURSION=0` で無効）。未対応: async reporter・renderer 接続・HotSpot・`--block-count`/`XYO_PROF`/AOT/WASM/OSR。検証は 42 fixture で 5 経路一致（**905/0**）。詳細は `docs/notes/scheduler/mega/mega-general-coro-2026-11-19.md`。
- **MLIR num guard での numericOnly 更新省略（`lower_llvm.cpp`）**: replace/add/insert の fast path で `SList::numericOnly` の read-modify-write を、書き込み値が必ず数値 bits と証明できるとき省略（証明は `numericBitsGuaranteed` — `from_int`/小 `xyo.const`/numeric 証明済み読みに加え `box.f`/`from_num` は `numFacts` 非 NaN を使う）。実測 micro_write hotspot 73→44ms（legacy 59ms を逆転。legacy は常に書くが numFacts が効くのは MIR だけ）。
- **MLIR const-int mod の整数 fast path（`emitModF64`）**: 除数が正の i32 定数で被除数が i32 範囲の整数なら `fptosi→srem`（2 冪なら and）。`rem==0` は `copysign(0,a)` で -0.0 を保持。sha256_65536 hotspot 83→72。
- **MLIR par-for（`lower_llvm.cpp` の `emitParForTry` + `emitPendingParFors`）**: legacy と同じ並列 append 経路を MIR 上に移植 — warp `control_repeat` に `analyzeParFor`、適合なら `xyo.parfor.try`（`xyo.pfwrite`/`xyo.pfctr`）+ `xyo.if`（else は逐次）。ゲート/trip/bases/`parForMinTrip`（`XYO_PARFOR_MIN` 既定 512）/`items[bases[k]+iter]` 直書きは legacy と同形。純粋な warp 手続き呼出しは `pushLoopScope` 折り込みで効果を LoopScope に畳む。実測 micro_append hotspot 27→16ms（legacy 15ms）。
- **MLIR frontend の Mega 対応（`src/mlir/`、`XYO_MLIR=1` または Mega プリセット）**: `scratch_build.cpp` の `directStackMap`/`directGetMap`（~80 opcode）で setter/reporter を `xyo.direct`/`xyo.direct.get`→ランタイム helper 直呼びへ（compat 呼出し 109→0）。融合・コルーチン（`BuildOptions.coroMode`）・再帰（`xyo.call` attr + `lowerRecursive`）も対応。検証: `XYO_JIT_MEGA=1` で 0 FAIL（749 emit/0 fallback/1 skip）、`xcheck.py` 45/45。実測 motion ループ compat 4270→native 3541ms（legacy 3423）。`XYO_MLIR_DIRECT=0`/`XYO_MLIR_DIRECT_GET=0` で旧経路。
- **danger モード（`--danger` / `XYO_DANGER=1`、既定 off）**: スレッドごとの実行時間予算（warp 500ms、`warpTimeMs`）を外す opt-in。interp は `warpBudgetExhausted`、JIT は `isStuck` を丸ごと飛ばすので、純粋な計算ループは譲らず走り続ける（計測・変換検証用）。**待機系は danger でも譲る**（`wait`/`wait until`/`broadcast and wait` はハングしない）。非 warp ループは「毎反復 yield」の意味論なので 1 反復ずつ進む。`run`/`play`/`test` と AOT 実行ファイルが受け、**実行開始前に決める**。テスト `tests/unit/test_danger.cpp`。
  - **danger での最適化実測（採用 1・不採用 1）**: interp が時計を読まない分だけ 1M `change` warp ループ 204→199ms（約2%）。**JIT の yield 点除去は不採用** — `xyo_loop_yield` を IR から落とす変種は 200M `change` で 696–711→743–755ms と悪化。`dangerCache_` と codegen 特殊化は撤回、`XYO_DANGER_NOYIELD` も無し。
- **AOT コンパイル（実験的、`src/aot/`）**: `xyo aot <project.sb3> -o <app>` が全スクリプトをオブジェクト化し、自己 exe 末尾へ `[sb3][object][footer]` を追記した単体実行ファイルを作る（`./app` で再生、`./app run` でヘッドレス）。アドレスは `xyo_aot_slot_N` グローバル経由で `JitEngine::loadAot` が束縛する（GPU 並列ループも `GpuProgram` slot で再コンパイル）。sb3 ハッシュ不一致や AOT 不能な script は JIT/interp へ回す。`--obj <file>`・`--aot <obj>` も可。検証: execute 115/116・mandelbrot byte 一致・3D2（56 scripts/275 slots）・TFG・parity。
- **ランチャー GUI（Qt Quick: `src/shell/gui_qt.cpp` + `qml/Launcher.qml`）**: 引数なしの `xyo` または `xyo gui [dir]` で Qt6 Quick ランチャーが開く。`.sb3`/`.sb2`/dir を一覧して実行でき、[AOT 実行]/[AOT 書き出し]（`<名前>-aot`、非対応は JIT）を備える。サムネイルは `~/.cache/xyo/thumbs`、起動フォルダは `XYO_PROJECT_DIR`。自動検証は `XYO_GUI_ACTIONS`（`XYO_GUI_DEBUG`/`XYO_GUI_SHOT`/`XYO_GUI_EXIT_FRAMES`）。Qt は `slots` をマクロ定義するため `gui_qt.cpp` は `#undef slots` してから xyo ヘッダを読む。
- **WASM ビルド（Emscripten、Phase A）**: `cmake/emscripten.cmake`。**interp のみ**（ブラウザ WASM に ORC JIT 無し）。Node headless CLI とブラウザ `play`（SDL2 canvas + Asyncify → `xyo.html`+`xyo.js`+`xyo.wasm`）。検証: execute **116/116**、headless Chrome で実機確認（`docs/wasm-browser.png`）。JIT/GPU は CPU フォールバック、SVG はビットマップのみ、並列無効。詳細は `docs/wasm.md`。
- **WASM JIT（Phase C、土台 + JS ブリッジ完成）**: `src/wasm/wasm_emit.hpp`（wasm バイナリエンコーダ）、`host_bridge.*`（生成モジュールは `env.memory`+`env.h` import）、`wasm_compile.*`（ABI `f(i32 slots)->i32`、変数直接 load/store、非数値は bail）。`-DXYO_WASM_JIT=ON` で selftest/execute 116/116 検証済み。yield しないスクリプト限定。残は JitEngine/Sequencer 組込（`docs/wasm.md` §5）。
- **Scratch 互換の修正（差分ハーネス）**: `tests/differential/` に repro 19 件（`run_repros.py`）。修正 16 件: NaN インデックス SIGSEGV・2^53 超 `String(x)`・JIT `mod`/`sin/cos`/逆三角を V8 fdlibm・`jsRound` 統一・非 BMP `toLowerCase`・`jsLog`・Final_Sigma・Unicode 17 新小文字・`data_listcontents` 型判定・`sensing_timer` 整数 ms・JIT 変数キーを `(id,name,list)` へ。検証 `run_repros.py` 19/19・`test_scratch_compat` 90 checks・execute 116/116・ctest 6/6。詳細は `docs/bench/scratch-divergences.md`。
- **リスト長上限（`--list-limit N` / `XYO_LIST_LIMIT=N`、既定 0 = 無制限）**: `xyo::xyo_list_limit` を `SList::push`/`insert0` が見る（超過で push drop・`insert "last"` no-op）。JIT inline 経路・AOT も同シンボル経由で効く。`--list-limit 200000` で Scratch/Mistwarp と同じ挙動。テスト `tests/unit/test_list_limit.cpp`。
- **Scratch の有名アルゴリズム・ベンチマーク（`tests/bench/algos/`、結果は `docs/bench/algo-bench.md`）**: quicksort/mergesort/bubblesort/sieve/fib/sha256 の sb3 を xyo と `refs/scratch-vm`（TurboWarp 2.1.46）で比較する（`bench.py` が checksum を Python 参照と照合、全 30 構成一致）。実測（i7-12700/Node 26/LLVM 23）: TW compiled に対し xyo は 1.4〜2.2 倍遅いが、fib・sha256・`change i by 1` は xyo が速く、`add k to L` 20M は TW 163ms に対し 20ms で **8.2 倍**速い（sieve は構築を計測外）。`--danger` との差は 0〜5%。
- **ループ内変数のレジスタ昇格とタグ変換 fast path（`codegen.cpp`）**: emitToNumF64/CmpNumF64 に素 double fast path・定数デルタで `numCanonical` 省略・`Variable::list`/`len`/`items` の preheader hoist・inline 完結ループでの alloca 昇格（id で重複排除、yield 前後だけ spill/reload）・`isWarp`/`yieldCheckN` のレジスタ化・**数値影** `(bits, d)` 対・`branch_weights`・`isNumericOperand` 条件畳み。**不採用（実測）**: `fptosi.sat`・ループアンロール・リテラル比較の無条件畳み・`d==d`・`bits<kNan` 版・リスト読み高速化 4 案。実測: quicksort 60→46、mergesort 114→85、bubblesort 183→123、sieve 37→21、fib 43→36、**micro loop 161→35**、read 285→180、write 247→135。
- **TW 生成コードを V8 まで降ろした比較（`docs/bench/v8-vs-xyo-codegen.md`、取得 `dump_v8.js`）**: `testingApparatus` フックで TW 生成 JS を取り `node --print-bytecode`/`--print-opt-code`（xyo 側は `aot --obj`+`llvm-objdump`）。TW の速さの源は **Smi 型特化**（変数は JS ローカルへ昇格せず）。xyo は i64 タグ値でタグ検査+NaN 正規化が入り micro_loop 2.98 vs 1.06 ns/反復。fib/sha256 で勝つのは TW の文字列キー手続き参照のため。
- **scratch-bench との統合（`scratch-bench/`、結果は `docs/bench/scratch-bench.md`）**: `bun run bench --runtime xyo-cpp`。各カーネルは `days since 2000` 計測でリスト `output` へ追記するため `xyo run --print-list NAME` と `--fps N` を追加（**`--fps 30` が要る**）。アダプタは `XYO_JIT_MODE` で variant 計測。実測: **xyo 26,213 / TW 40,265 / Scratch 653**。パリティは `xyo play --print-list`+offscreen+`--list-limit 200000` で **2061/2123（97.1%）**。
- **rope（cons string）で `++` を O(1) に（`sstr.hpp`/`string_util.cpp`/`gc.cpp`）**: `s = s ++ "ab"` が O(n²) になる（`str_join` 1 ラウンドで 800MB memcpy）。`SStr` に `flags`/`left`/`right`/`body` を足し、合計 16 文字以上ならコピーせずノードを返す（`sstrCons`、初回 `view()` で materialize）。JIT inline fast path は `flags & kConsFlag` で helper へ、GC mark は `pending_` を反復消化。**昇格との干渉**: 数値影は「代入値が数値と静的に言える変数」にだけ付ける（`nonNumericVars`）。実測 `str_join` 209 → **5,401 ops/ms**。テスト `tests/unit/test_sstr_cons.cpp`（29 checks）。
- **GC の確保を速くする（`gc.hpp` の `PtrSet`）**: `Gc::alloc` の `unordered_set` 2 つをフラットなオープンアドレッシング `PtrSet` へ（1 ノード ~200 → 100〜150ns）。`join(x,"")`/`join("",x)` はコピーせず、`sstrContains` は ASCII なら小文字化せず走査。実測 `str_join` **12,514 ops/ms**、overall **26,213**。**残る差は確保ではなくボクシング**（`math_arith` 0.25× — `docs/bench/v8-vs-xyo-codegen.md`）。
- **小文字列のスラブ確保（`gc.hpp`/`gc.cpp`）**: 128B 以下の `GcType::Str` を 64KiB 整列スラブ（80/96/128B 3 クラス）から切り出し、生存は割り当てビットマップで持つ（プール内セルは `objSet_` に入れずレイアウト不変）。所属判定は「登録済みスラブ→範囲内→セル境界一致」で確定。ASan は未使用セルを poison、`XYO_GC_POOL=0` で malloc。**スレッド専有スラブ**にしてロックを外す（`poolGen_` 世代一致のみ見る）。実測: 文字列連結 68→38ms、数値 append 181→97ms。テスト `test_gc_pool.cpp`（10,079 checks）。残る主因は helper 呼出しで、`emitLength` による `operator_length` インライン化で ~5ns/回を消した。
- **小整数のインターンと ASCII 判定キャッシュ（`cast.cpp`/`sstr.hpp`）**: (1) `cast::toString` は `-128..1023` を起動時に 1 回だけ文字列化して共有。(2) `join` に `(数値,文字列)`/`(数値,数値)` 融合経路。(3) `SStr::flags` に ASCII 判定を遅延保存（`kAsciiKnown`/`kAsciiValue`）。実測: 数値 append 157→36ms、短文連結 133→57ms、`contains` 1471→725ms（TW 逆転）。テスト `test_sstr_cons.cpp`（65,617 checks）。差分 19/19・ctest 6/6。
- **スレッド側の AccessSet キャッシュ（`thread.hpp` / `sequencer.cpp` / `jit.hpp`）**: 並列スケジューラはパスごと・スレッドごとに `accessFor`（unordered_map 2 段）を呼んでいたが、結果は (top block, target) で不変なので Thread に `accessCache`+世代（`accessGen`、`JitEngine::invalidate()` で進む）を持たせて 1 ロード化。`sched_waituntil` 86 → 123 ops/ms。
- **compat 経由の broadcast and wait の startedThreads を回収側へ公開（`jit/rt.cpp`）**: `xyo_rt_compat` の合成 ExecContext はファイバローカルなので、そこの `startedThreads` は `reclaimThreads` から見えず生ポインタが宙に浮く可能性があった。呼出し中だけ `JitCtx::joinRefs` に公開する（1 回観測された SIGSEGV 調査で見つけた穴）。
- **JIT ファイバ yield のステータス正規化（`sequencer.cpp`）**: パス内 yield は `STATUS_RUNNING` に戻す（JS の `stepThread`/`stepThreadBound` と同じ）。YIELD のまま抜けると `numActiveThreads` に数えられず、スレッド 1〜2 本のプロジェクトでは「yield = 1 tick 待ち」になっていた（`sched_pingpong` が scratch-vm の 1/12000）。実測: `sched_pingpong` 0.03 → 382 ops/ms、`sched_fanwait` 3 → 984（TW の 1.2 倍）、scheduler 93 → 708。`STATUS_YIELD_TICK`/`STATUS_PROMISE_WAIT` は触らない。
- **GC: ファイバ上のスタック走査範囲と collect 下限（`gc.cpp`）**: 保守的ルート走査は現 rsp を pthread スタック範囲へクランプするが、ファイバ上の rsp は範囲外なのでメインスタックの未使用域（8MiB）まで毎回走査していた（JIT collect 2.9ms/回）。そこでスケジューラの退避 rsp（`detail::gSchedulerRegs.rsp`）から走査し（現ファイバは `Runtime::markRoots` が見る）、collect 下限を 2MiB → 8MiB へ上げた。`str_join` の 20,000 連結が **1,256ms → 46ms**、`str_cast` が 33 → 6ms。診断は `XYO_GC_DEBUG=1`。
- **完了スレッドの回収（`thread.cpp` / `runtime.cpp` / `jit.hpp`）**: `ownedThreads` は `startedThreads` が生ポインタを持つため解放できず、32 秒で **1,006,615 Thread（RSS 3.0GB）** になった。完了時に `Thread::releaseFinished()` でリソースを返し、`Runtime::reclaimThreads()` が「待機中の broadcast-and-wait が握っていない」完了スレッドを解放する（interp は `ExecContext::startedThreads`、JIT は `JitCtx::joinRefs`）。25% 増のたび間引くので償却 O(1)。`XYO_DEBUG_COUNTS=1` で残数を確認。
- **数値 → 文字列を `std::to_chars`（Ryu）で（`jsnum.cpp`）**: 従来は p=1..17 の `"%.*e"`+`strtod` 往復（非整数 1 回に最大 17 回、1.25µs。V8 は 50ns）。`std::to_chars(scientific)` の桁は既存探索と一致（40 万 + Node `String(x)` 120,020 件で完全一致）ので ES 整形はその桁から行う。`str_cast` 725 → 2,450 ops/ms、`num2str` 20,000 回 27ms → 10ms。
- **`xyo play --print-list NAME` と `XYO_EXIT_ON_DONE=1`**: ウィンドウ実行でもリストを 1 フレームごとに stdout へ出し、全スレッド終了で抜ける。パリティスイート（`parity/project`）用で、アダプタは SDL `offscreen` + `--list-limit 200000` — 結果 **2061/2123（97.1%）**。

## JIT ブロックヘルパ層（src/jit/rt_blocks.cpp）

- `codegen.cpp` の `directInputs()`/`directStack()` テーブルが opcode→C ABI ヘルパを解決。引数は inputs→fields の順で渡す。`ctx=false` エントリは `JitCtx*` を取らない純粋関数（join/strlen 等）。
- ヘルパの意味論は `src/blocks/core.cpp` 冒頭の共有関数（`updateBubble`/`setCostumeArg`/`setBackdropArg`/`clampEffect`/`moveSteps`/`getTargetXY`/`getSoundIndex`）か `src/rt/runtime_helpers.hpp` の `rt::list*` を再利用 — **意味論を重複実装しない**。
- リスト系は TW jsexecute の `listIndex` 意味論（`rt::list*`）に従う — interp の `toListIndexD` とは 'all'/NaN 扱いが意図的に異なる（TW 自体が interp/JIT でこう分かれている）。
- `procedures_return` は `exitBB`（関数単位の遅延生成終了ブロック）へ分岐 — warp 手続きでは `isWarp` 復元をエピローグに集約。
- 新規ヘルパ追加時の手順: rt_blocks.cpp に extern "C" 定義 → jit.cpp に宣言+`kRtSymbols` 登録 → codegen のテーブルにエントリ。**引数順不一致が最大のバグ源**（emit は inputs→fields 順）。
- **入力評価順**: `call2("f", emitInputOf(A), emitInputOf(B))` は C++ 引数評価順（未規定・GCC は右→左）に IR emit 順が依存し、reporter 副作用が逆順実行される。複数入力は必ず `callOp`/`callOpCtx`（左→右逐次 emit）。`callN` の braced init-list は左→右保証で安全。
- **reporter 内 yield**: `stepThreadBound` は `u.yield`・非 warp ループ境界で `status=STATUS_RUNNING` に戻して早期 return（STATUS_YIELD は呼出し側に見えない）。`xyo_rt_reporter` は「`stack.size()>depth && status==RUNNING` で return」を yield 境界として `xyo_yield` する。
- **手続き呼出しのスタックフレーム**: `emitProcedureCall` はネイティブ関数呼出しだが、呼出し中は `xyo_proc_push`/`pop` で interpreter スタックにも `procedures_call` フレームを残す（yield 中の warpMode・`Thread::stack` 形状・`isRecursiveCall` がこれを見る）。**leaf 手続きでは省略可** — `elideLeafProcFrames` が「yield/compat ヘルパを呼ばない（fnDirect 空）」proc の push/pop を消す（観測者は yield/compat 経由でしか到達しない。再帰 proc も対象）。`helperMayYield` リストと同期必須。`Thread::framePool` が StackFrame を再利用。
- **手続き warpMode は継承+昇格のみ**: `isWarp = caller.isWarp || def.warp`（JS `stepToProcedure` / TW irgen 準拠）。warp 文脈から呼ばれた非 warp 手続きは **warp のまま**動く — 定義側フラグで降格しない（"The fastest tri fill engine" で非 warp `fill.draw` が毎反復 yield して fill が破壊されていた件）。`xyo_proc_push` のフレーム warpMode も同じ合成値。
- **手続き呼出し ABI はレジスタ引数**: proc fn は `(JitCtx*, a0..aN)` の固定シグネチャでレジスタ直渡し（`ctx->args`/`pargs` の save/restore は無し）。callee の `emitArgGet` は仮引数（`argVals`）を直接返す。外部呼出し経路が無いので ABI 変更は codegen 内に閉じる。
- **小 proc の強制 inline**: `inlineSmallProcCalls`（emit 後・verify 前）が ≤220 inst の proc を `llvm::InlineFunction` で callsite に展開（O2 が渋る call/引数準備/ret を消す）。再帰・不可分は LLVM が拒否して残る（安全側）。意味論判定は emit 時点のグラフで済んでいるので観測不能。
- **`operator_letterof` ASCII fast path**: `emitLetterOf` が (num index && str && 1<=idx<=len && unit<0x80) のとき GC ルート済み共有キャッシュ `g_asciiCache`（`xyo_ascii_cache()`）を直接返す。それ以外は `xyo_letterof`。interp 側は `rt::letterOf` 内の `letterCached` が同じキャッシュを使う（単一実装）。
- **list ヘルパは `Variable*` 直渡し**: `xyo_list_*` の第2引数は `Variable*`（`varSlot` 由来）。`data_addtolist`/`itemoflist`/`replaceitemoflist` は JIT inline fast path（num index・numCache 有効な str index・cap 有り add）— helper 呼出しは非 num index・cap 不足・null のみ。
- **`rt::join` (str,num) fast path**: `sstrConcatAscii` が jsnum 表記（ASCII のみ）を直接書き込み中間 SStr を省く（linux.sb3 では join の ~90%）。`jsnum::toString` は |v|<=2^53 の i64 往復整数を `to_chars` で直出し、2^53 超は最短往復探索へ。
- **GC scanRange のページフィルタ**: `Gc::pageSet_`（ヘッダ/ペイロード開始のページ）で非マッチワードの objSet ルックアップ3回を1回に削減。sweep 毎に生存オブジェクトから再構築。
- フィールド欠落（LIST/VARIABLE 無し）は interp の `obj()==nullptr` 経路 — codegen で早期 return/デフォルト値を emit し、実行時の空変数自動生成を避ける。
- **未定義 `procedures_call` は NOP**: prototype または definition が無い呼び出し（削除済み手続きの残骸 — linux.sb3 のゼロ幅スペース入り proccode 等）は TW irgen と同じく**引数も評価しない純粋な no-op**。scanChain で bail するとスクリプト全体が interp へ落ちて致命的に遅いので、emitProcedureCall も interp の procCall も同じ条件で no-op。
- **メニュー shadow のフィールド名は親入力名と違い得る**: `pen_menu_colorParam` の実 SB3 フィールド名は `colorParam`（小文字）。登録済み menu は独立 op として走り、自身の `argValues` には**自身のフィールド名**で値が入る。親入力名で読むと undefined になる（transparency=50 無視の件）。JIT は `fields.begin()` で名前非依存、interp も第一フィールドへフォールバック。
- **`sensing_dayssince2000` は ms 精度必須**: 値は `(Date.now() - Date.UTC(2000,0,1))/86400000`（scratch-vm の local/DST 補正は代数的に相殺され UTC 起点に帰着）。`std::time` は秒精度のため `days*86400` で差分を取るプロジェクト（"tri fill engine" の fps.time 計測）が整数秒に量子化される。`rt::daysSince2000()`（chrono ms）に interp/JIT 双方を統一した — 再重複させない。

## ビルド

- **ビルドは `-j` で並列度を抑えるだけにする**（`cmake --build build -j 4`）。cgroup / systemd-run / sudo など特権が要る制限は使わない（polkit の root 要求が出るだけで邪魔）。メモリも `-j` で抑える（`RLIMIT_AS` は JIT の VM 予約とリンカで過大になり得る）。

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j 4        # 並列度は -j で抑える
# ASan:
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXYO_JIT=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build-asan -j 4
```

- LLVM は `llvm-config` で検出（`llvm orcjit native passes`、現在 LLVM 23）。`XYO_JIT` は既定 ON。`XYO_JIT=OFF` では `src/jit/nojit_stub.cpp`（JitEngine 空実装 + C ABI スタブ）が入り、`->jit` 参照に ifdef を散らさない — interp のみ。
- SDL2 は Linux では pkg-config、Windows（MSYS2 mingw）では `find_package(SDL2 CONFIG)` で探す（`-DSDL2_DIR=` / `-DCMAKE_PREFIX_PATH=`）。Windows では `SDL2::SDL2-static` を優先して静的リンクする（`libSDL2.a` は MSYS2 の `mingw-w64-x86_64-SDL2` に同梱。`SDL2.dll` 不要）。`XYO_REQUIRE_SDL2=ON` は SDL2 が見つからないとき configure を失敗させる — CI/リリースで黙って headless ビルドが出る事故を防ぐ。`SDL_MAIN_HANDLED` を定義して SDL に main を奪わせない。
- **librsvg は任意**（`option(XYO_RSVG ...)`、既定 ON）: 無いビルド（完全静的 `XYO_STATIC` / `-DXYO_RSVG=OFF` / Emscripten）は vendored **nanosvg** でラスタ化（`<text>`/`<tspan>` は描かれず、viewBox 無しは図形 bbox で採寸 — `tests/render/test_renderer.cpp` の `XYO_TEST_RSVG` で分岐）。Windows の動的リンク版（`xyo.exe`）は MSYS2 の `mingw-w64-x86_64-librsvg` で `XYO_HAVE_RSVG=1` になり、DLL は zip に同梱＋フォントは `fonts/` と `etc/fonts/fonts.conf`（`winInit` が `FONTCONFIG_FILE` を向ける）で Pango から見せる。Cairo/Pango/Fontconfig は librsvg 依存で SVG テキスト・シェーピング担当。モニタ/バブルの文字は同梱フォントと `XYO_FONT`。
- **静的リンク（リリース用）**: `-DXYO_STATIC_LLVM=ON`（静的 LLVM+libstdc++/libgcc）、`-DXYO_STATIC=ON`（完全 `-static`）、`-DXYO_LLVM_MODE=auto|config|llvm-config`（既定 auto）。静的リンクは **`llvm-18-dev`（静的アーカイブ同梱）+ `llvm-config-18 --link-static`** を使う（llvm-23-dev は同梱しない）。LLVM 18〜23 の API 差は `src/jit/llvm_compat.hpp` が吸収。Linux 完全静的は SDL2/librsvg 無し headless のみ、Windows は `SDL2::SDL2-static` 込み可（SVG は nanosvg）。`-static` 時は `libz.a` を明示リンク。
- **静的リンクでは ORC の dlsym が効かない**: バックエンド生成の libcall（`fmod`/`log`/`bcmp` 等）を `-static` では `DynamicLibrarySearchGenerator` が解決できず `JIT session error: Symbols not found` になる（interp へ静かにフォールバック）。`jit.cpp` の `hostLibcallSymbols()` を absoluteSymbols として JITDylib へ define する — **新たな libcall はこの表へ追加**。
- `-fno-exceptions -fno-rtti`。例外は使わず error コード/`std::optional`。
- アセンブリ: `src/engine/fiber_x86_64_sysv.S`（FiberRegs レイアウトは fiber.hpp と厳密一致）。aarch64 は未実装。Windows x86-64 は `fiber_x86_64_win64.S`（Win64 ABI: rdi/rsi/xmm6-15 も callee-saved、`this` は r15→rcx）。

### Windows .exe（MSYS2 ネイティブ = JIT）

- **ビルド（MSYS2 mingw LLVM）**: `windows-latest` + `mingw-w64-x86_64-llvm`（**静的アーカイブ同梱**）で JIT 有効ビルド可:
  ```sh
  cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DXYO_JIT=ON -DXYO_LLVM_MODE=llvm-config \
    -DXYO_STATIC=ON -DXYO_STATIC_LLVM=ON \
    -DXYO_SDL2=ON -DXYO_REQUIRE_SDL2=ON \
    -DXYO_BUILD_TESTS=OFF
  ```
  `XYO_STATIC=ON` で LLVM/libstdc++/libgcc を静的リンクした単体 `xyo.exe`（`LLVM-*.dll`/`SDL2.dll` 不要）。**AOT 書き出しも動作**（COFF 追記→ORC ロード）。JIT 有効ビルドは `src/jit/` を含むため POSIX 依存（`localtime_r`、`M_PI`）は `_WIN32` ガード必須 — `windows-jit` CI が検証。
- `run` / `test` と**Vulkan compute による headless GPU オフロード**（`LoadLibraryA("vulkan-1.dll")`）、**UTF-8 パスとコンソール**は従来どおり。
- **UTF-8**: コマンドラインは `GetCommandLineW` + `CommandLineToArgvW` で UTF-16 から UTF-8 へ取り直し（`main.cpp` の `winInit`）、`SetConsoleOutputCP(CP_UTF8)` で日本語出力も化けない。ファイルは `src/core/fileio.*` の `fopenUtf8` / `pathFromUtf8` が UTF-8 → UTF-16 へ変換して wide API に渡すので、日本語ファイル名の sb3 も開ける（`XYO_FONT` / `XYO_SHOT` も同様）。
- **`play` / `gui` は CI/リリースでも有効**: `mingw-w64-x86_64-SDL2` + `-DXYO_SDL2=ON -DXYO_REQUIRE_SDL2=ON`。Windows で `SDL2::SDL2-static` を優先し非 Windows SDL2 は `IMPORTED_IMPLIB` 検査で拒否。**librsvg 無し** → SVG は nanosvg（文字なし）。同梱フォントは SDL2 時のみ埋め込み。CI は `SDL_VIDEODRIVER=dummy` + `XYO_SHOT` で描画検証、`objdump -p` で `SDL2.dll` 非依存も検査。
- **AOT**: `writeExecutable`/`readSelf` は Windows でも動く（`GetModuleFileNameW` + `CopyFileW` + 追記）。MSYS2 JIT ビルドでは `xyo aot` が実際に動く（COFF オブジェクトを追記 → ORC でロード、CI で検証済み）。
- 移植済み shim: `M_PI`/`M_LN10`（#ifndef）、`localtime_s`、`_strtod_l`/`_create_locale`、`GetCurrentThreadStackLimits`+メインスタック記録、VirtualAlloc/VirtualProtect/VirtualFree。`windows.h` include は `NOMINMAX`+`WIN32_LEAN_AND_MEAN` を定義。
- デバッグ用環境変数: `XYO_JIT_DUMP=1`/`XYO_JIT_DUMP2=1`（生成/O2 後 IR）、`XYO_JIT_DISABLE=1`（interp のみ）、`XYO_JIT_MODE=hotspot|eager|off`、`XYO_HOT_INVOCATIONS|BLOCKS|SIZE=<n>`・`XYO_HOT_LOOP=0`・`XYO_HOT_STATS=1`、`XYO_JIT_VERBOSE=1`（コンパイルログ）、`XYO_PROF=1`（内訳）、`XYO_PAR=0`（並列無効）、`XYO_DEPGRAPH=<dir|->`（衝突グラフ DOT）、`XYO_SEED=<n>`（`Rng::global` 固定 — `XYO_PAR=0` 併用）、`XYO_DUMPVARS=1`、`XYO_LISTSTATS=1`、`XYO_AOT_VERBOSE=1`、`XYO_DANGER=1`（`--danger` 同義）、`XYO_GUI_ACTIONS|DEBUG`、`XYO_PROJECT_DIR=<dir>`。
- `xyo play` 用: `XYO_FONT=<path>`、`XYO_EXIT_FRAMES=N`、`XYO_SHOT=path.ppm`、`XYO_INJECT="F:op:args|..."`（`md/mu/mm:x,y`、`txt:utf8`、`key:name`）、`XYO_DEBUG=1`、`XYO_PENLOG=1`（ストローク詳細）、`XYO_PEN_NOCACHE=1`/`XYO_PEN_FENCE_OLD=1`（A/B 用）、`XYO_PENPROF=1`、`XYO_PENSKIP=1`、`XYO_PEN_CPU=1`、`XYO_SET="name=value;..."`（緑旗直後セット）。`SDL_VIDEODRIVER=dummy`/`SDL_AUDIODRIVER=dummy` でヘッドレス検証。デモ `tests/fixtures/gen_play_demo.py`、全機能は `gen_fx_demo.py`。

### WASM（Emscripten、interp のみ）

```sh
source "$HOME/.emsdk/emsdk_env.sh"
# 1) Node の headless CLI（run/dump/test）
emcmake cmake -B build-wasm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/emscripten.cmake \
  -DCMAKE_BUILD_TYPE=Release -DXYO_SDL2=OFF -DXYO_BUILD_TESTS=OFF
cmake --build build-wasm            # → build-wasm/xyo.js + xyo.wasm
node build-wasm/xyo.js test refs/scratch-vm/test/fixtures/execute
# 2) ブラウザ play（SDL2 canvas + Asyncify）
emcmake cmake -B build-wasm-web -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/emscripten.cmake \
  -DCMAKE_BUILD_TYPE=Release -DXYO_SDL2=ON \
  -DXYO_WASM_ASYNCIFY=ON -DXYO_WASM_BROWSER=ON -DXYO_BUILD_TESTS=OFF
cmake --build build-wasm-web        # → xyo.html + xyo.js + xyo.wasm
python3 -m http.server -d build-wasm-web 8000   # → /xyo.html に sb3 をドロップ
```

- `-DXYO_JIT=OFF` 強制（`cmake/emscripten.cmake`）。zlib は `-sUSE_ZLIB=1`、SDL2 は `-sUSE_SDL=2`。Vulkan GPU executor は無効。
- CLI は `-sNODERAWFS`+`-sEXIT_RUNTIME`、ブラウザは `-sASYNCIFY`+`-sINVOKE_RUN=0`+`web/shell.html`。
- interp は fiber を使わないため wasm の `Fiber` は abort スタブ。**GC のスタック走査だけは必須**（`src/core/gc.cpp` の `__EMSCRIPTEN__` 分岐）。`rtprof.hpp` の `__rdtsc` は非 x86 で `steady_clock` へ。
- 詳細・制約・Phase B(WebGPU)/C(WASM モジュール化 JIT) は `docs/wasm.md`。

## JIT パフォーマンス構造（ベンチマーク実測から）

- **FiberPool / JitCtx pool**（fiber.cpp / sequencer.cpp）: スクリプト完了時に fiber と JitCtx を即座にプールへ返却（Thread 自体は `ownedThreads` で ~Runtime まで生存するため ~Thread では遅すぎる）。**実行中の fiber は絶対に release しない**。
- **`CompiledScript::mayYield`**: 生成コード内に yield 可能ヘルパの呼出しが無い場合 false → `jitStepThread` は fiber を介さず `entry(ctx)` を同期実行（broadcast 受信等の短命スレッドの切替コストを排除）。**codegen の `helperMayYield` リストは rt.cpp/rt_blocks.cpp の `xyo_yield` 使用箇所と対応 — 新規 yield 追加時は必ず更新**（漏れると `c->fiber==nullptr` で yield → SEGV）。
- **HotSpot のホットネス**: `JitEngine::profiles_` がトップ `Block*` ごとに spawn 回数・解釈ブロック数・ループ有無を持つ。`tryCompile` は spawn 時に一度だけ `decideCompile` を呼び、false なら最後まで interp（**OSR 無し**）。ループ入りを初回 JIT するのは単発長時間スクリプトを解釈のまま放置しないため。昇格は spawn 境界のみなので量産されるハットほど早く温まる。
- **AOT の明示性**: 通常経路（`run/play/gui`）は AOT をロードしない。AOT を使うのは (1) `xyo aot` が書き出した単体実行ファイルの自己ロード（`aot::readSelf`）、(2) `--aot <obj>` の明示指定のみ。`loadAot` は `JitEngine::cache_` に `aot=true` で登録し、HotSpot の判定はそのまま「コンパイル済み」として扱う（再コンパイルしない）。
- **compile キャッシュ**: トップ `Block*` キー（`Blocks::blocks_` は unordered_map でポインタ安定）。失敗/対象外は `failed_` に入れ再試行しない。`RuntimeScriptCache::block` が hint として `pushThread`→`tryCompile`→`compile` に渡り getBlock を省略。
- **broadcast 系ワークロードはスレッド spawn 支配**: 送信1回毎に pushThread（Thread 構築 ~600ns、両モード共通）+ compile ヒント（JIT のみ ~150ns）。mayYield 同期実行・compile ヒントで JIT も interp 並み（実測 ~1.08×）。fiber switch 自体は往復 ~11ns で律速ではない。
- **hot loop 向け direct lowering（3D2 系で 24.5s→3.0s/60f）**: 変数スロット（`varSlotData`+`xyo_varslot_init` で一括解決）・算術/比較/round/mathop の inline 化・`SStr::numCache`・`rgbCache`・`PenState::lastSetColor`+`colorDirty` の遅延 rgba・`sensing_mousedown`/`motion XY`/`pen` の直接フィールドアクセス（Layout は**実インスタンスのアドレス差**から算出）。
- **`knownNum` 追跡**: `emitCanon` の戻り値は num タグ保証だが NaN は非保証（関数境界で clear）。`emitToNumF64` は全経路で NaN→0 にする（Scratch Cast.toNumber）。演算結果の NaN は `emitCanon` が保持し、`emitCmpNumF64` と分ける。
- **JIT の固定サイズ alloca**: ループカウンタ `rep_i`/`fe_i` と引数配列 `pargs` は `entryAlloca` で entry block に置く。ループ本体の `CreateAlloca` は反復ごとに rsp を下げ続ける（3DOcean で 1MiB ガードに到達）。
- **失敗した最適化（再試行禁止）**: 「yield スキップ」（他実行可能スレッド無し時に `xyo_yield` を早期 return）は waitThreads/compat reporter の yield がスケジューリングを駆動する設計と衝突し ~6× 劣化。fiber switch が唯一の進行保証。
- warp 中のインタプリタ手続き（`xyo_rt_reporter`→`stepThreadBound`）は `warpTimer>500ms` まで無中断（TW isStuck と同モデル）→ 1 step が 300-600ms になり得る。`passes` 少数は異常ではない。

## 描画性能と回帰テスト

- **DLSS 相当のアップスケーリング（`src/render/gl_upscale.*`、既定 off）**: 低内部解像度で描いたステージを 4×4 Catmull-Rom + unsharp mask（時間方向は min/max clamp）で再構成する。`renderScale_<1.0` のときだけ有効で UI は表示解像度のまま、**1.0 は byte 一致**。ソフトウェア SDL/Emscripten では使わない。`--render-scale N`/`XYO_RENDER_SCALE`（0.25〜1.0）/`XYO_DLSS_SHARP`/`XYO_DLSS_TEMPORAL=0`/`XYO_DLSS_CPU=1`/`XYO_DLSS_DEBUG=1`。実測 SSIM 0.75→0.9975。詳細は `docs/render/dlss-upscale.md`。
- **空コスチュームの再デコードを防ぐ**: `SdlRenderer::skins_` は成功 Skin と決定的失敗の `nullptr` を両方キャッシュ（3D2 では失敗キャッシュ欠落で 30 フレームあたり ~520 万回の SVG 再解析だった）。Cairo/SDL_CreateTexture のリソース失敗はキャッシュせず再試行可。
- **移動フェンスはスプライト全体を画面内へ収めない**: `getFencedPosition` は RenderWebGL と同じ AABB（`inset=floor(min(w,h)/2)`、各半ステージから `min(15,inset)` を引く）で、一部が見えるまで移動を許す。空/不正は現在位置の点 bounds 扱い。四隅と全 172800 画素のテストを維持。
- **モニタ表示は scratch-gui 準拠**: `drawMonitors()` は `monitor.css`/`monitor.jsx`（light mode）準拠で、リストの幅高は sb3 の monitor `width`/`height`（0 は既定 110×200）。はみ出す行はクリップしスクロールバーを出す。値は `monitor-adapter.js` と同じく**数値のみ `toFixed(6)`→Number→文字列**（リスト項目は丸めない）。**green flag/stop・ask バーより後に描く**。
- **同梱フォント（`third_party/fonts/`）**: Scratch の font-family は Linux で Liberation Sans に解決されるため **Regular/Bold（OFL）を埋め込む**（`cmake/embed_binary.cmake` が TTF を `xyo_fonts.inc` へ）。`FontRenderer` は 3 フェイス（0=regular／`$XYO_FONT` 優先、1=bold、2=OS フォールバック）をコードポイント単位で試し、太字は Bold が無ければ alpha 横 grow で合成する。AOT exe にも入る。
- **実プロジェクト（3D2）でのペン負荷と限界**: `XYO_INJECT="20:md:480,360|21:mu:480,360|60:md:480,360|61:mu:480,360"` で開始待ちを解除すると約 90 フレーム目からペン描画が始まる（200 フレームで 19,526,400 ストローク）。**1 フレーム約 42ms のうちペンの CPU は約 3.5ms・GPU は約 2ms** なので、実プロジェクトではもう律速ではない。
- **実 GPU ではオーバードローより CPU 側のストローク発行が律速**: `overdraw_heavy`（100 万本）で `stroke`(CPU) 67.6ms + `quad`(CPU) 23.6ms に対し `flush`(GPU 描画) は **4.9ms**。**律速は GPU のフラグメント/ブレンドではなく、CPU が 1 本ずつペン API を呼ぶコスト**なので、GPU タイルビニング + 逆順合成は採用しない（設計は残課題）。次に効くのはポリライン化など呼出し回数の削減。
- **ペンの画素モデル**: ペン層は native 解像度・premultiplied RGBA8。幅が厳密に 1 または 3 のみ +0.5 補正。座標は画素中心 (x+0.5,y+0.5) で評価し参照と同じ `clamp(width/2+0.5-distance,0,1)` を合成。幅≤8 の点被覆率は 8 個カーネルにキャッシュ。SDL/色サンプルは straight RGBA へ変換。
- **実 GPU での計測手順（重要）**: RTX 3070 はアイドル 360MHz で短いベンチではブーストしないため、**1〜2 秒の GPU ウォームアップが必須**（`xyo-pen-bench --warmup-ms 2000`、`--frames 50〜100`、中央値 5 回）。OpenGL の実機は offscreen + `__EGL_VENDOR_LIBRARY_FILENAMES` で NVIDIA を指定する。Vulkan は既定 DISCRETE。
- **ペン描画の検証手段**: `xyo-pen-bench --workload/--frames/--interleave/--shot/--trace/--compare` が 9 種ワークロードと実トレースで投入・表示を分けて計測し、`--compare` は SSIM、`XYO_PENTRACE=<path>` は実ストローク列を保存して `real_trace` で再生する。per-stroke レコード展開（`XYO_PEN_REC`、既定 ON）は 1 ストローク 10 float を `glDrawArraysInstanced` で流し VS が `gl_VertexID` から 6 頂点を作る（`ui=(c==1||4||5)`, `vj=(c==2||3||5)` を誤ると 2 枚目が退化）。**SSIM 1.0 / 最大誤差 ≤1**。
- **実 GPU（RTX 3070, OpenGL 4.6）実測**: `--interleave --warmup-ms 2000 --frames 100` の中央値 5 回で通常系は **6.8〜7.8×**、overdraw_heavy 3.3×、alpha_heavy 2.6×。Vulkan は `--frames 120` で @tps **3,333,289**（CPU JIT の 4.0×）、旧 repeat 専用経路は 10000 lanes で **4,999,933**（CPU JIT の 10×）。
- **失敗した最適化: 連続移動のポリライン化（実装後に実測中立で撤回）**: 連続移動を 1 回の `penPolyline` にまとめる実装は画素 byte 一致だが性能中立だった（TFG 3.8M ストロークで 0.6%、ノイズ内）。`penPolyline` が結局セグメントごとに `penStroke` を呼ぶため。撤回済み（`src/blocks/pen.cpp` は HEAD と同一）。
- **3D2 の点ペン専用経路**: records 有効時は penDown→penUp の点を汎用線分に通さずレコードへ直接積む（`XYO_PEN_POINT_GENERIC=1` で A/B、`XYO_TEST_PEN_RECORDS=1` で parity）。投入 wall は 241.9→148.5ms だが総 wall は GPU 待ちで変わらない。詳細は `docs/render/pen-point-performance.md`。
- **色変換の往復キャッシュ（setPenColorToColor）**: rgbToHsv→hsvToRgb 往復は入力 RGB と一致しない（全 16.7M 色検証）ので、RGB キーの 2048 エントリ直接マップ（`g_colorCache`）に HSV+rgba を保存し `colorDirty=false` で確定。**34→16ns/色（2.1x）**。`XYO_PEN_NOCACHE=1` で A/B。
- **ペン性能の計測法**: `tests/fixtures/gen_pen_stress.py` がストレス sb3（color/colorpal/nocolor/off）を生成し、`xyo-pen-bench --colorloop|--vmloop|--vmfence`（`--vmreal` で実 renderer 込み）で測る。A/B は CPU 時間の min-of-5（wall は ±20% 揺れる）。実測 20M ストロークで移動のみ **2.18x**、色+移動+ストローク **1.68x**、GL **1.57x**。
- **フェンス AABB キャッシュ（getFencedPosition）**: 4 隅オフセット・min/max・inset を `Drawable` にキャッシュし **18.5→7.2ns/移動（2.6x）**。**加算順序は元実装と同一に保つ**（変えると境界 1px が変わる）。`XYO_PEN_FENCE_OLD=1` で A/B。
- **ペン層の readback 除去（XYO_PEN_INTEROP、既定 ON）**: `SDL_GL_BindTexture` で SDL テクスチャの GL 名を取り、ペン FBO のカラーをそれへ差し替える（コンテキスト共有時）。以降は表示テクスチャへ直接描画でき、`glReadPixels`+変換+`SDL_UpdateTexture` が不要になる。SDL 側は premult 用カスタム blend で描画は `SDL_FLIP_VERTICAL`。**CPU 読者の前だけ `penSyncCpu` が readback する**。`XYO_PEN_INTEROP=0` で旧経路。実測 llvmpipe で tri_fill 1.91× 他、**byte 一致（SSIM=1.0）**。
- **CPU ラスタの逆順合成 + 早期終了（XYO_PEN_RC、既定 auto）**: 256 本超でストロークを遅延バッチ化し (T,P) 半群を**新しい順**に適用、透過率 T ≤ `XYO_PEN_EPS`（1/512）の画素はスキップする。`XYO_PEN_RC=0` は前向き逐次（**byte 一致**）、auto は被覆/AABB 比で判定、`XYO_PEN_RC_DEBUG=1` でログ。実測 llvmpipe で scanline 5.96×、opaque_overdraw 5.11×、dense_alpha 3.18×、SSIM 0.9996〜。
- **GPU ペン層（XYO_PEN_GL）**: SDL backend が opengl 系なら自前 GL + 480×360 FBO + カプセル距離 FS（CPU と同式）でペン描画。ストロークは quad 蓄積、**readback は CPU 読者の前だけ**。`XYO_PEN_CPU=1` で CPU へ。tri fill: @tps 1,506 → **166,664**（110×）。
- **64bit ペン順序 ID**: 上位 32bit が論理反復（lane）、下位 32bit が反復内 stroke。VM 完了後に lane 別 count から prefix offsets を作り、GPU が順序付き `uvec2` 列を生成してその順で描く。dispatch 間と CPU との順序は flush と GL 描画順で保つ。lane0 は実ペン状態、観測前リセットを証明できない本体は拒否する。
- **GPU 並列スクリプト実行（`src/gpu/`）**: warp 中の `control_repeat`・`for_each`・回数を証明できる `while`/`repeat_until` を `ParallelLoop` にまとめる。構文だけで独立性を判断せず、生成した f32 VM バイトコードを `gpu_analysis.cpp` の CFG 固定点解析で検査し、失敗時は元の CPU ループを走らせる。内側ループ・分岐・`control_all_at_once`・安全な手続きのインライン化にも対応。
- **GPU 化の証明条件**: スタック高さとジャンプ先、全経路の代入、位置・ペン状態・属性の読み書きを検査する。反復内で読む前に代入する変数は lane-local、本体で書かない変数は有限数値のスナップショット、反復をまたぐ変数は定数整数ステップの affine recurrence だけを受理する（開始値・中間値・終了値を f32 の厳密整数範囲 ±2^24 に制限）。while/until は実行時にも検査し、生の NaN は 0 にせず CPU へ戻す。
- **GPU の実行時ガード**: 非 warp・stage・dragging・解決失敗・書込み alias・小ループ・数値/ハードウェア制限は CPU へ回す。GL 経路は上限（20 万命令等）を描画前に判定し、失敗時は変数・ターゲット・RNG を書き戻さない。乱数は lane 別 xorshift で CPU 共有 RNG とは別系列（design.md §5.3）。`XYO_GPU=0`、`XYO_GPU_MIN=N`（既定 16）、`XYO_GPU_DEBUG`/`XYO_GPU_DUMP`。
- **GPU 回帰テスト**: `xyo-gpu-tests --parallel` が共通解析を検査。GL4.3+ では `SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy XYO_TEST_GPU=1 ./build/tests/xyo-gpu-tests --parallel` で実 dispatch/重なり/フォールバックを検査（画像比較は同 GL ペン層の CPU 実行との byte 一致）。検証: CTest 3/3、実 GL 377 checks、`XYO_JIT=OFF` 共通解析 113 checks、失敗 0。
- **Vulkan デバイス選択はハードウェア優先**: DISCRETE > INTEGRATED > VIRTUAL。CPU 実装（llvmpipe/lavapipe）は `XYO_VK_DEV=<名前部分列>` 明示時のみ（ソフトウェア Vulkan は CPU JIT より遅い: llvmpipe @tps 1,111,116 vs CPU 2,500,065）。`XYO_GPU_DEBUG=1` で選択を stderr へ。
- **GPU ループが受理する text リテラル**: `text` shadow のうち「数値化して ES 表記に戻すと元の文字列と一致する」もの（例 `"1"`）だけを受理し、`"01"`/`"1.0"`/`"-0"`/`"abc"` は拒否する。これが無いと tri fill の `fill.draw` が落ちて GPU プログラムが生成されない（`gpu loop skip:`）。
- **TurboWarp `_twconfig_`（カスタムステージサイズ等）**: sb3 のコメントで ` // _twconfig_` で終わる行の直前にある ExtendedJSON がプロジェクトオプション。`applyTwProjectOptions` がロード**前**に適用する — `width`/`height`→`stageWidth/Height`（0/非数値は 480×360）、`framerate`→`currentStepTime`（>250→250、<0→1、0=60fps）、`turbo`/`interpolation`/`runtimeOptions`。fence/bounds/pen/GPU dispatch は全て `stageWidth/Height` 経由。
- **現行ビルドの実測（headless `xyo run`、tri fill 120 フレーム）**: text 修正前は @tps ≈2.0–2.5M だったが、修正後は RTX 3070 で **@tps 9,999,866 / 498ms**（CPU JIT 2,499,966 の 4.0×）。**AOT でも動く**（`GpuProgram` slot をロード時に再コンパイル）。
- SDL2 がある場合は `xyo-render-tests` をビルドする。`ctest --test-dir build -R xyo-render-tests --output-on-failure` は dummy SDL ドライバで実行し、空/不正/正常コスチューム・切替・クローン共有・合成の byte 一致・点の境界と丸めを検証する。
- **FPS は描画ありで測る**: renderer 不在の `step()` 回数は実フレーム数ではない。3D2 は開始待ち解除・ペン描画開始・ウォームアップ後に計時（30 active frames で penPoint=5,184,000、fence=5,205,630、penClear=60、draw=30 を照合）。比較は `XYO_PROF` 無効・解像度/入力/ドライバ一致で。
- 空コスチューム失敗キャッシュ導入時点の A/B 実測（3D2、scale=2、30 active frames）: dummy/software が 9109.6→1206.1ms（3.29→24.87 FPS、**7.55×**）、実 Wayland/OpenGL が 8929.5→1139.4ms（3.36→26.33 FPS、**7.84×**）で、dummy の PPM は FPS 数字を除く全 683,808 pixel 一致（xyo の前後比較で TW との比較ではない）。

- **ビットマップコスチュームの rotationCenter は生画像 px**: sb3 の `rotationCenterX/Y` は logical×bitmapResolution で保存される（`*2`）。`Skin::rcx/rcy` は論理単位で持つ約束なので、ビットマップ経路は `/bitmapResolution` してから格納する。raw のまま掛けると `bitmapResolution>1` の背景がずれて半分しか覆わない。SVG は元々論理 px なので変換不要。

## SVG コスチュームの文字

- NanoSVG は `<text>`/`<tspan>` を描画しない。コスチュームは librsvg でパースし、96DPI・元の論理寸法の viewport に `rs=2*max(1,bitmapResolution)` の Cairo scale を適用する（切り上げた texture 寸法を viewport にしない）。
- Cairo ARGB32 は native endian の premultiplied 値。`Skin::px` は straight RGBA8 に戻してから SDL・effects・pick/touchingColor・penStamp に渡す（色成分の alpha 二重掛けを防ぐ）。
- SVG handle に base URI を設定しない（外部ファイル/ネットワークを解決しない）。XML の UNLIMITED flag は使わない。0/負/非有限の寸法と壊れた XML は失敗キャッシュにし、positive viewBox-only は viewBox から解決する。texture は各辺≤8192・≤64Mi pixels に制限する。
- 回帰テストは英字・日本語・tspan の改行/色・opacity・anchor・group transform・viewBox・stroke・失敗キャッシュ・当たり判定/スタンプを検証する。フォント依存なので文字幅を固定 byte golden にしない。
- `3DOcean.sb3` は entryAlloca 修正後、実 Wayland/OpenGL で初期化を完了して描画を続ける（最初の描画まで 14.173s、以降 6 step=2.183s）。warp 500ms yield の部分描画 step を含むため FPS は報告しない。

## 3D2 の計算差分検証

- 謎の線を追うときは描画画像だけでなく実 TW VM の変数・法線・座標・色リストと比較する。`Res=1` の 475 pixel・`Res=4` の 239 pixel の差分は `emitToNumF64` の NaN→0 欠落（`x/abs(x)` が x=0 で NaN → 乗算へ伝播）だった。修正後は両 Res の計算リストが TW compiled と一致する（abs=1e-9、rel=1e-10、色文字列は厳密一致）。
- 計測値は条件が違うので単純比較しない。native AA + NaN 修正後の Wayland/OpenGL は 30 active frames=1330.5ms・22.548 FPS（単発、vsync 維持）、librsvg/entryAlloca 追加後の dummy/software は 1452.1ms・20.660 FPS、移動フェンス修正後は 1497.8ms・20.030 FPS（いずれもドライバが異なる単発値）。3DOcean のペン座標は x=[-240,239]・y=[-180,180]。
- ペンは実シェーダ実行の GPU golden 単点テストで検証する。固定 172802-command trace の replay は premult RGBA mean error 0.85–1.02 byte、max 13–15 byte で、byte 完全一致はまだ主張しない。

## 重要な制約（実装時）

- **yield は任意の呼び出し深さから**（Scratch 手続きが別スレッドを呼ぶため）。JIT コード・プリミティブ双方が Fiber 上で動く。コルーチン不可。
- `Thread::status` の数値は ABI 契約（JIT 生成コードが直接参照）。
- `JitCtx` のフィールドレイアウトも ABI 契約 — codegen.cpp が i64/i32 インデックスで直接 GEP する。フィールド追加・並替は codegen のオフセットと整合させること。
- NaN 正規化: 生 NaN を生む全演算後は `Value::numCanonical`（kNan に揃える）。`-NaN` を生値のまま保持すると `isNumber` 判定が壊れる。
- 差分テストの真実のソースは `refs/scratch-vm/test/fixtures/execute/*.sb3|sb2`（116 件）+ `refs/scratch-vm/test/unit/*`。`./build/xyo test <dir|file>` で実行。
- Scratch のキャスト/比較/演算仕様は `refs/scratch-vm/src/util/cast.js`・`math-util.js` が正典。`==` は数値比較→大文字小文字無視文字列比較の二段構え。
- 変数解決は JS 同様「ID 検索 → 名前+型フォールバック → 自動作成」（`Target::lookupOrCreateVariable`）。broadcast メニューの shadow は ID→名前解決が必須（id/name desync フィクスチャ）。

## ASan / サニタイザとの併用（重要）

- **Fiber は ASan の fiber プロトコルに従う**（fiber.cpp 参照）:
  - `resume()`: scheduler 側で `__sanitizer_start_switch_fiber(&asanCallerFake_, bottom, size)` — **bottom は low end**（`stackBase_ - stackReserve_`、guard ページ含まず）。
  - `yield()`/終了時: fiber 側で `__sanitizer_finish_switch_fiber(asanCallerFake_, &bottom, &size)` → 呼出し側 fake stack を復元してからスイッチ。
  - これを怠ると scheduler 側の instrumented コードが fiber の fake stack を使い続け、奇妙な SEGV/ハングを引き起こす（実績あり）。
- **GC 保守的スタックスキャン**（`Gc::scanRange`）は poisoned redzone を読むため `no_sanitize("address")` 指定済み。
- **GC ルートの網羅性（実績のある障害）**:
  - GC オブジェクトを「ルート未接続のヒープコンテナ」に格納する区間では `Gc::DeferCollect` を必須とする（sb3 ロードはこれに該当 — `loadTargetObject` 中の Target は `runtime.targets` 未登録）。スタック残滓が守ってくれるのは非決定的で、大規模プロジェクトで UAF になる（debug/projects/3D2.sb3 実績）。
  - 中断中・実行中ファイバのスタック+退避レジスタは `markThread`（runtime.cpp）が走査する。新たなスレッド実行形態を増やす場合はここを更新すること。
  - `collect()` は `allocMtx_` 保持下でルート走査を呼ぶ → 走査中の再入割当（`Mutation::parseArgs` 等）が合法。**マーク中の割当は黒色割当**（mark=1 で生成、同 sweep で解放されない）。`allocMtx_` は `std::recursive_mutex`。
- **ASan 下では未完了（中断中）の fiber を FiberPool に戻さず意図的にリーク**（fiber.cpp の `FiberPool::release` 内 `#ifdef XYO_ASAN`）。中断 fiber のスタック範囲には ASan fakestack が「実行中」のまま紐付いており、reset/munmap 後の再利用で `stack-use-after-scope` を誤検出する（実績あり）。非 ASan では通常通り再利用。
- 既知の軽微リーク: 中断中に破棄された fiber のフレーム内ヒープオブジェクト（std::string 等、アンワインド不可）。ASan 実行時は `ASAN_OPTIONS=detect_leaks=0` を推奨。
- ASan ビルドで JIT を試す場合は `-DXYO_JIT=ON` の明示が必要（古いキャッシュは OFF のまま残り得る）。

## 参照コードの対応表

| こちら | TurboWarp |
| - | - |
| `src/ir/` | `src/compiler/irgen.js`, `intermediate.js`, `enums.js`, `iroptimizer.js` |
| `src/engine/execute.cpp` | `src/engine/execute.js`, `blocks-execute-cache.js` |
| `src/engine/` | `src/engine/runtime.js`, `sequencer.js`, `thread.js`, `target.js` |
| `src/jit/` | `src/compiler/jsgen.js`, `jsexecute.js`, `compile.js` |
| `src/blocks/` | `src/blocks/*` |
| `src/core/` | `src/util/cast.js`, `math-util.js`, `string-util.js`, `color.js` |
| `src/load/sb3.cpp`, `sb2.cpp` | `src/serialization/sb3.js`, `sb2.js`, `sb2_specmap.js` |
