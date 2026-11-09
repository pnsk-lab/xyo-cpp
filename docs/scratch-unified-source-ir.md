# Scratchスクリプト共通Source IR

## 現在の実装

`src/ir/scratch_script.hpp` は、既存の `Blocks` と LLVM/WASMのそれぞれの中間表現の間に、Scratchソース構造を欠落なく保持する層を導入する。

- `ScratchScript::capture(blocks, top)`: トップレベルのハットを起点に、順序付き入力、アクティブな入力、シャドウ入力、SUBSTACK、nextチェーンを反復DFSで収集する。
- `ScratchProject::capture(blocks)`: トップレベルスクリプト一覧、手続き定義・プロトタイプ、孤立ブロック、拡張opcodeまで含めて全ブロックを保持する。ブロックの元の登録順序（`seq`）を維持する。
- 各ノードは `Block` の完全コピーで、`opcode`、`fields`、`inputs` の挿入順、`Mutation`、`next`、`parent`、その他の実行メタデータを保存する。
- 不明なopcodeは「非対応」として残し、削除しない。処理不能なバックエンドは失敗を返してインタプリタへフォールバックする。
- 不正な参照、参照グラフの循環、100,000ノード超はエラーとして扱う。

## バックエンド接続

**WASM:** `wasm::lowerScript` がまず `ScratchScript` を生成し、`lowerCapturedScript` がコピーされたブロックを既存の `Program`（数値演算、条件分岐、ループ、数値リストなど）へLoweringする。変換が失敗した場合、途中まで作成した `Program` を呼出し元へ渡さない。

**LLVM:** `jitEmitScript` で同じ `ScratchScript` の生成・構造検証を実行する。**既存LLVMコード生成器は依然として元の `Block*` を使う。** 当初コピーからの直接参照を試したが、既存のOSR/SSA回帰テストに不一致が生じたため、この切り替えは取り消した。GC、`BlockCached`、スケジューラのポインタ同一性が関係するため、コンパイラ内部全体の改修が必要。

## 境界と未完了事項

これは全Scratchソースを保持する**構造的Source IR**であり、全Scratch命令をLLVM/Binaryen共通の実行可能SSAへ変換するものではない。

- `ScratchProject` の手続き・拡張・任意効果は元のブロックのまま保持するだけで、実行契約・型情報・GCルート・yield継続状態をまだ共通の命令として定義していない。
- Source IR内の `BlockField::value` にはGC管理のValueポインタが含まれる。Source IR自体はGC rootではない。ターゲットが生存するコンパイラスコープの外へ持ち出したり、ワーカースレッドへ非同期移動したりしてはいけない。
- LLVMへの完全移行では、source-node参照のID化、`BlockCached` とポインタ同一性を利用する解析の修正、手続きのコンパイルと効果管理の統合が必要。
- WASM HotSpotは既存の対応可能な命令だけを生成する。motion、sound、pen、再帰、一般のyieldなどの処理は依然としてインタプリタへ戻る。

## 検証

`xyo-scratch-ir-smoke` は、数値式・シャドウ入力・未対応opcode・手続きプロトタイプ・mutation、編集後の独立性、全プロジェクトの孤立ノード、循環・欠損の検出、WASM Loweringの等価性、10,000ブロック長の反復走査を検証する。

実際の `.sb3` ファイルも `basic.sb3`、`calculator_tests.sb3`、`calculator_graph_tests.sb3`、`parfor_fill.sb3` で検証し、ロードした全ターゲット・全スクリプトでSource IR生成に成功することを確認する。

既存の `xyo-tests`、`xyo-jit-repeat-ssa`、`xyo-jit-osr`、`xyo-wasm-binaryen-backend` を回帰テストとして使用する。


## Backend-neutral node effects (2026-11-07)

`src/ir/scratch_effect.hpp` adds a backend-neutral *annotated execution
graph* on top of the lossless Source IR snapshot. Every node retains its
opcode, branch/next edge, ordered Scratch operands (active reporter and
shadow fallback), scalar/list IDs and a conservative local effect mask:

