# xyo-cpp 現状仕様差分

このファイルは `scratch_llvm_vm_spec.md` を基準に、現在の `xyo-cpp`
実装が異なる点を列挙する。大きな設計思想、C ABI、`SValue`、
`SFrame`、host/LLVM 境界の基本形は元仕様を踏襲しているが、現状は
Scratch 互換 VM/JIT の MVP として段階実装されている。

## 1. プロジェクト構成

- 元仕様の `scratch-llvm-vm/` ディレクトリは作らず、このリポジトリの
  ルートをそのままプロジェクトルートとして使う。
- 元仕様にある `src/module_linker.cpp`, `src/sb3_loader.cpp`,
  `src/lowering.cpp`, `src/codegen.cpp` は未作成。
- SB3 読み込み、JSON 解析、簡易 lowering は `src/project_loader.cpp` に
  集約されている。
- LLVM script module 生成は `src/jit.cpp` に集約されている。
- `quickjs_subset/` は最小構成で追加済み。QuickJS VM全体ではなく
  `quickjs/dtoa.c` / `quickjs/cutils.c` と薄い wrapper だけを使い、
  数値文字列 parse と number-to-string を runtime bitcode に含める。
- 元仕様の複数 fixture / integration test 構成ではなく、現状のテストは
  主に `tests/unit/runtime_tests.cpp` に集約されている。
- `Makefile` が主ビルド経路として追加されている。CMake も存在するが、
  README ではこの環境に `cmake` がない場合の代替として Makefile を案内する。

## 2. Host 側の責務

- 元仕様では host は GUI、入力、アセット、LLJIT、trampoline に限定する方針。
  現状もこの方針を保つが、MVP の都合で以下を host 側に持つ。
- `src/host_app.cpp` に SDL2 の最小ウィンドウ描画がある。
- stage は 480x360 の固定 Scratch 座標系として扱い、SB3 内の SVG costume を
  rasterize して stage / sprite を表示する。
- 変数 monitor は SDL 側で簡易バー/テキストとして描画される。
- pen buffer は runtime が保持し、C++ host が論理ステージとは独立した
  最低2倍解像度の Skia `SkSurface` に増分描画し、SDL が永続 pen layer
  として表示する。ペン解像度の変更によって表示ステージ自体のサイズは
  変更しない。
- sprite の ghost effect は SDL texture alpha、pixelate effect は一時的な
  low-resolution render target と nearest-neighbor upscale で表示する。effect の
  runtime state は Scratch の7種類すべてを保持する。
- `src/main.cpp` は `--window`, `--gui`, `--frames`, `--fps`, `--turbo`,
  `--compatibility`, `--turbowarp-compat`, `--scratch-compat`, `--list-limit`,
  `--stage-scale`, `--emit-ll` の CLI を持つ。ウィンドウはリサイズ可能で、
  ステージは4:3を維持して中央配置する。
- TurboWarp互換では、マウスホイールの上下を`up arrow`/`down arrow`の
  key hatとして発火する。これはキーボードの押下状態には反映しない。

## 3. JIT / LLVM 生成範囲

- 元仕様では script/procedure ごとに pc-based state machine を LLVM IR へ
  lowering する方針。
- 現状の JIT は script entry の `switch(frame->pc)` dispatch を LLVM IR で
  生成し、top-level statement だけでなく control substack 内の statement にも
  pc を割り当てる。
- `sensing_resettimer`, `data_setvariableto`, `data_changevariableby`,
  `data_addtolist`, `data_deleteoflist`, `data_deletealloflist`,
  `data_insertatlist`, `data_replaceitemoflist`, `looks_say`,
  `pen_clear`, `pen_penDown`, `pen_penUp`, `pen_setPenSizeTo`,
  `pen_setPenColorToColor`, `pen_changePenColorParamBy`,
  `motion_gotoxy`, `motion_setx`, `motion_sety`, `motion_changexby`,
  `motion_changeyby`, `looks_show`, `looks_hide`, `looks_setsizeto`,
  `looks_switchbackdropto`, `looks_seteffectto`, `looks_changeeffectby`,
  `looks_cleargraphiceffects`, `sensing_setdragmode`,
  `control_stop` の `this script` などは switch case から直接 lowering する。
  `all` と `other scripts in sprite` は scheduler 全体への効果を保つ汎用 ABI 境界を
  呼び、後続 statement を実行しない。
- `control_repeat`, `control_repeat_until`, `control_while`,
  `control_forever`, `control_for_each`, `control_if`, `control_if_else`
  は JIT 側の pc state machine に展開し、loop counter / branch_active /
  yield 判定だけ C ABI helper を呼ぶ。
