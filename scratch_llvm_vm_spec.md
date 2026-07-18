# Scratch LLVM VM / JIT 新規実装仕様書

## 0. 目的

本仕様書は、アップロードされた `scratch-editor` ソース、特に `packages/scratch-vm` の実装を基準に、Scratch互換VMを「GUI以外は可能な限りLLVM側で実行する」方針で一から作り直すための実装仕様である。

この仕様の主目的は次の3点である。

1. Scratch VMの意味論を、JavaScript実装から読み取ってC ABI / LLVM bitcode / LLVM IR生成へ移植する。
2. host側をGUI・描画API・入力収集・アセット読み込み・LLJIT起動に限定し、scheduler、thread、frame、variable、list、event、broadcast、clone、block実行をLLVM側へ寄せる。
3. Codex等の実装エージェントに渡して、一貫した設計で段階的に実装できる仕様にする。

本仕様でいう「LLVM側」とは、以下のどちらかを指す。

- LLVM C++ APIでScratch scriptごとに生成するJIT module
- C/C++ runtimeをclangでLLVM bitcode化し、JIT moduleへlinkするruntime module

つまり、全処理を必ずLLVM IR builderで直書きするという意味ではない。動的値、文字列、リスト、scheduler、clone、event dispatchなどの複雑な処理はC runtimeとして書き、それをLLVM bitcodeにしてJIT moduleとlinkする。

---

## 1. 参照した実ソース

この仕様は、以下のファイルを基準にしている。

### リポジトリ構成

- `AGENTS.md`
  - monorepo構成が `packages/scratch-gui`, `packages/scratch-vm`, `packages/scratch-render`, `packages/scratch-svg-renderer` 等であることを確認。
  - `scratch-vm` はJavaScript + webpack + Tap構成。
- `packages/scratch-vm/package.json`
  - パッケージ名は `@scratch/scratch-vm`。
  - ライセンスは `AGPL-3.0-only`。
  - build/test/lintは `webpack`, `tap`, `eslint`。

### VM中核

- `packages/scratch-vm/src/engine/runtime.js`
  - Runtimeがthread一覧、target一覧、hat起動、green flag、stopAll、step処理、renderer呼び出しを管理。
  - `_pushThread` は `Thread` を作り、targetとblock containerを設定し、stackへtop blockを積んで `this.threads` へ入れる。
  - `startHats` はhat opcode、match field、restart policyに基づいてthreadを起動または再起動する。
  - `_step` はedge-activated hatsを処理し、monitorをpushし、`Sequencer.stepThreads()` を呼び、renderer drawと更新eventを発火する。
  - `greenFlag` は `stopAll`、project timer reset、targetのgreen flag通知、`event_whenflagclicked` 起動を行う。

- `packages/scratch-vm/src/engine/sequencer.js`
  - `stepThreads` は現在step timeの75%をwork timeとして使う。
  - `ranFirstTick` により `STATUS_YIELD_TICK` の扱いを制御する。
  - `stepThread` はblock stackを進め、yield/wait/done/loop/procedureを処理する。
  - `stepToBranch` はC型ブロックのsubstackへ入る。
  - `stepToProcedure` はprocedure definitionをstackへpushし、warp modeとrecursive yieldを扱う。

- `packages/scratch-vm/src/engine/thread.js`
  - Threadは `topBlock`, `stack`, `stackFrames`, `status`, `target`, `blockContainer`, `warpTimer`, `justReported` を持つ。
  - statusは `RUNNING`, `PROMISE_WAIT`, `YIELD`, `YIELD_TICK`, `DONE`。
  - StackFrameは `isLoop`, `warpMode`, `justReported`, `reporting`, `reported`, `waitingReporter`, `params`, `executionContext` を持つ。

- `packages/scratch-vm/src/engine/execute.js`
  - block input/reporter評価とprimitive呼び出しを行う。
  - Promiseが返った場合はthreadを `STATUS_PROMISE_WAIT` にし、resolve後に復帰する。
  - reporterの中断復帰用に `reporting` と `reported` を保存する。

- `packages/scratch-vm/src/engine/block-utility.js`
  - primitiveから見えるruntime API。
  - `yield()`, `yieldTick()`, `startBranch()`, `stopAll()`, `stopThisScript()`, `startProcedure()`, `startHats()`, `ioQuery()` が重要。

### ブロック意味論

- `packages/scratch-vm/src/blocks/scratch3_control.js`
  - `repeat`, `forever`, `wait`, `if`, `if_else`, `stop`, `create_clone_of`, `delete_this_clone`。
  - `repeat` はstackFrameにloopCounterを保存する。
  - `wait` はstack timerを使い、初回と未完了時にyieldする。

- `packages/scratch-vm/src/blocks/scratch3_event.js`
  - `broadcast`, `broadcastAndWait`, event hats。
  - `broadcastAndWait` は起動したthread群をstackFrameに保存し、全thread完了までyield/yieldTickする。