- `ReadState`, `WriteState`: reading/writing Scratch runtime state
- `MayAllocate`: GC-visible allocation or coercion may occur
- `MayYield`: a scheduler checkpoint or effectful runtime operation
- `MayCall`: a procedure, extension or other external handler
- `Unknown`: semantics not explicitly modeled, must not be optimized away

Unknown extension/sensing/looks/sound blocks are retained as opaque with
all effect bits conservatively set. Only well-known operators and data/control
blocks are categorized. `subtreeEffects` unions the effects of selected
inputs without accidentally treating the child of a pure Boolean operator
as pure; the optional `includeNext` flag also traverses a sequential
script's continuation chain.

`wasm::lowerCapturedScript` now builds and verifies this effect graph and
uses its ordered input bindings when converting to its existing executable
`Program`. `jitEmitScript` validates the same graph but continues to
read the live `Block*` nodes for LLVM code generation: OSR/SSA and the
scheduler still rely on canonical block identity. This stage does NOT
yet provide full shared executable SSA or convert unsupported Scratch
effects into LLVM/WASM opcodes; it is a common semantic classification
and effect barrier for subsequent lowering work.

The shared-node smoke tests check variable writes, numeric arithmetic,
shadow fallback, opaque side effects, subtree effect unions and the
existing real-SB3 corpus.


## Strict exact-integer executable Scratch kernel (2026-11-07)

`src/ir/scratch_numeric_kernel.hpp` adds a verified, opt-in conversion
`lowerExactIntegerScript(ScratchScript)` from real Scratch nodes into the
existing executable CFG/SSA instruction format, consumed unchanged by
`LlvmCfgBackend` and `WasmCfgBackend`.

Supported: a straight-line `event_whenflagclicked` script containing
`data_setvariableto` and `data_changevariableby` statements;
`math_number`/integer numeric literals, initialized
`data_variable` reporter reads and `operator_add/subtract` expressions.
The result is the last written scalar's value; variables live in private
i64 slots, not the live Scratch Variable heap.

This is a **provably exact, fixed-input integer specialization**:
variables must be assigned before their first read; all numbers and
intermediate results are statically known integral values inside
[-(2^53-1), +(2^53-1)]. The frontend rejects noninteger literals,
unknown/side-effectful reporters, unsupported control flow, and arithmetic
outside that exact range. It does not silently accept Scratch's dynamic
string-to-number coercion, NaN boxing, floating-point rounding, user-event
yielding, or external state changes. This kernel is a compiler/LLVM/WASM
parity milestone, **not** yet a replacement for the runtime LLVM JIT.

The `xyo-ir-cfg-backends` differential test now lowers an actual Scratch
block sequence with two variables, an initialized variable reporter,
addition, subtraction, shadow fallback, and ChangeVar statements. It
executes the same generated SSA/CFG under LLVM `lli` and Node.js WASM,
with LLVM `opt -O2` and `wasm-opt -O2`, alongside negative conversion
cases (nonintegral, unsupported effect, read-before-write, overflow).
`xyo-scratch-ir-smoke` also compiles a captured `Blocks` graph through
the strict frontend and verifies its result IR.

Next stages: conservatively widen numeric value types from i64 to
guarded f64 and tagged `Value`, add actual Scratch variable slot bindings
and safe GC/Deopt contracts, then build region-based CFG with conditional
branches and yield barriers before switching the production JIT over.


## f64 / NaN-boxed slots and guarded live Variable ABI (2026-11-07)

The executable CFG now supports f64 constants, addition, subtraction,
multiplication, division, numeric comparison (`<`, `>`, `==`),
`LoadNumber`, `StoreNumber`, `ChangeNumber`, and
`EncodeNumber`. Scalar slots always carry the 64-bit Scratch Value
representation. Each floating-point consumer performs Scratch numeric
NaN-to-zero conversion, while writes canonicalize NaN to `val::kNan`.
Signed zero, finite doubles and infinities retain their IEEE754 bits.