- 数値/真偽/nullおよび単純な数値文字列literalはLLVM定数へ直接loweringする。
- timer, mouse x/y, mouse down, literal key pressed, 数値四則演算/mod,
  数値比較, 数値/真偽値 variable read, 数値/真偽値 variable set/change の
  hot path はLLVMのload/store/mathへ直接loweringする。通常モードの variable
  lookup は runtime identity、owner target id、index、type、expected name を検査する handle helper
  で現在の storage を解決し、その後の数値 load/store を native IR で行う。raw owner
  cache を生成 IR から直接 dereference するのは明示的な unsafe profiling mode だけで
  ある。
- 生成IRは数値化済みの `double` を `sjit_sprite_set_xy`,
  `sjit_jit_pen_set_size_number`, `sjit_jit_sprite_set_size` などへ渡す。
- procedure call は、JIT 対象 procedure なら生成済み procedure 関数を直接呼び、
  対象外、arity 不一致、型 guard 不成立などの場合は C ABI の汎用 procedure 実行へ
  フォールバックする。
- 生成 IR から ordinary statement ごとに
  `sjit_script_execute_statement_with_frame` を呼ぶ構成には戻さない。ただし、native
  lowering の正しさを保証できない opcode は、値を捏造したり NOOP にしたりせず、
  top-level tree なら module 生成前に script entry 全体を interpreter へ切り替え、
  procedure body ならその procedure call を汎用実行へ切り替える。
- JIT entry は `build/sjit_runtime.bc` または `SJIT_RUNTIME_BC` が指す bitcode を
  script module 自体へ private link する。review 済みの numeric/frame/sprite leaf
  helper に対象を絞り、runtime 定義で script declaration を解決
  した後に private internalize し、到達する helper の本体と Clang が付与した
  attribute を script と同じ O3 pipeline から見えるようにする。未到達の定義は
  GlobalDCE で除去する。interpreter/deoptimization entry は外部境界へ残し、cold
  fallback のためだけに runtime 全体を各 module へ複製しない。bitcode がない場合、または
  `SJIT_DISABLE_SCRIPT_RUNTIME_LINK` が設定されている場合は、従来どおり外部 symbol
  境界を残す。
- LLJIT 初期化や script JIT に失敗した場合、`sjit_script_interpreter_entry`
  にフォールバックする。
- `compiler.cpp` の `compileProjectSkeleton()` はまだ骨組みで、実際の SB3
  lowering は `project_loader.cpp` が担う。
- `--emit-ll <output.ll>` は smoke module を出力する。
- `--emit-ll <project.sb3> <output.ll>` は SB3 を読み込み、登録された hat script
  の LLVM IR を出力する。複数 script がある場合は2本目以降に `.1.ll` などの
  suffix を付けて出力する。

### 3.1 Opcode effect と fallback の契約

- expression / statement の effect 判定は `runtime/sjit_opcode_effects.h` の
  `OpcodeEffects` に集約する。各 opcode について `canYield`, `canAllocate`,
  `canMutateTarget`, `canChangeValueType`, `canCallUnknown`,
  `requiresInterpreter` を返し、JIT eligibility、同期 region 判定、procedure の
  fast path 判定が同じ情報を参照する。
- enum に存在しない opcode は全 effect が true の保守的な分類になる。したがって、
  新しい opcode を追加したのに分類を追加し忘れても、native code が空文字、0、
  NOOP などの仮の意味で実行されることはない。
- SB3 loader が認識できない reporter / statement に遭遇した場合は、opcode と block
  id を含む error で project load を失敗させる。これは、意味を実装していない
  block に interpreter semantics も存在しないためである。
- loader と interpreter が意味を知っている一方で native lowering が未完成の
  reporter は `requiresInterpreter` にする。現在は `motion_direction` がこの経路を
  使い、reporter が条件、算術、list 操作などに
  ネストしていても見落とさない。top-level script tree に含まれる場合は script
  entry 全体を interpreter にし、procedure body に含まれる場合はその procedure を
  native eligibility から外して汎用 procedure 実行へ戻す。
- `operator_contains` は case-insensitive な Scratch 文字列 semantics を共有 C helper
  で実装し、生成 IR は左右の値を一時 `SValue` slot に materialize して helper を
  直接呼ぶ。空文字や数値を仮値に置換する fallback は使わない。
- `JitEngine::compileScript()` がこの preflight に該当すると
  `sjit_script_interpreter_entry` を返し、`lastFallbackReason()` に opcode と AST path
  を保存する。debug build、または `SJIT_LOG_JIT_FALLBACK` 設定時には同じ理由を
  stderr に出す。

### 3.2 PC state と同期 region

- `SFrame::pc` は各 native block 間の一時的な branch variable ではなく、scheduler
  から再開するための resume/deoptimization record とする。