- `packages/scratch-vm/src/blocks/scratch3_data.js`
  - 変数、リスト、list item limit、list index変換、monitor更新フラグ。
  - listの中身表示は、全要素が1文字stringなら結合、そうでなければ空白区切り。

- `packages/scratch-vm/src/blocks/scratch3_operators.js`
  - 加減乗除は `Cast.toNumber`。
  - 比較は `Cast.compare`。
  - `random` は整数判定時だけ整数乱数。
  - `mod` はScratch互換のfloor mod。
  - 三角関数はdegree単位。

- `packages/scratch-vm/src/util/cast.js`
  - Scratch特有の型変換。
  - `toNumber`: NaNは0扱い。
  - `toBoolean`: `''`, `'0'`, `'false'` はfalse。その他のstringはtrue。
  - `compare`: 両方数値化できれば数値比較。ただし空白系が0になる場合はNaN扱いに戻し、文字列比較へ落とす。文字列比較は小文字化してcase-insensitive。
  - `toListIndex`: `all`, `last`, `random`, `any` を扱う。

### Target / Sprite / Clone / Renderer境界

- `packages/scratch-vm/src/engine/target.js`
  - Targetはcode-running object。
  - `variables`, `comments`, `_customState`, `_edgeActivatedHatValues` を持つ。
  - `lookupOrCreateVariable`, `lookupBroadcastMsg`, `duplicateVariables` が重要。

- `packages/scratch-vm/src/sprites/rendered-target.js`
  - Sprite/cloneの実行時状態。
  - `x`, `y`, `direction`, `visible`, `size`, `currentCostume`, `rotationStyle`, `effects`, `variables` を持つ。
  - `setXY`, `setDirection`, `setVisible`, `setCostume`, `makeClone`, `dispose` が重要。
  - renderer呼び出しはhost側境界に置き換える。

- `packages/scratch-render/src/RenderWebGL.js`
  - Scratch rendererはdrawable ID、skin ID、position、direction/scale、visibility、draw listを持つ。
  - LLVM VMでは実描画せず、draw command bufferとしてhostへ返す。

---

## 2. 最重要設計方針

### 2.1 Host側に残すもの

host C++側に残すのは以下に限定する。

- window作成
- WebGPU/OpenGL/Vulkan/SDL/ブラウザCanvas等への実描画
- キーボード、マウス、タッチ、音声入力などOS依存入力の収集
- ファイル読み込み、SB3 zip展開、画像/音声アセット読み込み
- LLJIT初期化
- runtime bitcode読み込みとlink
- JIT済み関数の呼び出し
- host API trampoline
- デバッグログ出力

host側はScratch VM意味論を持たない。つまりhost側は「このblockはrepeatだからloopCounterを持つ」「broadcast and waitはthreadを待つ」「Scratch比較はcase-insensitive」などを知らない。

### 2.2 LLVM側へ移すもの

以下はLLVM側で実行する。

- Scratch script実行
- block graph実行
- scheduler
- thread管理
- frame/stack管理
- yield/yieldTick/wait
- warp modeの土台
- green flag hats
- event hats
- broadcast / broadcast and wait
- clone生成・削除
- sprite/clone状態管理
- 変数管理
- list管理
- 動的値 `SValue`
- Scratch互換型変換
- Scratch互換比較
- operator/control/data/event/motion/looksの中核意味論
- draw command queue生成
- audio command queue生成
- monitor更新状態生成

---

## 3. 全体アーキテクチャ

```text
Host C++ Application
  ├─ window / canvas / renderer
  ├─ asset loader
  ├─ input collector
  ├─ LLJIT setup
  ├─ runtime.bc loader
  ├─ generated script module loader
  └─ per-frame call: sjit_runtime_tick(runtime, input, time)

LLVM linked world
  ├─ runtime bitcode module
  │   ├─ SValue / string / list / variable
  │   ├─ target / sprite / clone
  │   ├─ scheduler / thread / frame
  │   ├─ event / broadcast / hats
  │   ├─ Scratch-compatible cast / compare
  │   ├─ draw command buffer
  │   └─ host API stubs
  └─ generated script modules
      ├─ one function per script/procedure
      ├─ pc-based state machine
      ├─ calls runtime helper via C ABI
      └─ returns SRuntimeStatus
```

---

## 4. ディレクトリ構成

新規プロジェクトは以下を標準構成とする。