`Executable::externalValueSlots` enables a distinct ABI: LLVM generates
`run(ptr stagedSlots) -> i64`, while WebAssembly generates
`run(i32 stagedMemoryBase) -> i64`. All slot loads and stores use the
caller-provided scratch array; they never dereference a live `Variable*`
in generated code. Existing integer-only CFGs retain their zero-initialized
local i64 slots and prior ABI.

`src/ir/scratch_variable_slots.hpp` introduces `ScratchNumericSlots`
and `invokeNumericKernel`. The host resolves each scalar by Scratch ID
through the live `Target`, checks numeric type, non-cloud status and
aliasing, stages raw Value bits, executes a synchronous caller-provided
backend invocation, and commits **only after successful execution**.
Commit rechecks every original Value and numeric result before publishing
any change. Non-numeric values, a changed variable, a failed compiled
segment, or duplicate aliases cause rejection without partial writeback.
The Target remains responsible for GC roots and ownership. The
generated code does not retain a GC pointer and **may not yield**
during this transaction.

`lowerNumericF64Script` accepts `event_whenflagclicked` scripts
containing scalar assignments and `change by` operations, numeric
literals, scalar reporters, +, -, *, /, and numeric `if / if else`
comparisons with nested numeric statements. It creates a typed common
CFG with actual conditional branches and state reads at joins; no
branch-dependent Scala Value phi needs to be guessed. Unknown effects,
procedures, arbitrary dynamic strings, Boolean operators, and
yielding loops remain unsupported and must be handled by the existing
Scratch interpreter/JIT.

LLVM/WASM backend differential tests execute real generated f64 CFGs
including `-0`, a canonical NaN, NaN-to-zero at the next operator,
initialized variable loads, arithmetic, and both sides of `if`.
Each result is compared as **raw 64-bit bits** across the return value
and every staged scalar slot, also after LLVM O2 and Binaryen O2.

**Important integration boundary:** The common f64 CFG and
`invokeNumericKernel` callback ABI are now tested independently; the
production `jitEmitScript` scheduler still uses its existing LLVM
code generator. No compiled common f64 region is yet installed as a
production JIT continuation. Until a scheduler-aware region boundary,
deoptimization checkpoint and GC-root lifetime contract are wired in,
this subset must not silently replace normal per-block Scratch execution.


## 共通CFGへの限定的な repeat Lowering（2026-11-07）

`lowerNumericF64Script()` は、検証専用の明示オプション
`NumericF64LoweringOptions{.allowUnscheduledLoops = true}` を指定した
場合に限って固定回数の `control_repeat` を共通SSA/CFGへLoweringする。
既定値では拒否し、既存のInterpreter/JITへフォールバックする。

- `TIMES` は有限・非負の整数リテラル（1024回以下）のみ。
  小数・負数・動的回数は、丸めなどの意味論が異なるため拒否する。
- 反復回数はpreheaderで一度だけ確定、headerにi64 Phiを設置。
  比較・body・latch・backedge・exitのCFGを生成する。
  条件分岐や入れ子のrepeatを本体に含められる。
- 入れ子ループの過大な総反復回数は静的な作業量上限で拒否する。
- 各ループのlatchとheader、および元のScratchブロックIDを
  `Executable::yieldBoundaries` に記録する。CFG検証器は
  backedgeの構造を検証する。
- `lowerExecutable` はyield境界を含むIRを通常
  `VerifyError::RequiresScheduler` で拒否する。
  テスト専用の明示引数を指定した場合のみチェックポイントを省略する。
- 型付きホスト入口 `invokeNumericKernel(Target&, kernel, runner)`
  もyield境界を持つカーネルを拒否し、Scratchの通常実行へ混入しない。

**未完了：** 通常のScratchループは反復ごとにスケジューラへ制御を返す。
warpでも時間制限監視が必要。この共通CFGには正確なyield/resume、
途中確定、再開時のDeopt/GC契約がないため、本番JITでの実行は禁止。
`repeat_until`、`while`、`forever`も未対応。