- yield しない region 内の遷移は関数内の `pcSlot` だけを更新する。この slot は O3
  pipeline で SSA 化されるため、statement ごとに `frame->pc` を load/store しない。
- `frame->pc` へは、`OpcodeEffects` が yield、unknown call、interpreter 呼び出しの
  可能性を示す helper の直前、`YIELDED` / `YIELD_TICK` / `WAITING` を返す箇所、
  および script 完了時に resume pc を commit する。runtime helper が pc を読む場合も
  この safepoint の値を観測する。

### 3.3 Procedure argument ABI

- procedure 関数の ABI は numeric argument array (`double *`) と boxed argument
  array (`SValue *`) の両方を持つ。callee が対象 argument を数値としてしか使わず、
  procedure が suspend/unknown call を行わず、argument expression が native 数値
  lowering と再評価の両方に安全な場合は、numeric array だけを構築して boxed array
  に null を渡す。
- 動的な値には数値型 guard を付ける。guard 成功時だけ direct numeric ABI を使い、
  失敗時は汎用 procedure 実行で元の argument semantics を評価する。guard 判定後の
  fallback で二重評価されるため、乱数や wall-clock reporter のように replay-safe
  でない式はこの fast path の対象にしない。
- boxed value が必要、yield し得る、または native eligibility を満たさない場合は、
  numeric/boxed の両 array を構築する従来経路か interpreter procedure 経路を使う。
- procedure argument reporter を variable set / list add / list replace に直接渡す場合は、
  interpreter と JIT が共有する `sjit_value_try_number_for_set` で数値化の可否を判定する。
  数値文字列、bool、null、NaN の正規化を両経路で一致させ、非数文字列・空白文字列や
  argument reporter を介さない文字列 literal の tag は保持する。

## 4. Runtime Bitcode

- persistent ThreadPool 実装を除く `runtime/*.c` と QuickJS-derived subset は LLVM
  bitcode 化できる。`runtime/sjit_thread_pool.c` は host native image にだけ含める。
- `Makefile` の `runtime-bitcode` は `build/sjit_runtime.bc` を生成する。
- `runtime-ll` は `build/sjit_runtime.ll` を生成する。
- `JitEngine` は起動時に `SJIT_RUNTIME_BC`、未指定なら executable 近傍または
  `build/sjit_runtime.bc` を読み込み、runtime entry 用 module として LLJIT に追加する。
  ファイルがない場合は host process の C runtime を absolute symbol として使う。
  `SJIT_RUNTIME_BC` / `SJIT_RUNTIME_OBJECT` override は現行 ABI と一致し、5つの
  `sjit_thread_pool_*` symbol を外部宣言として残す必要がある。pool 定義を埋め込んだ
  旧 artifact は host-resident worker lifetime 契約の対象外とする。
- script module 生成時にも同じ bitcode を同じ LLVM context へ読み込み、選択した leaf 定義を
  external linkage のまま `LinkOnlyNeeded` で link して script declaration を解決し、
  link された runtime 定義だけを internalize する。boxed/string/list/lookup/interpreter
  helper は共有 runtime 境界へ残す。link 直後に未到達定義を
  GlobalDCE し、その後 LLJIT の最適化 pipeline へ渡す。10,000命令以下の通常moduleは
  O3、これを超えるrenderer規模のmoduleはO1を使う。大moduleではuse数が16を超える
  list-number / cached-compare helperもoutlineし、同じ分岐graphがcall siteごとに複製
  されることを防ぐ。`SJIT_JIT_OPT_LEVEL=0..3`で診断用に明示上書きできる。
  これにより helper の inline、constant propagation、
  memory effect attribute を使った load/store 最適化を module 境界を越えて行える。
- script module の診断用 ABI surface を安定させるため、link 前から存在した外部宣言が
  GlobalDCE で消えた場合は宣言だけを復元する。実行時に到達する private helper 定義は
  script module 内に残る。
- `SJIT_DISABLE_SCRIPT_RUNTIME_LINK` は script への private link だけを無効にする比較・
  診断用 option であり、runtime entry module の読み込みや absolute symbol fallback
  自体を無効にはしない。
- `SJIT_DISABLE_SCRIPT_JIT=1` は同じruntime/schedulerを維持したまま全script entryを
  interpreterへ固定するdifferential test用optionである。

## 5. SB3 読み込み

- 元仕様では loader と project model が独立モジュールになる想定。
- 現状は `src/project_loader.cpp` が `.sb3` zip を zlib で読み、`project.json`
  を独自 JSON parser で読む。
- target/sprite、変数、リスト、procedure 定義と、green flag、key、stage/sprite
  click、broadcast、backdrop switch、touching-object、greater-than、clone の各
  hat script を runtime へ登録する。edge hat は毎 tick の立ち上がりだけを検出し、
  hat input が reporter の場合も再評価する。