```text
scratch-llvm-vm/
  CMakeLists.txt
  README.md
  SPEC.md

  include/
    sjit/
      abi.hpp
      compiler.hpp
      jit.hpp
      project_loader.hpp
      host_app.hpp

  src/
    main.cpp
    host_app.cpp
    jit.cpp
    module_linker.cpp
    project_loader.cpp
    sb3_loader.cpp
    compiler.cpp
    lowering.cpp
    codegen.cpp

  runtime/
    sjit_abi.h
    sjit_config.h
    sjit_value.h
    sjit_value.c
    sjit_string.h
    sjit_string.c
    sjit_number.h
    sjit_number.c
    sjit_compare.h
    sjit_compare.c
    sjit_list.h
    sjit_list.c
    sjit_variable.h
    sjit_variable.c
    sjit_target.h
    sjit_target.c
    sjit_sprite.h
    sjit_sprite.c
    sjit_clone.h
    sjit_clone.c
    sjit_frame.h
    sjit_frame.c
    sjit_thread.h
    sjit_thread.c
    sjit_scheduler.h
    sjit_scheduler.c
    sjit_event.h
    sjit_event.c
    sjit_draw.h
    sjit_draw.c
    sjit_motion.h
    sjit_motion.c
    sjit_looks.h
    sjit_looks.c
    sjit_control.h
    sjit_control.c
    sjit_data.h
    sjit_data.c
    sjit_operator.h
    sjit_operator.c
    sjit_runtime.h
    sjit_runtime.c
    sjit_host_api.h
    sjit_host_api.c

  quickjs_subset/
    LICENSE.quickjs.txt
    qjs_dtoa.c
    qjs_atod.c
    qjs_number.h

  tests/
    fixtures/
      numeric.sb3.json
      control.sb3.json
      broadcast.sb3.json
      clone.sb3.json
    unit/
      value_test.cpp
      cast_test.cpp
      compare_test.cpp
      list_test.cpp
      scheduler_test.cpp
      frame_test.cpp
    integration/
      smoke_test.cpp
      control_test.cpp
      event_test.cpp
      clone_test.cpp
      draw_command_test.cpp
```

---

## 5. C ABI

JIT生成IRとruntime bitcodeはC ABIで接続する。C++ classやstd::string/std::vectorをABI境界に出してはいけない。

### 5.1 基本型

```c
typedef struct SRuntime SRuntime;
typedef struct SProject SProject;
typedef struct STarget STarget;
typedef struct SSprite SSprite;
typedef struct SClone SClone;
typedef struct SThread SThread;
typedef struct SFrame SFrame;
typedef struct SList SList;
typedef struct SString SString;

typedef enum {
    SJIT_STATUS_OK = 0,
    SJIT_STATUS_YIELDED = 1,
    SJIT_STATUS_YIELD_TICK = 2,
    SJIT_STATUS_WAITING = 3,
    SJIT_STATUS_DONE = 4,
    SJIT_STATUS_ERROR = 5
} SRuntimeStatus;
```

### 5.2 動的値

Scratch互換値は `SValue` で表現する。

```c
typedef enum {
    SJIT_VALUE_NUMBER = 0,
    SJIT_VALUE_BOOL = 1,
    SJIT_VALUE_STRING = 2,
    SJIT_VALUE_LIST = 3,
    SJIT_VALUE_NULL = 4
} SValueTag;

typedef struct {
    int tag;
    double number;
    void *ptr;
} SValue;
```

制約:

- numberはIEEE 754 double。
- boolは `number` に0/1を置く。
- string/listはruntime allocator管理のポインタを `ptr` に置く。
- JIT生成IRでは、`SValue` をLLVM structとして扱う。
- 初期実装ではarena allocatorでよい。
- 将来GCに差し替えるため、allocation APIを必ずruntime関数化する。

---

## 6. Host / LLVM 境界

### 6.1 HostからLLVMへ渡す入力

```c
typedef struct {
    double now_ms;
    double delta_ms;
    double mouse_x;
    double mouse_y;
    int mouse_down;
    int key_down[256];
    int key_pressed_edge[256];
    int stage_clicked;
    int sprite_clicked_id;
} SHostInputSnapshot;
```

hostは毎frame入力snapshotを作ってruntimeへ渡す。

```c
void sjit_runtime_set_input(SRuntime *runtime, const SHostInputSnapshot *input);
void sjit_runtime_set_time(SRuntime *runtime, double now_ms, double delta_ms);
```

LLVM側はこのsnapshotを読んで、key hats、mouse sensing、click hats等を処理する。

### 6.2 LLVMからHostへ返す描画命令

```c
typedef enum {
    SJIT_DRAW_CLEAR = 0,
    SJIT_DRAW_SPRITE = 1,
    SJIT_DRAW_TEXT_BUBBLE = 2,
    SJIT_DRAW_PEN_STROKE = 3,
    SJIT_DRAW_LAYER_CHANGE = 4
} SDrawCommandKind;

typedef struct {
    int kind;
    int target_id;
    int drawable_id;
    int costume_id;
    double x;
    double y;
    double direction;
    double size;
    int visible;
    int layer;
} SDrawCommand;

typedef struct {
    SDrawCommand *items;
    int length;
    int capacity;
} SDrawCommandBuffer;

const SDrawCommandBuffer *sjit_runtime_get_draw_commands(SRuntime *runtime);
void sjit_runtime_clear_draw_commands(SRuntime *runtime);
```

host rendererは命令を読むだけで、Scratch VM意味論を実装しない。

---

## 7. Runtime API