LLVM `lli` とNode WASMの差分テストでは、0回・3回、分岐あり、
入れ子ループ、無効な回数・過大な作業量の拒否を確認している。
LLVM O2 / wasm-opt O2 を適用した後も結果を比較する。


## 共通CFGの停止・再開ABI（2026-11-07）

Executable.resumable を追加した。enableResumableLoops=true で
生成した共通CFGはyield境界で中断し、次の呼出しで再開する。

- LLVM: run(i64* stagedSlots, i64* frame) -> i64。
  WASM: run(i32 stagedSlotsBase, i32 frameBase) -> i64。
  戻り値は 0=Completed, 1=Yielded, 2=invalid PC。
- frame[0] = 次のCFG基本ブロック番号、frame[1] = 完了時の
  Scratch Valueビット列、frame[2+ValueId] = SSA値のビット列。
  初回は全ワード0、完了時はPC=UINT64_MAX。
- repeatのlatchからheaderへ戻る際、Phiの並列コピーを完了してから
  継続フレームを保存し、Yieldedを返す。次回はheaderから再開する。
- WASMはPCディスパッチとSSAローカルをフレームから復元し、
  LLVMはentryのswitchとSSA値フレームを使用する。
  既存の一括実行関数のABIは変えない。
- src/ir/scratch_continuation.hpp ではTarget変数をIDで再解決し、
  変数値とSSAフレームを両方とも一時領域に保持する。
  正常なYielded/Completedの戻り時だけ、型・元値・書換え競合を
  検査してから両方を確定する。競合、無効なPC、backendエラー、
  完了後の再実行では一切確定しない。
- driveNumericKernelはホストスケジューラ向けの同期反復補助。
  non-warpでは1反復で制御を返す。warpでは呼出し側の期限判定が
  yieldを必要とするまで次スライスを継続する。

LLVM lliとNode.js WebAssemblyで実際に同一モジュールを繰り返し
呼び出す差分テストを追加。0回・3回repeat、分岐付き、入れ子、
2個のPhiが交換されるループを、各yield時点の変数値・終了値・
LLVM O2 / wasm-opt O2と照合する。
ホストABIでは中断の取り消し、外部値変更、warp/non-warpの
スライス駆動を検証する。

本番有効化前には xyo-cpp のSequencer / Threadへの継続フレーム
所有・実時計の期限監視・deopt・GC/Thread寿命の統合が必要。
この変更は独立した共通CFGとホスト呼出し口の実行検証であり、
既存の本番LLVM JITやWASM HotSpotはまだ置き換えていない。


## 実Thread / Sequencerへの明示登録経路（2026-11-07）

`src/engine/numeric_thread.hpp` に `NumericThreadSession` と
`attachNumericThread`、単一repeat用のインタプリタ復帰処理を追加した。

- `Thread::numericSession` は従来の `compiled` / fiber とは別の
  継続状態。通常のプロジェクト実行では未設定。明示的に登録された
  スレッドだけ `Sequencer::stepThread` の数値共通CFG分岐へ進む。
- LLVM ORC / WASMの機械語関数は、呼出し側からスロット配列と
  継続フレームを受け取る同期関数として登録する。関数の実行メモリ
  は登録したRunner自身が保持する。GC管理のVariableポインタを
  コンパイル済み関数へ渡すことはない。
- `numericStepThread` はスライス実行の成功時だけ変数と
  継続フレームを確定。通常実行は1反復ごとにスケジューラへ戻す。
  warpは既存の `warpTimer` と `warpBudgetExhausted` で期限を検査。
- 停止、完了、Thread破棄・`releaseFinished` で状態を解放する。
  `parallelEligible` はまだ通常JIT用なので、新しい経路は直列実行。
- 登録時に、現在のScratchブロック木を再Loweringし、コンパイル済み
  CFGの命令・SSA・Phi・ブロック接続・yield情報・変数IDを照合する。
  変更された別プログラムへのコード取り違えを拒否する。