- costume metadata と SVG/PNG bytes は loader が保持する。音声 bytes の decode と
  実再生は host 側の残件だが、sound block は runtime の audio command queue へ
  正規化して渡す。

## 6. 対応済み statement

現状の SB3 lowering で statement として扱う標準 core opcode は以下。新規に追加した
opcode は保守的な interpreter fallback を通る。

- data: `data_setvariableto`, `data_changevariableby`, `data_addtolist`,
  `data_deleteoflist`, `data_deletealloflist`, `data_insertatlist`,
  `data_replaceitemoflist`
- control: `control_repeat`, `control_repeat_until`, `control_while`,
  `control_if`, `control_if_else`, `control_forever`, `control_for_each`,
  `control_wait`, `control_wait_until`, `control_stop` の全 stop option、
  `control_create_clone_of`, `control_delete_this_clone`, counter、
  `control_all_at_once`
- event: `event_broadcast`, `event_broadcastandwait`
- procedures: `procedures_call`
- looks: say/think、show/hide、costume/backdrop switching（wait/next を含む）、
  size、layer、graphic effect、legacy stretch/hide-all blocks
- motion: `motion_movesteps`, `motion_gotoxy`, `motion_goto`, turn、point、glide、
  edge bounce、rotation style、x/y set/change
- sensing: `sensing_resettimer`, `sensing_setdragmode`, ask/answer
- sound: play/play-until-done、stop、effect、clear effect、volume
- pen: clear/down/up/stamp、set/change size、set color、set/change color parameter、
  legacy shade/hue blocks

`looks_hideallsprites` と stretch 系は Scratch 2 互換の legacy no-op として受理する。

## 7. 対応済み reporter / expression

現状の SB3 lowering で expression として扱う主な opcode は以下。

- operators: `operator_lt`, `operator_equals`, `operator_gt`,
  `operator_and`, `operator_or`, `operator_not`, `operator_add`,
  `operator_subtract`, `operator_multiply`, `operator_divide`,
  `operator_mod`, `operator_random`, `operator_round`, `operator_join`,
  `operator_length`, `operator_letter_of`, `operator_contains`,
  `operator_mathop`
- arguments: `argument_reporter_string_number`, `argument_reporter_boolean`
- data/list: `data_variable`, `data_itemoflist`, `data_itemnumoflist`,
  `data_lengthoflist`, `data_listcontainsitem`
- sensing: `sensing_timer`, `sensing_mousex`, `sensing_mousey`,
  `sensing_mousedown`, `sensing_keypressed`, `sensing_keyoptions`, touch/color、
  distance、`sensing_of`、current date/time、answer、loudness/ loud、online、
  username
- looks: size、costume number/name、backdrop number/name
- sound/control: sound volume、counter
- motion: `motion_xposition`, `motion_yposition`, `motion_direction`
- data/list: `data_listcontents` を含む list reporter
- pen menu: `pen_menu_colorParam`

`motion_direction` と今回追加した target/renderer 依存 reporter は interpreter
evaluation に対応し、native lowering は保守的に無効である。top-level script tree に
含む場合は interpreter entry を使い、procedure body の場合はその procedure call を
汎用実行へ戻す。`operator_contains` は native / interpreter の両方で対応する。
上記以外の未対応 reporter は空文字 literal へ
変換せず、project load 時に opcode と block id を示す error にする。

## 8. Scheduler / Thread / Frame

- `SFrame` の基本 layout は元仕様に近い。
- `SThread` は `id`, `target_id`, `script_id`, `status`, `is_killed`,
  `entry`, `script_data`, `frame` に加え、実行中の同一 hat restart を安全に遅延する
  `is_executing`, `restart_pending` を持つ。
- JavaScript VM の reporter 中断復帰、Promise、warp timer の完全再現はまだない。
- `SJIT_THREAD_PROMISE_WAIT` などの状態値は予約されているが、Promise 実行は未実装。
- `control_repeat`, `control_repeat_until`, `control_for_each`, `control_forever`
  は Scratch VM の loop branch に合わせ、通常モードでは substack 完了後に
  scheduler へ yield する。同一 runtime tick 内でも redraw 要求がなく scheduler
  の巡回が続く場合は、同じ control block が再評価される。
- scheduler は Scratch VM の `Sequencer.stepThreads` に寄せ、1回の
  `sjit_runtime_tick` 内で各 thread を複数巡回できる。巡回は
  `current_step_time_ms * 0.75` の時間予算、active thread の有無、redraw 要求、
  turbo mode によって止まる。通常モードでは redraw 要求で追加巡回を止め、
  turbo mode では redraw 要求があっても時間予算内で継続する。