```c
SRuntime *sjit_runtime_create(void);
void sjit_runtime_destroy(SRuntime *runtime);

int sjit_runtime_load_project(SRuntime *runtime, const SProject *project);
void sjit_runtime_green_flag(SRuntime *runtime);
void sjit_runtime_stop_all(SRuntime *runtime);
SRuntimeStatus sjit_runtime_tick(SRuntime *runtime);

void sjit_runtime_set_input(SRuntime *runtime, const SHostInputSnapshot *input);
void sjit_runtime_set_time(SRuntime *runtime, double now_ms, double delta_ms);

const SDrawCommandBuffer *sjit_runtime_get_draw_commands(SRuntime *runtime);
void sjit_runtime_clear_draw_commands(SRuntime *runtime);
```

`sjit_runtime_tick` 内部で行う順序は、`runtime.js` の `_step` に合わせる。

1. killed threadの掃除
2. edge-activated hatsの評価と起動
3. redrawRequested初期化
4. monitor更新threadのpush
5. scheduler step
6. done threadの回収
7. draw command buffer生成
8. target/monitor更新状態の確定

---

## 8. Thread / Frame / Scheduler

### 8.1 Thread status

JavaScript版の `Thread.STATUS_*` に対応させる。

```c
typedef enum {
    SJIT_THREAD_RUNNING = 0,
    SJIT_THREAD_PROMISE_WAIT = 1,
    SJIT_THREAD_YIELD = 2,
    SJIT_THREAD_YIELD_TICK = 3,
    SJIT_THREAD_DONE = 4,
    SJIT_THREAD_KILLED = 5
} SThreadStatus;
```

MVPではPromiseは完全実装しない。ただし拡張/I/O blockのために状態値は予約する。

### 8.2 Frame

ScratchのStackFrameはJSでは柔軟なobjectだが、LLVM VMでは型付きframeに落とす。

```c
#define SJIT_MAX_LOCALS 256
#define SJIT_MAX_STACK 256
#define SJIT_MAX_PARAMS 64

typedef struct {
    int pc;
    int return_pc;
    int is_loop;
    int warp_mode;
    int finished;

    double wake_time_ms;
    int loop_counter;

    SValue locals[SJIT_MAX_LOCALS];
    SValue stack[SJIT_MAX_STACK];
    int stack_top;

    SValue params[SJIT_MAX_PARAMS];
    int param_count;

    int waiting_child_count;
    int started_thread_begin;
    int started_thread_count;
} SFrame;
```

JS版の `executionContext` はCではopcode別の専用fieldへ分解する。例えば `control_wait` のtimerは `wake_time_ms`、`control_repeat` のcounterは `loop_counter`。

### 8.3 State machine lowering

LLVM coroutineは使わない。scriptは明示的なpc state machineにloweringする。

```c
SRuntimeStatus sjit_script_entry_42(SRuntime *runtime, SThread *thread, SFrame *frame);
```

生成IRは概念的に以下になる。

```c
switch (frame->pc) {
case 0:
    // block A
    frame->pc = 1;
    return SJIT_STATUS_YIELDED;
case 1:
    // block B
    frame->pc = 2;
    break;
case 2:
    frame->finished = 1;
    return SJIT_STATUS_DONE;
}
```

`yield` と `yieldTick` は必ず `frame->pc` を保存して戻る。

### 8.4 Scheduler

`Sequencer.stepThreads` の意味論に合わせる。

- 1tickのwork budgetは `current_step_time * 0.75`。
- `turboMode` でなければredraw request後にtick内実行を止める。
- `STATUS_YIELD_TICK` は、tickの先頭でのみrunningへ戻す。
- warp mode threadは `WARP_TIME = 500ms` までyieldを内部で再実行可能。
- loop stack frameはbranch終了後に親loop blockへ戻る。

API:

```c
void sjit_scheduler_add_thread(SRuntime *runtime, int target_id, int script_id);
void sjit_scheduler_restart_thread(SRuntime *runtime, int thread_id);
void sjit_scheduler_stop_thread(SRuntime *runtime, int thread_id);
void sjit_scheduler_stop_for_target(SRuntime *runtime, int target_id, int except_thread_id);
SRuntimeStatus sjit_scheduler_tick(SRuntime *runtime);
```

---

## 9. Event / Hat / Broadcast

### 9.1 Hat metadata

`scratch3_event.js` と `scratch3_control.js` のhat定義に合わせる。

```c
typedef struct {
    int opcode_id;
    int restart_existing_threads;
    int edge_activated;
} SHatMeta;
```

最低限のhat:

- `event_whenflagclicked`: restart existing true
- `event_whenkeypressed`: restart existing false
- `event_whenthisspriteclicked`: restart existing true
- `event_whenstageclicked`: restart existing true
- `event_whenbroadcastreceived`: restart existing true
- `event_whengreaterthan`: restart existing false, edge activated true
- `event_whentouchingobject`: restart existing false, edge activated true
- `control_start_as_clone`: restart existing false

### 9.2 startHats

`runtime.js` の `startHats` 互換。

要件:

- requested hat opcodeが未登録なら何もしない。
- match fieldは大文字化して比較する。
- 対象target群を後ろから前へ走査する。
- `restartExistingThreads = true` のhatは、同じtarget/topBlockの既存threadをrestartする。
- `restartExistingThreads = false` のhatは、同じtarget/topBlockのthreadが走っていたら新規起動しない。
- 起動直後、Scratch 2互換のためhat blockを1回executeしてからnext blockへ進める。

### 9.3 broadcast

`event_broadcast`:

- stageのbroadcast variableをidまたはnameでlookupする。
- 見つかったbroadcast名で `event_whenbroadcastreceived` hatsを起動する。

`event_broadcastandwait`:

- 初回実行時にbroadcast variableとstarted threadsをframeへ保存する。
- started threadsが0なら即終了。
- started threadsのいずれかがruntime thread listに残っている間は待つ。
- 全started threadsがwaiting状態なら `YIELD_TICK`、そうでなければ `YIELD`。

---

## 10. Scratch互換型変換

### 10.1 toNumber

`cast.js` の `Cast.toNumber` 互換。

```c
double sjit_to_number(SRuntime *runtime, SValue v);
```

要件:

- numberならそのまま。ただしNaNは0。
- boolは0/1。
- stringはECMAScript的なNumber変換に近い処理。ただしNaNは0。
- `''` や空白は0。
- Infinity, -Infinityは維持。
- `-0` は可能な限り維持。ただしScratch演算結果比較ではJS互換を優先。

QuickJS由来の文字列数値変換を使う場合:

- QuickJS VM全体を取り込まない。
- `JSRuntime`, `JSContext`, `JSValue`, GC, object, array, promiseは使わない。
- dtoa/atod相当の関数だけを `quickjs_subset/` へ分離する。
- ライセンス表記を残す。
- JIT生成IRからQuickJS内部名を直接呼ばない。

### 10.2 toBoolean

`cast.js` の `Cast.toBoolean` 互換。

```c
int sjit_to_bool(SRuntime *runtime, SValue v);
```

要件:

- boolならそのまま。
- stringの `""`, `"0"`, case-insensitive `"false"` はfalse。
- その他のstringはtrue。
- numberはC/JS truthiness互換。0とNaNはfalse。
- nullはfalse。

### 10.3 toString

```c
SValue sjit_to_string(SRuntime *runtime, SValue v);
```

要件:

- JS `String(value)` に近い結果。
- booleanは `true` / `false`。
- numberはECMAScript/Scratch表示に近い形式。
- NaN/Infinity/-Infinity表記を揃える。

### 10.4 compare

`Cast.compare` 互換。

```c
int sjit_compare(SRuntime *runtime, SValue a, SValue b);
int sjit_eq(SRuntime *runtime, SValue a, SValue b);
int sjit_lt(SRuntime *runtime, SValue a, SValue b);
int sjit_gt(SRuntime *runtime, SValue a, SValue b);
```

要件:

- 両辺をNumber変換して、両方NaNでなければ数値比較。
- ただし、空白文字列がNumber変換で0になる場合はNaN扱いへ戻し、文字列比較へ落とす。
- 文字列比較は `String(v).toLowerCase()` 同士。
- Infinity同士、-Infinity同士は等価。
- `eq` はcompareが0ならtrue。

---

## 11. Operator block

`scratch3_operators.js` 互換で実装する。

必須MVP:

```c
SValue sjit_op_add(SRuntime *, SValue a, SValue b);
SValue sjit_op_sub(SRuntime *, SValue a, SValue b);
SValue sjit_op_mul(SRuntime *, SValue a, SValue b);
SValue sjit_op_div(SRuntime *, SValue a, SValue b);
SValue sjit_op_join(SRuntime *, SValue a, SValue b);
SValue sjit_op_mod(SRuntime *, SValue a, SValue b);
SValue sjit_op_round(SRuntime *, SValue x);
int sjit_op_lt(SRuntime *, SValue a, SValue b);
int sjit_op_eq(SRuntime *, SValue a, SValue b);
int sjit_op_gt(SRuntime *, SValue a, SValue b);
int sjit_op_and(SRuntime *, SValue a, SValue b);
int sjit_op_or(SRuntime *, SValue a, SValue b);
int sjit_op_not(SRuntime *, SValue x);
```

`operator_mod` はJS `%` ではなくScratch互換floor mod:

```c
result = n % modulus;
if (result / modulus < 0) result += modulus;
```

`operator_mathop` はphase 2で実装する。三角関数はdegree単位。

---

## 12. Data / Variable / List

### 12.1 Variable

JS版 `Variable` に対応する。

```c
typedef enum {
    SJIT_VAR_SCALAR = 0,
    SJIT_VAR_LIST = 1,
    SJIT_VAR_BROADCAST = 2
} SVariableType;

typedef struct {
    int id;
    SString *name;
    int type;
    int is_cloud;
    SValue value;
} SVariable;
```

scalar初期値は0。list初期値は空list。broadcast messageのvalueはname。

### 12.2 lookup

`Target.lookupOrCreateVariable` 互換。

- まずidでlookup。
- なければname/typeでlookup。
- なければtarget localに新規作成。

### 12.3 List