- **Deoptは単一の静的回数repeatのみ。** 失敗したスライスを
  書き戻さずに、最後に確定したPhiカウンタから残り反復回数を算出。
  元のrepeatブロックとその実行コンテキストを復元して、
  既存インタプリタから再開する。入れ子repeatなど再構築可能性が
  証明されていない構成は実行登録を拒否する。
- `tests/platform/numeric_sequencer_smoke.cpp` では実際の
  LLVM ORC機械語をコンパイルし、Runtime / Thread /
  Sequencerから起動。1反復ずつの停止、Runtime::step経由、
  warp完了、途中Boolean型への変化後のインタプリタ復帰、
  ソース変更の拒否、stopThread後の状態解放を検証する。

本番のJITコンパイル選択（JitEngine / HotSpot / AOT）にはまだ
自動登録していない。現時点では限定された実行経路を検証する
明示的なC++ APIである。一般的な手続き、複数ループ、
動的count、任意位置のDeopt、実際のwarp手続きの継続復帰は
まだ未対応。新しい機能をデフォルトで有効化しない。


## JitEngineからの共通CFG自動選択（試験的、2026-11-07）

環境変数 `XYO_JIT_COMMON_CFG=1` を**明示的に**設定すると、
通常の `JitEngine::decideCompile`（HotSpot/Eager）の判定に続いて
`JitEngine::tryAttachNumeric` が呼ばれる。既定値では無効。

- `ScratchScript::capture` → `lowerNumericF64Script` →
  `LlvmCfgBackend` → LLVM ORC JITの順にコード生成する。
- 対象は**静的・整数回数の単一repeat**、数値型だけの変数、
  対応する副作用のない数値演算と条件分岐に限定する。
  型が数値でない、未知の演算、複数ループ、手続き等は従来JITへ
  フォールバックする。中断済みスレッドの後付け切替はしない。
- LLVM backendへ `compileNumeric` を追加し、既存LLJITに
  名前が一意なコードを公開する。通常LLVM JITと同じ
  最適化レベルを使用する。別LLJITは生成しない。
- コンパイル済みコードは `JitEngine::numericCache_` に
  トップBlockのポインタ単位で保持して同一スクリプト再起動時に
  再利用。各Threadは独立した `NumericContinuation` を持ち、
  異なるTarget/クローンでも再登録前に変数ガードを実行する。
  `JitEngine::invalidate` でキャッシュと不適格スクリプト集合を
  消去する。ソースが変わった場合は登録直前に現在のScratch木から
  再LoweringしてCFG全体を比較し、古いコードを実行させない。
- `Runtime::pushThread` はroot StackFrameの生成より先に
  `Thread::tryCompile` を呼ぶため、新規Threadの空スタック状態でも
  登録を許可する。スレッドが既に実行を始めた場合は拒否する。
- `XYO_JIT=OFF` では新規入口を空スタブにし、以前欠けていた
  `JitEngine::pollAsync` / `readyScript` の空スタブも補完。
- `xyo-numeric-sequencer-auto` のCTestは自動選択、
  2回目の起動でのコード再利用、変数型ガード、
  対応外命令の既存JITへのフォールバック、
  Block更新時のキャッシュ拒否・invalidate後の再生成を検証。

デフォルト経路は一切変更しない。これは限定された実装を
比較・検証するための実験フラグであり、Scratch全体の共通IR化ではない。
実用化にはスクリプト全体の可変型対応、複数ループと手続き、
任意箇所のDeopt、warp手続き、性能測定が必要。


## 順次実行する複数repeatへの共通JIT拡張（2026-11-07）

XYO_JIT_COMMON_CFG=1 の実験経路を、**最大16個のトップレベル・順次実行される静的回数repeat** に拡張した。