- `SJIT_STATUS_YIELDED` は Scratch VM の `STATUS_YIELD` と同じく他 thread へ
  譲るだけで、同一 runtime tick 内の後続巡回で再実行され得る。
  `SJIT_STATUS_YIELD_TICK` は次の runtime tick まで再実行しない。
- loop counter と substack の再開位置は、Scratch VM の stackFrame と同じく
  control statement ごとの状態として `SFrame` に保持する。これによりネストした
  `repeat` でも内側ループ中に外側の counter を余分に進めない。
- native entry の同期 region 内では resume state を関数内 SSA に保持し、scheduler
  から観測可能な pause point と完了時にだけ `SFrame::pc` へ commit する。yielding
  helper の直前には現在 pc を materialize するため、helper/interpreter fallback が
  frame を調べても一貫した継続位置を得る。
- `procedures_prototype` の `mutation.warp` が true の手続き内では、loop branch
  完了後も同じ tick 内で継続する。warp timer による時間上限は未実装。
- `control_for_each` の回数判定は Scratch VM と同じく `Number(VALUE)` と現在
  index の比較で行い、`VALUE = 2.7` は `1, 2, 3` の3回になる。
- `sjit_runtime_tick` は thread cleanup、input/edge hat polling、draw buffer clear、
  scheduler tick、done thread cleanup、pen/sprite draw command 生成を行う。

### 8.1 所有権証明付き並列 scheduler

- SB3 loader は script 登録直後に、top-level statement と到達可能な custom
  procedure の木全体を `sjit_analyze_script_ownership` で検査する。interpreter entry は
  public な `sjit_runtime_analyze_script_ownership()` を使う。この API は登録 entry が
  `sjit_script_interpreter_entry` で、全 procedure の `jit_entry` が null の場合だけ証明を
  登録でき、任意の native function pointer を証明済みに昇格させる用途には使えない。
- native entry の証明は `JitEngine::certifyScriptOwnership()` だけが行う。JitEngine の
  private registry が、同じ `SCompiledScript` address、コンパイル済み top-level entry、
  および null を含む全 procedure entry vector と完全一致する場合だけ internal attestation
  API へ進む。runtime は top-level entry と全 procedure entry を証明時に複製保存し、
  scheduler は実行直前に登録 entry/`script_data` と両 snapshot が未変更か照合する。
- native compile/attestation 完了後は、AST の tree shape/opcode/name/literal と procedure
  table/entry vector からなる semantic content を変更せず、arena の address も証明書の
  lifetime 中は安定させる。ownership に関わる variable/procedure cache provenance は
  analysis/attestation 時に検査する。それ以後は標準 helper が同じ owner/index/procedure を
  意味する cache を設定する場合だけ更新を許し、任意の cache 書き換えは契約外とする。
  通常の project loader はこの immutable/stable-address 契約を満たす。
- 証明結果は登録時の `script_id`, target id, entry, `script_data` と結び付ける。generic C API
  で登録しただけの script、重複 script id、登録と異なる script/target、clone-start hat は
  既定で unsafe とする。
- 最初の parallel tier は、original かつ non-stage の sprite が所有する既存の
  pointer-free な number/bool scalar だけを可変状態として認める。pure な数値/論理式、
  input/timer と target-local x/y の読み出し、control flow、および非再帰で木全体を
  証明できる procedure call は対象になり得る。
- ownership pass は、top-level と reachable procedure が参照する owner-local scalar の
  append-only で安定した variable index だけを収集し、重複を除いた dependency manifest と
  して証明書へ保存する。同じ variable を複数回参照しても index は1件になり、AST から到達
  しない local variable は manifest に入れない。
- stage/global/unresolved/cloud variable、dynamic/string-owning scalar、全 list read/write、
  random、days-since-2000、wait、interpreter-only/unknown opcode、欠損・重複・再帰 procedure
  は fail-closed で reject する。list は別 target との深い alias と非 atomic な
  refcount/cache を排除できないため、local に見えてもこの tier では認めない。
- string の literal expression は caller の `SString` を refcount clone せず、文字列 byte を
  AST ごとの新しい `SString` へ deep-copy する。ownership pass はその literal の
  `ref_count == 1` も要求し、別 AST 間で non-atomic refcount/numeric cache が共有されるのを
  防ぐ。
- pen/draw、motion/looks、monitor、broadcast/backdrop、timer reset、drag mode、
  stop-other/stop-all など、runtime または renderer の共有状態へ作用する statement も
  reject する。解析不能、メモリ確保失敗、または新 opcode の分類漏れは安全とは仮定せず
  逐次実行へ戻す。