`Scratch3DataBlocks.LIST_ITEM_LIMIT = 200000` を守る。

```c
#define SJIT_LIST_ITEM_LIMIT 200000
```

list index変換は `Cast.toListIndex` 互換。

- `all`: acceptAll時のみALL。
- `last`: length > 0ならlength。
- `random` / `any`: length > 0なら1..lengthの乱数。
- その他は `floor(toNumber(index))`。
- 1未満またはlength超過はINVALID。

list contents:

- 全itemが1文字stringならseparatorなしで結合。
- そうでなければspace区切り。

list contains / item number:

- まず直接一致を試してよい。
- 最終的には `sjit_compare(...) == 0` でScratch互換比較する。

---

## 13. Control block

`scratch3_control.js` 互換。

### 13.1 repeat

- `TIMES` は `round(toNumber(...))`。
- 初回にframeのloop counterへ保存。
- 各実行でcounterをdecrement。
- counter >= 0ならbranch 1へ入る。
- loop branch終了後は親loop blockを再実行する。

### 13.2 forever

- 常にbranch 1へ入る。
- branch終了後に再実行する。
- 通常modeでは1tick内で無限に回し続けないようyield制御する。

### 13.3 wait

- 初回に `duration_ms = max(0, 1000 * toNumber(DURATION))`。
- `wake_time_ms = now + duration_ms` をframeに保存。
- 初回はredraw requestしてyield。
- now < wake_timeならyield。
- now >= wake_timeなら次blockへ進む。

### 13.4 if / if_else

- conditionは `toBoolean`。
- trueならbranch 1。
- falseのif_elseならbranch 2。

### 13.5 stop

- `all`: runtime stop all。
- `other scripts in sprite/stage`: target上の他thread停止。
- `this script`: 現在threadのstackをprocedure boundaryまでpop、またはdone。

### 13.6 clone

- `create clone of _myself_`: 現targetをclone。
- 名前指定ならsprite target名でlookup。
- clone limitを守る。
- clone生成後、元targetの後ろに配置する。
- `delete this clone`: originalなら無視。cloneならdisposeしtarget thread停止。

---

## 14. Target / Sprite / Clone

### 14.1 Target状態

```c
typedef struct {
    int id;
    int is_stage;
    int is_original;
    SString *name;

    SVariable *variables;
    int variable_count;

    SFrame *edge_hat_values;
    int edge_hat_count;
} STarget;
```

### 14.2 Sprite状態

```c
typedef struct {
    STarget base;
    int sprite_id;
    int drawable_id;

    double x;
    double y;
    double direction;
    double size;
    int visible;
    int current_costume;
    int rotation_style;
    int draggable;
    double volume;

    int layer_order;
} SSprite;
```

### 14.3 Motion/Looksはrenderer直接呼び出しをしない

JS版 `RenderedTarget.setXY` はrendererを直接更新し `runtime.requestRedraw()` する。LLVM版では、stateを更新しdraw commandを積む。

```c
void sjit_sprite_set_xy(SRuntime *runtime, SSprite *sprite, double x, double y, int force);
void sjit_sprite_set_direction(SRuntime *runtime, SSprite *sprite, double direction);
void sjit_sprite_set_visible(SRuntime *runtime, SSprite *sprite, int visible);
void sjit_sprite_set_costume(SRuntime *runtime, SSprite *sprite, int costume_index);
```

`setDirection` は-179..180へwrap clampする。

`setCostume` はroundし、NaN/Infinityなら0、範囲内へwrap clampする。

---

## 15. SB3 loader / Compiler input

初期実装では、SB3 zip全体をLLVM側で読む必要はない。host側でzip/json/assetsを読む。

ただし、host側はロード後にScratch意味論を実装してはいけない。host側はJSONを正規化して `SProject` に詰めるだけ。

### 15.1 Project IR

```c
typedef struct {
    STargetDesc *targets;
    int target_count;
    SBlockDesc *blocks;
    int block_count;
    SScriptDesc *scripts;
    int script_count;
    SAssetDesc *assets;
    int asset_count;
} SProject;
```

`sb3.js` の構造に合わせ、targetは以下を持つ。

- `isStage`
- `name`
- `variables`
- `lists`
- `broadcasts`
- `blocks`
- `costumes`
- `sounds`
- `currentCostume`
- `volume`
- stageの場合: `tempo`, `videoTransparency`, `videoState`, `textToSpeechLanguage`
- spriteの場合: `visible`, `x`, `y`, `size`, `direction`, `draggable`, `rotationStyle`

### 15.2 Block graph

`Blocks` の内部構造に合わせる。

```c
typedef struct {
    int id;
    int opcode_id;
    int parent_id;
    int next_id;
    SInputDesc *inputs;
    int input_count;
    SFieldDesc *fields;
    int field_count;
    SMutationDesc mutation;
    int top_level;
} SBlockDesc;
```

`getNextBlock`, `getBranch`, `getInputs`, `getMutation`, `getProcedureDefinition` 相当をruntime/compiler側に持つ。

---