- NumericThreadSession::safeToAttach はトップレベルの hat → next チェーンを検証し、すべてのyield境界がチェーン上の異なるrepeatに同じ順序・同じ定数回数で対応することを確認する。入れ子repeat、ifのSUBSTACK内repeat、一致しない構造・変数型は従来JITへ戻す。
- restoreNumericInterpreter は最後に保存した継続PCから**実行中のrepeat**を特定し、対応するPhiカウンタから残り反復回数を復元する。先行するループを再実行しない。
- LLVM ORCでの明示登録/自動選択を検証。例: x=10、3回+2、2回+5、y=x+7 → x=26, y=33。途中のxは12,14,16,21,26。
- 2つ目の途中でBooleanへ変化した場合、1つ目の完了直後の型変更、0回の第1ループ、入れ子ループの自動登録拒否をテストする。
- LLVM lliとNode WebAssemblyの差分テストで2つのループの各yieldとSSA値を照合し、LLVM O2 / Binaryen O2でも検証する。

通常の実行はデフォルトOFF。対応対象は数値型のスクリプトのみであり、動的回数、手続き、任意のyieldや複雑なDeopt、一般Scratchの完全互換はまだ提供しない。


## 数値共通CFGのyield管理高速化（2026-11-07）

実験フラグ XYO_JIT_COMMON_CFG=1 で使う数値共通CFGの継続実行から、**1反復ごとのSSA再検証・vector再確保**を除去した。

- NumericContinuation生成時にverifyExecutableを1回実行する。以降は、共有された不変のScratchNumericKernel/Executableと継続状態を前提にする。実行ごとの大きなSSAグラフコピー・支配関係検査を行わない。
- 中断先を事前構築したビットマップで検証する。連続する複数repeatの異なるheaderに対応する。
- 継続状態には通常フレームとワークフレームをそれぞれ1つだけ確保し、値のコピーと成功時swapで更新する。失敗時は通常フレームが変更されない。
- ScratchNumericSlots::refreshは元のScratch IDを毎回再解決し、値の型とcloud条件、重複エイリアスを検証し直す。スロット用vectorは再利用。削除/再生成されたVariableへの古いポインタは参照しない。途中の型変更、割り込み、commit時競合チェックは継続する。
- LLVM CFG loweringでは、基本ブロックが実際に使用する他ブロック定義・Phi値だけを継続フレームから読み込む。以前の「毎ブロック全SSA値をloadする」実装よりロードを削減。最適化を適用しない一括実行・WASMのABIは変更しない。
- tests/platform/scratch_variable_slots_smoke.cppで、フレーム/スロット領域の再利用、値の外部変更、Variableのerase/recreate、失敗スライスの巻き戻しを確認。LLVM ORC実スレッド、WASM差分、LLVM O2 / Binaryen O2も回帰する。

手動マイクロベンチマーク:
cmake --build build-ir-nogpu --target xyo-numeric-continuation-overhead -j4
./build-ir-nogpu/tests/xyo-numeric-continuation-overhead 10000

今回の実測例（同一マシン、各方式3回の最小ns/iteration）:
  legacy : 670.7 ns / iteration
  reused : 15.5 ns / iteration
  43.4x

これはホスト側yield管理コストだけを分離した比較で、Scratch全体の処理速度やLLVM関数呼出し時間、Renderer/GC/scheduler込みの性能ではない。実行前後の公平なE2E比較は別途必要。


## 動的repeat回数 / Scratch Math.round 互換の共通CFG（2026-11-07）

実験フラグ XYO_JIT_COMMON_CFG=1 の数値サブセットで、
repeatのTIMESに数値変数reporterまたは+,-,*,/の数値式を指定できる。
ループ開始時（preheader）に一度だけ計算し、SSAのi64値として
フレームに保存する。中断・再開の間に元変数が変更されても
**反復回数は再評価しない**。複数の順次repeatそれぞれが独立に
現在の値を取り込む。

- ExecOp::RepeatCount は f64→i64の回数変換専用命令。
  LLVMとWASMの両バックエンドに実装した。
- Math.roundのties-to-positive-infinity、負数→0回、
  数値NaN→0回、正の小数の丸めを揃える。
  0.49999999999999994など、単純なfloor(x+0.5)で
  浮動小数点境界が異なる例は明示的に0回にする。
- 正の計算結果が丸め後に1024を超える場合、生成コードは
  status=2を返す。ホストが未確定の変数/継続フレームを
  破棄して、元のインタプリタへ復帰する。これは実験経路の
  安全な上限であり、Scratch本来の反復回数制限ではない。