- scheduler は thread vector の元の順序で走査し、連続する runnable な証明済み entry
  だけを batch 化する。同じ owner target または同じ compiled-script arena を持つ entry
  は同一 batch に入れず、その境界で先行 batch を join する。unsafe、dead、waiting、
  yield-tick などの境界でも同様に flush するため、証明されていない処理と pool task は
  重ならない。
- batch 登録の直前には top-level/全 procedure entry pointer snapshot を照合した後、manifest
  の k 個の index だけを O(k) で走査する。各 index の bounds、`SJIT_VAR_SCALAR` type、
  non-cloud、number/bool `scalar_kind`、現在値が pointer-free な NUMBER/BOOL/NULL tag であること
  を検査する。いずれかの dependency または entry snapshot が stale なら、その実行は pool
  に投入せず既存の逐次経路へ fallback する。manifest にない無関係な local が string/list
  などの owning value でも、この hot guard の reject 理由にはしない。semantic AST と cache
  provenance は前述の immutable/standard-helper 契約により、ここでは再走査しない。
- worker は entry を実行して `SRuntimeStatus` を保存するだけで、batch 全体の完了後に
  scheduler thread が thread vector の元の順序で status、restart、active/aggregate 値を
  commit する。worker の完了順は Scratch-visible な commit/cleanup 順序へ影響しない。
- ThreadPool は runtime ごとに最初の複数 task batch で遅延生成する fixed pthread pool
  である。parallelism が N のとき background worker は N-1 本で、scheduler thread 自身も
  task を取得する。既定値は online CPU 数、上限は32で、runtime 破棄時には target/frame
  などを解放する前に全 worker を join して pool を破棄する。
- persistent ThreadPool の関数本体は LLVM runtime bitcode とそこから作る ORC runtime
  object に埋め込まない。bitcode 内の scheduler は外部宣言を保ち、`JitEngine` が runtime
  code の load 前に process-resident native symbol へ absolute binding する。batch の join
  後は一時的な task callback を null に戻すため、idle worker の実行 PC は host worker
  loop または pthread wait 内にあり、ORC module への instruction pointer を保持しない。
  したがって JitEngine の unload に依存しない。native script entry 自体については従来の
  engine/AST lifetime 契約を引き続き守る。
- `SRuntime` の host-facing lifecycle/event/input/tick API は外部で直列化し、通常 host は
  すべて同一 host thread から呼ぶ。`sjit_runtime_tick` 同士、tick と input mutation、tick と
  `sjit_runtime_destroy` を同時実行してはならない。pool worker は1回の tick 内部でだけ走り、
  tick が戻る前に必ず join されるため、この host API 非 reentrant 契約を広げない。
- `SJIT_THREAD_POOL_SIZE=N` は scheduler を含む総 parallelism を指定し、`N=1` は逐次、
  `SJIT_DISABLE_THREAD_POOL=1` は pool を強制無効化する。`SJIT_LOG_THREAD_POOL=1` は
  requested/active parallelism と最初の実 batch、`SJIT_LOG_OWNERSHIP_ANALYSIS=1` は
  script ごとの可否と reject bit mask を stderr に出す。
- batch が1件だけ、parallel step buffer/pool の確保失敗、pool submit 失敗、または
  実行時の証明書照合失敗では、同じ thread 順序の既存逐次経路へ fallback する。
  `SJIT_PROFILE_RUNTIME` build では profile counter が意図的に non-atomic なため pool を
  作らず常に逐次実行する。
- `sjit_runtime_script_parallel_safe()` と
  `sjit_runtime_script_ownership_reject_flags()` は script 単位の解析結果を返す。
  `sjit_runtime_thread_pool_parallelism()`、`sjit_runtime_parallel_batch_count()`、
  `sjit_runtime_parallel_task_count()` は有効 parallelism と飽和型の実行統計を返し、
  correctness test と診断に使う。

## 9. Event / Hat

- runtime には script registration と `sjit_runtime_start_hats` がある。
- green flag は `sjit_runtime_green_flag` で `stop_all`、timer reset、
  `SJIT_HAT_EVENT_WHENFLAGCLICKED` 起動を行う。
- `sjit_event_broadcast` と `sjit_event_broadcast_and_wait` は runtime helper として
  存在する。
- SB3 loader は green flag、key、stage/sprite click、broadcast、backdrop switch、
  touching-object、greater-than の hats を登録する。clone hat は clone 作成時に
  clone target id へ起動する。
- 実行中の同一 hat を restart する場合は live frame をその場で破壊せず、entry が
  unwind した後に scheduler が `restart_pending` を commit する。stop-all が同時に
  発生した場合は stop を優先する。
- greater-than / touching-object hats は edge activation と dynamic input 再評価に
  対応する。