## 16. LLVM codegen

### 16.1 module構成

- `runtime/*.c` をclangで `.bc` にする。
- 生成script moduleをLLVM C++ APIで作る。
- runtime bitcode moduleを読み込み、同じ `LLVMContext` でlinkする。
- target triple / data layout / opaque pointer設定を一致させる。
- `-ffast-math` は禁止。

CMake要件:

```cmake
find_package(LLVM REQUIRED CONFIG)
# LLVMと同じclangを使ってruntime bitcodeを生成すること。
```

### 16.2 script function

scriptごとにentry関数を生成する。

```c
SRuntimeStatus sjit_script_entry_<script_id>(SRuntime *runtime, SThread *thread, SFrame *frame);
```

procedureも同様に関数化する。

```c
SRuntimeStatus sjit_proc_entry_<proc_id>(SRuntime *runtime, SThread *thread, SFrame *frame);
```

### 16.3 lowering単位

block graphを以下へloweringする。

1. block ID graph
2. structured control graph
3. pc付きstate machine
4. LLVM basic blocks

MVPでは、AST風loweringではなく、最初からpc付きstate machineを採用する。Scratchは途中yieldと再開が本質だからである。

---

## 17. 実装フェーズ

### Phase 0: Skeleton

- CMake
- LLVM検出
- runtime `.bc` 生成
- LLJIT起動
- runtime helper 1個をJITから呼ぶ

合格条件:

- `sjit_make_number(7)` をJIT関数から呼べる。
- `ctest` が通る。

### Phase 1: Value / Cast / Compare

- `SValue`
- number/string/bool/null
- arena string
- `toNumber`, `toBoolean`, `toString`, `compare`
- QuickJS subset導入準備

合格条件:

- `"" -> 0`
- `"false" -> false`
- `"0" -> false`
- `"123" == 123`
- `"abc" + numeric op -> 0扱い`
- Infinity同士比較

### Phase 2: Operator / Data

- add/sub/mul/div
- lt/eq/gt
- join/letter/length/contains/mod/round
- variables
- lists

合格条件:

- `1 + 2 * 3 = 7`
- `"3" + 4 = 7`
- list index `last/random/all` の基本動作

### Phase 3: Thread / Frame / Control

- thread/frame
- pc state machine
- repeat/forever/if/if_else/wait/yield/yieldTick

合格条件:

- `repeat 10` で10回加算
- `wait 0.1` がtickを跨ぐ
- `forever { x++; yield }` が3tick後に3

### Phase 4: Event / Broadcast

- hats
- green flag
- key hats
- broadcast
- broadcast and wait
- edge-activated hats

合格条件:

- green flagでscript起動
- broadcastでreceiver起動
- broadcast and waitがreceiver完了まで待つ

### Phase 5: Sprite / Clone / Draw Queue

- sprite state
- clone作成/削除
- motion setXY/direction
- looks show/hide/costume
- draw command buffer

合格条件:

- `go to x:10 y:20; show` がdraw commandを生成
- cloneが変数とsprite状態をコピーする
- delete cloneでthread停止

### Phase 6: SB3 smoke

- host側SB3 loader
- project IR構築
- compilerがscriptsをJIT化
- runtimeにscript entry table登録

合格条件:

- 最小SB3 JSONを読み、green flag scriptが動く。

---

## 18. テスト方針

JavaScript版の意味論と照合する。最初はJS VMを直接呼ばなくてよいが、期待値は参照ソースに合わせる。

必須テスト:

1. `Cast.toNumber` 互換
2. `Cast.toBoolean` 互換
3. `Cast.compare` 互換
4. operator add/sub/mul/div
5. Scratch mod
6. variable set/change/get
7. list add/delete/insert/replace/item/itemnum/contains
8. repeat/forever/if/wait
9. green flag hats
10. broadcast/broadcast and wait
11. clone create/delete
12. draw command queue
13. interpreter fallbackとJITの一致

---

## 19. 非目標

初期実装では以下をしない。

- Scratch GUI完全移植
- WebGL rendererのLLVM化
- 音声エンジン完全移植
- extension worker完全互換
- Promise/async extension完全実行
- Scratch Blocks editorとの完全結合
- Cloud variable本番接続
- Video sensing / BLE / translation / text-to-speech完全対応
- QuickJS VM全体の取り込み
- LLVM coroutine使用

---

## 20. Codexに投げる最終プロンプト

以下をそのままCodexへ投げる。