- LoopYieldBoundary::limitValue は、そのrepeatの実際に
  確定したカウントのSSA値。途中Deopt時は継続フレーム内の
  保存済み値とカウンタPhiから残り回数を復元し、
  途中で更新された元のTIMES変数を再読込しない。
- 元々サポートしていた静的回数の高速経路は維持。
  静的な小数の定数も同じ丸め規則へ拡張した。
  サポート外の式、非数値の変数、未対応副作用は既存JITへ
  フォールバックする。

検証: LLVM lli / Node WASM / LLVM O2 / wasm-opt O2の
差分テストに動的2.5回、y+2回、0.5回、-0.5回、NaN、
0.49999999999999994回、静的な1.5回を追加。
実Thread/Sequencerでは、TIMES変数を反復途中で
変更しても回数が固定されること、動的回数からのDeopt、
2番目のrepeatが直前の変更を読み取ってから固定すること、
上限超過時の元インタプリタへの復帰を検証する。

注意: 式の演算は本共通IRでサポートする数値専用サブセットに
限定される。文字列変換、リスト、カスタムブロック等を含む
Scratchの全仕様にはまだ対応していない。


## Target変数マップ世代管理と変数束縛キャッシュ（2026-11-07）

共通Source IRの数値スクリプトで、従来ScratchNumericSlotsのrefreshとcommitが各スロットに対して毎回行っていたID検索を最適化した。

- src/model/variable_map.hppにVariableMapを追加。既存unordered_mapインターフェースの利用部分と互換性を保ち、emplace、erase、operator[]、代入、swap、insertなどの操作でマップ構造の世代番号を更新。世代番号の発行はプロセス共通の原子的カウンタを使用する。
- Target::variableBindingStamp() はローカル変数マップの世代、参照するステージTarget、ステージ側マップの世代を合わせて返す。ステージ変数のshadow・削除・再生成と、ステージTarget自体の変更を検出する。
- ScratchNumericSlotsは最初の1回だけ変数IDの解決と重複エイリアス検査を実施。後続スライスでは世代が一致すればVariable*を再利用し、世代不一致のときだけID再検索する。commitでは世代検証をポインタ逆参照より先に行い、途中の構造変更があれば一切書き込まない。
- 変数の型・Cloud属性・元値との一致、NaN正規化、型フィードバック更新の検証は従来通り残した。スケジューラの同期スライス間での使用を前提とし、変数マップの並列書換えはサポートしない。
- Runtime::getTargetForStage()は、一般的なScratchプロジェクトのようにtargets[0]がステージならO(1)で返し、それ以外は従来どおり線形検索する。
- tests/platform/scratch_variable_slots_smoke.cppは変数削除・再生成、マップ代入、ステージ変数のローカルshadow、その解除、独立したローカルバインディング、ステージ変数の削除時に安全にrollbackするケースを確認する。

変数IDの再検索のみを分離した手動マイクロベンチマーク（JIT実行時間は含まない）:

    cmake --build build-ir-nogpu --target xyo-numeric-binding-cache -j4
    ./build-ir-nogpu/tests/xyo-numeric-binding-cache 100000

100,000スライス、各構成5回の最良値:
- 1変数: 毎回ID検索10.7 ns/slice → 世代キャッシュ8.5 ns/slice（1.3倍）
- 8変数: 毎回ID検索241.7 ns/slice → 世代キャッシュ24.1 ns/slice（10.0倍）
- 32変数: 毎回ID検索1934.3 ns/slice → 世代キャッシュ85.7 ns/slice（22.6倍）

一部をステージ共有変数、一部をローカル変数とした試験。両方式とも固定バッファ再利用・型ガード・元値検査・コミットを行う。LLVMの実行処理やScratch全体のフレームレートに対する改善率ではない。

留意事項: Target::variablesは公開APIなので、VariableMapを基底unordered_map型へ明示キャストして直接構造変更する操作は世代管理の契約外。extractとmergeは未対応として削除している。