## 10. Clone / Sprite / Draw

- sprite state、clone 作成の基礎、変数複製、pen state 複製は実装されている。
- `create clone of`, `delete this clone`, `when I start as a clone` は SB3 lowering と
  clone target 用 scheduler 起動まで対応する。clone は元 sprite の変数、pen、costume
  metadata、sound state をコピーし、clone の native specialization は行わない。
- draw command は元仕様より拡張され、pen stroke 用に `x2`, `y2`,
  `pen_width`, `r`, `g`, `b`, `a` を持つ。
- draw buffer は毎 tick の sprite command と、永続 pen buffer から構成される。
- costume/skin/layer の Scratch renderer 互換性は限定的だが、SDL host は SB3 の
  SVG costume を rotation center、position、size、direction を反映して描画する。
- pen stroke の host rasterization は Skia C++ API を使い、round cap、
  anti-alias、SrcOver alpha composition を適用する。

## 11. Pen

- 元仕様では pen は draw command の一種として触れられている程度だが、現状では
  persistent pen buffer が明示的に実装されている。
- `sjit_pen_clear`, `sjit_pen_down`, `sjit_pen_up`,
  `sjit_pen_set_size`, `sjit_pen_set_color_rgb`,
  `sjit_pen_change_color_param` がある。
- 色は RGB と Scratch 風 HSV percent state を持ち、brightness/transparency などの
  param 変更に対応する。
- Skia の premultiplied surface は SDL upload 前に unpremultiplied RGBA32 へ
  変換し、SDL の通常 alpha blend による二重乗算を避ける。

## 12. Procedures

- procedure definition/prototype を SB3 loader が読み、`procedures_call` から
  `SCompiledProcedure` を呼び出す。
- argument reporter は name-based binding で解決する。
- 最大 procedure call depth は runtime 側で固定上限を持つ。
- JIT eligible な procedure は script module 内の native procedure 関数へ直接 call
  できる。数値としてしか使わない argument については `double *` だけを渡す fast
  path があり、boxed `SValue` の生成・破棄を省く。
- 数値 fast path の型仮定は実行時 guard で検査し、失敗時は汎用 procedure evaluator
  へ戻る。yield/unknown call、boxed value 利用、replay-safe でない argument、arity
  不一致は保守的な経路を選ぶ。
- warp mode、recursive yield、JS VM と同等の procedure frame semantics は未完成。

## 13. Values / Cast / Compare / List

- `SValue` layout は元仕様どおり `tag`, `number`, `ptr`。
- Scratch 互換の `toNumber`, `toBoolean`, `compare`, math operator、list index 変換は
  runtime に実装されている。
- list は item limit、`all`/`last`/`random`/`any` 系 index 変換に対応する。
- memory 管理は手動 clone/destroy が中心で、元仕様にある将来 GC 差し替えのための
  allocator API 整備はまだ限定的。
- native entry は immutable な `SCompiledScript` と、それが所有する statement /
  expression / procedure tree の address を借用して IR 定数に埋め込む。この arena は
  JIT entry が呼ばれ得る間、移動も破棄もしてはならない。project を差し替える場合は、
  script arena を破棄する前に owning `JitEngine` を破棄して entry を無効化する。
  engine destructor は、自身が設定し、かつ他 engine に置換されていない procedure
  entry pointer を null に戻す。
- entry 冒頭では `thread->script_data` がコンパイル時の `SCompiledScript` と一致するか
  検査する。これは別 script の entry/arena を誤って組み合わせることを防ぐが、arena
  自体の use-after-free を許容するものではない。
- `thread->target_id` が compile 時の original target と異なる clone/dynamic target
  では、entry 全体を interpreter へ切り替える。procedure の native entry も同じ
  条件では使わない。これは correctness guard であり、clone の native specialization
  は今後の課題である。
- runtime-specialized entry は、単調増加する runtime instance id を entry 冒頭で検査した
  後に限り、runtime が所有する original `SSprite *` を stable owner として借用できる。
  original target object 自体は runtime 破棄まで移動・破棄されない。`SVariable` 配列は
  capacity 増加で再配置され得るため、native script/procedure invocation ごとに owner から
  現在の base を一度読み直し、append-only で安定した index を GEP する。通常モードでは
  `SVariable *` / `SList *` を IR 定数へ埋め込まない。runtime id、script data、target id の
  guard が不成立なら owner を dereference する前に interpreter へ戻る。
- variable handle の解決は invocation 内で同じ `(type, name)` ごとに共有する。native region
  の途中では新しい変数を生成せず、yield 後の再入時には variable-array base を再読込する。
  engine は、その entry が runtime registration から呼ばれ得る間は生存しなければならない。