```text
アップロード済みの scratch-editor ソースを参照しながら、Scratch互換LLVM VM/JITを一から新規実装してください。

設計方針は「GUI以外をできるだけLLVM側に任せる」です。

参照すべき既存実装:
- packages/scratch-vm/src/engine/runtime.js
- packages/scratch-vm/src/engine/sequencer.js
- packages/scratch-vm/src/engine/thread.js
- packages/scratch-vm/src/engine/execute.js
- packages/scratch-vm/src/engine/block-utility.js
- packages/scratch-vm/src/engine/blocks.js
- packages/scratch-vm/src/engine/target.js
- packages/scratch-vm/src/engine/variable.js
- packages/scratch-vm/src/sprites/rendered-target.js
- packages/scratch-vm/src/blocks/scratch3_control.js
- packages/scratch-vm/src/blocks/scratch3_event.js
- packages/scratch-vm/src/blocks/scratch3_data.js
- packages/scratch-vm/src/blocks/scratch3_operators.js
- packages/scratch-vm/src/blocks/scratch3_motion.js
- packages/scratch-vm/src/blocks/scratch3_looks.js
- packages/scratch-vm/src/blocks/scratch3_procedures.js
- packages/scratch-vm/src/util/cast.js
- packages/scratch-vm/src/serialization/sb3.js

host側に残してよいもの:
- window/canvas/renderer
- input collection
- asset loading
- SB3 zip/json loading
- LLVM ORC LLJIT setup
- JIT関数呼び出し
- debug log

LLVM側へ寄せるもの:
- Scratch script execution
- scheduler
- thread/frame/stack
- yield/yieldTick/wait
- variables/lists
- dynamic SValue
- Scratch cast/compare
- operators/control/data/event/motion/looks/procedureの中核
- hats/broadcast/broadcast and wait
- clone management
- sprite state
- draw command queue generation

実装はC++ host + C runtime bitcode + LLVM C++ API codegenで行ってください。

runtime/*.c は clang -emit-llvm で .bc にし、JIT生成moduleとlinkしてください。
JIT生成IRから呼ぶ関数はすべてC ABIにしてください。
C++ class, std::string, std::vector をABI境界に出さないでください。

最初から完全Scratch互換を狙わず、以下の順序で実装してください。

1. CMake/LLVM/LLJIT skeleton
2. runtime bitcode生成とlink
3. SValue, string arena, toNumber/toBoolean/toString/compare
4. operator add/sub/mul/div/eq/lt/gt/mod/join
5. variable/list runtime
6. Thread/Frame/Scheduler
7. pc付きstate machine lowering
8. repeat/forever/if/wait/yield
9. green flag/event hats/broadcast/broadcast and wait
10. sprite state/clone/draw command queue
11. 最小SB3 JSON loader

Scratch互換上の重要要件:
- toNumberはNaNを0扱いする。
- toBooleanは '', '0', 'false' をfalse扱いする。
- compareは数値比較優先。ただし空白文字列が0になる場合は文字列比較へ落とす。
- 文字列比較はcase-insensitive。
- listは1-indexed。
- list item limitは200000。
- repeatはMath.round(toNumber(TIMES))。
- waitはstack timer相当をframeに保存してyieldする。
- broadcast and waitは起動thread群をframeに保存し、完了までyield/yieldTickする。
- cloneはsprite状態と変数をコピーする。
- rendererは直接触らずdraw command bufferだけ生成する。
- host側はdraw commandを読むだけにする。

LLVM coroutineは使わず、まずはpc付きstate machineにしてください。
script entryは以下の形にしてください。

SRuntimeStatus sjit_script_entry_<id>(SRuntime *runtime, SThread *thread, SFrame *frame);

yield時は frame->pc を保存して SRuntimeStatus を返し、次tickで同じframeから再開できるようにしてください。

QuickJS由来コードを使う場合は、数値変換・文字列変換に必要な最小部分だけを quickjs_subset/ に分離してください。
QuickJS VM全体、JSRuntime、JSContext、JSValue、GC、Object、Array、Promise、evalは取り込まないでください。
ライセンス表記を残してください。

実装前に、まず以下を短く説明してください。
- 全体アーキテクチャ
- host/LLVM境界
- runtime bitcodeとJIT moduleのlink方法
- SValue設計
- scheduler/frame/state machine設計
- 最初に通すテスト

その後、段階的に実装してください。
大きな一枚実装にせず、テストを追加しながら進めてください。
最終的に以下が通るようにしてください。

cmake -S . -B build
cmake --build build
ctest --test-dir build
./build/scratch-llvm-vm
```

---

## 21. 完了条件

MVP完了条件:

- runtime bitcodeがJIT moduleへlinkされている。
- JIT生成関数からruntime helperを呼べる。
- `SValue` のnumber/string/bool/nullが動く。
- Scratch互換 `toNumber`, `toBoolean`, `compare` が主要ケースで一致する。
- `repeat`, `if`, `wait`, `yield` がpc付きframeで再開可能。
- green flag, broadcast, broadcast and waitが動く。
- clone作成/削除の土台がある。
- draw command bufferをhostが読める。
- host側にScratch VM意味論が漏れていない。
- すべてのMVPテストが `ctest` で通る。

長期完了条件:

- 主要Scratch 3 core blocksをJIT loweringできる。
- fallback interpreterとJITの結果が一致する。
- SB3 projectを読み、GUI以外の実行状態をLLVM側で保持できる。
- renderer/audio/inputはhost adapterとして交換可能。
- QuickJS由来コードは最小subsetに閉じ込められている。
- Scratch VMの既存意味論と差分がテストで管理されている。