- renderer の color-list / brightness-list / replace / stamp 融合経路には、この guard 済みの
  invocation-local variable handle を直接渡す。pixel ごとの name/cache lookup だけを省き、
  list index 変換と read → color change → replace → stamp の副作用順序は interpreter と同じにする。
- warp 内の pen row loop は、対応する完全な AST 形状に厳密一致した場合だけ guarded native
  row kernel の候補にする。全 runtime guard と pen buffer reserve が成功する前は Scratch から
  観測可能な副作用を発生させず、不成立時は既存の LLVM loop をそのまま実行する。
  `SJIT_LOG_NATIVE_PEN_ROW=1` で最初の適用または reject 理由を stderr に出せる。
- interpreter / C helper 側の AST variable cache も runtime pointer だけではなく instance id
  を保存し、両方が一致する場合だけ owner id/index を再利用する。allocator が破棄済み
  `SRuntime` と同じ address を再利用しても、古い cache が別 runtime で hit しない。
- `SJIT_UNSAFE_EMBED_RUNTIME_POINTERS` は旧方式との性能比較だけを目的とする実験用
  override である。この設定では runtime object の再配置に対する寿命保証がなく、
  通常実行の契約には含めない。

### 13.1 Hotness metadata

- `SScriptRegistration::invocation_count` は script thread の start/restart 時だけ scheduler
  が増やす、飽和型の `uint64_t` counter である。yield 後に JIT entry へ再入しても増えない。
- counter は現在の単一 scheduler thread が所有する non-atomic metadata で、native hot
  loop に命令を追加しない。`sjit_runtime_script_invocation_count()` から script id 単位で
  読み出せる。
- procedure invocation と loop backedge の counter、閾値判定、optimizing tier、OSR、
  generation 付き code replacement は未実装である。

## 14. Input / Sensing

- `SHostInputSnapshot` layout は元仕様どおり。
- mouse x/y、mouse down、key pressed、timer、reset timer は MVP として動く。
- 音声入力、video sensing、touching-color の decoded pixel sampling は未接続。
  touch object、distance、attribute-of、ask/answer、date/time、loudness/online などの
  runtime reporter は実装済み。headless ask は host が `sjit_runtime_set_answer` で
  answer を注入できる。
- key name は arrow key 用の拡張値を ABI に持つ。

## 15. Looks / Motion

- motion は steps/goto/turn/point/glide/bounce/rotation style と x/y set/change を
  実装する。glide は frame state を使って tick 間で再開する。
- looks は show/hide/size/say/think、costume switching、stage backdrop switching、
  layer、graphic effect state を実装する。backdrop は exact name、1-based number、
  numeric string、next/previous/random の選択と `when backdrop switches to` hat 起動に
  対応する。
- color/fisheye/whirl/pixelate/mosaic/brightness/ghost の7 effect state を sprite ごとに
  保持し、set/change/clear と clone copy に対応する。host renderer で現在表示に反映する
  effect は ghost と pixelate である。
- `sensing_setdragmode` は sprite の draggable state を更新する。
- `say`/`think` は runtime sprite state と `printf` を更新する。SDL の bubble skin
  rendering は未接続。

## 16. 未実装または元仕様から大きく不足している領域

- block graph 全体を LLVM IR へ直接 lowering する codegen（追加 core block は
  interpreter fallback）。
- Scratch renderer 互換の costume/skin/draw list。
- audio asset decode、実再生、`play until done` の音声長に基づく厳密な待機。
- monitor 更新 thread と Scratch monitor 表示 semantics。
- touching-color / color-touching-color の decoded costume pixel sampling。
- ask/answer の GUI 入力 widget。
- Scratch renderer 互換の bubble skin、残りの effect shader、clone の native specialization。
- warp mode / procedure yield semantics。
- 音声 asset loader/decode と host audio backend。
- Scratch VM と同等の stop option 全種。
- GUI 以外を完全に LLVM 側へ寄せる最終形。

## 17. 現状の実行方法

```sh
make
make test
./build/scratch-llvm-vm
./build/scratch-llvm-vm project.sb3
./build/scratch-llvm-vm --window project.sb3
SDL_VIDEODRIVER=offscreen ./build/scratch-llvm-vm --window project.sb3 --frames 3
./build/scratch-llvm-vm --window 3D.sb3 --fps 60 --turbo
./build/scratch-llvm-vm --window 3D2.sb3 --fps 60 --turbo
./build/scratch-llvm-vm --window 3DOcean.sb3 --fps 60 --turbo
./build/scratch-llvm-vm --window teapot.sb3 --fps 60 --turbo
./build/scratch-llvm-vm --emit-ll build/sjit_smoke.ll
make runtime-ll
```

`cmake` が使える環境では以下も意図された経路として残っている。

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```
