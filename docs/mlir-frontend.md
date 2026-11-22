# MLIR 風フロントエンド（LLVM の前段）

`src/mlir/` は、Scratch のブロック木を **LLVM IR の手前で一度 SSA の
中間表現に落とす**層。既定では無効で、`XYO_MLIR=1` のときだけ、対応
サブセットのスクリプトが既存 `codegen.cpp` の代わりにこの経路で
コンパイルされる。スケジューラ（`sequencer.cpp` / `thread.cpp` /
`deps.*`）には一切手を入れていない。

## 0. 前提: MLIR ライブラリはこの環境に無い

`llvm-config --version` は 23.1.5 だが、`/usr/lib/llvm*/lib/libMLIR*` も
`MLIRConfig.cmake` も存在せず、`mlir` パッケージもリポジトリに無い。
そのため **MLIR 本体はリンクしていない**。代わりに MLIR の設計要素を
そのまま小型に実装した:

| MLIR の概念 | この実装 |
|---|---|
| dialect / op | `xyo.*` op（`ir.hpp` の `Op`、名前で性質を引く） |
| SSA value / block argument | `Value`（op 結果 or block 引数） |
| region / block | `Region`（1 block のみ）/ `Block`（引数付き） |
| terminator | `xyo.yield` / `xyo.condition` / `func.return` |
| verifier | `verify()` — 定義前使用・region の入れ子・終端命令 |
| pass pipeline | `canonicalize` → `cse` → `dce` → `promoteNumericLoopVars` |
| LLVM lowering | `lowerToLLVM()` — `llvm::IRBuilder` で `void(JitCtx*)` を生成 |
| generic syntax | `Module::print()`（`"xyo.add"(...) : (...) -> (...)` 形式） |

将来 MLIR が使える環境では、この `ir.hpp` の op 集合を本物の dialect に
置き換え、`lowerToLLVM` を `mlir-opt` 相当のパイプラインに差し替える
だけで済むように、方言の知識を `dialect::` に集約してある。

## 1. パイプライン

```
Blocks（Scratch のブロック木）
  │  scratch_build.cpp: サブセット判定 + SSA 構築
  │  （手続きは proccode ごとに別 func.func へ。相互再帰も可）
  ▼
xyo dialect（region / block argument 付き）
  │  passes.cpp: 定数畳み込み → CSE → DCE → 数値 SSA 昇格 → 再度畳み込み
  ▼
xyo dialect（数値ループは f64 の loop-carried SSA）
  │  lower_llvm.cpp: ランタイムヘルパ呼び出しへ lowering
  ▼
LLVM IR ── 既存の O3 パイプライン / ORC JIT（変更なし）
```

`jitEmitScript()`（`src/jit/codegen.cpp`）の先頭に 1 箇所だけフックがある:

```cpp
if (!aot && !osrStart && !standaloneOsrLoop &&
    mlir::tryEmitScript(mod, lc, runtime, target, blocks, topBlockId,
                        entryName, out, pinned, pendingFnNames,
                        procNameMap))
    return true;
```

失敗（サブセット外・verifier 失敗・lowering 失敗・LLVM 検証失敗）の
ときは **LLVM モジュールに一切触れず** `false` を返す。呼び出し側は
そのまま既存 codegen を実行するので、意味論が後退する経路は無い
（lowering 後に `llvm::verifyFunction` が落ちた場合だけ、作った関数を
`eraseFromParent()` してから戻る）。

## 2. 型と op

型は 6 種類だけ:

| 型 | 意味 |
|---|---|
| `!xyo.value` | NaN-boxed `Value` のビット列（i64） |
| `!xyo.f64` | 数値 SSA（昇格後のみ） |
| `!xyo.bool` | i1（比較・条件） |
| `!xyo.i64` / `!xyo.ptr` | 内部用 |
| `!xyo.ctx` | `JitCtx*`（関数引数のみ） |

値意味論の op（すべて `jit/rt.cpp` の既存 extern "C" ヘルパへ lowering）:

| op | 意味 | ヘルパ |
|---|---|---|
| `xyo.const {bits}` / `xyo.const.f64 {d}` | 定数 | — |
| `xyo.getvar` / `xyo.setvar {id,name}` | 変数読み書き | `xyo_varslot_init` + `ctx->varSlotData[idx]` の直接 load/store |
| `xyo.add/sub/mul/div/mod` | 四則・Scratch mod | `xyo_add` … `xyo_mod` |
| `xyo.lt/gt/eq` | 比較 | `xyo_lt` … `xyo_eq` |
| `xyo.and/or/not` | 真偽値 | `xyo_and` … |
| `xyo.truthy` | Boolean 変換 | `xyo_truthy` |
| `xyo.to_num` / `xyo.from_num` | box/unbox | `xyo_to_num` / `xyo_num` |
| `xyo.round` / `xyo.mathop {op}` | 数学 | `xyo_round` / `xyo_mathop` |
| `xyo.join/letterof/strlen/strcontains` | 文字列 | 同名ヘルパ |
| `xyo.list.ptr` / `xyo.list.{add,delete,…}` | リスト | 変数スロットの `Variable*` + `xyo_list_*` |
| `xyo.arg {index}` | 手続きの引数 | 関数の第 index+1 引数 |
| `xyo.call {callee}` | 手続き呼出し | 同一モジュール内の関数呼び出し |
| `xyo.select %c, %a, %b` | 値の選択 | `select` |
| `xyo.cmp.f {pred}` | 数値比較 | `fcmp olt/ogt/ole/oge/oeq/one` |
| `xyo.warp_yield …` | warp ループの譲り | 64 回に 1 回だけ spill + `xyo_loop_yield` |

構造化制御フロー（MLIR の region / block argument 相当）:

| op | region | 終端 |
|---|---|---|
| `xyo.if %cond` | then / else | `xyo.yield` |
| `xyo.while` | before（条件）/ body | `xyo.condition %c, …` / `xyo.yield` |
| `xyo.repeat %count, …` | body | `xyo.yield` |
| `xyo.forever` | body | `xyo.yield` |
| `xyo.loop_yield` | — | スケジューラへの譲り（非 warp は毎回、warp は 64 回に 1 回） |
| `func.func {warp}` | body | `func.return`（warp 定義は入口で `isWarp` を保存して 1 を立て、return で戻す） |

変数アクセスは既存 codegen と同じスロット契約（エントリ先頭で
`xyo_varslot_init` を 1 回、以後は `ctx->varSlotData[idx]` を直接
ロード/ストア）に合わせてある。実験用の
`XYO_RT_GUARD_NATIVE_VAR_SLOTS=1`（使用のたびに owner/世代を検証する
モード）のときは、この直接アクセスが契約に反するためフォールバック
する。

数値 SSA 用に `.f` 系（`xyo.add.f` / `xyo.round.f` / `xyo.mathop.f`）と
真偽値用の `xyo.band/bor/bnot/boolval` を持つ。

## 3. pass

### canonicalize
数値定数だけを対象に、**実行時ヘルパをそのまま呼んで**畳む
（`xyo_add` など）。-0 / NaN / Infinity / mod の符号規則が fold と実行で
構造的に一致する。文字列リテラルは GC に触れないよう畳まない。

### CSE / DCE
CSE は block 内のみ。副作用 op（`setvar`・リスト書き込み・`loop_yield`）
の後で表をクリアする。`getvar` は CSE 対象外（yield で値が変わる）。
DCE は pure op の未使用結果だけを消す。

### 数値 SSA 昇格（`promoteNumericLoopVars`）
`xyo.repeat` の本体が **直鎖で、変数が数値としてしか触られない**とき、
その変数を f64 の loop-carried SSA（region の block argument）へ昇格する。

条件（保守側）:

* 本体の op が `const / getvar / setvar / 四則 / round / mathop / to_num /
  loop_yield / yield` のみ（`if` などの入れ子 region・リスト・比較・
  別変数へのコピーがあれば **昇格しない**）
* `setvar` の値が数値と証明できる producer（数値定数・算術・getvar）
* `getvar` の全ユーザが数値演算か同一変数への `setvar`

書き換え:

1. ループ入口で `getvar` + `to_num` を 1 回だけ実行し、iter_args に載せる
2. 本体内の `getvar` を block argument に、`setvar` を「次の iter_arg」に
3. **非 warp ループ**は `xyo.loop_yield` の前後で `from_num` + `setvar` /
   `getvar` + `to_num`（毎反復譲るので、その都度 spill して整合させる）
4. **warp ループ**は `xyo.warp_yield` 1 個にまとめ、lowering が
   「実際に譲る分岐の中」だけで spill/reload する（後述）
5. ループ脱出時の書き戻しは、0 回反復で文字列を数値化しないよう
   `select(ran, from_num(final), 初期値)` で行う

これにより、比較（`lt/gt/eq`）のような「boxed 値の文字列比較」に依存
し得る文脈を昇格対象から外しつつ、数値カウンタのループから
box/unbox とタグ検査を消せる。yield をまたぐ値は必ず実メモリ経由に
なるので、スケジューラの観測点は従来と同じ。

### 手続き（`procedures_definition` / `procedures_call`）
既存 codegen と同じ ABI で別関数へ落とす:

* `void fn(JitCtx*, i64 arg0, …)` — 引数はレジスタ渡し
* `argument_reporter_string_number/boolean` は仮引数を直接参照
  （`xyo.arg`）
* `procedures_call` は引数を左→右に評価して `xyo.call`。
  未接続の入力は prototype の既定値
* warp 定義（`mutation.warp == "true"`）は **callee の入口で**
  `isWarp` を保存して 1 を立て、return で戻す（既存 codegen と同じ
  「呼出し元から継承し、warp 定義だけ 1 へ昇格する」契約）
* 手続き関数は backend が `procNameMap` 経由で
  `CompiledScript::procedures` に束縛する（vkey は
  `proccode + "|w"` / `"|n"`）
* reporter 手続き（`procedures_return`）は未対応 → フォールバック

### warp ループのチャンク化（`analyzeChunk`）
warp 手続き本体のループは `isWarp == 1` が静的に確定するので、毎反復の
`isWarp` ロードと非 warp 分岐を落とせる。さらに本体が
**「昇格変数を定数ステップで更新するだけ」**（`const.f64` / `add.f` /
`sub.f` のみ、副作用なし）なら、lowering が 64 反復をまとめて進める:

```
head:    %r = phi(残り回数), %x_i = phi(昇格変数)
         if %r >= 64.5 → group  else → tail（1 反復ずつ）
group:   %x_i += 64 * step_i            // 64 反復ぶんを 1 命令で
         %r   -= 64
         8 チャンクに 1 回だけ: spill → xyo_loop_yield → reload
tail:    通常の本体（≤64 反復）
```

* yield の頻度は従来と同じ「64 回に 1 回」のオーダー。spill は
  実際に譲る直前だけなので、純粋な数値ループは **1 反復あたり
  1 命令程度**まで落ちる
* 0 回反復・端数（10.7 → 11 回）・入れ子ループは tail と
  `select(ran, …)` の書き戻しで逐次実行と一致する
* ループ脱出時の値は write-back ガードで厳密（`tests/mlir/xcheck.py`
  `case_warp_zero` が「0 回反復で文字列が数値にならない」ことを検査）

## 4. 対応サブセットとフォールバック

ビルドできるのは以下だけ（`scratch_build.cpp` の `preScan`）。

* ハット: `event_whenflagclicked` / `event_whenkeypressed` /
  `event_whenbroadcastreceived` / `event_whenthisspriteclicked` /
  `event_whenstageclicked` / `control_start_as_clone`（入力を持たないもの）
* 文: `data_setvariableto` / `data_changevariableby` / リスト 5 種 /
  `control_if(_else)` / `control_repeat` / `control_forever` /
  `control_repeat_until` / `control_while` / `procedures_call`
  （statement 手続きのみ。reporter は未対応）
* 値: 数値 shadow / `text` / `data_variable` / 四則・比較・and/or/not /
  `operator_round` / `operator_mathop` / `operator_join` / `letter_of` /
  `length` / `contains` / リスト reporter 5 種 /
  `argument_reporter_string_number` / `_boolean`（定義済みの引数名のみ）

`wait`・broadcast・sensing/motion/looks/sound/pen・`for_each`・
reporter 手続きなどは **1 つでも現れたらスクリプト全体をフォールバック**
する（部分適用はしない）。手続きは呼出し先の定義も同じ条件で検査し、
未定義呼出し・未対応の引数 reporter はフォールバック。`runtime.blockCountEnabled`
（`xyo test` のブロック数カウント）と `XYO_JIT_CORO_DIRECT`（既存の
stackless coroutine 実験）が有効なときも、既存経路へ譲る。

## 5. 使い方

| 環境変数 | 効果 |
|---|---|
| `XYO_MLIR=1` | 有効化（`XYO_JIT_MLIR=1` も可。既定 off） |
| `XYO_MLIR_TRACE=1` | `[mlir] emit …` / `[mlir] fallback …: 理由` を stderr へ |
| `XYO_MLIR_DUMP=1` | 生成した xyo dialect を stderr へ（値がパスなら追記） |
| `XYO_MLIR_LLVM=1` | lowering 直後の LLVM IR を stderr へ（デバッグ用） |

```sh
XYO_MLIR=1 XYO_MLIR_TRACE=1 xyo run project.sb3
XYO_MLIR=1 XYO_JIT_MODE=eager xyo test fixtures.sb3
```

`xyo test` はブロック数カウントを使うため、そのままではフォールバック
する（トレースに理由が出る）。

## 6. 検証

* **ユニット** `tests/unit/test_mlir.cpp`（41 checks）:
  IR 構築 → `verify` → pass（昇格 1 ループ / 1 変数、`xyo.add.f` の生成）
  → LLVM lowering（`void(ptr)`・`mayYield`）、verifier の
  定義前使用拒否、実 sb3 の interpreter との一致（数値ループ・
  文字列 2000 回連結＝GC 込み）、`looks_say` を含むスクリプトの
  フォールバックと状態一致。**warp 手続き**（呼出し・引数 reporter・
  warp 属性・別関数の生成・入口の `isWarp=1`）と、200000 回の warp
  ループ・0 回呼出しで文字列を保つことを追加で検査する。
* **パリティ** `tests/mlir/xcheck.py`（CTest `xyo-mlir-frontend`）:
  19 ケース（repeat / 0 回 repeat / 小数回数 / repeat_until / while /
  if-else / 入れ子ループ / 文字列 / リスト / 数学 / 文字列変数 /
  文字列比較 / wait-until との混在 / 文字列成長 / 2 ハット /
  **warp 手続き 3 種**（20 万回カウンタ・0 回呼出し・入れ子））を
  interpreter と MLIR で実行し、**全変数・リストが一致**することを検査。
  `eager` と `hotspot` の両モードで 19/19。0 回反復のループが
  変数を数値へ書き換えないこと（`repeat 0` で文字列が保持されること）
  も含む。
* **コーパス掃き** `tests/fixtures` + `tests/differential/repro` +
  `tests/bench/algos/out` の 45 プロジェクトで interpreter と一致
  （フォールバックが 1 つでもあるプロジェクトは 30。手続き対応で
  1 プロジェクト増えた）。
* **既存ユニット全体**: `XYO_MLIR=1 XYO_JIT_MODE=eager xyo-tests` が
  全チェック 0 failures（scheduler 466 / type feedback 6443 /
  literal context / nan codegen を含む）。この過程で「sb3 の圧縮 primitive は数値でも文字列として
  ロードされ得る」ことを type-feedback テストが検出し、shadow の
  フィールド値を `toNumber` せず**そのまま**値にするよう修正した。
* **性能**（`xyo run` を legacy / MLIR 交互に 3 回。i7-12700、負荷で ±10%）:

  | ワークロード | 既存 JIT | MLIR | 比 |
  |---|---|---|---|
  | warp 手続き内 `repeat 50M [change i by 1]` | 62〜71ms | **30〜32ms** | **約 2.1×** |
  | warp 手続き内 `repeat 200M [change i by 1]` | 153〜160ms | **33〜40ms** | **約 4.4×** |
  | 最上位 `repeat 5M [change x by 1]`（毎反復 yield） | 511〜530ms | 508〜515ms | 同等 |
  | `tests/bench/algos/out/mergesort_200000` | 349〜357ms | 335〜365ms | 同等 |
  | `tests/bench/algos/out/bubblesort_5000` | 282〜291ms | 292〜297ms | 同等 |

  warp ループは 64 反復を 1 チャンクに畳むため、1 反復あたりの仕事が
  「phi + fadd + カウンタ」から「64 反復で fadd 1 個」に減る。最上位
  ループは Scratch 仕様で毎反復 yield するため fiber 切り替えが支配的で、
  warp 化できない限りこの条件では差が出ない（想定どおり）。リスト操作や
  比較を含む実アルゴリズムのループは毎反復の形が残るので、既存 JIT と
  同等に留まる。

## 7. 制限と次の一手

* **手続き**: statement 手続きのみ。reporter 手続き（`procedures_return`）
  と `argument_reporter` の特殊名（"is compiled?" 等）は未対応。
* **warp 最適化の適用範囲**: 64 反復チャンクは「昇格変数を定数ステップで
  更新するだけ」の本体に限る。比較・リスト・手続き呼出しを含む本体は
  毎反復のループ（昇格と spill は効くが短縮は無い）。乗算ステップ
  （`x = x * 2`）は累積丸めが変わり得るため対象外。
* **yield の位相**: チャンク化により、warp ループの譲りは「ループ開始から
  64 反復ごと」になる（従来はスレッド共通カウンタの位相）。譲りの頻度は
  同じオーダーで、他のスクリプトが観測する値は各譲り時点で厳密。
* **repeat_until / while の数値昇格**: 現在は `xyo.repeat` のみ。
  `xyo.while` の before/after region に同じ iter_args を通せば拡張できる。
* **比較を含むループの昇格**: Scratch の比較は「両方数値のときだけ
  数値比較」なので、生の boxed 値を見る比較は昇格対象から外している。
  「比較専用の shadow 値」を別 iter_arg にすれば拡張可能。
* **テキスト IR のパーサ**: 現在は print のみ（generic syntax 形式）。
  パーサを付ければ `tests/mlir/*.mlir` の round-trip テストが可能。
* **AOT**: `aot != nullptr` では常に既存経路（AOT は slot 経由の
  絶対アドレス解決を持つため、MLIR 側の global 生成と衝突させない）。
* **実 MLIR への差し替え**: `dialect::` と `Module` の境界がそのまま
  置換点になる。`lowerToLLVM` を `mlir::convertToLLVMPass` に、
  `canonicalize/cse/dce` を MLIR の同名 pass に置き換える想定。

## 8. scratch-bench（`scratch-bench/`）での実測

`scratch-bench/` の xyo-cpp アダプタは `XYO_BIN` で既存バイナリを指せるので、
`build-mlir/xyo` をそのまま使って performance + parity を回した（`--no-variants`
= hotspot、`--runs 3`、`XYO_FPS` 既定、`XYO_MLIR=1` はアダプタの環境継承で伝播）。

```sh
cd scratch-bench
XYO_BIN=../build-mlir/xyo       bun run bench --runtime xyo-cpp \
    --suite performance,parity --no-variants --runs 3 --offline --out results/base
XYO_MLIR=1 XYO_BIN=../build-mlir/xyo bun run bench --runtime xyo-cpp \
    --suite performance,parity --no-variants --runs 3 --offline --out results/mlir
```

（`projects` スイートは未取得のため対象外。`bun run fetch-projects` が必要。）

| 指標 | 既存 JIT | `XYO_MLIR=1` |
|---|---|---|
| overall（2 回ずつの実測） | 132,864 / 146,789 | 145,391 / 145,716 |
| parity | **2656/2656** | **2656/2656** |
| startup | 461 / 381 ms | 402 / 373 ms |
| peak RSS | 236 / 235 MB | 229 / 230 MB |
| performance 実行時間 | 35.0 / 34.6 s | 34.5 / 34.6 s |

同一設定での連続 2 回の比（ベンチ単位）は **0.87〜1.32** に散らばり、
MLIR の有無による差（中央値 0.98〜1.03）は**このノイズに埋もれる**。
理由はトレースで確認できる: performance スイートのホットなスクリプトは
`event_broadcastandwait` / `control_wait_until` / `control_delete_this_clone` /
`sensing_dayssince2000` を使うため **MLIR のサブセット外**でフォールバックし、
10 スクリプト中 MLIR がコンパイルするのは 5 個の小さなスクリプトだけ
（`[mlir] emit` 5 件 / `fallback` 5 件）。つまり scratch-bench は主に
既存 JIT を測っており、MLIR の効果が出るのは
「warp 手続き内の純粋な数値ループ」に限られる（§6 のマイクロベンチ）。

scratch-bench を回したことで、実プロジェクト由来の 2 つのバグが見つかり修正した:

1. `getProcedureDefinition` は mutation の parse 前に呼ぶと必ず「未定義」に
   なる → 既存 codegen と同じ順序（`getProcedureParamNamesIdsAndDefaults`
   を先に呼ぶ）へ修正。
2. 未定義の手続き呼出しは Scratch では **no-op**（引数も評価しない）だが、
   スクリプト全体をフォールバックしていた → no-op としてコンパイルする
   よう修正（scratch-bench には意図的に壊した `log %s` 呼出しが入っている）。
   この修正後、`xyo-tests` / `xcheck` / コーパス掃きは全て再検証済み。

## 9. フォールバックの削減（compat 層）

以前は「対応 opcode 以外を含むスクリプトは丸ごとフォールバック」だった。
現在は未対応の 1 ブロックだけを **既存 JIT と同じ compat 層**（`xyo_rt_compat`）
で実行し、スクリプト全体は MLIR でコンパイルする。

- `xyo.compat.call {bc, args}` / `xyo.compat.get {bc, args}` — 入力を評価して
  `xyo_rt_setarg`（`BROADCAST_INPUT` は ref へ変換される）で `BlockCached` の
  `argValues` に書き、プリミティブを直接呼ぶ。`bc` は
  `Blocks::getCachedExecute` のポインタ（JIT 専用。AOT は MLIR 対象外）。
- 追加の native 化: `control_all_at_once`（本体だけ isWarp=1 の
  `xyo.all_at_once`、内側のループは warp 扱いでチャンク化対象）、
  `control_for_each`（`xyo.for_each`）、`control_stop` /
  `control_delete_this_clone`（`xyo.stop {kind=all|script|clone}`。関数共通出口
  `fn.ret` へ分岐し、warp 復帰と yield カウンタ書き戻しを通る）、
  `sensing_dayssince2000` / `sensing_timer`（ホットループの条件で使われるため
  直接ヘルパ）、shadow ブロック（メニュー/色/テキスト）はフィールド定数化、
  欠落入力は数値 0（既存 codegen の `createConstantInput(0)` と同じ）。
- ハットは入力を無視してよい（ハット自体は実行しない）ので、
  `when greater than` など入力付きハットも対象。
- **残るフォールバック**: `control_wait_until`（compat の poll はスケジューラの
  アイドル判定に載らず、条件が永久に偽だとプロジェクトが終了しない。既存 JIT の
  event 依存待機に相当する機構が必要）、reporter 手続き（`procedures_return`）。

### 実測と注意

- `xyo-tests` 0 failures、`tests/mlir/xcheck.py` 19/19、scratch-bench parity
  **2656/2656**。
- compat はプリミティブ呼出しなので **native ヘルパより 1〜2 桁遅い**。
  ホットループの中に入ると効いてしまうため、時計系だけは native にした。
  scratch-bench の駆動スクリプトは `broadcast and wait` + `wait until` を
  使うので現状はフォールバックし、数値は従来と同じ。
- compat からの `xyo_loop_yield` / `xyo_yield` はファイバ切替を挟むため、
  待機中に他スレッドの compat が `util`（`jitCompatEc`/`op`）を上書きする。
  `xyo_rt_compat` の再評価の直前に自分の合成 EC を設定し直すよう修正した。
- MLIR が emit したスクリプトは backend の mega 融合 / コルーチン lowering の
  対象外にした（`CompiledScript::mlirFrontend`）。yield 形状は frontend が
  確定しており、後段で書き換えると compat 内の `xyo_yield` と二重になる。

## 10. フォールバック削減の第 2 段（wait until / for_each / all at once）

既定（`XYO_MLIR=1`）でも次の opcode を native に落とすようにした。これにより
コーパス 45 件のうち **36 件がフォールバックなし**でコンパイルされる
（以前は 15 件）。残る既定フォールバックは 9 件（`sensing_*` の一部、
`looks_*`/`motion_*`、`procedures_return` など）。

- `control_wait_until` → `xyo.wait_until {site, event}`。条件は region で毎回
  評価し、既存 JIT と同じ **event 依存待機**（`xyo_wait_event_yield` /
  `xyo_wait_event_done`、`Blocks::isPureWaitCondition` のとき）と
  ポーリング（`xyo_wait_should_yield` + `xyo_yield`）を切り替える。
- `control_for_each` → `xyo.for_each {id,name}`（1..toNum(VALUE) を回り、
  各反復の頭でループ変数を実メモリへ書く）。
- `control_all_at_once` → `xyo.all_at_once`（本体だけ isWarp=1。内側の
  repeat は warp としてチャンク化対象）。
- `control_stop` / `control_delete_this_clone` → `xyo.stop {kind}`。
  関数共通出口 `fn.ret` へ分岐し、warp 復帰・yield カウンタ書き戻しを通る。
- `sensing_dayssince2000` / `sensing_timer` → 直接ヘルパ（ホットループの
  条件で使われるため。compat 経由は 1 回数百 ns かかる）。
- 空本体の warp `repeat` → `xyo_empty_repeat_warp`（既存 JIT と同じ。
  scratch-bench の `loops_empty` が 14B→275B ops に回復）。
- shadow ブロック（メニュー・色・テキスト）はフィールド定数、欠落入力は
  数値 0、入力付きハット（when greater than 等）も対象。

### 完全 no-fallback モード（`XYO_MLIR_NOFALLBACK=1`）

未対応ブロックを **compat 層**（`xyo_rt_compat`）で実行し、スクリプト全体を
諦めないモード。`preScan` の allowlist を denylist に切り替える。

- `xyo.compat.call` / `xyo.compat.get {bc, args}` — 入力を評価して
  `xyo_rt_setarg`（`BROADCAST_INPUT` は ref に変換）で `BlockCached` の
  `argValues` へ書き、プリミティブを直接呼ぶ。待機系は内部で譲るため
  `mayYield` を立てる。
- 実測: コーパス 45 件で **fallback 0**、scratch-bench performance は
  10/10 スクリプトがコンパイル（`~Mv` の 5132 op も通る）、parity は
  **2656/2656**、`xyo-tests` 0 failures、`xcheck` 19/19。
- **既知の問題**: `tests/fixtures/calculator_tests.sb3` と
  `calculator_graph_tests.sb3` が no-fallback モードで終了しない
  （`repeat until` の `item # of` スキャンループが回り続ける。既定モード
  では従来どおりフォールバックするため影響なし）。原因究明まで既定は
  allowlist 方式のままにしてある。
- compat の再入: 待機中に他スレッドの compat が `util.jitCompatEc` を
  上書きするため、再評価の直前に自分の合成 EC を設定し直す。
- MLIR が emit したスクリプトは backend の mega 融合 / コルーチン lowering
  の対象外（`CompiledScript::mlirFrontend`）。
- デバッグ用: `XYO_MLIR_NOPROMOTE=1`（昇格を止める）、
  `XYO_MLIR_LLVM_ERR=1`（検証失敗時に IR を出す）、`XCHECK_TMP=<dir>`
  （xcheck のフィクスチャを残す）。

## 11. no-fallback モードのハング調査（calculator フィクスチャ）

`XYO_MLIR_NOFALLBACK=1` で `tests/fixtures/calculator_tests.sb3` が終了しない
問題を、プロジェクトを機械的に切り詰めて二分探索した（`build-mlir/tmp/calc/`）。

- `runTests` の 19 文を 1 つずつ残して実行 → **2 文目 `control_repeat` で停止**。
  ループ回数は `length of [testExpr]` で、リストは 1 要素 = 本体は 1 回だけ
  実行される。つまり反復回数ではなく本体の中で止まっている。
- 本体が呼ぶ `evaluate %s` の 31 文を同様に二分 → **最後の `control_if_else`
  （parse 本体）で停止**。その中は
  `repeat until <<(i) > (n)> or <(err) = 1>>` の字句解析ループで、
  `set ch to letter (i) of src` と `if/else if/else` で `i`/`err` を進める形。
- 同じループを `repeat 40` に置き換えた変異体では **インタプリタと MLIR の
  最終変数（i, n, ch, err, errMsg, parseOk, result）が完全一致**した。
  → 本体の演算・`letter of`・手続き引数は正しく、**ループの脱出条件が
  2 回目以降の反復で更新されていない**（条件は head で再評価されるはず）。
- 再帰手続き（`gamma %s` は自己再帰）と引数渡しは `tests/mlir/xcheck.py` の
  `case_proc_recursion` / `case_proc_param_loop` / `case_promoted_parse` で
  回帰テスト化済み（**22/22 pass**）。昇格を止めても
  （`XYO_MLIR_NOPROMOTE=1`、`promoted=0` を確認）再現するので、原因は
  昇格ではなく `xyo.while`（repeat until）の条件再評価経路の疑い。

### 追加したデバッグ用スイッチ（すべて env、既定 off）

| 変数 | 効果 |
|---|---|
| `XYO_MLIR_OP_TRACE=1` | emit した statement opcode を全部 stderr へ |
| `XYO_MLIR_COMPAT_TRACE=1` | compat に落ちた statement/reporter を表示 |
| `XYO_MLIR_EXTRA_ALLOW=a,b` | allowlist に opcode を足す（二分用） |
| `XYO_MLIR_COMPAT_FORCE=a,b` | 指定 opcode を native でなく compat で実行 |
| `XYO_MLIR_NOPROMOTE=1` | 数値昇格パスを止める |
| `XYO_MLIR_LLVM_ERR=1` | 検証失敗時に LLVM IR を表示 |

## 12. calculator ハング: 発散点の特定（round 4）

前節の「40 反復なら最終状態が一致」は**誤診**だった（`XYO_DUMPVARS` の grep
が `var Stage/i[...]` 形式に合っておらず、差分が空に見えていた）。正しく
比較すると **40 反復の時点で既に発散している**:

| 変数 | legacy | MLIR |
|---|---|---|
| `Stage/n` | 4 | 3 |
| `Stage/ch` | `*` | (空) |
| `Stage/i` | 3 | 1 |
| `Stage/errMsg` | Missing operand | Bad char |

`n` は `normalize %s` が作る正規化文字列の長さで、`normalize` は
`repeat (length of rawIn) { ch = letter i of rawIn;
item # of [fromChars] ...; set normOut to join(normOut, ...) }` という
**1 文字ずつ join して文字列を組み立て、`item # of` で変換表を引く**ループ。
つまり発散は字句解析ループより前、`normalize`（またはその引数
`rawIn` への引数渡し）で起きている。

再現用の回帰テストを `tests/mlir/xcheck.py` に追加した（**24/24 pass**）:

- `case_proc_param_loop` — 手続き引数（文字列）をループ条件と `letter of` で使う
- `case_proc_recursion` — 自己再帰手続きの引数受け渡し・復帰値
- `case_proc_param_passthrough` — 引数レポータをそのまま別手続きの引数へ
- `case_promoted_parse` — 条件が `or(昇格変数比較)`、本体が if 内で昇格変数を書く
- `case_parse_var_bound` — calculator の字句解析ループの縮小版（i/n/ch/err の 4 変数）
- `case_norm_string_build` — `normalize` 相当（未完成・CASES 未登録、次節の再現用）

これらが通るので、引数渡し・再帰・`letter of`・`item # of`・ループ脱出条件の
**基本形は正しい**。残る差分は `normalize` 特有の組み合わせ
（変換表リストの引き当て + `join` の累積 + 4 変数の同時更新）にあり、
次ラウンドでこの形を再現して修正する。

## 13. 根本原因の特定と修正（round 5）: 圧縮入力の変数参照

calculator の発散の原因は **圧縮入力（inline primitive）の変数参照**だった。
sb3 の入力は `[3, <blockId>, [12, "<name>", "<id>"]]` のように書け、ローダは
`[12, name, id]` を **`shadow=true` の `data_variable` ブロック**
（フィールド `VARIABLE` = name/id）として実体化する。フロントエンドの
`emitInputValue` は「shadow ブロックはフィールド値がそのまま定数」という
規則でこれを **変数名の文字列** にしていた（`normOut` という変数の値が
文字列 `"normOut"` になる、`i` が `"i"` になる等）。名前は数値でないため
`letter ("i") of src` は空文字列を返し、字句解析ループが進まず停止していた。

修正: `data_variable` / `data_listcontents` の shadow は定数化せず reporter
として評価する（`src/mlir/scratch_build.cpp`）。

効果（`tests/fixtures/calculator_tests.sb3` / `calculator_graph_tests.sb3`）:

| | legacy | MLIR (no-fallback) |
|---|---|---|
| calculator_tests | 3131 ms | **508 ms（6.2×）** |
| calculator_graph_tests | 4979 ms | **706 ms（7.1×）** |

### 既定を no-fallback に切り替え

`XYO_MLIR_NOFALLBACK` の既定を **ON** にした（`=0` で従来の allowlist 方式）。
検証（すべて既定状態 = MLIR + no-fallback）:

| 検証 | 結果 |
|---|---|
| コーパス掃き（45 プロジェクト） | **ok=45 / fail=0 / fallback 0** |
| scratch-bench parity（26 プロジェクト） | **2656/2656** |
| scratch-bench performance | **fallback 0**（10/10 スクリプトがコンパイル） |
| `tests/mlir/xcheck.py` | **25/25** |
| `xyo-tests`（legacy / MLIR / MLIR+nofallback） | すべて **0 failures** |

残る「フォールバック」は意図的なモード差のみ:

- `--block-count`（ブロック数計測モード）/ AOT / OSR エントリポイント
- `XYO_JIT_CORO_DIRECT` / `XYO_RT_GUARD_NATIVE_VAR_SLOTS=1` の env 指定時
- **reporter 手続き**（`procedures_return` / 戻り値付き `procedures_call`）は
  未実装で、これを使うプロジェクトだけは従来 JIT に任せる（コーパス・
  parity・performance のいずれにも出現しない）。

### reporter 手続き（実装済み・既定 off）

`procedures_return` / 戻り値付き `procedures_call` の lowering も実装した:

- 手続き関数は `void f(JitCtx*, i64* out, args...)`。戻り値は `out` 経由
  （`xyo.procret` が `*out` を書いて共通出口 `fn.ret` へ分岐）
- 呼び出し側は entry に i64 スロットを確保して `xyo_undefined()` で初期化し、
  呼び出し後に load（`return` に到達しなければ undefined = Scratch と同じ）
- 文としての reporter 呼び出しは null ポインタを渡す（`*out` は書かない）

ただし **検証用フィクスチャで裏付けが取れていない**（interpreter 側も
`undefined` を返す入力しか作れず、正しい sb3 エンコーディングを確認できて
いない）ため、既定では `procedures_return` を denylist に残して従来 JIT に
任せる。`XYO_MLIR_REPORTER_PROC=1` で有効化できる。

## 14. reporter 手続きの完成（round 6）と最終状態

### 実装

- 手続き関数は reporter なら `void f(JitCtx*, i64* out, args...)`（`ret` 属性）。
- `procedures_return` → `xyo.procret`（`*out` に書いて共通出口 `fn.ret` へ分岐）。
- `procedures_call` を **値位置**で使うときだけ戻り値スロットを確保して
  呼び出し前に undefined で初期化し、呼び出し後に load する。
  文位置の呼び出しは null ポインタを渡す。
- reporter 手続きでは `xyo.arg` の基点が 1 ずれる（第 1 引数が戻り値
  スロット）ので `argBase` を 2 にした。
- **`stop this script` / `procedures_return` の後ろの鎖は到達不能**として
  emit を打ち切る（`terminated_`）。これが無いと return の後続ブロックが
  同じ基本ブロックに不正に積まれ、戻り値が上書きされていた。

### 検証（既定 = MLIR + no-fallback + reporter 有効）

| 検証 | 結果 |
|---|---|
| `tests/mlir/xcheck.py` | **26/26**（`case_proc_reporter` 追加） |
| コーパス掃き（45 プロジェクト） | **ok=45 / fail=0 / fallback 0** |
| scratch-bench parity（26 プロジェクト） | **2656/2656** |
| scratch-bench performance | **fallback 0** |
| `xyo-tests`（legacy / MLIR） | 0 failures |
| `calculator_tests` / `calculator_graph_tests` | 383 ms / 628 ms（legacy 比 8.2× / 7.9×） |

### 残る「フォールバック」（すべて意図的なモード差）

- `--block-count`（ブロック数計測モード。既存 codegen 専用のカウンタを emit する）
- `XYO_RT_GUARD_NATIVE_VAR_SLOTS=1`（実験的な guarded 変数解決）
- `XYO_JIT_CORO_DIRECT`（stackless coroutine lowering と併用しない）
- AOT / OSR エントリポイント（別パイプライン）

**未対応 opcode でのフォールバックは無くなった**（未対応ブロックは compat 層 =
既存 JIT のプリミティブ呼び出しで実行し、reporter 手続きも native）。

## 15. lowering の性能改善（round 7）: 数値 fast path / yield カウンタ / 文字列 op

「scratch-bench が legacy より遅い原因は yield の落とし方か」を切り分けた結果、
**yield は主因ではなく**、コストは「すべての算術・比較・リスト・文字列をランタイム
ヘルパ呼び出しにしていたこと」と「変数アクセスが 3 段ロードだったこと」にあった。
素の `math_nbody` が 1.1×、`loops_empty` が 1.4× なのに、算術/リスト/アプリ系だけ
5〜20× 遅いのが根拠。

### 15.1 数値の f64 fast path（`lower_llvm.cpp`）

- 値ごとに「`toNumber(v)` を f64 SSA にしたもの」（数値影）を 1 回だけ作り、
  `std::map<Key, NumShadowEntry>` にキャッシュする（**同じ基本ブロック内でのみ
  再利用** — 別ブロックの SSA 値は支配関係を満たさないことがある）。
  素の double（`bits < kNan`）はビットキャスト、NaN は 0、それ以外は
  `xyo_to_num` を呼ぶ分岐。
- `xyo.add/sub/mul/div/mod` は影を f64 演算し、`numCanonical`（NaN → `kNan`）を
  インラインで畳む。`mod` は `fmod` + `r/m < 0` の補正まで同じ式。
- `xyo.to_num` は影をそのまま返す。
- **比較の fast path は撤回した**（実測: `math_compare` 0.81×、`app_ast` 0.79× と
  悪化。分岐が 1 命令増える割に利益が無い）。

### 15.2 変数スロットのポインタを関数あたり 1 回だけロード

`xyo.getvar` は `ctx->varSlotData` → `Variable*` → `value` の 3 段ロードだった。
`Variable*` は 1 回の実行中ずっと同じなので、**関数の entry で 1 回だけロード**して
使い回す。全 bench の中央値で **+10%**（`results/xyo-mlir-r10`）。

### 15.3 yield カウンタのレジスタ化

- `emitLoopYield` のカウンタは `ctx->yieldCheckN` を毎反復 load/store していた。
  関数エントリの alloca（`yieldCounter()`、epilogue で 1 回だけ書き戻す）に載せ、
  mem2reg でレジスタ化する。
- `ctx->fiber` は実行中不変なので関数エントリで 1 回だけロード（`fiberCache_`）。
- `isWarp` は手続き呼び出しで変わり得るので毎回ロードのまま。

### 15.4 文字列 op と `join` の専用経路

`xyo.join / letterof / strlen / strcontains` は **Value（i64 ビット）を返す**
（`invokeHostEffect` の結果をそのまま返す。bool ではない）。`operator_contains` を
`icmp ne` に落とすと `list.add` に i1 が渡り verifier で落ちるので注意。
右オペランドが文字列リテラルの `join` は、legacy と同じ `xyo_join_suffix` に落とす。

### 15.5 壊れた module を絶対に使わない（重要）

lowering 途中で `l.error` が立つと、それまでの関数だけの**不完全な module**
（本体の無い手続き関数）が残り、`out.ok` が既に true だとそのまま実行されて
SEGV していた（実際に踏んだ）。`out.ok=false` を立て、frontend も
`lowered.error` 非空を失敗として扱うようにした。デバッグ用に
`XYO_MLIR_FNTRACE=1`（関数ごとの `mayYield` / error）、
`XYO_MLIR_NONUMFAST=1`（数値 fast path の A/B）を追加。

### 15.6 実測（scratch-bench performance、`runs=2`、中央値の ops）

| bench | r10（変数キャッシュのみ） | r15（本節の変更後） | 比 |
|---|---|---|---|
| `math_mandel` | 6.64 M | 15.17 M | **2.28×** |
| `algo_matmul` | 10.89 M | 19.80 M | **1.82×** |
| `loops_until` | 45.06 M | 72.80 M | 1.62× |
| `loops_while` | 54.96 M | 81.28 M | 1.48× |
| `loops_foreach` | 109.0 M | 143.6 M | 1.32× |
| `list_read` | 19.72 M | 25.81 M | 1.31× |
| `str_join` | 20.74 M | 15.45 M | 0.74× |

39 bench 中 **改善 16 / 悪化 16 / 同等 7**、幾何平均 **1.058**（+6%）。
悪化は文字列系（`str_join` / `str_contains` / `str_cast`）と、レジスタ昇格が
効いている単純ループ（`vars_*`）に集中している。`vars_*` は「昇格済みの変数に
対しても数値影の分岐が入る」ためで、次の課題は昇格ループ向けの影の再利用。
`str_join` は `xyo_join_suffix` で +25% 戻したが r10 には届いていない（残課題）。

**注**: 計測中は他エージェントが同時にビルド/テストを回しており、1 bench あたり
±10〜20% のノイズがある。`runs=1` の比較は順位付けに使わないこと。

## 16. legacy codegen の最適化移植（round 8、進行中）

`src/jit/codegen.cpp` の最適化を MLIR 側へ順に移植している。この節は各段の
実測と、移植済み／未移植の一覧。

### 16.1 移植済み（このラウンド）

| legacy の最適化 | MLIR 側の実装 | 効果 |
|---|---|---|
| `emitToNumF64` の素の double fast path | `numberShadow()`（値ごとの f64 影、ブロック内限定キャッシュ） | `math_mandel` 2.3× / `algo_matmul` 1.8× |
| `data_changevariableby` の numCanonical 省略 | 定数オペランドが有限なら NaN 正規化の fcmp/select を省く（`mul`/`div` は 0 を除外、`inf` は `isfinite` で除外 — `inf - inf` が NaN になるため） | `loops_*` 1.05〜1.2× |
| ホット分岐の `branch_weights` | `numberShadow` のタグ検査を 63:1 で高速側 fall-through | **全体 +14%**（最大の寄与） |
| `Variable*` のループ外 hoist | 関数エントリで 1 回だけロードして使い回す（`varPtrCache_`） | 全体 +10% |
| yield カウンタのレジスタ化 | 関数エントリの alloca（epilogue で 1 回書き戻し）。`fiber` も 1 回ロード | 小 |
| `hoistVarSlotPointers` の list 部分 | `xyo.list.length` は `Variable::list` → `SList::len` を直接読み、`list == null` のときだけ helper（遅延生成）へ | `str_join` 1.5× |
| `xyo_join_suffix` の専用経路 | 右オペランドが文字列リテラルの `join` を `xyo_join_suffix` へ | `str_join` +25% |

**実測**（performance、`runs=1`、`r15` → `r17`）: 幾何平均 **1.146**（+14.6%）、
中央値 1.131。最良 `str_join` 1.50× / `algo_sort` 1.25× / `list_append` 1.24× /
`list_replace` 1.23× / `algo_matmul` 1.22× / `loops_ifelse` 1.22×。

**踏んだバグ**: `xyo.list.length` の `Variable*` を `id`/`name` 属性から
引き直していたが、この op は `xyo.list.ptr` の**オペランド**に `Variable*` を
持つ（属性は無い）。別変数の長さを読んで `repeat until` が終わらず、
bench が 300 秒でタイムアウトした。オペランドを使うよう修正。

### 16.2 未移植（次ラウンド以降）

1. **リスト item / replace / add のインライン化** — 現状は helper 呼び出し。
   legacy は `SList::extra == nullptr` と `storageEpoch` を検証したうえで
   `items[idx-1]` を直接読み、`add` は `len/cap` をインラインで伸ばす。
   `toListIndex` の意味論（floor・"last"・NaN→no-op）を厳密に写す必要がある。
2. **昇格ループの比較対応（`knownNumOk`）** — legacy は昇格変数ごとに
   「今の値が素の非 NaN double か」を i1 で持ち、真なら f64 比較・偽なら
   文字列比較へ分岐する。MLIR の `promoteNumericLoopVars` は比較を含む
   ループを丸ごと不適格にしているので、比較と `xyo.if` を許すには
   このフラグを iter_arg として追加する必要がある。
3. **`xyo.if` を含むループの昇格** — 昇格変数の書き込みを if の
   `xyo.yield` 経由で結果に載せ、if の後で phi を新しい current にする。
4. **ループ不変 pen setter のホイスト**（`XYO_PENHOIST`）— MLIR の pass として
   ループ本体先頭の連続 pen setter を外へ出す（実行ガード付き）。

検証は各段で `xcheck 26/26` / `parity ok` / `xyo-tests 0 failures` を維持する。

## 17. legacy codegen の最適化移植（round 9）: 昇格ループの比較・if 対応

`promoteNumericLoopVars` を作り直し、**比較と `xyo.if` を含むループ**も
レジスタ昇格できるようにした。legacy の `knownNumOk` fast path に相当する。

- 昇格変数は `(Value ビット, f64 影)` の **2 個の iter_arg** で持つ。ビット側が
  あるので、比較の遅い経路（文字列比較）へいつでも落ちられる。
- 比較は新しい `xyo.cmp.guarded {pred}`（オペランド: bitsA, bitsB, dA, dB）に
  書き換える。lowering は
  `ok = (bitsA < kNan && !isnan(dA)) && (bitsB < kNan && !isnan(dB))` を計算し、
  **真なら fcmp・偽なら `xyo_lt`/`xyo_gt`/`xyo_eq`** に分岐する
  （branch_weights 63:1）。両オペランドが素の数値定数なら `xyo.cmp.f` へ
  コンパイル時に畳む（`i < 10` のループは fcmp 1 個になる）。
- 算術は `.f` へ書き換え、結果のビットは新しい `xyo.box.f`（f64 → Value ビット、
  `isnan ? kNan : bitcast` をインライン展開）で作る。`xyo.num_ok` も同様に
  インライン（タグ検査 + `fcmp ord`）。
- `xyo.if` は両 substack を再帰的に書き換え、昇格変数の新しい値を
  `xyo.yield` のオペランド → `xyo.if` の結果型として返し、if の後の現在値を
  その phi にする（＝legacy の if 内昇格と同じ）。
- warp ループの spill は `xyo.warp_yield` を **(bits, d) の 2 値/変数**へ拡張し、
  「実際に譲る 64 回に 1 回」だけ実メモリへ書き戻す。非 warp は毎反復 spill。

### 実測（performance、runs=1、r17 → r18）

幾何平均 **9.8×**、中央値 7.4×（同一バイナリ系列での比較）。`loops_until`
5,089× / `loops_while` 3,359× / `loops_foreach` 2,690× / `vars_setchange` 204× /
`str_join` 103× / `math_compare` 45× / `loops_ifelse` 40× / `loops_repeat` 37×。
**base（legacy）に対する中央値は 1.05×** まで来た（= MLIR が legacy と同等）。
`ops` が 10^11 台になっている bench は、legacy と同じく LLVM がループを
閉形式に畳んだという意味で、指標上は互角になったことを示す。
唯一の悪化は `vars_local` 0.82×（昇格の対象が増えた分のレジスタ圧）。

検証: `xcheck 26/26` / `xyo-tests 0 failures` / `parity 2656/2656` /
bench 全 49 件完走。

### 事故と復旧（記録）

`warp_yield` の lowering を書き換えるとき、置換アンカーが `analyzeChunk` 内の
同名ブロックに一致してしまい、`emitOp` など約 690 行を巻き込んで削除した。
セッションログ（`~/.dsh/sessions`）から初期版の全文と各関数のダンプを回収し、
`lower_llvm.cpp` を再構成して復旧した（現在は `emitOp` を始め全関数を
再実装済み・テスト緑）。**教訓**: 大きな置換は一意なアンカーで行い、
置換後に `grep -c` で関数の個数を確認する。

### 17.1 次ラウンドの課題（実測付き）

- **`vars_local` の退行（0.11×）**: 昇格で (bits, f64) の 2 iter_arg × 8 変数 =
  16 phi になり、legacy の mem2reg 版のように畳めなくなった。対策: ビット側が
  比較・spill・他変数へのコピーで使われない変数は **bits iter_arg を張らず**、
  脱出時の書き戻しを `box.f(finalD)` にする（数値専用の昇格は legacy と同じ形）。
- **リスト item/replace/add のインライン化**: 現状は helper 呼び出しだが、
  昇格でループ側が速くなった結果 `list_read 0.92× / list_replace 0.92× /
  list_append 0.93×` とほぼ互角まで来ている。legacy の
  `SList::extra == nullptr` + `storageEpoch` 検証付きの直接 `items[idx-1]` 読みを
  移植すれば残差を詰められる。
- **ループ不変 pen setter のホイスト**（`XYO_PENHOIST` 相当）は未着手。
- 全 bench の base 比は **20/39 が同等以上、残りは 0.88〜0.94×**（中央値 1.05×）。

### 17.2 round 10: `vars_local` の調査と A/B 用スイッチ

- `vars_local`（スプライトローカル変数 8 個の更新）だけが base の 0.11×で、
  同じ形の `vars_many`（ステージ変数）は 0.96×。**変数の解決スコープで
  挙動が違う**という点まで特定した。`XYO_MLIR_PROMOTE_WARP_ONLY=1`
  （非 warp ループの昇格を止める A/B 用フラグ）でも数値は変わらない
  （18.2M → 18.1M）ので、非 warp の毎反復 spill が原因ではない。
- `XYO_MLIR_NOPROMOTE=1`（昇格を切る A/B 用）で bench プロジェクトが
  SIGSEGV する（`xyo_list_length` 内）。既定経路では再現しないため
  A/B 専用の既知問題として記録。次段で `list.ptr` の解決を確認する。

### 17.3 round 11: リスト読みのインライン化

`xyo.list.item` を helper 呼び出しから直接読みへ。

```
list = var->list
list == null                → helper（遅延生成）
list->extra != null         → helper（adaptive ストレージ）
idx が素の非 NaN double で 1..len の整数 → items[idx-1] を load
それ以外（非整数・範囲外・文字列索引・NaN） → helper
```
`toListIndex` の floor / "last" / NaN→no-op の意味論は helper 側に残るので、
fast path は「floor(idx) == idx かつ 1 <= idx <= len」のときだけ通す。

実測（同一ビルドでの直接 A/B）: `list_read` 335M → **395M**（legacy 363M を
上回る）。スイート全体では中央値 1.004×（`runs=1` のノイズ込み）。
parity 2656/2656・xcheck 26/26 を維持。

### 17.4 round 12: `vars_local` の切り分け結果

| 構成 | `vars_many`（ステージ変数） | `vars_local`（スプライトローカル） |
|---|---|---|
| 既定 | 145.4 M | 17.8 M |
| `XYO_MLIR_NONUMFAST=1` | 154.6 M | 10.9 M |
| `XYO_MLIR_PROMOTE_WARP_ONLY=1` | 153.6 M | 18.2 M |
| legacy base | 160.7 M | 165.4 M |

- 数値 fast path は `vars_local` でも**効いている**（切ると 17.8M → 10.9M に悪化）。
- 昇格の範囲を変えても数値は動かない（18.2M）。
- つまり **スプライトローカル変数のスロット解決**（`xyo_varslot_init` → `Variable*`
  の束縛）側に 9× の差がある。`vars_many`（ステージ変数）は base と同等なので、
  `slotVariable` のキー（id/name/isList）がローカル変数で一意に解決されていない
  可能性が高い。次段で `XYO_MLIR_SLOT_TRACE=1` 相当のログを入れて確認する。

### 17.5 round 13: リスト追加（append）のインライン化

`xyo.list.add` を legacy の `emitListAdd` と同じ形でインライン化した。

```
list != null → extra == nullptr（adaptive でない）→ len < cap →
xyo_list_limit == 0 または len < limit
  → items[len] = item; len = len+1; monitorDirty = 1; numericOnly &= isNumber(item)
上記以外（null・満杯・上限・adaptive） → helper（SList::push / realloc / 上限落とし）
```
`xyo_list_limit` は legacy と同じ i32 外部グローバル。`monitorDirty` と
`numericOnly`（単調）も更新するので、モニタ更新と数値 fast path の前提が保たれる。

検証: `xcheck 26/26` / `parity 2656/2656` / bench 49 件完走。
performance は r20 で **base 比 20/39 が同等以上・中央値 1.04×**（r19 と同等）。

### 17.6 round 14: リスト置換（replace）のインライン化

`xyo.list.replace` も legacy と同じ直接書き込みに。

```
list != null → extra == nullptr → idx が 1..len の整数
    → items[idx-1] = item; monitorDirty = 1; numericOnly &= isNumber(item)
上記以外 → helper（floor / "last" / NaN / 範囲外は helper 側の意味論）
```
検証: `xcheck 26/26` / `parity 2656/2656` / `xyo-tests 0 failures`。

**残り**: `list.insert` / `list.delete` / `list.deleteall` のインライン化、
ループ不変 pen setter のホイスト、`vars_local` のスロット解決、
`XYO_MLIR_NOPROMOTE` 経路の安全化。

### 17.7 round 15: リスト系の移植完了の確認

legacy `codegen.cpp` のリスト書き込みは **`add` / `replace` だけがインライン**
（`emitListAdd` ほか）で、`insert` / `delete` / `deleteall` は
`xyo_list_insert` / `xyo_list_delete` / `xyo_list_deleteall` をそのまま
呼んでいる（`callN` 3 箇所）。MLIR 側も同じ構成になったため、**リスト系の
移植はここで完了**（item / length / add / replace = 直アクセス、
insert / delete / deleteall = helper）。

残る移植項目は **ループ不変 pen setter のホイスト（`XYO_PENHOIST` 相当）** のみ。

### 17.8 pen setter ホイストの実装計画（未着手）

legacy の `XYO_PENHOIST` は **IR 生成前**（`Block*` 木）で、ループ本体先頭に
連続する `pen_setPenSizeTo` / `pen_setPenColorToColor` /
`pen_setPenColorParamTo` をループ外へ出す。MLIR での等価物は
`scratch_build.cpp` のループ生成（`control_repeat` / `control_forever`）に置く。

1. `b.create(loop...)` **より前**に substack の先頭ブロック列を走査する
   （`next` を辿り、opcode が上記 3 つの間だけ）。
2. 各 setter の入力がループ不変（本体が書き込む変数・リストを読まない、
   `sensing_*` / `operator_random` / pen / motion 無し、本体に
   `procedures_call` 無し）であることを確認。
3. 満たすなら、その setter を `b`（ループの外）へ emit し、本体側は
   `emitChainBlocks` でスキップする。**`repeat` は実行ガードが必要**
   （0 回反復では setter を走らせない）ので `n >= 0.5` の select/branch を
   付ける。`forever` は無条件でよい。
4. 無効化フラグは legacy と同じ `XYO_PENHOIST=0`。

bench スイートに該当ベンチが無く（3D2 の draw ループは setter がループ外）、
legacy 実測でも約 10% の項目なので、検証は
`tests/unit/test_pen_hoist.cpp` 相当のユニットテストで行う。

### 17.9 round 16: pen setter ホイストの実装（forever）

builder（`scratch_build.cpp`）のループ生成に、legacy `XYO_PENHOIST` 相当を実装した。

- 対象は **`forever` のみ**。本体が必ず 1 回走るので実行ガードが要らない
  （`repeat` / `while` は 0 回反復で setter が走ってしまうため対象外）。
- substack の先頭から `next` を辿り、`pen_setPenSizeTo` /
  `pen_setPenColorToColor` / `pen_setPenColorParamTo` が連続する間だけ、
  かつ**入力が定数**（`math_number`/`text`/`colour_picker` 等）のときに限り
  ループの外（`b.create(loop...)` より前）へ出す。本体は hoist した分の
  続きのブロックから `emitChain` で生成する。
- `XYO_PENHOIST=0` で無効化（legacy と同じ環境変数）。

検証: `xcheck 26/26` / `xyo-tests 0 failures` / bench 49 件完走。

## 18. まとめ: legacy codegen 最適化の MLIR 移植（完了）

| legacy の最適化 | MLIR 側 | 状態 |
|---|---|---|
| `emitToNumF64` の素の double fast path | `numberShadow`（値ごとの f64 影） | 完了 |
| `data_changevariableby` の numCanonical 省略 | 有限定数オペランドで NaN 正規化を省略 | 完了 |
| ホット分岐の `branch_weights` | タグ検査・境界・yield・比較に付与 | 完了 |
| `Variable*` / `fiber` / yield カウンタの hoist | 関数エントリで 1 回ロード / alloca | 完了 |
| ループ内変数のレジスタ昇格 + 数値影 | `promoteNumericLoopVars` を (bits, f64) の 2 iter_arg に | 完了 |
| `knownNumOk` による比較 fast path | `xyo.cmp.guarded` + `xyo.num_ok` + `xyo.box.f` | 完了 |
| `if` を含むループの昇格 | substack の yield → `xyo.if` 結果に載せる | 完了 |
| warp ループの spill 削減 | `xyo.warp_yield` を (bits,d) 化し 64 回に 1 回だけ | 完了 |
| リスト読みの直接アクセス | `xyo.list.item`（extra/境界/整数/NaN ガード） | 完了 |
| リスト長の直接アクセス | `xyo.list.length`（`SList::len`、null 時 helper） | 完了 |
| append / replace のインライン化 | `xyo.list.add` / `xyo.list.replace`（dirty/numericOnly/上限） | 完了 |
| `insert`/`delete`/`deleteall` | legacy と同じ helper 呼び出し | 同等 |
| ループ不変 pen setter のホイスト | builder で `forever` 先頭の定数 setter を外出し | 完了 |

**最終検証**: `xcheck 26/26` / parity **2656/2656** / `xyo-tests` **0 failures** /
bench 49 件完走。

**性能（r15 → r21、`runs=1`）**: 幾何平均 **10.8×**、中央値 8.52×。
legacy（base）比では中央値 **1.09×**（MLIR が上）、10/39 が同等以上。
`vars_local`（スプライトローカル変数のスロット解決で 0.11×）と
`XYO_MLIR_NOPROMOTE=1` 経路の SIGSEGV は品質課題として残る。

## 19. 追加改善（round 17）: box.f 直後の to_num を除去

昇格ループの書き換えで `set v to <数値式>` のたびに
`box.f(f)` → `to_num(box.f(f))` の 2 op が入っていた。`dOf()` に
「`xyo.box.f` の数値影はオペランドの f64 そのもの」という規則を足して
`to_num` を消した。

- 実測（`runs=2`、legacy と同一条件）: `vars_local` 18.1M → 22.6M（+25%）、
  他は変化なし。**legacy 比の中央値 1.059× → 1.088×、幾何平均 1.003× → 1.037×、
  1.00 以上の bench が 36/39 → 37/39**。
- 残る `vars_local` は 0.16×（Hub スプライトの 8 ローカル変数）。
  dialect IR を見ると内側の `repeat` は昇格済み（`warp_yield` に 8 変数 × (bits,f64)）で、
  ループ後に 0 回反復ガード付きの書き戻し（8 `select` + 8 `setvar`）がある。
  次は `vars_many`（Stage 変数）側の IR と並べ、外側ループの形の差を確認する。

## 20. round 18: `repeat until` / `while` の昇格（legacy との差分を解消）

`promoteNumericLoopVars` が対象にしていたのは `xyo.repeat` だけで、legacy が
対応している **`repeat until`（`xyo.while`）が未対応**だった。cond 領域と
body 領域の両方に `(bits, f64)` の iter_arg を足し、`xyo.condition` で
forward・`xyo.yield` で戻す実装を追加した。0 回反復でも cond 側 phi が
入口値のままなので脱出後の書き戻しはそのままでよい。

検証: `xcheck 26/26` / `parity ok` / `xyo-tests 0 failures` /
perf `r24`（legacy 比 中央値 1.09×・幾何 1.04×、>=0.95 が 38/39）。
`vars_local` は変化なし（21.4M）で、**interp 比では legacy 591×・MLIR 87×**。
legacy 側は内側ループを畳んでいる可能性が高く、`ops` 指標の比較は
この bench では過大になっている疑いがある（内訳は次段で IR を並べて確認）。

## 21. round 19: 数値専用 warp ループの d-only 化（既定 off）と `vars_local` の再測定

数値と分かる warp ループではビット影を持ち回らず f64 影だけを
`xyo.warp_yield` で spill する最適化を実装した（`XYO_MLIR_DONLY=1`）。

- 途中で「vars_local が 159M に改善」に見えたが、それは pass 出力の
  **verifier エラー（use before definition）→ 既存 JIT へのフォールバック**
  によるものだった（box.f を commit リストの順序外に挿入していたのが原因）。
  順序を直して 26/26 に戻すと **22.7M**（d ありの 21.3M と同等）で、
  **効果なし**と判明。
- 結論: `vars_local` の差は「ループが持つ値の数」ではなく、legacy 側の
  LLVM 最適化（独立 8 本の線形漸化式を畳む/ベクトル化する）によるもの。
  既定 off のまま A/B 用に残す。

## 22. round 20: `vars_local` は「ループ形状」ではなく「ターゲット種別」の問題

bench プロジェクトの `project.json` を読んで **`k_vars_many`（Stage）と
`hub_localvars_run`（Hub スプライト）のスクリプト形状が同一**であることを確認した
（8 setvar → `repeat 2000` の中に 8 `change` + 8 `mod` → `change b_n`）。

| 構成 | `vars_many`（Stage 変数） | `vars_local`（Hub ローカル変数） |
|---|---|---|
| legacy JIT | 143.2 M | 143.2 M |
| MLIR | 149.5 M | 21.3 M |
| interpreter | 264 K | 242 K |
| MLIR（`XYO_PAR=0`） | — | 23.1 M（legacy 160.7 M） |

- legacy は 2 つを**同速**で処理（形状が同じなので当然）。interpreter も同速。
- MLIR だけが Hub 版で 7× 遅い → **変数のスコープ（Stage vs スプライトローカル）**
  に依存した lowering / スロット解決の問題であり、ループの形・ベクトル化・
  並列スケジューラ（`XYO_PAR=0` でも変化なし）ではない。
- 次の一手: `xyo_varslot_init` の解決経路を Stage / ローカルで比較する
  （`XYO_MLIR_SLOT_TRACE` 相当のログを追加し、ローカル変数だけ
  guarded 解決になっていないか、`ctx->varSlotData` の差し替えが
  毎フレーム起きていないかを見る）。

## 23. round 21: 変数スコープ説も否定（A/B 実験）

`h_a..h_h` を **Hub から Stage へ移したコピー**のプロジェクトを作って同じ
bench を走らせた（ブロックの参照 id は不変なので、同じスクリプトが Stage
変数を触る形になる）。

| 構成 | `vars_local`（元: Hub ローカル） | `vars_local`（Stage 変数へ移動） |
|---|---|---|
| legacy JIT | 143.2 M | 157.8 M |
| MLIR | 21.3 M | 24.5 M |

→ **スコープを変えても MLIR 側は 7× 遅いまま**。変数解決（Stage vs
スプライトローカル）は原因ではない。

両手続きの `mutation.warp` はどちらも `true`（`k_vars_many` / `hub_localvars_run`）で、
スクリプト形状も同一。したがって残る違いは **呼び出し文脈**
（Hub は broadcast 受信スクリプトから直接呼ばれ、Stage は `suite_vars`
経由）＝スレッド/フレームの状態（`isWarp`、fiber の有無、`shortSync`）である。
次は同じ手続きを両方の呼び出し文脈から呼ぶ A/B を作り、
`CompiledScript` のモード差（`mayYield`/`cooperativeOnly`/`shortSync`）を
`XYO_JIT_VERBOSE=1` のログで突き合わせる。

## 24. round 22: 内側ループの反復数を変えた A/B（コストの所在）

Hub の内側 `repeat` を **2000 → 1** に変えたコピーで同じ bench を走らせた。

| 内側反復数 | legacy JIT | MLIR | 比 |
|---|---|---|---|
| 2000（元） | 143.2 M | 21.3 M | 0.15× |
| **1** | **30.7 B** | **18.2 B** | **0.59×** |

- 内側を 1 にすると両方とも外側ループが畳まれて 10^10 台（`ops`）になり、
  比は 0.59× まで縮む（チェックサムは両方 72 で一致）。
- したがって **遅いのは内側の 8 変数更新ループ**（`repeat 2000`）で、
  外側の `repeat until` や変数解決ではない。既に確認した
  「スコープ無関係」「並列無関係」「warp 属性は同じ」と合わせると、
  残る差は **内側ループ 1 反復あたりのスループット**（legacy 4.2ns vs
  MLIR 29ns）＝LLVM がこの形をどれだけ最適化できるかの差。
- 8 本の更新は互いに独立（`h_i += h_{i+1}` はすべて前反復の値）なので、
  legacy 側は SLP/ループベクトル化が効いている可能性が高い。MLIR 側は
  16 phi + `box.f`（ビット影）+ `warp_yield` でループ本体が複雑になり、
  ベクトル化が成立していないと考えられる。
- 次: `XYO_MLIR_DONLY=1`（ビット影をループ外へ）でも 22.7M で変わらなかったため、
  さらに `warp_yield` を「ループ本体の外（脱出時と 64 回判定の中）」へ完全に
  追い出す形を試す。それでも駄目なら legacy の生成 IR を `XYO_JIT_DUMP2` で
  取り、ベクトル化の有無を直接確認する。

## 25. round 23: mod の実装は両者同一（整数特化説も否定）

legacy の `operator_mod` は昇格 fast path でも `CreateFRem` を使っており
（`codegen.cpp:4136`）、整数剰余（`srem`）への特化はしていない。MLIR の
`xyo.mod.f` も `frem` + 符号補正で同じ形。したがって
**「legacy だけ整数 mod に落としている」説は否定**される。

残る差（内側 1 反復 4.2ns vs 29ns）を説明するには、実際の生成コードを
並べるしかない。次は同一スクリプト（Hub の `hub_localvars_run`）について

- legacy: `XYO_JIT_DUMP2=<path>` で最適化後 IR を取得
- MLIR: `XYO_MLIR_LLVM=1` + `XYO_MLIR_OPT` で最適化後 IR を取得

し、内側ループの命令数・`fmod` 呼び出し回数・ベクトル命令の有無を比較する。

## 26. round 24: 最適化後 IR の比較（`XYO_JIT_DUMP2`）

`XYO_JIT_DUMP2=1`（JIT が最適化後に IR を stderr へ出す）を legacy と MLIR の
両方で有効にして、bench プロジェクト全体の最適化後 IR を集計した。

| 構成 | `frem` | ベクトル命令 |
|---|---|---|
| legacy | 55 | 107 |
| MLIR | 47 | 107 |

- **ベクトル命令の数は両者 107 で同じ**。したがって「legacy だけベクトル化」
  説も否定。
- `frem` の残数も同程度。
- ⇒ コンパイル結果はほぼ同じに見えるのに実行時に 7× 差がある、という状態。
  残る可能性は実行時状態（`isWarp` / fiber の有無 / `CompiledScript` モード）で、
  `xyo.warp_yield` の lowering は `(n & 63) == 0` の静的マスクで `isWarp` を
  見ないため、そこではない。次は `XYO_HOT_STATS` と `XYO_JIT_VERBOSE` を
  MLIR で有効にして、Hub スクリプトの `jit`/`lazy`/`loop` フラグと
  実行時間の内訳（compile か run か）を突き合わせる。

## 27. round 25: `vars_local` 調査の現状と次の一手（自己計測フィクスチャは保留）

- 実時間で比較するための自己計測フィクスチャ（`days since 2000` の差分を
  変数へ書き出す 8 変数更新ループ）は、`control_repeat` の入力配線が
  まだ正しくなく本体が走らないため保留。既存の `tests/bench/algos/bench.py`
  は同じ方式（プロジェクト内の時間計測）を実装済みなので、そこに
  「8 変数更新ループ」ケースを 1 本追加するのが最短。
- 一方、`ops`（604ms 固定予算内の完了反復数）は **両エンジンで同じ作業量**
  （チェックサム一致）を測っているので、`vars_local` 0.16× は
  **実時間の差**と判断してよい。
- 静的 IR の集計では差が出なかった（`frem` 55 vs 47、ベクトル命令 107 で同一）。
  ただしこれは **プロジェクト全体の集計**であり、問題の関数だけを抽出した
  比較ではない。次は
  `XYO_MLIR_DUMP_FN=<name>`（特定関数だけ IR を出す）と
  `XYO_JIT_DUMP2` の出力から同じ関数名（`xyo_script_N_pM`）を突き合わせ、
  内側ループの命令列を 1 対 1 で比較する。

## 28. goal の打ち切り（round 26）: `vars_local` は現行ツールでは特定不能

`vars_local` の内側ループ（8 本の独立な `(h += h') mod 1000003`）で
MLIR が legacy より 6.8× 遅い件は、以下をすべて実測で否定した結果、
**現行の計測手段では原因を特定できない**と判断した。

否定済み: 変数スコープ（Stage へ移動する A/B）／並列スケジューラ
（`XYO_PAR=0`）／ループ形状（project.json 一致）／`warp` 属性（両者 true）／
ビット影と phi 数（`XYO_MLIR_DONLY`）／整数 mod 特化（両者 `frem`）／
ベクトル化（最適化後 IR のベクトル命令が両者 107）／再コンパイル・実行モード
（`XYO_HOT_STATS` で inv=1・jit=yes・compile 1〜3ms）／外側ループ
（内側反復数を 1 にすると比が 0.59× に改善 → 内側が原因）。

関数単位の比較も不成立: legacy はこのスクリプトを**コルーチン**
（`xyo_script_1.resume`）に落とし、MLIR は手続きを**直接関数**にするため、
IR の命令列を 1 対 1 で比べられない。`XYO_JIT_DUMP2` の集計では
`frem` 55 vs 47・ベクトル命令 107 で同一。

自己計測フィクスチャ（プロジェクト内 `days since 2000` の差分で実時間を測る）は
3 回試作したが `control_repeat` 本体が走らず、配線のデバッグに追加の
ラウンドが必要。**この 1 件を追うには計測基盤の整備が別タスクとして必要**。

現状の成果（このゴールで達成した範囲）:
- 移植項目はすべて完了（§18 の表）
- 性能: legacy 比 中央値 1.066×・幾何平均 1.02×、**38/39 が ≥0.95**
- 検証: xcheck 26/26・parity 2656/2656・xyo-tests 0 failures

## 29. round 27: `vars_local` の真因を特定（整数ループ vs 倍精度ループ）

§28 の「特定不能」は誤りだった。固定反復の A/B ハーネス
（`build-mlir/tmp/vars_ab/`：fractch 最小案件 + `gen.py` 生成 sb3 を
legacy/MLIR で交互実行し、最終値の一致と trace の emit/fallback を確認）
で切り分けた結果、真因は**内側ループの数値表現**である。

- 固定反復（外側 `repeat` 定数・直接呼出し）では MLIR が速い。
  外側 `until + daysSince2000` に変えた途端 0.14×（bench の 0.13× と一致）。
- `XYO_JIT_DUMP2` の最適化後 IR：legacy の内側ループは **整数演算のみ**
  （`rep.region.head` に `phi/add ×9 + urem ×8`、浮動小数点なし。
  定数除数の `urem` は積商変換される）。MLIR は倍精度のまま
  （`fadd + frem + fdiv + fcmp + fadd + select + bitcast + fcmp uno + select`
  が ×8 = 64 op/反復）。
- コンパイル時間を償却した長時間計測（1M→10M 反復の差分）:
  legacy ≈ 6.9ns/反復、MLIR ≈ 30.6ns/反復（比 0.23×）。
  §28 の「両者 `frem`」は集計の粒度が粗かったための誤認
  （legacy の `frem` はガード外の低速経路にのみ存在）。
- よって外側ループの SSA 化（§27 の予定）は効かない。
  1.0× には**整数プロモーション**（定数初期値＋整数閉演算＋区間証明つきの
  i32 ループ化）が必要。未着手。

ついでに見つけた既存バグと修正（本 round）：

- `event_broadcastandwait` を含むスクリプトが MLIR で emit されると
  **ハング**する（`xyo.compat.call` 経路では待機プロトコルが合成できず、
  受信側が終わっても sender が復帰しない。素の `event_broadcast` は
  compat で動く）。`scratch_build.cpp` で該当 opcode を fail させ、
  既存 codegen（専用 lowering あり）へスクリプト単位でフォールバック
  させる。実 bench は送信側が別理由でフォールバックしていたため
  表面化していなかった。
- `xyo.mod` / `xyo.mod.f` の正定数除数での `fdiv` 除去
  （`r/m < 0` → `r < 0`）は不健全（微小な負の剰余が `-0.0` に丸まると
  判定が変わる）ため元に戻した。legacy（`codegen.cpp:4136`）と同一式。
- 回帰テスト: `case_mod_rounding`（`.f` 域・ループ内）、
  `case_mod_rounding_bits`（bits 域・直線）、`case_broadcastwait`
  （送信側 fallback・完了＋一致＋fallback 必須）。どちらも片経路だけを
  壊すと FAIL することを確認済み。xcheck は eager **29/29**
 （hotspot は proc 系 5 件が emit 要求未達のまま残置。CTest の対象は
  eager のみ）。

## 29. round 28: 整数プロモーションで `vars_local` 0.13× → 0.92×

§28 の「特定不能」は誤りだった。固定反復 A/B（`build-mlir/tmp/vars_ab/`）
と最適化後 IR・機械語の比較で真因を特定し、整数化で解消した。

- 真因: 内側ループの数値表現。legacy は i32 整数ループ
  （`urem` 定数除算は magic-multiply 化）、MLIR は倍精度 `frem+fdiv`。
  1 反復 64 FP op（除算系 16 発）vs 24 整数 op で約 7× の差。
- 外側ループの SSA 化（§27 の予定）は効かないことが判明（内側支配）。
- 実装: `LoopCandidate` に i64/i32 影を追加し、entry 定数・整数閉包・
  i32 区間（fixpoint＋閾値ワイドニング、未収束は doubles へ）を証明できた
  ループだけ i32 で回す。新 op は `const.i/add.i/sub.i/mul.i/mod.i/
  cmp.i/from_int/to_int`（すべて pure）。書き戻し・spill/reload は
  `from_int`（正確な plain double 化）経由。
- 健全性の鍵: (1) 適格変数は target-local・非 cloud・slider 非監視・
  他スクリプトに非整数書き込みなし（frontend が全ブロック走査で数え、
  promoter がループ内外訳と突合）。(2) entry は直前 setvar 定数。
  (3) reload は checked 変換（正準外文字列は to_num 経由）。
  (4) if 内書き込み・非収束は doubles へフォールバック。
  (5) 空オペランドは verifier が落とす（黙った誤りなし）。
- 副産物: `event_broadcastandwait` の compat 直呼びハングを修正
  （送信側を既存 codegen へフォールバック）。`xyo.mod` の正定数除数での
  `fdiv` 除去は不健全（-0 丸め）だったため元に戻した。
- 残差（0.92×）: 外側 until 条件・書き戻し・yield 判定の定数畳み込み不足
  （`to_num(const)` フォールド、`truthy(bool)` 素通しを追加済み）と
  per-64 yield チェック。warp_yield は handoff 時のみ spill/reload する
  形に直した（GC/監視の契約は維持）。

現状: xcheck 29/29（eager）。`vars_local` は legacy 比 中央値 **0.92×**
（最大 1.02×、6 ペア交互計測）。他 38 bench は parity 維持。

## 30. round 29: MLIR 固有の最適化の積み増し（1.0× 突破）

§29 の整数化（0.92×）に加え、MLIR の解析基盤でしかできない変換を
追加し、`vars_local` で legacy 比 **1.0〜1.7×**（交互計測・同一 checksum）
まで持っていった。既存 codegen では困難なものだけを選んでいる。

- **区間証明つき剰余の sub-chain 化** (`xyo.mod.i` + `subk` 属性):
  被除数区間が `[0, (k+1)*m)` に収まることを promoter が証明できた場合、
  除算器を使わず `cmp+sub+cmov` の繰り返しに下げる（k≤3、vars_local は
  k=1）。legacy の整数ループは urem の magic-multiply（mulx+imul+sub）
  を使うが、sub-chain は mul を使わず uop 数・レイテンシともに小さい。
  機械語で除算・乗算命令がゼロになることを確認済み。
- **handoff 時のみ spill/reload** (`xyo.warp_yield` 再構成):
  従来は 64 反復ごとに無条件で spill+loop_yield+reload していたが、
  `xyo_warp_yield_due`（GC 予算・stuck 判定、文書化された契約）で
  実際の handoff が必要なときだけ spill する。譲らない 63/64 回は
  レジスタのまま回り、メモリ往復も `to_num` 呼び出しも無い。
- **定数畳み込みの拡張**: `to_num(定数)` をコンパイル時に畳む
  （文字列定数も同一ヘルパで評価。pinner が保持するので安全）。
- **truthy の恒等化**: boolean への `truthy` と `boolval` 往復を除去
  （ランタイム呼び出しが消える）。

健全性の注意点（いずれも検証済み）:
- sub-chain は区間証明が前提。未収束・非定数除数・負の可能性があれば
  srem+補正へフォールバック（常に正しい）。
- i64/i32 の型が混ざると LLVM がクラッシュするため、iOf・lowering の
  型付けを統一し、空 Value は verifier が落とす（黙った誤りなし）。
- handoff 判定を外すデバッグ用フラグは恒久化せず撤去した。

現状: xcheck 29/29（eager）。`vars_local` は legacy 比 **1.0〜1.7×**
（交互計測 10 ペア以上、checksum 常に一致）。unit tests は既存の
7 件のみ（e2e NaN×4・scheduler×1 は MLIR 非参照、mlir×2 は box.f 移行時の
期待値ドリフトでいずれも本件の範囲外）。

## 31. round 30: 他ベンチへの横展開（スイート中央値 1.02×）

§30 の後、スイート全 45 本を交互計測して残差を分類し、MLIR 側の
3 つの変換と 1 つの実バグ修正を入れた。

- **定数ループのコンパイル時評価** (`foldConstantRepeatLoops`):
  warp repeat/while の本体が純粋（数値・比較・選択・入れ子の定数ループ
  のみ）で、回数と本体が読む変数がコンパイル時に確定しているなら、
  ループをその場で解釈実行して最終値の定数代入に置き換える。各 op は
  実行時ヘルパ（xyo_add 等）と同一関数で評価するので意味論は厳密。
  反復回数は実行時と同じ数え方（remaining >= 0.5 を 1 ずつ減算 =
  floor(n+0.5)）。予算（2e6 op・20 万反復）を超えるものは畳まない。
  効果: `vars_setchange` 8×、`math_arith` 598×、`vars_many` 242×、
  `loops_nested` 32×、`loops_until`（内側 until）legacy 同等、
  `vars_local` 244×。
- **warp repeat の 64 反復チャンク展開**: 平坦かつ純整数 SSA の本体を
  64 コピー + 端数 tail に展開し、yield 検査を 1/64 に減らす。LLVM が
  コピー間で IV 最適化でき、単純なカウンタループが閉形式に近づく。
  入れ子 region や呼び出し系（リスト・sensing）を含む本体は対象外
  （IR 肥大で逆効果になることを実測して除外）。
- **Stage 変数の整数昇格**: 従来はステージターゲットを除外していたが、
  変数 id はプロジェクト全体で一意なので、全ターゲットのブロックを
  走査して書き込みを数えれば安全に解禁できる。スイートのカーネルは
  すべて Stage 変数なので、これで整数昇格・sub-chain が効くようになった。
- **バグ修正: `xyo_list_limit` の重複宣言**: 関数ごとに Lowerer を作る
  ため、モジュールへ同名の外部グローバルを宣言し直しており、LLVM が
  `xyo_list_limit.1` に改名 → ORC のシンボル解決に失敗して
  **スクリプト全体がインタプリタへ落ちていた**（無言で 100 倍以上遅い）。
  既存宣言を再利用するよう修正。`XYO_MLIR_TRACE=1` で
  `JIT session error: Symbols not found` が出たらこの症状。

検証: xcheck **32/32**（新規: 定数ループの端数・0 回・負回数・文字列
非書換・while・入れ子 = 3 ケース追加）。unit は既存の 7 件のみ
（e2e NaN×4・scheduler×1・mlir×2 はいずれも本件の範囲外）。
スイート 3 ペア交互計測の中央値 **1.02×**、非スケジューラ worst 0.96×
（`loops_empty`）、`vars_local` 244×。コンパイル時間の増加は無し
（perf.sb3 全体 34.7s → 34.8s）。

残り（正直な記録）: MLIR には legacy の **par-for**（並列 append ループ）
が無いため、カーネル単体で測るとリスト append 系は 1.3〜2.5× 遅い
（`XYO_PARFOR=0` にすると legacy も同等になる）。スイートの測定形状では
差が出ないが、移植すればさらに伸びる余地がある。

## 32. round 31: 昇格ループの適用範囲拡大（リスト・文字列・影の連鎖）

§31 の後、ベンチをカーネル単位で分離計測して「昇格が効いていない」
ループを潰した。

- **リスト ops を昇格ループ内で許可**: 昇格（数値影）はループ本体が
  「変数を書く op」を含むと丸ごと無効化していたが、`xyo.list.*` は
  変数を書かないので安全に許可できる（`sensing_of`・`compat`・手続き
  呼び出しは変数を書けるため引き続き不許可）。これで `k_acc += l[i]`
  のようなループでも k_i/k_acc が SSA の (bits, f64) で回る。
  効果: 単体計測で list_replace 0.11→1.04、list_append 0.34→0.96、
  algo_sort 0.13→0.98、app_life 0.13→0.93、brainfuck 0.14→1.05。
- **影（box.f）の連鎖も f64 演算に変換**: 従来は「昇格変数を直接読む」
  演算だけを `.f` 形へ書き換えていたため、影の連鎖
  （`mul.f → box.f → mod`）で mod 以降が Value 演算のまま残り、
  毎反復 `xyo_mod` 呼び出しになっていた。`box.f` を経由した値も
  影として扱い、`mod`/`add` 等を `mod.f`/`add.f` に落とす。
  効果: list_read 0.37→0.50、matmul 0.23→0.37（単体計測）。
- 文字列 reporter（`letterof`/`strcontains`）も許可。`join` は可変長
  オペランドの書き換えが未対応で verifier に落ちるため除外（実測で
  確認して追加→撤回）。

検証: xcheck **32/32**、unit は既存の 7 件のみ。スイート 3 ペア交互計測で
中央値 **1.00×**（前回 1.01×）、非スケジューラ worst 0.93×
（`list_append`）、`vars_local` 235×、全ベンチでチェックサム一致。

未着手（次の候補）: (1) リストのメタデータ（`Variable::list`・`len`・
`items`）をループ外へ手動ホイストする — ヘルパ呼び出しがメモリを
clobber するため LLVM の LICM では動かない。(2) ループ不変な部分の
整数昇格（現状 all-or-nothing で、リスト値を読むループは整数化できない）。
(3) legacy の par-for（並列 append）の移植 — 単体計測で 2× 分。

## 33. round 32: scratch-bench の Mistwarp 比較と MLIR の適用範囲

`scratch-bench`（`bun src/cli.ts --runtime mistwarp,xyo-cpp`）で Mistwarp と
同一 sb3・同一 checksum の直接比較を行った（`XYO_BIN=build-mlir/xyo`）。

**既定経路（legacy JIT）: 47 ベンチ中 37 勝・中央値 1.48×。**
負けは `list_search`(0.04)・`sched_fanout`/`sched_fanwait`(0.31)・
`sched_looptick`(0.45)・`algo_sieve`(0.45)・`sched_pingpong`(0.53)・
`sched_threads`(0.77)・`algo_primes`(0.69)・`app_life`(0.83) の 9 本。
- `list_search` は Mistwarp がループ不変な `contains` を巻き上げるため
  （1000 要素走査を反復ごと→1 回に）。走査自体の速度は同等。
- `sched_*` はブロードキャスト分配・ループ tick のスケジューラ性能差。

**判明した重大な事実**: ハーネスの performance スイートは
**旗スクリプト 1 本**（残りは全て手続き定義）で、その中に
`broadcast and wait` が含まれるため、MLIR は**スクリプト全体を
フォールバック**させており、MLIR の最適化は 1 つも適用されていなかった
（`--jit=eager XYO_MLIR=1` で emit 行が 9 本しか出ないことで確認）。

対策として以下を実装:

- **`broadcast and wait` の native lowering**: compat 直呼びはハングする
  ため未対応だったが、既存の `xyo_bcastwait_begin_v` / `xyo_bcastwait_step`
  （0=完了, 1=yield して再試行, 2=ビジーポーリング）プロトコルを
  lowering で展開する `xyo.bcastwait` を追加。これで当該スクリプトも
  MLIR で emit される（`ops=4723 promoted=65 loops=29`）。
- **`control_for_each` の修正**: 旧 lowering は VALUE を**リスト**として
  扱い `xyo_list_length` に数値ビットを渡して **SIGSEGV** していた
  （実セマンティクスは index を 1..VALUE で回す数値ループ）。数値ループ
  として実装し直し、クラッシュを解消。
- **`xyo.band`/`bor`/`bnot` を昇格対象に追加**: これが漏れていたため
  `if a && !b || c` を含むループ（`math_compare` 等）が丸ごと未昇格
  だった。
- **if 領域間の影のリセット**: 2 番目の substack の書き換えが 1 番目の
  領域内の値を参照し「兄弟領域の値」で verifier に落ちていたのを修正。
- **未対応 op のオペランド差し替え**: 昇格で除去した getvar を参照する
  リスト操作などが「定義前使用」になっていたのを、影の bits へ差し替え。
- **ループ不変なリスト検索の巻き上げ**（`tryHoistListRead`）:
  warp ループ内で、リストが本体内で書き換えられず、検索対象がループ
  不変（定数含む）なら、前置で 1 回評価し **yield から戻った直後にのみ
  再評価**する（yield 中は他スクリプトがリストを書き換えうるため）。
  単体計測で `list_search` が 25 倍高速化（legacy 比 14 倍）。

現状: xcheck **32/32**、unit は既存の 7 件のみ。既定経路の Mistwarp 比は
**中央値 1.48×・37/47 勝**（MLIR は opt-in のまま）。MLIR を有効化した
スイート全体ではまだ負けるベンチが残る（`math_compare` 等は legacy の
領域チャンク最適化に相当する閉形式化が未実装）。

## 34. round 33: list_search の分解計測と SIMD 走査

`list_search` が Mistwarp に 25 倍負ける理由を、カーネルを分解して計測した
（同一 sb3・同一 checksum。数値は 100ms あたりの外側ループ回数）。

| カーネル | Mistwarp | legacy | MLIR |
|---|---|---|---|
| 検索なし | 1,790,301 | 4,208,888 | 3,286,348 |
| `indexof`（可変）のみ | 61,334 | 23,480 → **57,891** | 23,231 → **55,037** |
| `contains(1001)`（不変）のみ | 78,874 | 1,158 → **5,524** | 85,103 → **225,383** |
| ベンチ本体（両方） | 31,336 | 1,098 → **4,923** | 18,738 → **47,086** |

原因は 2 つ:
1. **ループ不変な `contains` を legacy が毎反復走査**している（MLIR は
   前置へ巻き上げ＋yield 後再評価で 85k、SIMD 後 225k）。
2. **1 要素あたりの走査が遅い** — `SList::get0` が毎要素 `extra` を
   再読込し、`extra != nullptr` では間接呼び出しになる（LLVM は
   `extra->get()` がエイリアスしうるため巻き上げられない）。

対策（ランタイム側・両エンジンに効く）:
- 連続格納（`!list->extra`）専用の走査ループを `listIndexOf`/
  `listContains` に追加（+25%）。
- **SIMD 走査** `scanNumericContiguous`: 素の数値は AVX2（実行時検出）/
  SSE2 で 4/2 要素ずつ比較し、NaN ボックス（タグ付き＝文字列・真偽値
  など）を含むブロックだけスカラで `sameValueZero || compareEqual` に
  フォールバックする。NaN は必ず unordered になるので、ベクタ側で
  一致するのは素の数値だけ — 意味論は従来と同一（`-0`/`0` は値比較、
  NaN ニードルは不一致、文字列⇔数値の Scratch 一致も保持）。
  効果: `indexof` 2.5 倍、`contains` 4.8 倍。

結果（既定経路）: `list_search` 0.04 → **0.14**（Mistwarp 比 3.5 倍改善）。
MLIR 経路では `list_search` が **Mistwarp の 1.5 倍**（巻き上げ＋SIMD）。
スイート全体では中央値 **1.53×**・36/47 勝。

検証: xcheck 32/32、unit **8387 checks / 0 failures**（SIMD の境界・
タグ付き混在・-0・NaN・空リスト・真偽値を追加）、差分ハーネス 19/19
（Mega/HotSpot/interpreter の 3 経路）。

## 35. round 34: for_each の畳み込みと、if 系ループの残差

「MLIR 単体で Mistwarp に勝つ」ための追加作業。

- **for_each の定数畳み込み**: `xyo.for_each` も定数ループ畳み込みの
  対象にした（回数が定数・本体が解釈可能なら、添字変数を各反復の定数と
  して本体を解釈実行）。`loops_foreach` は Mistwarp 比 0.04 → **0.96**
  （legacy 比 0.00004 → 0.96）。あわせて for_each の添字変数を
  「読み取り未知」から除外する扱いを追加。
- **join を解釈可能に**: 文字列連結も実行時ヘルパ（`xyo_join`）で
  コンパイル時評価できるようにした（結果 64KiB 超は畳まない）。ただし
  現状は「最終値が数値/真偽値」のときだけ畳む制約があり、文字列を
  持ち回るループ（`str_join`）はまだ畳めていない。

スイート（Mistwarp 比・MLIR 経路）: 中央値 0.61 → **0.71**、勝ち 16 → 17。

残差は **if を含むループ**（`math_compare` 0.07、`math_collatz` 0.08、
`algo_primes` 0.10、`algo_sieve` 0.12、`loops_ifelse` 0.15、`app_life`
0.18）。計測で分かったこと:
- MLIR の昇格ループは 1 反復あたり約 71 命令・IPC 0.6 程度（約 39ns）で
  実行されている。
- 一方 legacy は同条件で 1 反復 1.9ns。同一の計算量でこの差は
  「分岐だらけのコードを LLVM がベクトル化/分岐除去できていない」ことを
  示す（legacy 側は比較が select ベースで分岐が少ない）。
- 比較のスローパス（`xyo_lt` 等の呼び出し）は実行時には一度も通って
  いないことをカウンタで確認済み。コストは「ガード用の分岐と
  フラグ計算」そのもの。

次の一手: `cmp.guarded` と `xyo.if` の lowering を分岐なし（select
ベース）にし、ループ本体を LLVM がベクトル化できる形にする。これが
if 系ループの 20 倍差を埋める本命。

検証: xcheck 32/32、unit は既存 7 件のみ、差分 19/19。

## 36. round 35: if を含む整数ループのコンパイル時評価（MLIR 単体で 0.71 → 0.82）

if 系ループが Mistwarp に 0.07〜0.18 で負けていた件。原因は
「if が候補変数を書くと整数昇格を諦める」ことと、畳み込みが `if` を
解釈できなかったこと。legacy は `rep.region` で整数化＋select 化して
いた（IR で確認: `icmp`/`urem`/`select` のみで分岐なし）。

- **畳み込みに `if` 対応**: `xyo.if` を解釈実行できるようにした
  （条件を評価して該当領域だけを走らせる。`scanInterpBody` も if 領域を
  再帰的に検査）。定数入口・定数回数・純整数のループは**丸ごと定数に
  畳まれる**。
  効果（単体）: `math_compare` 519 → **3,295,706**（Mistwarp 比 9.1×）、
  `loops_ifelse` 1,072 → **3,263,155**（同 5.4×）。
- **整数昇格の if 対応（select 化）**: if 領域が「候補への単純代入のみ」
  （純粋 op・候補の getvar/setvar だけ）なら、両分岐を SSA で作って
  `select` で統合する。区間解析は両分岐の union を取る（条件付き書き込み
  を片側の値で狭めない）。mod の sub-chain は if を含む本体では無効化
  （srem は常に正しい）。

スイート（Mistwarp 比・MLIR 経路）: 中央値 0.71 → **0.82**、
勝ち 17 → **20/47**、既存経路より速いベンチ 12 → 15。

残差: `math_collatz` 0.08（データ依存ループ・i32 範囲外）、
`algo_primes` 0.09 / `algo_sieve` 0.12（リストを含む整数ループ）、
`app_life` 0.17 / `math_mandel` 0.17 / `app_raycast` 0.18 /
`str_join` 0.19（文字列の持ち回り）/ `app_brainfuck` 0.24。
次の候補: (1) 整数昇格の i64 対応（collatz 系）、(2) 文字列値を
持ち回るループの畳み込み（SStr の pin を pass から frontend へ渡す）。

検証: xcheck 32/32、差分 19/19（3 経路）、unit は既存 7 件のみ。

## 37. round 36: 文字列の畳み込みと倍精度 if の select 化

- **文字列を持ち回るループの畳み込み**: pass がコンパイル時に作った SStr を
  `Module::pinnedStrings` に積み、frontend が JitPinned へ移して GC から
  守る。畳み込み結果が文字列でも定数として焼き込めるようになった
  （連結の総量に上限を設け、コンパイル時間の暴走を防ぐ）。
  効果: `str_join` 210 → **3,298,870**（Mistwarp 比 15×）。
- **倍精度ループの if も select 化**: 2 領域が「候補への単純代入のみ」
  なら (bits, d) の両方を select で統合する。生成 op は一時配列に溜め、
  失敗時は従来の region 形式へフォールバックする（IR を汚さない）。

スイート（Mistwarp 比・MLIR 経路）: 中央値 **0.86**、勝ち **21/47**、
既存経路より速いベンチ 14〜17 本（実行ごとに ±3 程度の揺れ）。
倍精度 if の select 化は**計測上の改善が無かった**（`math_mandel` 0.17 前後）。

残差（MLIR が負けている本命）: `math_collatz` 0.08（データ依存ループで
区間が証明できない）、`algo_primes` 0.10 / `algo_sieve` 0.12（リストを
含む整数ループ）、`app_life` 0.17 / `math_mandel` 0.17 / `app_raycast`
0.19（浮動小数ループ）、`app_brainfuck` 0.24 / `app_projection` 0.25。

次の候補: (1) リスト状態をモデル化した畳み込み（sieve/primes 用）、
(2) データ依存ループ向けの「実行時ガード付き整数化」（legacy 相当）。

検証: xcheck 32/32、差分 19/19（3 経路）、unit は既存 7 件のみ。

## 38. round 37: 部分整数化・ループ上界・64 反復チャンクの拡張

MLIR 単独で Mistwarp の中央値を超えるためのまとめ。個別の最適化を
「証明器」「lowering」「パス」の 3 層に分けて入れた。

### 証明器（passes.cpp）

- **部分整数化**: `analyzeIntLoop` を全変数 all-or-nothing から
  「整数で持ち回れる変数だけを選ぶ」方式へ。書き込みが証明できない
  変数は落とし、残りだけで整数ループを続行する。落とした変数は従来
  どおり (bits, f64) の対で持ち回る。`xyo.warp_yield` には変数ごとの
  運搬表現 `kinds`（`i`/`d`/`b`）を付け、lowering が個数と型を
  解釈する。while の iter_arg、`xyo.condition`、`xyo.yield`、
  `xyo.if` の結果はすべて「変数ごとの 1 個 or 2 個」を走査オフセットで
  扱う（混合ループで添字がずれるバグを修正）。
- **ループ上界による区間の締め**: `repeat N`（定数）は
  「書き込みが定数代入 or 自分自身 + 定数だけ・入れ子ループ内に
  書き込みなし」の変数について、head 区間を `entry + N*Σdelta` で
  上から押さえる。`until`/`while` は条件が `変数 (>,<) 定数` の形なら
  bnot の偶奇から head の片側界を取る。どちらも音な上界なので
  交差してよく、これがないと `i = i + 1` の評価が i32 を溢れて
  変数ごと落ちていた。
- **整数で閉じた op の記録**: 書き換えは前から順に op 名を `.i` へ
  変えるため、後続の op から見るとオペランドが未知 op になる。
  最終区間で本体の各式を評価して `intProvenOps`（op ポインタ）を
  記録し、`intOkOf` がそれを参照する。演算の書き換えには
  「結果も i32 に収まる」条件を追加（部分式の溢れを防ぐ）。
- `xyo.condition` を走査の許可リストへ（これが無いと while の昇格が
  一度も走らなかった）、`xyo.join` を昇格ループ内で許可。
- 整数 if の select 化に失敗したとき、空 Value を伝播させて IR を
  汚すのをやめ、生成した op を捨てて状態を戻し汎用の region 形式へ
  フォールバックする。

### lowering（lower_llvm.cpp）

- **整数索引のリストアクセス** `xyo.list.item.i` / `.replace.i`:
  i32 の `1..len` 比較だけで直接読み書きし、浮動小数の範囲・整数
  検査と fptosi を省く。範囲外・非平坦ストレージは helper へ。
- **平坦ストレージの extra 検査除去**: `xyo_list_storage_mode <= 3`
  はコンパイル時に確定するので、`extra` のロードと分岐を出さない。
- **数値定数のコンパイル時評価**: `numberShadow` が `xyo.const` を
  見たら（数値リテラルは文字列 Value で来る）その場で `xyo_to_num`
  する。毎反復の helper 呼び出しが消える。`from_int` は sitofp だけで
  数値影を作る。
- **mathop の特殊化**: `abs`/`floor`/`ceil`/`sqrt` は LLVM の
  intrinsic、`sin`/`cos` は legacy と同じ「libm 呼び出し + 1e10 丸め」
  のインライン展開、それ以外は op 専用 helper（`xyo_math_ln` 等）。
  `round` は jsRound と同値の分岐なし列（floor(x+0.5) + 0<x<0.5 →
  +0 + -0.5<=x<0 → -0 + x==0 → x）へ。
- **64 反復チャンクの対象拡張**: 昇格後の整数/倍精度 SSA
  （`.i`/`.f` op、`cmp.i`、`mathop.f`、`round.f` 等）を許可。
  呼び出しを含まない数値ループが 64 コピーされ、LLVM がチャンク間で
  最適化できる（`math_round` は legacy 比 4.2×）。

### 結果（scratch-bench performance・Mistwarp 比・MLIR 経路）

| 指標 | round 36 | round 37 |
|---|---|---|
| 中央値 | 0.90 | **1.06〜1.10** |
| 勝ち | 21/47 | **25/47** |
| `math_round` | 0.575 | **1.126** |
| `math_trig` | 0.909 | **1.375** |
| `math_expsqrt` | 0.926 | **1.098** |
| `str_cast` | 0.793 | 0.943 |
| `app_ast` | 0.846 | 0.872 |
| `list_read` | 0.303 | 0.328 |
| `algo_sieve` / `algo_primes` | 0.158 / 0.084 | 0.187 / 0.119 |
| `math_collatz` | 0.078 | 0.133 |

起動時間は 390ms → 402ms（チャンク化で IR は増えるが実害なし）。
残差はスケジューラ系（`sched_*`）、リスト中心（`list_replace`/
`algo_matmul`/`app_ast`）、`math_collatz`（データ依存ループ）。

検証: xcheck 32/32、差分 19/19（3 経路）、unit は既存 2 件
（`mlir: spills boxed value...` / `mlir: unsupported script fell back...`）
のみ。

### 38.1 実装したが採らなかったもの（計測で判断）

- **リストヘッダ（items/len）のループ不変キャッシュ**: 平坦ストレージ
  （`xyo_list_storage_mode <= 3`）では `extra` が存在せず、リストを
  伸縮しない `repeat` なら items/len は反復中不変。前置で読み、yield
  後に読み直す仕組みを入れた（alloca 経由なので mem2reg で phi に
  昇格する）。**list_read / list_replace / list_search の A/B で
  有意差なし**（±5% のノイズ内、list_search はむしろ微減の回も）。
  yield ごとの読み直しがホットループの最適化を妨げるため、既定 off の
  opt-in（`XYO_MLIR_LISTHDR=1`）として残した。コードは
  `LoopScope::headers` / `findListHeader` / `readListHeader`。
- **平坦ストレージでの extra 検査除去**: `extra` のロードと分岐を
  消したが、単体の効果はノイズ内。分岐が 1 個減るだけなので、
  そのまま残す（害はない）。

## 39. round 38: 整数書き込み判定の緩和（20 ループが整数化）

前ラウンドまで、整数化できるループはスイート全体で 3 個しかなかった。
原因は「その変数の非リテラル書き込みが全部このループ内にある」ことを
要求する検査（`intNonLiteralWrites` とループ内訳の一致）。ベンチの
カウンタ変数（`k_i` など）は複数のループで `+= 1` されるため、ほぼ常に
不一致になり、整数化が拒否されていた。

- **整数式の判定を導入**（`frontend.cpp` の `blockIntExpr` /
  `passes.cpp` の `intExprProvable`）: リテラル整数・四則・mod・round・
  `for_each` の添字・整数デルタの change・整数化対象変数の読みは
  「整数を生成する書き込み」として数えない。数えるのは
  「非整数になり得る書き込み」だけにした。
- これによりスイートの整数ループは **3 → 20** に増え、
  `list_read`（0.32→0.35）、`list_replace`（0.39→0.44）、
  `app_projection`（0.34→0.40）、`algo_matmul` などが改善した。
- **証明セットの鮮度バグを修正**: 書き換えで op 名が `box.f` に変わった
  後も `intProvenOps`（op ポインタ）に残っていたため、後続の比較が
  「整数証明済み」と誤認して空オペランドの `cmp.i` を作り、スクリプト
  全体が legacy へフォールバックしていた。現在の名前が整数 op のとき
  だけ証明セットを信用するようにした（`intNamed`）。`xyo.from_int` は
  `iOf` でオペランドをそのまま返す。
- **d-only 輸送の撤去**: `XYO_MLIR_DONLY=1` の「ビット影を持ち回らない」
  モードは実測で中性（mandel -6%、sort -9%、life +8%）で、
  大きなプロジェクト（brainfuck, 185 ブロック）ではコンパイルが
  2 分以上かかる（実質ハング）ため撤去した。

スイート（Mistwarp 比・MLIR 経路）: 中央値 **1.11 → 1.25〜1.27**、
勝ち 25/47。整数ループ数 20。検証: xcheck 32/32、差分 19/19
（`XYO_MLIR=1` でも 3 経路一致）、unit は既存 7 件のみ。

## 40. round 39: 1 文字比較のインライン化（letter of ... == "c"）

`str_letter`（0.334）と `app_brainfuck`（0.254）は「文字列から 1 文字
取り出して定数と比較」がホットループの中心。`xyo.letterof` は 1 文字
SStr のキャッシュを作り、`xyo_eq` は文字列比較にもう 1 回ヘルパを
呼んでいた。

- **`xyo_letter_eq(idx, str, ch)`** を追加（`rt_blocks.cpp`）。位置の
  解釈は `rt::letterOf` と同一（`toNumber(idx) - 1`、NaN→0、範囲外は
  不一致）で、1 文字文字列を作らずにコード単位を比較する。
- lowering の `xyo.eq` と `xyo.cmp.guarded`（pred=eq）で、
  「`letter of` の結果 vs 文字列定数（長さ 1）」を見つけたらこの
  ヘルパへ落とす。定数側は `SStr` を直接読み、コード単位を即値で渡す。

効果: `str_letter` 0.334 → **0.382**、`app_brainfuck` 0.254 → **0.317**。
スイート中央値 1.24〜1.27（勝ち 25/47）を維持。検証: xcheck 32/32、
差分 19/19（`XYO_MLIR=1` でも一致）、unit は既存 7 件のみ。

## 41. round 40: if の substack が代入する変数だけを結果にする

汎用の `xyo.if` 経路は、両領域の `xyo.yield` に**全昇格変数**の
(bits, d) 対（整数なら i32）を載せ、if の結果も全変数分作っていた。
ループ内に if が 8 個ある `app_raycast` のようなコードでは、if 1 個に
つき約 2N 個の phi と分岐が増え、レジスタ圧と命令数が跳ね上がる。

- 両方の substack の `setvar` を**先に**走査して代入先だけを集め、
  その変数だけを if の結果（と yield のオペランド）にする。
- 実装中に踏んだ罠: 領域ごとに代入先を集めると、先に書き換える領域の
  yield 個数が後から増えて `PHINode should have one entry for each
  predecessor` になる。両領域を先に集めてから書き換えること。
- `ifAssigned_` は入れ子の if で保存・復元する（子が nullptr に戻すと
  親の yield が全変数に戻って phi と不一致になる）。

効果: `app_brainfuck` 0.254 → **0.325**、`app_life` 0.221 → **0.231**、
`app_projection` 0.409 → **0.421**。スイート中央値 1.24、勝ち 25/47。
検証: xcheck 32/32、差分 19/19（`XYO_MLIR=1` でも一致）、unit は
既存の失敗のみ。

## 42. round 41: 整数化の健全性（初期値）と if 結果の最小化

### 整数化の前提を初期値まで広げる

整数化の可否判定は「プロジェクト全体の書き込みが整数を生成するか」で
決めている（round 38）。しかし変数の**初期値**が "3.5" や "abc" の
場合、他スクリプトの `change by 1` が非整数を作り得るため、yield 後の
`to_int` が切り捨てを起こす。`computeIntEligibility` で
`intLiteralBits(var.value.bits)` を通らない初期値（非整数・非正準文字列・
空文字列）の変数を対象外にした。ベンチの変数は "0"（正準形）なので
整数ループ数は 20 のまま。

### if の結果を代入先だけに（round 40）の後始末

- 領域ごとに代入先を集めると phi の個数が後から増えて壊れるため、
  両領域を先に走査するよう修正（`PHINode should have one entry ...` の
  再発防止）。
- `ifAssigned_` の入れ子保存・復元。

スイート: 中央値 **1.31**、勝ち 25/47。検証: xcheck 32/32、差分 19/19
（`XYO_MLIR=1` でも一致）、unit は既存 7 件のみ。

## 43. round 42: 変数・リストの「常に数値」型推論

`toNumber(v)` は素の double なら bitcast だけで済むが、文字列なら
パースが必要。どの変数・リストが常に数値かをコンパイル時に証明できれば、
タグ検査の分岐とパース呼び出しを消せる。

- **型推論**（`frontend.cpp` の `computeNumericTypes`）: 変数とリストに
  ついて「常に素の数値」を不動点で求める。
  - 初期値が素の数値（数値文字列・bool・空文字は不可）
  - 書き込みがすべて数値式: 算術/比較系の演算子は Scratch では常に
    数値を返すのでオペランド型を問わない。変数読みは数値と証明済みの
    変数のときだけ数値。数値ブロックの field が文字列（"0"）なら不可。
  - リストは add/insert/replace の ITEM が数値式であること。
  - スライダ監視・cloud は除外。compat 経路（未知 opcode）がある
    プロジェクトでは保守的に何もマークしない。
- `Module::numericVars/numericLists` に保存し、`list.ptr` op に
  `numeric = 1` を付ける（IR 構築後に実行 — 構築前に走らせても op が
  無いので印が付かない。実際に踏んだ）。
- **低減 1**: `numberShadow` が数値変数の `getvar` を見たら
  `bitcast + select(uno, 0)` を返す（分岐もパースも無し）。
- **低減 2**: `to_num(list.item(.i)(...))` が数値リストの読みで、
  その item の use が 1 個だけなら `xyo.list.item.num(.i)` に融合し、
  読みを 1 回のままタグ検査だけ消す（fast: bitcast+select /
  slow: helper + to_num）。

効果は中立〜わずかに正（sort +5%、sieve +2%、raycast 同等）。
スイート中央値は連続 3 回で 1.040 / 1.086 / 1.127（中央値 1.086、
勝ち 24/47）。単発では 1.31 の回もあれば 0.97 の回もあり、マシン負荷で
±10% 揺れる。負荷時に 0.87 を記録したが、同一バイナリの再実行で
1.19 に戻ることを確認している。

スイート（連続 3 回）: 1.040 / 1.086 / 1.127（中央値 **1.086**、
勝ち 24/47）。検証: xcheck 32/32、差分 19/19（`XYO_MLIR=1` でも一致）、
unit は既存 7 件のみ。

## 44. round 43: Value 算術の数値オペランド昇格（測定は中立）

`xyo.mul(a, b)` のような Value 算術は helper（`xyo_mul`）呼び出しで
toNumber を 2 回行う。オペランドが数値と証明できていれば
`mul.f` + `box.f` に落とせる。

- パス `promoteNumericArith`（`runStandardPipeline` の昇格後）:
  - オペランドを f64 に変換できるときだけ
    `xyo.add/sub/mul/div/mod` → `<name>.f` + `box.f` に書き換える。
  - `xyo.cmp.guarded` は両辺が f64 化できるなら `xyo.cmp.f` に
    （タグ検査の分岐が消える）。
  - f64 化できるのは: f64 型の値 / 数値変数の `getvar`（数値影の
    分岐なし経路へ）/ 数値リストの `list.item`（use が 1 個なら
    `list.item.num` へ融合）/ 数値定数 / `box.f` / `to_num`。
  - 変換できなければ**その op は触らない**（部分適用のロールバック付き）。
- 実測は中立（sort・sieve・life・raycast・brainfuck・matmul・primes・
  collatz・mandel のいずれも ±3% 以内）。これらのホットループは既に
  昇格済みで、Value 算術が残るのは非ホット部だけだったため。
  コードは残す（非昇格の数値ループでは helper 呼び出しが消える）。
- **リストループのチャンク化は不採用**: リスト読み書きを 64 コピー
  チャンクの許可リストに入れて A/B したところ、`list_replace` が
  1388/1446 → 1341/1296（チャンク無しが速い）、matmul は同等だった。
  コード肥大の分だけ損なので許可リストは元に戻した。

スイート（連続 3 回）: 1.230 / 1.142 / 1.126（中央値 **1.142**、
勝ち 24〜25/47）。検証: xcheck 32/32、差分 19/19（`XYO_MLIR=1` でも
一致）、unit は既存 7 件のみ。

## 45. round 44: ループ条件の and/or 木からの区間絞り込み

`condBound` は `bnot` を剥がした 1 個の比較しか見ていなかった。
`until k_n >= 50 || mag > 4` のような条件（IR では
`condition(bnot(or(ge, gt)))`）では head の区間が絞れず、`k_n` の
整数化が拒否されていた。

- 極性付きの再帰に一般化した:
  - `not(x)` は極性反転
  - `and(a,b)` が真 / `or(a,b)` が偽 → 両方の子を同じ極性で降りる
  - `and` が偽 / `or` が真 → 片方だけの制約は不明なので諦める
  - 比較（`gt`/`lt`）は極性に応じて片側区間に反映
- これで mandel の内側ループは head で `k_n <= 49`、`mag <= 4` が
  分かるようになり、`k_n` が整数化される。

単体カーネル: mandel 4373 → **3551/3692**、life 840 → **757/699**、
raycast 3750 → **3123/3077**（いずれも約 15〜18% 改善。ノイズ ±10% を
考慮しても改善方向）。整数化ループ数は 20 のまま（変わるのは区間の
精度）。

スイートと検証の結果は下記。
スイート（連続 3 回）: 1.123 / 1.062 / 1.088（中央値 **1.088**、勝ち 24/47）。
検証: xcheck 32/32、差分 19/19（`XYO_MLIR=1` でも一致）、unit は既存 7 件のみ、
MLIR フォールバックなし。

## 46. round 21: while ループの静的 warp・数値リテラルの数値化・比較ガードの畳み込み

スイートの負け組（primes 0.12 / collatz 0.13 / sieve 0.18）を追ったら、
ホットループの**入口で毎反復 spill/reload と to_num が走っていた**。
原因は 3 つとも独立だった。

### (a) while 系ループを静的 warp にした
`control_repeat_until` / `control_while` は `warp` 属性を付けていな
かった（`scratch_build.cpp` のコメント: 脱出値が cond の phi から来る
ため）。しかし promotion 側は `runWhile` が cond/body の両方に iter_arg
を足し、脱出値は cond の phi（= body の yield の forward）から取るので
書き戻しは正しい。属性が無いと (1) `warpLoop=false` で**毎反復
`setvar` spill + `getvar`/`to_num` reload**、(2) 64 反復チャンク化も
無効、になっていた。

- フロントは warp 手続き内の全ループ種別に `warp=1` を付ける
  （`XYO_MLIR_WHILEWARP=0` で従来動作）。
- `runWhile` でも `warpLoop = loop->attrInt("warp") != 0` を設定する
  （`run()` の早期 return で設定されていなかった）。
- これで body は `xyo.warp_yield` になり、spill/reload は
  `xyo_warp_yield_due` が真のとき（64 反復に 1 回で、かつ実際に譲る
  とき）だけになる。意味論は従来と同じ（ハンドオフ時にメモリを更新し、
  戻りで読み直す）。

### (b) 数値リテラルの shadow を数値として持つ
`math_number` などの NUM フィールドは project.json では文字列なので、
`xyo.const` に**文字列のまま**入っていた。すると
- `xyo.add` などが `promoteNumericArith` の `isNumber()` 判定に落ち、
  毎回 `xyo_add` ヘルパ（文字列→数値変換込み）呼び出しになる、
- `xyo.cmp.guarded` の高速側タグ検査が定数でも偽になり、常に
  `xyo_eq`/`xyo_gt` ヘルパへ落ちる、
という 2 重のペナルティがあった。Scratch もブロック値は
`Cast.toNumber` してから使うので、**数値に変換できる shadow は
`const.num` として持つ**のが正しい（`isNumericShadow` の opcode のみ、
非数値文字列はそのまま）。

### (c) `cmp.guarded` のタグ検査を静的に畳む
`cmp.guarded(bitsA, bitsB, dA, dB)` は「両方が素の double かつ d が
非 NaN」のときだけ fcmp で比較し、そうでなければ文字列比較ヘルパへ
落ちる。ビット側が
- `xyo.from_int`（常に正準な数値）
- `xyo.box.f`（NaN のときだけ NaN ビット。d 側の `fcmp ord` が同じことを見る）
- 数値の `xyo.const`
ならタグ検査は不要なので `true` に畳む。これで `k_n == 0` のような
比較から分岐とヘルパ呼び出しが丸ごと消える。

### 実測（隔離カーネル、b_n = 100ms 予算内の反復数、legacy 比）
| kernel | before | after |
|---|---|---|
| algo_primes | 0.204 | **0.596** |
| algo_sieve | 0.340 | **0.514** |
| algo_matmul | 0.402 | 0.391 |
| math_collatz | 0.083 | **0.118** |
| list_read | — | 0.556 |
| list_replace | — | 0.221 |

スイート（連続 3 回）: 中央値 **1.120**（勝ち 25/47）。
主な改善: algo_primes 0.12 → 0.292、algo_sieve 0.18 → 0.228、
math_collatz 0.13 → 0.156。
検証: xcheck 32/32、差分 19/19（`XYO_MLIR=1` でも一致）、unit は既存
7 件のみ、フォールバックなし。

### 分かったこと（次の一手）
primes / sieve / matmul の内側ループは依然として整数化されない。
`scanIntConsts` は「同じブロックを後方に走査して直近の定数代入」しか
見ないため、**外側ループの本体で初期化される変数**（sieve の
`k_i = 2` は `repeat 4999` の外、`k_j = k_i*k_i` は preheader）は
定数入口にならず、`analyzeIntLoop` が丸ごと諦める。sieve の int デバッグ:

```
[mlir-int] no const entry loop=xyo.while cands=[k_g k_i k_j] consts=[]
```

ここを開けるには「関数全体の整数区間解析」（各ループ本体を反復回数付きで
抽象解釈し、変数ごとに常時整数か・区間は何かを不動点で求める）が要る。
個別パッチでは届かない。

## 47. round 22: 整数 op の「改名前」判定ミス（intNamed）と数値リテラルの一本化

round 21 の続き。`k_i * 7919 % 100000 + 1` のインデックスが
`mod.f` → `list.replace`（Value 版）に落ちていた原因は 2 つ。

### (a) `intNamed` が改名前の整数 op を拒否していた
`intProvenOps`（整数と証明できた op の集合）を引くとき、op 名が
`.i` で終わること等を要求していた。stale 対策（`box.f` へ改名された
op を信用しない）として入れたものだが、**まだ改名していない現在の
op**（この時点では `xyo.mod`）も弾いてしまい、フォールバックの
`evalIntValue` は既に `.i` へ改名済みのオペランド（`xyo.mul.i`）を
評価できずに失敗 → mod だけ f64 に落ちていた。

- 受け入れる名前に `xyo.add/sub/mul/mod`（改名前の整数 op）を追加。
  集合には「最終区間で整数と評価できた op」しか入らないので安全で、
  改名済みの `box.f` / `*.f` は従来どおり弾く。

### (b) 数値リテラルの変換を一本化
`emitInputValue`（inline shadow）と `emitReporter`（shadow ブロック
経由）でリテラルの扱いが違い、後者は文字列定数のままだった。
`numericLiteralValue()` に一本化し、数値 shadow はどちらの経路でも
数値定数にする。

### 実測（隔離カーネル、legacy 比）
| kernel | before | after |
|---|---|---|
| list_replace | 0.236 | **0.509** |
| list_read | 0.558 | **0.667** |
| algo_primes | 0.554 | 0.530（横ばい） |
| algo_sieve | 0.534 | 0.498（横ばい） |

出た IR: `xyo.mod.i {subk=-2}` + `xyo.list.replace.i(ptr, i32, value)`。

スイート（連続 3 回）: 中央値 **1.261**、勝ち 27/47
（list_read 0.28 → **1.11**、list_replace 0.345 → **1.29**）。

検証: xcheck 32/32、差分 19/19（`XYO_MLIR=1` でも一致）、unit は既存
7 件のみ。

### 残りの負け組と、その理由（実測ベース）
- `math_collatz` 0.165 / `algo_sieve` 0.244 / `algo_primes` 0.301 /
  `algo_sort` 0.298 / `algo_matmul` 0.465: **入れ子ループの外側**が
  `scanBlock` の region op 拒否で解析されず、内側ループの候補変数の
  入口定数も preheader に無い。sort の `k_j` は繰り返し回数
  `99 - k_i` が非定数なので区間が伸び、i32 で頭打ちにした後
  `+1` で溢れて落ちる（`all int vars dropped`）。
- `sched_*` 0.25〜0.76: スレッド/ファイバのチャーンが主体で、
  MLIR の算術とは別レイヤ。
- `str_letter` 0.316 / `app_brainfuck` 0.327: 文字列ヘルパ呼び出しが
  1 反復に 1 回（`xyo_letter_eq` 等）。索引が i32 になったので
  次はヘルパの i32 版（`SStr::unitAt` 直呼び）が候補。

次の大きな一手は依然として「関数全体の整数区間解析」で、
(a) 入れ子ループを含むループの外側変数の解析、(b) 繰り返し回数が
非定数でも親カウンタの区間から上界を出す、の 2 点が要る。

## 48. round 23: 2 の冪の除算・剰余から fmod/div を消す

`xyo.mod.f` は `frem` を出していた。x86-64 では `frem` は libm の
`fmod` 呼び出しに展開される（x87 の FPREM は使われない）ので、ループ内
の 1 回の `a % 2` が 20〜40 サイクルになる。除数が定数で **2 の冪**なら
IEEE で厳密に等価な式に置き換えられる:

- `x / 2^k` → `x * 2^-k`（`1/c` がちょうど表現でき、丸めが起きない）
- `x % 2^k`（|c| >= 1）→ `x - c * trunc(x * 2^-k)`
  - `|c| >= 1` を要求するのは `x * (1/c)` が溢れないため
    （`c = 0.5` だと `1e308 * 2 = inf` になり fmod の結果と食い違う）
  - `trunc` は `llvm.trunc`、積と差は 2 の冪のスケーリングなので丸めなし
  - Inf/NaN/-0 は fmod と同じ結果になる（Inf→NaN、NaN→NaN、-0 保持）

`xyo.mod.i` は元から `srem` なので対象外。定数除数でも 2 の冪でない
場合（`% 100000` など）は従来どおり `frem`（ただし整数化されれば
`srem` になる）。

### 実測
- 隔離カーネル: collatz 0.121 → **0.130**、sieve/primes/matmul/mandel は
  横ばい（除数が変数か、既に整数化済み）。`math_round` 4.6x、
  `math_trig` 0.985、`math_nbody` 1.03 と退行なし。
- 生成 IR から `frem` が消え `llvm.trunc` + `fmul`/`fsub` になることを
  `XYO_MLIR_LLVM` で確認。

### 分かったこと（collatz の残差について）
固定回数の collatz（6.28M ステップ）を legacy と MLIR で壁時計比較すると
**ほぼ同じ**（どちらも約 100ms、`k_acc` 一致）。一方、時間予算ループ版では
legacy 24.4M ステップ / MLIR 9.68M ステップ（同一 100ms 予算）で 2.5x 差。
つまり差は 1 ステップあたりのコスト（legacy 約 14 サイクル、MLIR 約 36
サイクル）で、内訳は `(bits, d)` の二重運搬・書き込みごとの `box.f`・
`cmp.guarded` のタグ検査分岐・while 頭の条件再評価・yield カウンタ。

削減の第一候補は **d-only 運搬**（`kinds="d"`）。lowering 側は実装済みで
「実際に譲る 1 回だけ `box.f` でビットを作る」ようになっているが、
promoter が `"b"` しか出していない（`m.numericVars` の変数で `"d"` を
出す＋`c.bits` を遅延生成すれば有効）。前回の d-only 実験（brainfuck で
ハング）は「全部 d」にした版なので、`numericVars` 限定で再挑戦する価値が
ある。

### スイート測定の注意（同一セッション参照の必要性）
MistWarp の参照 JSON は別時刻の測定なので、負荷が変わると全ベンチが
一様に動く（今回 r22 1.261 → r23 1.528 と、ほぼ全項目が 1.2〜1.5x
動いた。iso の交互 A/B では collatz 以外ほぼ横ばいなので、これは負荷差）。
**同一セッションで MistWarp を測り直して比べる**のが正しい:

- `--runtime mistwarp` を今のマシンで 1 回実行し、その中央値と比較。
- 今回の結果: 中央値 **1.424**、勝ち 30/47
  （collatz 0.198 / sieve 0.260 / sort 0.358 / primes 0.373 /
  life 0.315 / brainfuck 0.373）。
- コード変更の効果そのものは iso の交互 A/B（負荷非依存）で判断する。

## 49. round 24: d-only 運搬（kinds="d"）と数値変数判定の数値文字列対応

### (a) 数値変数の判定が数値文字列リテラルを落としていた
`computeNumericTypes` の `blockNumericExpr` は `math_number` の NUM が
**素の数値**のときだけ数値式と見なしていたが、project.json の NUM は
文字列（"1"）なので、`k_acc += 1` のような書き込みは数値式と認識されず、
ほとんどの変数が `numericVars` に入っていなかった。フロントは数値
リテラルを数値 const に変換するようになった（round 21）ので、判定側も
「数値として読める文字列」を数値式として扱う。

これで `numericVars` が本来の大きさになり、既存の数値専用最適化
（`getvar` の分岐なし数値影、`list.item.num`、`cmp.guarded`→`cmp.f`）が
効くようになった。実測: mandel 1.057 → **1.186**、trig 0.985 → **1.168**、
primes 0.540 → **0.592**。

### (b) d-only 運搬（`xyo.warp_yield` の `kinds="d"`）
lowering 側は実装済みだった（「実際に譲る 1 回だけ `box.f` でビットを
作り、戻りで `to_num`」）が、promoter が常に `"b"`（bits, f64 の対）を
出していた。`numericVars` の変数（＝常に素の数値）に限り、warp ループの
iter_arg を **f64 1 本**にする:

- `planKinds()` が変数ごとに `i`/`d`/`b` を決める（`i`=整数、`d`=数値専用、
  `b`=従来）。
- `xyo.warp_yield` / `xyo.condition` / `xyo.yield` / `xyo.if` の
  resultTypes・operand 個数を種別で揃える（ここがずれると phi 個数が
  食い違い、最悪ループが回り続ける — 実際に踏んだ）。
- ビットが必要になった時点で `bitsOf()` が `box.f(d)` を作る（遅延生成）。
- `d` は **warp ループ限定**。非 warp は毎反復メモリへ spill するので
  ビットが必要（条件を入れ忘れると verifier が
  `operand is empty for xyo.setvar` で落ちる — xcheck 2 件で検出）。
- 無効化は `XYO_MLIR_DONLY=0`。

d-only 自体の効果は A/B（`XYO_MLIR_DONLY=0/1`、同一カーネル交互 3 回）で
**中立〜わずかに正**（primes +8%、collatz/sieve/mandel は ±5% 以内）。
ビットの box.f は LLVM がループ外へ掃き出せないため、構造としては
d-only が正しい（handoff 時だけ materialize）。

検証: xcheck 32/32、差分 19/19、unit 既存 7 件のみ、全 iso カーネルで
MLIR フォールバック 0。

スイート（同一セッションで MistWarp を測り直して比較、3 回の中央値）:
**1.434**、勝ち 29/47（round 23 は 1.424 / 30）。スイート全体では横ばいで、
効果は iso カーネル単位（mandel/trig/primes）に現れる規模。

## 50. round 25: cooperativeOnly（予算 watchdog だけの yield）

legacy JIT は `CompiledScript::cooperativeOnly = !semantic(f)` を立て、
「warp ループの予算チェックでしか譲らないスクリプト」を**同期バッチに
参加させてよい**とスケジューラへ伝える。MLIR 側は常に false だったため、
warp ループを含む（＝ほぼ全てのベンチカーネルの）スクリプトが同期バッチ
から外れていた。

- `LoweredFunction::voluntaryYield` を追加し、**自発的に譲る**箇所
  （非 warp ループの毎反復 yield、`wait until`、待機系）で立てる。
  warp ループの 64 反復チャンク（`emitWarpLoopYield`）、`xyo.warp_yield`、
  ハンドオフ、`all_at_once`、`empty_repeat` は協調のみ。
- フロントで `out.cooperativeOnly = mayYield && !voluntaryYield`。
- `[mlir] emit` のトレースに `coop=` を追加（診断用）。

これで MLIR 経路のスケジューリング方針が legacy と一致する。
（スイート中央値への影響は小さい: 恩恵を受けるのは sched_* 系で、
中央値の位置には届かない。）

### A/B の結論（round 25）
`cooperativeOnly` の有無でスイートを 1 回ずつ比較: sched_* は
0.88〜1.05（ノイズ内）、他は 0.92〜0.99（実行全体がわずかに遅い回）。
単一スクリプトのベンチでは同期バッチが 1 スレッドになり効果が無いため
**測定上は中立**。legacy とのスケジューリング方針の一致自体は価値がある
ので採用し、`XYO_MLIR_NOCOOP=1` で従来動作に戻せるようにした。

## 51. round 26: letter of の整数添字版ヘルパ（toNumber を挟まない）

`xyo.letterof(idx, s) == <1 文字定数>` の peephole は
`xyo_letter_eq(idxBits, s, ch)` を呼ぶが、このヘルパは添字を
`cast::toNumber` で数値化していた。プロモータは整数 SSA の添字を
`from_int` で Value 化して `letterof` に渡すので、lowering 側で
`from_int` を剥がして i32 のまま渡せる。

- `xyo_letter_eq_i(int32 idx, uint64 s, int32 ch)` を追加
  （`idx` は 1 始まり、範囲外は不一致。`toNumber` を挟まないだけ）。
- lowering の peephole（`cmp.guarded` と `xyo.eq` の 2 箇所）で、
  添字オペランドが `from_int` なら中身の i32 を使い、型が i32 のとき
  だけ `_i` 版を呼ぶ。
- 登録は `kRtSymbols` に 1 行（AOT でも解決する）。

実測（iso 交互 A/B、legacy 比）: str_letter **0.336 → 0.389**
（`xyo_letter_eq_i` が生成 IR に 1 回、`xyo_letter_eq` は 0 回）。
str_cast / brainfuck は横ばい（前者は別経路が支配的、後者は反復の
大半がリスト操作）。

検証: xcheck 32/32、差分 19/19、unit 既存 7 件のみ。

## 52. round 27: 54 候補の棚卸し（このセッションの到達点）

優先順（リスト効果分離＋添字一括検査 → 変数単位整数化＋途中脱出 →
非box化f64ループ → リスト定数評価 → 盤面/画素SIMD → 文字列/手続き/
lowering/コンパイル費）に対する現在の状態。

### 採用済み（測定で効果を確認）
| 項目 | 節 | 効果 |
|---|---|---|
| 整数書き込み判定の緩和 | §39 | 整数化ループ 3 → 20 |
| 部分整数化＋ループ上界＋64反復チャンク | §38 | — |
| if 結果の最小化（代入先のみ） | §41/§42 | ループ内 phi 削減 |
| 変数・リストの「常に数値」型推論 | §43/§49 | mandel/trig/primes |
| ループ条件 and/or 木の区間絞り込み | §45 | mandel/life/raycast 15% |
| 1 文字比較のインライン化 | §40/§51 | str_letter 0.336→0.389 |
| while ループの静的 warp | §46 | primes 0.20→0.60 の主因 |
| 数値リテラルの数値化（2 経路の一本化） | §46/§47 | 算術・比較のヘルパ落ちを解消 |
| 比較ガードのタグ検査畳み込み | §46 | 比較の分岐とヘルパ消滅 |
| intNamed（改名前の整数 op） | §47 | list_read 0.28→1.11、list_replace 0.35→1.29 |
| 2 の冪の div/mod から fmod 除去 | §48 | collatz +8%、退行なし |
| d-only 運搬（kinds="d"） | §49 | 中立〜僅正（構造改善） |
| cooperativeOnly（legacy と方針一致） | §50 | スイート中立 |

### 不採用（測定で棄却）
- リストループのチャンク化（list_replace が悪化）§44
- Value 算術の数値オペランド昇格（中立）§44
- d-only の「全部 d」版（brainfuck でハング）§49 の注記
- リストヘッダキャッシュ（中立）— `XYO_MLIR_LISTHDR` で opt-in のみ

### 未着手（次のセッションの本命）
1. **入口ガード付きループ複製**（`変数単位整数化＋途中脱出`）
   - 入口で `isExactInt32(|v|≤2^20)` を検査し、真なら整数化した複製を
     実行、偽なら元ループ。入口だけのガードでよい（値の整数性は
     プロジェクト全体の書き込み形から帰納で保証、範囲は同じ不動点が
     証明する）。**複製は 1 つで済む**（途中脱出が無いため）。
   - 必要な部品: IR の deep clone（op/region/block arg の Value 付け替え）、
     `xyo.int_exact_ok` op とその lowering、promoter への統合。
   - 期待: collatz 0.19 / sieve 0.25 / life 0.28 / matmul 0.34。
2. **入れ子ループの外側解析**（sort 0.34 / matmul の `k_i*16+k_j+1`）
   - `scanBlock` は region op を拒否するため、外側ループが解析されない。
     ネスト内で触られない変数だけを対象に外側を解析し、内側ループの
     入口区間を渡す（2 パス）。
3. 文字列ヘルパの残り（`xyo_letterof` の単体使用、`join` 連鎖）
4. 手続き効果要約・選択的インライン化 / コンパイル費

### 検証ゲート（現状）
xcheck **32/32**、差分 **19/19**（`XYO_MLIR=1`）、unit は既存 7 件のみ、
iso 全カーネルでフォールバック 0、スイート中央値 **1.41〜1.43**
（同一セッションで MistWarp を測り直して比較）。

### 52.1 round 27 の小改善: mod 補正の除算も乗算へ
`xyo.mod.f` の符号補正は `r0 / c` で符号を見ていた。除数が 2 の冪なら
`r0 * (1/c)` で厳密に同じ（IEEE の 2 の冪スケーリング）。collatz の
`% 2` で毎反復 fdiv が 1 個減る。A/B は collatz 0.130 → 0.122（ノイズ内、
fdiv は律速ではなかった）。退行は無く、変換としては正しいので採用。

### 53. round 28: xcheck ケース追加の試み（未採用）と分かったこと

round 21〜27 の変更（2 の冪の除算/剰余・整数添字のリスト操作・
`letter of` の i32 比較・while の静的 warp）を 1 本で踏む xcheck ケースを
書いたが、**MLIR 経路に到達しなかった**ため採用しなかった。

- 内容: warp 手続き内で `y = ((0-x)/2) % 2`、`replace item (x%5+1) of L`、
  `item (x%5+1) of L` → M へ追加、`letter (x%7+1) of "abcdefg" == "c"`。
- 結果: インタプリタとの最終状態は**完全一致**（x=300, y=0, e=43, L, M の
  300 要素すべて同じ）だが、`XYO_MLIR_TRACE=1` に `[mlir]` 行が**1 本も
  出ない**（= MLIR ではなくインタプリタ/legacy が実行）。`emit` も
  `fallback` も出ないので、`jitEmitScript` 自体が呼ばれていない。
- 既存 32 ケースは同じハーネスで `emit` を確認できるので、このブロック
  構成（`data_replaceitemoflist` + `operator_letter_of` + `control_if` の
  組み合わせ、または手続き呼び出し側の形）が JIT 対象から外れる条件に
  触れている可能性が高い。**次セッションの調査項目**（JIT に載らない
  ケースは性能以前に検証できない）。
- ケースは `CASES` から外した（ゲートは 32/32 のまま）。ハーネスには
  body タプルのアリティ検査だけ残した（書き間違いを即エラーにする）。

### 53.1 round 30: §53 の原因を特定（MLIR の問題ではなかった）

§53 の「`emit` も `fallback` も出ない」は MLIR の拒否ではなく、
**`JitEngine::compile` の入口で `ScratchCompilationUnit::capture` が
`invalid_source` を返し、legacy を含む全バックエンドに到達しなかった**
（`XYO_JIT_COMPILE_STATS=1` で `reason=invalid_source count=1`、
`HOT_STATS` は `jit=-`）。`capture`（`src/ir/scratch_pipeline.hpp`）は
入力 edge・shadow・substack・手続き定義を辿り、存在しないブロック id 参照
（`MissingBlock` 等）で fail-closed する。`tryEmitScript` は呼ばれないので
`[mlir]` 行は 1 本も出ない — ハーネスの観測と一致する。

検証: §53 と同じ演算構成（`((0-x)/2)%2`、`replace/item (x%5+1)`、
`letter (x%7+1)=="c"`、warp 手続き `spin(300)`）を xcheck の P クラスで
正規に生成したプロジェクトは **`[mlir] emit hat ... ops=97 promoted=3`**
で正常に MLIR コンパイルされる。逆に、入力タプルを手書きして壊した
プロジェクト（存在しないブロックを参照）は `invalid_source` で
インタプリタ実行になり、`[mlir]` 行なし・最終状態一致という §53 と
同一の署名を示す。つまり前セッションのケースはブロックグラフ自体が
壊れていた（手書きタプルの書き間違い）可能性が高く、JIT ゲーティングの
バグではない。

対応（コード変更なし）:
- ハーネス側の body タプル arity 検査は §53 で既に追加済み。
- `XYO_JIT_COMPILE_STATS=1` の `[jit-refusal] reason=invalid_source` が
  「JIT に載らない」系の切り分けに使えることをここに記録する。
  `invalid_source` = capture 失敗（全バックエンド不達）、
  `fallback ...` あり = MLIR が構築後に辞退（legacy が実行）。

### 53.2 round 30: §53 の決着と副産物（scan 脆弱性の修正）

§53 の再現スクリプトを xcheck の P クラスで正規に作り直したところ、
`[mlir] emit hat ... ops=97 promoted=3` で正常にコンパイルされた。
前セッションのケースは手書きタプルのブロックグラフ破損
（`invalid_source` → 全バックエンド不達）が濃厚で、JIT ゲーティングの
バグではなかった。切り分け手順は §53.1 に記録した。

副産物として見つけた実バグ: `scanIntConstsFrom` が `xyo.list.add` 等の
非 pure op で走査を打ち切るため、リスト fill と変数初期化の順序によって
entry 定数を取り逃がす（`list.add → set x` の順だと x の entry を見失う）。
リスト葉 op（`xyo.list.*`、region なし）はスカラー変数を書かないので
飛ばすようにした。健全（変数の値を変えない）。

## 54. round 30-31: 入口ガード付きループ複製（変数単位整数化）

entry 非定数の変数を含むループを整数化する。入口で
`xyo.int_exact_ok`（正確な整数か。`nonneg` 属性で [0,2^20] も可）を
検査し、真なら整数化した複製・偽なら元ループを実行する
（`xyo.if` で分岐、両分岐の結果は (bits,f64) に揃えて脱出後に共通
書き戻し）。複製は deep clone（`Cloner`: op/region/block 引数の
Value 付け替え）で作る。1 変数でも整数化できれば成功（部分整数化）。

- 対象: `xyo.repeat` と `xyo.while`（`repeat_until` を含む）。
  `runWhile` の cond/body 両領域を複製・配線する。
- 解析: `analyzeIntLoop` に entry 範囲マップを渡す。full ボックス
  `[-2^20,2^20]` で失敗したら非負ボックス `[0,2^20]` で再試行する
  （下側発散で落ちるカウンタ・添字型向け）。`repeatBoundOf` の基点も
  ガード範囲を使う。
- ガードの健全性: (a) プロジェクト全体の非整数書き込み検査
  （`intNonLiteralWrites` との突き合わせ）はガード版でも維持する。
  warp yield の handoff reload（他スクリプトの書き込み）はこの検査が
  守る。(b) 同一関数内の外書き（入口設定等）は singleSpawn の
  スクリプト関数に限り許す（`Module::singleSpawn`、
  green flag のみ true。手続き本体・broadcast/clone 等は除外）。
  同一スレッド逐次実行＋入口毎の再検査で健全。
- `xyo.int_exact_ok` の lowering（タグ・順序・範囲・trunc の 4 条件、
  ヘルパ呼び出しなし）と `isPure` 登録。
- 診断: `PassStats::guardedLoops` を `emit` 行に `guard=N` として出す。
  xcheck の `GUARD_CASES` は `guard=0` を拒否する。
- 無効化: `XYO_MLIR_GUARD=0`。

実測: sieve 内側 until が `int loop: 3 vars` + `guard=1` になった。
sieve-mini（2 秒 budget、b_n 比較、交互 A/B）では guard on/off に
有意差なし（リスト操作が支配的）。sort の `99 - k_i` 形でも k_j が
int 昇格する（`[0,100]`）。

### 54.1 round 31: collatz は区間解析の限界（div＋3n+1 の二重壁）

collatz 内側 until を実測したところ、full/nonneg 両ボックスで失敗する。
原因は 2 つ: (1) `k_n / 2`（偶分岐でも div は区間証明できない）、
(2) `k_n*3+1` が区間を発散させる（負側・正側ともに。`until k_n==1`
は等値で範囲を与えない）。(2) はランキング議論なしには解けないため、
入口ガードだけでは collatz を整数化できない。途中脱出（オーバーフロー
時の float 継続）つきの複製が要るが、継続プロトコルが別規模の設計に
なるため今回は見送り、ここに記録する。

## 55. round 30-31: 証明の一致（clean fixpoint）と verifier 落ち 2 件の修正

### 55.1 frontend の数え方と解析の証明の乖離

`m.intNonLiteralWrites`（frontend のブロック木基準）と promoter の
`intExprProvable`（IR 基準）が変数経由の書き込みで食い違っていた。
例: `y = getvar_x`（frontend は getvar を見て計数、解析は x が active
なら証明可）。食い違いがあると `n != inside` で変数が落ち、ガード版も
成立しない。対策として clean fixpoint を導入した: 全書き込みが
リテラル整数・四則・mod・round・clean 変数読みから成る変数だけが
clean に残る（単調縮小で収束）。同じ基準で数えるため両側が一致する。
clean 変数は初期値整数＋各書き込みが整数保存なので、実行順序・
スレッド交錯によらず整数を保つ（健全性の根拠）。

### 55.2 sort プローブの全スクリプト fallback（実バグ）

整数昇格ループ内で `replace item X of L with <変数>` を使うと、
`list.replace.i` 書き換えが ITEM オペランドの getvar を未変換のまま
残し、getvar は本体から除去されるため dangling use になった。
`verifier failed after passes` で**スクリプト全体が fallback**
（意味は正しいが性能は legacy 並み）。ITEM が整数運搬でないときは
`bitsOf` で影に差し替えるよう修正。同じ不整合（`intOkOf` は entry
定数で ok、`iOf` は非 int 運搬で空）が `cmp.i` にもあったため、
空のときは float へフォールスルーするよう堅牢化した。

回帰ケース `case_int_replace_var_item` を追加（修正前は fallback で
FAIL、修正後は emit で PASS を確認）。

### 55.3 ガード版の verifier 落ち 3 連発（いずれも修正済み）

開発中に踏んだもの。いずれも xcheck 35/35 で回帰検出できる:
(1) then-yield が body 引数（別リージョン）を参照 → ループ結果を
使うよう修正。(2) else 側の `isInt` が生きたまま float 書き換えに
入り、`bitsOf` が空の iv で `from_int` を作る → else 側は `isInt` を
落とす。(3) `commitRepeat` が `this->cands.initBits` を読むがガード版
では未設定 → `gv`（ループ前実メモリ値）を設定。

## 56. round 31-32: 同一 invocation でのスイート再測定（fair-load）

別時刻の実行同士の比較はマシン負荷でぶれる（xyo 単独の再実行で全核
0.77〜1.07 に動いた）ため、`--runtime xyo-cpp,mistwarp` の同一
invocation で測り直した（`results/combined1`、各 `--runs 1`）。
MLIR 経路単独（hotspot、XYO_MLIR=1）の MistWarp 比:

- 中央値 **1.639**、36/47 勝（目標 1.0× 以上を維持）。
- 前回棚卸し（1.41〜1.43、28〜30 勝）からの主な移動:
  sort 0.34→1.39（§55.2 の fallback 修正。swap の replace.i で
  スクリプト全体が fallback していた）、sieve 0.25→0.63（§54 の
  ガード版で内側 until が int 3 vars 化）、brainfuck 0.35→1.92
  （同型の fallback 解消が効いたとみられる）、matmul 0.34→1.27、
  life 0.28→0.95、letter 0.34→1.17、primes 0.37→0.78、
  collatz 0.19→1.79（区間解析は未対応のまま。clean-fixpoint による
  計数一致と周辺ループの昇格改善が効いたとみられるが、要因の切り分けは
  未了。collatz 内側の div/3n+1 は §54.1 のとおり未解決）。
- 依然 1.0 未満: list_search、sched_fanwait/fanout/pingpong/looptick、
  sieve、primes、threads、restart、life、nbody（11 核）。
  sched 系は同期バッチ側の領域。list_search・sieve 残差・life・matmul
  にはリスト要素の整数化（リスト定数評価）が要る。collatz には
  偶分岐の除算 exactness と 3n+1 の発散対策（途中脱出つき複製）が要る。
  いずれも次ラウンドの候補として残す。

検証ゲート（最終）: xcheck **35/35**（ガード版 2 件を guard ピンつきで
追加、replace.i 回帰 1 件を追加）、差分 **19/19**（Mega/HotSpot/interp
の 3 経路）、unit は既存 7 件のみ（e2e 4・scheduler 1・mlir 2）。
iso 全カーネルで MLIR フォールバック 0。

## 57. round 31-32: リスト要素の整数化（リスト定数評価）

`k_acc += L[i]*L[j]` 形（matmul/life/sort の核）を整数化するため、
要素が全て整数と証明できたリストの読みを int として扱う。

- frontend: 初期要素が全て整数リテラルで、全ストアが整数リテラル
  （`add/insert/replace` の ITEM が `math_number` 等）のリストだけ
  `m.intListRanges[id] = [lo,hi]` を持つ（範囲外読みは ""→0 になる
  ので 0 で広げる）。変数読みのストアがあるリストは対象外。
  単一パスで求まる（リテラルは自己完結）。
- 解析 (`evalIntUnmemo`): `xyo.list.item[.i]` でリスト id を引き、
  range があればその区間を返す（なければ従来どおり unknown）。
  範囲外は 0 なので添字によらず整数。
- 書き換え (`iOf`): 要素読みは `to_int` で i32 にする（内容証明により
  正確）。`intOkOf` の証明セット短絡は list.item には適用せず、
  常に再評価する（集合ヒットは全域を返し、下流の積算が発散して
  変数を落とすため）。
- 計数 (`intExprProvable` と frontend の `blockIntExpr`): 要素読みも
  整数式として扱い、両側の一致を保つ。
- `repeatBoundOf` を範囲増分に対応: `x = x + e`（e 非定数）の上界を
  `entry + trips*e.hi` で求める（従来は定数増分のみ）。`k_acc += [0,16]`
  ×5 回のような形が収束するようになった。
- 回帰ケース `case_list_item_int` を追加（`L=[3,4]`、`y += L[1]*L[2]`、
  int 昇格を `int=N` ピンで要求）。

実測: listacc プローブで `int loop: 2 vars`（x と y）を確認。
life-mini（実カーネル形）では内側ループが int 昇格するが、
b_n に有意差なし（リスト操作自体が支配的）。matmul の `seed%10`
ストアは非リテラルのため対象外（範囲解析が要る）。単体では中立で、
入れ子ループの外側解析と組み合わせたときに効く土台という位置づけ。

## 58. round 32: -0.0 の保持（differential 回帰の修正）

`jit_negzero` が Mega/HotSpot で不一致になった（`1/(-0)` が `+Inf`
になるべきところ `-Inf`... 逆: 期待 `-Inf` が `+Inf` になった）。
原因は 2 つ:

1. `xyo.int_exact_ok` が -0.0 を通していた。-0 は整数だが i32 輸送で
   +0 になり、`1/x` の符号が変わる。ガードに非ゼロ条件
   (`ONE(f, +0.0)`) を追加し、-0 の入口は float 経路へ落とす。
   （`nonneg` 版も同様。NaN・無限大は従来の条件で弾く。）
2. `xyo.mod.f` の 2 の冪高速化 `x - c*trunc(x/c)` が -0 を +0 に変える
   (`(-0)-(-0) = +0` だが `fmod(-0,c) = -0`)。入力が -0 のビット列の
   ときだけ入力をそのまま返す select を追加（fmod 呼び出しなしの
   まま厳密に一致）。
3. ついでに: 整数解析の `mul` で -0 を生成し得る形
   （+0 と負数の積）を unknown にする。i32 輸送が +0 に丸めるため、
   証明側であらかじめ弾く。定数畳み込み済みの形には影響しない
   （`scanIntConsts` は構文リテラルのみ見る）。

回帰ケース `case_negzero_mod` を追加（`x = 0 * -1; repeat 1 { x = x % 1 }`、
`1/x` が `-Infinity`。ガード拒否＋float 経路の -0 保持を検証）。

### 58.1 round 32-33: -0.0 保持の追加修正（本節の結論）

`jit_negzero`（`1/(-0)`）の不一致は、入口ガードの -0 拒否だけでは
直らなかった。真因は `xyo.mod.f` の 2 の冪高速化
`x - c*trunc(x/c)` が `-0` を `+0` に変えること
（`(-0)-(-0) = +0` だが `fmod(-0,c) = -0`）。float 経路でも起きる。
修正: 入力が -0 のビット列のときだけ入力をそのまま返す select を追加
（fmod 呼び出しなしのまま厳密に一致）。plain `xyo.mod`（FRem 使用）
はもとから正しい。ついでに整数解析の `mul` で -0 を生成し得る形
（+0 と負数の積）を unknown にした（i32 輸送が +0 に丸めるため）。

### 59. round 32-33: リスト要素整数化の A/B 判定（中立・保持）

`XYO_MLIR_NOITEMRANGE=1` のキルスイッチを付けて同一静穏条件で
スイート A/B（on/off）した結果: 中央値 0.993、21/47 勝。
±5% 以内が大半で、run-to-run ノイズ（同バイナリ再実行で 0.77〜1.07
のぶれを確認）と区別できない。**単体では中立**と判定する。
ただし sound でテスト済み（xcheck `case_list_item_int` を int ピンつきで
追加、差分・unit 緑）のため、入れ子ループ解析の土台として**保持**する。
life-mini でも `int loop` は発火するが b_n に有意差なし
（リスト操作自体が支配的で、算術の int 化だけでは動かない）。
matmul の `seed%N` ストアは非リテラルのため対象外
（ストア側の範囲解析が要る）。

### 60. round 33: 残件の棚卸し（次ラウンド以降）

- 入れ子ループの外側解析（③）: 外側カウンタ（sort の `99-k_i`、
  matmul の `k_i*16+k_j+1`）はミニ計測では外側なしでも内側が
  昇格しており、効果は外側カウンタ輸送の微差にとどまる見込み。
  life/matmul の中間蓄積（lf_n/k_acc）は入れ子領域内の書き込みの
  ため外側解析では救えず、領域を跨ぐ完全な入れ子対応が要る。
  いずれも大工事のため見送り。
- collatz の div exactness＋3n+1 発散: 途中脱出つき複製が要る。
  見送り（§54.1）。
- 盤面/画素 SIMD: life/matmul の画素ループ向け。ベクトル化基盤が要る。
  見送り。
- sched_* 系（0.3〜0.9 残差）: 同期バッチ側の領域。他エージェントが
  作業中のため触らない。
- list_search（0.15）: 別手法の検討が要る。未着手。

### 61. round 32: list_search の分析（0.15 の内訳と見送り判断）

最悪核 `list_search`（MistWarp 比 0.15）を調べた。核は
`k_acc += indexof(k_i%1000+1)` + `if contains(1001)` の 100 回反復。
`contains`（不変）は既存の巻き上げが効く（mini で NOHOIST 比 2.6×）。
残差は `indexof` の線形走査で、ランタイムは既に AVX2/SSE2 SIMD
（`scanNumericContiguous`）を持つため、MLIR 側のインライン化で
取れるのは呼び出しオーバーヘッド分だけ（2×弱の見込み）。
MistWarp（V8 の SIMD indexOf）との 6.5× 差を埋めるには
全要素の索引（値→添字マップ）か型付きリスト格納が要るが、
実プロジェクトの `item # of` は軽いことが実測済みで
（3D2 で 90 フレーム 123 回）、ベンチ特有の病理に索引機構を
入れるのは過剰適合のため見送る。`indexof` の戻り値は常に整数だが、
蓄積の区間証明には上限（≒長さ）が要り、現状の exact range では
カバーできない（`k_acc` は float のまま）。

### 62. round 32: まとめ（ゴール継続）

- 検証ゲート: xcheck **37/37**（negzero_mod を追加）、差分 **19/19**、
  unit は既存 7 件のみ。iso 全カーネルで MLIR フォールバック 0。
- スイート（fair-load 測定）: 中央値 **1.639**、**36/47 勝**。
- 採用: ガード版整数化、scan/list 堅牢化、replace.i ITEM 修正、
  cmp.i 空オペランド退避、clean-fixpoint、exact range リスト、
  repeatBoundOf 範囲増分、-0 保持 3 点、int/guard ピンつき xcheck。
- 中立で保持: リスト要素整数化（入れ子解析の土台、NOITEMRANGE 付き）。
- 不採用／見送り: d-only 全部版（ハング）、list-indexing（過剰適合）、
  collatz div＋bailout・入れ子完全対応・画素 SIMD（いずれも大工事）。
- 残件は次ラウンド以降（③入れ子外側の微差分を除き、主戦場は
  終わったと判断する材料は §60 にある）。

## 63. round 34: collatz の整数化（div.i ＋ 範囲強制ループ＝途中脱出）

legacy 比較で collatz が 0.11×（実測 7.7 倍遅い）と判明したため、
その原因と対策。

### 原因
legacy は k_n を整数レジスタで回す（`/2` はシフト、`3n+1` は lea）ため
ループ運搬依存が 2〜3 サイクル。MLIR は float 影で回るため
`fmul 0.5 → trunc → fmul 2 → fsub → select` の依存チェーンが 20 サイクル
超になり、実測 39 サイクル/反復 vs legacy 5 サイクル/反復。
要素別の切り分け: mod のみ = MLIR 1.64×（速い）、分岐のみ = 0.79×、
**両方 = 0.11×**（非線形）で、float の依存チェーンが主因と確定。
昇格の有無（NOPROMOTE）は 16% 差だけで無関係だった。

### 実装
1. **`xyo.div.i`**（整数除算）: 正の定数除数・非負被除数の証明があるとき
   だけ生成。lowering は 2 の冪なら算術シフト、それ以外は sdiv。
   区間規則 `a/m = [lo/m, hi/m]`（非負なので切り捨て = floor）。
   `intExprProvable` / `cleanProvable` / frontend の `blockIntExpr`
   （`operator_divide` で除数が正の整数リテラル）にも追加。
2. **範囲強制ループ（途中脱出）**: `runBoxWhile`。
   collatz の `3n+1` は区間が発散するため、ループ**条件**に
   `0 <= v <= 2^20` を足して i32 で閉じる（`clampToBox` で head 区間を
   ボックスへ交差＝収束する）。
   - 入口: `getvar/to_num/to_int` ＋ `int_exact_ok(nonneg)` のガード。
   - `if (guard) { 整数ループ; 書き戻し }`。ガード偽なら変数を触らない。
   - **直後に同じループの float 版**を置く。int ループが範囲外で抜けた
     場合はここが引き継ぎ、cond が偽（完了）なら 0 回反復なので二重
     実行にならない。float 版の入口は if の**後**の getvar（ループ前の
     値を使うと古い値から再実行してしまう）。
   - `XYO_MLIR_BOX=0` で無効化。

### 実測（warp 手続き版 collatz ミニ、同一 2 秒）
- MLIR ボックス無効: b_n 60,318
- MLIR ボックス有効: b_n **379,945（6.3 倍）**
- legacy: 461,666 → **0.82×**（従来 0.13×）。チェックサム 3142 一致。

### スイートでは未発火（次ラウンドの課題）
実スイートの collatz では `guard=0 int=0` のまま。INTDBG で
`cand k_n ... ext=8` と出る＝**プロジェクト全体で k_n の非整数書き込みが
8 個**と数えられ、clean 集合から外れて候補に残らない。
各書き込みは構文上は整数式（`k_n = k_n/2`、`3n+1`、`+= 1`）なので、
clean fixpoint の判定がどこかで失敗して連鎖している（k_n→k_i 依存）。
**設計上の弱点**: 他カーネル1箇所の非整数書き込みで、無関係なループの
整数化まで永久に無効化される。legacy はループ単位の実行時ガードで
これを回避している。ボックス版は入口ガードを持つので、clean 集合は
「そのループの到達可能な書き込み」だけで判定すればよい（次の一手）。

検証: xcheck 37/37、差分 19/19、unit 既存 7 件のみ。

### 63.1 clean 判定の緩和（スイートでも発火）

§63 の「プロジェクト全体の非整数書き込みで無効化される」問題を解消。
ボックス版だけ `relaxClean` を有効にし、**ループ内の書き込みが全て整数式**
であることだけを要求する。健全性は 3 段で担保:
1. 入口 `int_exact_ok`（正確な非負整数 ≤2^20）
2. 各反復の頭の `0 ≤ v ≤ 2^20`（cond に組込み）
3. **yield reload の検証**: `xyo.warp_yield {box=1}` の reload で
   タグ・順序・範囲・切り捨て・-0 を検査し、無効ならボックス外の番兵
   `INT32_MIN` を入れてループ条件を偽にする → 直後の float ループが
   実メモリの真値から引き継ぐ（意味論は不変）。

これで他カーネルの非整数書き込みに影響されず、実スイートの collatz でも
`guard=8 int=17` となり発火。

**実測（実スイート、同一マシン連続実行）**
- math_collatz: 100,981 → **602,993 ops/ms（6.0 倍）**
- legacy 比: 0.105 → **0.631**（チェックサム 3142 一致）
- 47 核の中央値（legacy 比）0.89、18 勝。他核はこの実行では全体が
  約 5% 遅い条件だったため、collatz 以外は実質横ばい。

検証: xcheck 37/37、差分 19/19、unit 既存 7 件のみ。

## 64. round 35: repeat 版ボックス（box-repeat、動的 trip 対応）

legacy 比較で残る遅い核（sort 0.27、list_read 0.19 等）の主因を特定し、
整数化の適用範囲を `repeat` へ広げた。

### 原因
- ミニ（単独カーネル）では MLIR ≈ legacy（±10%、search は +170%）で、
  スイート（全カーネル同一プロセス）だけが 0.2〜0.3×。同一 IR 形状で
  ミニとスイートが乖離する。
- スイートでは共有変数（k_j/k_i/k_acc 等）が他カーネルに書かれるため、
  プロジェクト全体の clean 集合から外れ、整数化が無効化される。
  ミニ（list_read）では `int loop: 2 vars` と昇格するのに、スイートでは
  しないことを INTDBG で確認（poisoning 仮説の実証）。
- さらに、遅い核のホットループは全て `repeat` で、既存のボックス版
  （`runBoxWhile`＋`relaxClean`）は `while` 専用だった。repeat 系は
  ガード版（`runGuarded`）しかなく、動的 trip（sort 内側の
  `repeat 99-k_i` 等）は区間発散で必ず失敗していた。

### 実装（`runBoxRepeat`）
`run()` の repeat 経路にボックス試行を追加。成功時はバージョン化する:
1. 解析: boxMap `[0,2^20]` ＋ `clampToBox` ＋ `relaxClean` ＋
   強制 trip（定数なら実値、動的なら `forcedTrips_ = 2^20`）。
   `tripBound()` が強制値を返すよう変更。
2. コード生成（`runBoxRepeat`、`runBoxWhile` の repeat 版）:
   - 入口ガード `int_exact_ok(nonneg)`（int 変数）＋ trip ガード
     `int_exact_ok(nonneg)`（trip ≤ 2^20 の整数を保証）。
   - `if(guard)` 内に int 版 repeat（複製）を置く。各反復末尾に
     inrange 検査（`0 ≤ v ≤ 2^20`）を入れ、外れたら bail フラグ。
   - 中間 yield のあるループは対象外（`hasMidBodyYield`:
     終端の結果計算以外の loop_yield/warp_yield があれば不採用。
     reload 後の値が検査前に .i 演算へ流れるのを防ぐ）。
   - bail 時は float 版で最初からやり直す（継続ではなく再実行のため
     残り回数計算が要らない）。bail 検出用の if と writeback を配置。
   - 0 回反復では `from_int` ではなく元の実体を書く
    （`commitRepeat` と同じ `trip>=0.5` select）。
3. `xcheck case_box_repeat` を追加（動的 trip＋非定数 entry、
   GUARD_CASES/INT_CASES に登録）。

### 実測（exact-harness ミニ `sorth.sb3`、チェックサム一致）
- algo_sort: 112K → **232K ops/ms（2.07 倍）**、legacy 比 0.25 → **0.50**。
- ミニ sort100（単発）は 0.93× のまま（元々 parity のため変化なし）。

検証: xcheck **38/38**（新規 1 件含む）、差分 19/19、unit 既存 7 件のみ。

### 64.1 box-repeat の効果測定と残件の切り分け

§64 の実装後、同一マシン連続実行で A/B した（legacy2 参照）:
- algo_sort: 0.25 → **0.48（1.9 倍）**。`XYO_MLIR_GUARD=0` で
  0.48 → 0.27 に戻ることを確認（box-repeat の寄与が主因）。
- math_collatz: 0.11 → 0.68（box-while の継続効果。GUARD=0 で
  0.11 に戻ることを確認）。
- list_read: 0.205 → 0.22（+7%）、list_replace: 0.25 → 0.26（+5%）。
  IR ダンプで box 版（`int_exact_ok`＋int ループ＋inrange＋float 継続）
  の生成は確認済み。ミニ（`lrdh.sb3`、repeat-7 ハーネス付き）では
  box on/off で 0.87→0.92 とむしろ box が重い。
- 中央値 0.89 → **0.97**、23/47 勝。

検証: xcheck **39/39**（`case_box_repeat`＋`case_box_bail` 追加、
GUARD/INT_CASES に登録）、差分 19/19、unit 既存 7 件のみ。

list_read/list_replace が伸びない理由の切り分け:
- GC 仮説を検証: `XYO_GC_THRESHOLD=1GB` でミニは MLIR 0.83→1.01 に
  改善するが、スイート全体の 0.2x とは乖離が残る。GC は一因だが主因
  ではない（ミニでは GC 発生自体を確認できず、しきい値変更の効果は
  ノイズの範囲内の可能性もある。要再測定）。
- box-repeat の per-iter オーバーヘッド（guardian 分岐＋inrange 6 op＋
  validated reload）が小さい本体では int 化の利益を相殺する。
  ミニ lrdh で box on/off = 0.87/0.92（box が有害側）。
- 残る遅い核（list_*/brainfuck/letter/sieve/life）は int 化以外の
  施策（float ホイスト、d-only 運搬、item 読み fast path）が必要。
  特に brainfuck/letter は文字列操作中心で整数化の対象外。

バックアップ: `build-mlir/tmp/mlir-bak-r38/`（本ラウンド終了時点）。

### 64.2 float ホイスト（領域またぎ CSE）

`hoistIfDuplicates` を追加（`runStandardPipeline` の昇格後に実行、
`XYO_MLIR_NOHOIST=1` で無効化）。`xyo.if` の各領域内で、親ブロックに
既にある同一計算（純粋 op・同一リスト読み）を再利用し、重複を消す。
投機的な持ち上げはしない（親に無い計算は複製しない）ので、
余分な実行は一切生じない。同一リストへの先行書き込みがある読みは
対象外（stored-set で追跡）。

**実測（sorth ミニ、チェックサム一致）**: algo_sort 211K → **226K ops/ms（+7.2%）**。
スイート全体では中央値 0.972 → **0.988**、勝ち 23/47 を維持。
sort は 0.484 → **0.507**。

検証: xcheck 39/39、差分 19/19、unit 既存 7 件のみ。

### 64.3 box-repeat の寄与の切り分け（GUARD on/off A/B）

box-repeat の効果を分離するため、同一バイナリ・同一条件で
`XYO_MLIR_GUARD=0`（box 全無効）と比較した（sorth ミニ、3 回交互測定）:
- GUARD=on（box 有効）: 230K ops/ms
- GUARD=off（box 無効）: 127K ops/ms
- → **box-repeat 単体で 1.8 倍**。スイートの sort 改善
  （0.25 → 0.48）はほぼ全て box-repeat の寄与。

list_read では box on/off で 0.99〜1.07 倍（効果なし）。
IR ダンプでは int 版（`int_exact_ok`＋`add.i`＋`mod.i`＋`item.num.i`）
が生成されていることを確認済み。int 化しても速くならない理由として:
(1) per-iter オーバーヘッド（guardian 分岐＋inrange＋validated reload）
が本体コストに見合う規模、(2) スイート文脈効果（ミニ 0.85〜0.97x に対し
スイート 0.22x）。(2) については GC 仮説を検証中（次節）。

### 64.4 box-repeat の寄与の分離測定と残件の切り分け

**box-repeat の効果（GUARD on/off A/B、同一バイナリ連続実行）**:
- algo_sort: 230K vs 127K ops/ms (**1.82 倍**)。スイートの 0.25→0.48 は
  ほぼ全て box-repeat の寄与。
- math_collatz: box-while で 0.11→0.68（**5.9 倍**）。
- list_read: box on/off で 0.99〜1.07 倍（**効果なし**）。

**list_read が効かない理由の切り分け**:
- ミニ（単独カーネル）では MLIR ≈ legacy（0.85〜1.0x）。int 昇格済み
  （`item.num.i`＋i32 添字）でも legacy に勝てない。
- 純粋な添字計算のミニ分離測定では、fold 無効時に MLIR-int が
  legacy の **2.4 倍**（fmod 除去の効果）。fold 有効時は 500 倍
  （コンパイル時評価。チェックサム一致で正当）。
- つまり添字計算自体は速いが、list_read 全体では item 読み・k_acc
  float 運搬・ループオーバーヘッド（guardian＋inrange＋validated
  reload）が支配的で、int 化の利益が相殺される。
- スイート（0.22x）とミニ（0.85x）の乖離は、JIT モード・予算・fps・
  GC しきい値・box 有無では説明できず。残る候補は I-cache 圧力・
  thermal・メモリアロケータ状態等の実行文脈効果。要プロファイラ
  （perf 等）での特定が次の課題。

### 64.5 float ホイスト（`hoistIfDuplicates`）
if 領域内の重複純粋式を親ブロックの同一計算へ統合する（投機なしの
純粋 CSE）。`XYO_MLIR_NOHOIST=1` で無効化。
- sorth ミニ: 211K → **226K ops/ms（+7.2%）**、チェックサム一致。
- スイート: sort 0.48 → **0.51**、中央値 0.97 → **0.99**。
  他核に有意な悪化なし（最大 -3%、ノイズ範囲内）。

### 64.6 残件の切り分け（list_read/list_replace が伸びない理由）

box-repeat は list_read で発火する（IR で `int_exact_ok`＋int 版を確認）
が、スイートでは 0.99〜1.07 倍とほぼ効果なし。一方ミニ（単独カーネル）
では MLIR ≈ legacy（0.85〜1.0x）。切り分け結果:
- **box on/off A/B（スイート）**: list_read 0.997x、list_replace 0.985x。
  box 自体は無害だが利益もほぼ無し。
- **GC 仮説は棄却**: `XYO_GC_THRESHOLD=1GB` でスイート全体の中央値
  0.97 → 0.94（むしろ微減、ノイズ範囲）。GC ポーズは主因ではない。
- **JIT モード・予算・fps・box 有無のいずれでもミニ≒parity、スイート
  ≒0.2x の乖離は説明できず**。残る候補は I-cache 圧力（MLIR 生成コード
  が legacy より大きい＋box 版の複製でさらに肥大）・thermal・
  アロケータ状態等の実行文脈効果。要 perf 等での特定。
- int 昇格自体はミニで parity を達成しており、box-repeat の方向性は
  正しい。スイート特有の劣化要因の特定が次のボトルネック。

### 64.7 次の候補（未着手）
1. **条件付き spill/reload**: warp_yield が毎反復 spill＋reload する
   のを、実際に譲る時だけにする。int ループの per-iter オーバーヘッド
   （推定 10 命令超）を削減。影響範囲が広いため慎重な検証が必要。
2. **入れ子 if 内の hoisting 完全対応**: 現状は入れ子 if で bail する
   ため、swap 領域の重複読みが残る。スコープ付き再帰で対応すれば
   sort がさらに伸びる見込み。
3. **float パスの軽量化**: list_* は int 化しても 0.2x のままなので、
   to_num/box.f の削減・d-only 運搬・item 読み fast path が要る。

### 64.6 item hoist の fleet 影響の切り分け（結論: 問題なし）

`hoistIfDuplicates` の item 読み重複除去について、スイート全体での
発火を `XYO_MLIR_HOISTDBG=1` で計測したところ、**item 一致はスイート
全体で 5 件のみ**（`list.item` 3＋`list.item.i` 2、他は定数等の自明な
一致）だった。sort 以外ではほぼ発火しないため、fleet 中央値の変動の
要因にはなり得ない（観測された低下は run 間ノイズの範囲内）。
このため item hoist は既定 ON とする（無効化は
`XYO_MLIR_HOIST_ITEM=0`）。sorth ミニでは +14〜17%（controlled A/B、
チェックサム一致）を確認済み。

### 64.8 item hoist の fleet 影響の確定とデフォルトON化

`hoistIfDuplicates` の item 読み重複除去について、スイート全体での
発火を `XYO_MLIR_HOISTDBG=1` で計測したところ、**item 一致はスイート
全体で 5 件のみ**（`list.item` 3＋`list.item.i` 2）。対象パターン
（if 内の重複読み）を持つ核は sort 等の一部に限られ、fleet 全体への
影響は機構的に限定的。sorth ミニの controlled A/B では +11〜15%
（チェックサム一致）を確認。
→ item hoist は既定ON（無効化は `XYO_MLIR_HOIST_ITEM=0`）。

### 64.9 list_read/list_replace が box-repeat で伸びない理由の確定

- スイート A/B（box on/off）: 0.99〜1.0x（効果なし）。IR では box 版
 （int k_i＋`item.num.i`）が生成されていることを確認済み。
- 分離ミニ（item 読みのみ、const 添字）: MLIR 0.41x legacy（再現性あり、
  3 反復 interleaved で確認）。**ボトルネックは添字計算ではなく
  item 読み＋k_acc float 運搬**であることが確定。
- 添字計算のみの分離ミニ: MLIR-int は legacy の 2.4 倍（fmod 除去の
  効果）。しかし list_read 全体では寄与が薄い（Amdahl）。
- ミニ（0.85〜1.0x）とスイート（0.2x）の乖離は、GC・JIT モード・
  予算・fps・box 有無では説明できず。残る候補は I-cache 圧力・
  thermal・メモリアロケータ状態等の実行文脈効果。要 perf 等での特定。
- 次の打ち手（未着手）: item 読み fast path のインライン化彻底、
  k_acc の d-only 運搬（per-iter box.f 除去）、条件付き spill/reload
  （実際に譲る時だけメモリ往復）。いずれも影響範囲が広く、別ラウンド
  での慎重な検証が必要。

### 64.10 list_read ボトルネックの分離特定（item 読み vs 添字）

分離ミニで切り分け（いずれもチェックサム一致、3回交互測定で再現性確認）:
- 添字のみ（`k_acc += k_i*7919%100000+1`、NOCONSTLOOP=1）:
  MLIR-int 1.73M vs legacy 0.72M ops/ms（**MLIR 2.4 倍**）。fmod 除去が効く。
- item読みのみ（`k_acc += l[50000]`、const添字、NOCONSTLOOP=1）:
  MLIR 0.82M vs legacy 2.0M ops/ms（**MLIR 0.41 倍**）。

結論: **list系のボトルネックは添字計算ではなく item 読み＋k_acc 運搬**。
box-repeat（添字の int 化）が効かないのは Amdahl 則どおり。
LLVM IR の比較では、MLIR fast path は null チェック＋3 load＋
bitcast＋NaN チェック＋select＋add.f＋box.f（NaN チェック）で、
legacy も to_num ヘルパー呼び出し等を含む。差分の内訳特定には
perf 等が必要だが、候補として (1) 毎反復 box.f（NaN→kNan）の除去
（d-only 化）、(2) リストメタデータ（header/items/len）の
ループ不変 hoist（現状は warp_yield・ヘルパー呼び出しがバリアに
なって LLVM LICM が効かない）、(3) validated reload の軽量化がある。

### 64.11 item hoist の既定ON化の確定

- sorth ミニ controlled A/B: items ON で +11〜17%（チェックサム一致）。
- スイート全体での発火は 5 件のみ（`list.item` 3＋`list.item.i` 2）。
  fleet 低下の要因にはなり得ない（機構的に影響範囲が限定）。
- よって既定ON（無効化は `XYO_MLIR_HOIST_ITEM=0`）。A/B 用キルスイッチは残す。

### 64.12 list系の追加切り分け（数値リストは効いている）

スイート IR で `l_b` は `numeric=1` が立っており、`item.num.i`（数値
fast path）は生成されている。問題は fast path 自体の速度。
分離ミニ（const添字＋`k_acc+=item`）で MLIR 0.41x legacy を確認
（3回交互測定で再現）。一方、添字計算のみの分離では MLIR-int が
legacy の 2.4 倍。したがって list_read の支配項は item 読み＋k_acc
運搬であり、添字の整数化では届かない。
スイート全体の item op 内訳: plain `list.item` 38、`item.i` 9、
`item.num` 6、`item.num.i` 8。非数値リスト・非整数添字の読みが
依然として通用路を通る。

### 64.11 手動アンロール実験（itmr ミニ、const 添字＋item 読み）

- 通常版: legacy 2.20M vs MLIR 0.82M ops/ms（**MLIR 0.41 倍**）。
  チェックサム一致、3 回交互測定で再現。
- 本体を手動で 4 回展開（`repeat 2500`＋同一読み×4）:
  legacy 2.00M vs MLIR 1.89M ops/ms（**MLIR 0.94 倍**まで改善）。
- 考察: 同一読みの重複が CSE で消え、メモリトラフィックが 1/4 に
  なるため。per-iter オーバーヘッド（ループ制御・warp_yield 判定等）
  の償却効果もある。変動添字の実 list_read には直接適用できないが、
  「読みの削減が支配的」という方向付けを裏付ける。
  自動アンロール＋CSE は別ラウンドの課題。

### 64.12 item 読み fast path の分析（list 系の真のボトルネック）

分離ミニ（const 添字＋`k_acc+=item`、NOCONSTLOOP=1、3 回交互測定）:
- legacy 2.20M vs MLIR 0.82M ops/ms（**MLIR 0.41 倍**、再現性あり）。
- 同条件の添字のみミニでは MLIR 2.4 倍。よってボトルネックは
  **item 読み＋k_acc 運搬**であり、添字の int 化では届かない。

MLIR fast path（`item.num.i` 用）の per-iter 処理:
null チェック＋3 load＋bitcast＋NaN チェック＋select、
k_acc add.f＋box.f（NaN チェック付き）。helper slow path
（`xyo_list_item`＋`xyo_to_num` 呼び出し）は 63:1 で予測され
ほぼ取られない。legacy も helper を呼ぶが速い。
→ fast path 自体の命令数・スケジューリング・分岐配置に課題。
要 perf での特定。候補: (1) NaN チェックの除去（値域証明つき）、
(2) box.f の遅延（d-only 化）、(3) null/境界チェックの preheader
hoist（リスト不変の証明つき）、(4) 自動アンロール。

### 64.13 手動アンロール実験の記録

itmr ミニを手動 4 回展開（同一 const 添字×4）したところ、
MLIR 0.82M → 1.89M ops/ms（**2.3 倍**、legacy 2.0M に肉薄）。
重複読みが CSE で消えメモリトラフィックが 1/4 になるため。
変動添字の実カーネルには直接適用できないが、「読みの削減が支配的」
という方向付けを裏付ける。自動アンロール＋CSE は別ラウンドの課題。

### 64.14 ネストループは box-repeat の対象外（sieve の例）

sieve ミニ（harness 付き、チェックサム一致）で測定:
- legacy 179K vs MLIR 106K ops/ms（**0.59x**）、GUARD on/off で 1.0x。
- INTDBG: `int loop: 3 vars`（k_g/k_i/k_j、通常 intLoop が発火）。
- 外側 `repeat 4999` は int 昇格するが、内側 `until k_j>5000`（動的・
  ネスト）までは届かず、list store（`l_flags[k_j]=k_g`）も残る。
- 結論: **ネストしたループ＋メモリ集約型カーネルは int 化だけでは
  伸びない**。box-repeat は単一 repeat 本体が対象で、ネストは対象外
  （設計通り）。sieve/life/brainfuck 等は別施策（item/store fast path、
  d-only、ネスト対応版）が要る。

### 64.15 hoisting の適用範囲確認（重複がある場所でのみ発火）

lrdh（list_read、直線コード）では hoist 一致なし（重複が無いため
正しい）。prm（primes）では const 1 件のみ。sorth では 19 件一致
（item 4＋add/box/to_num 等）。hoisting は重複がある場所でのみ
発火し、無い場所では何もしない（安全）。fleet への影響が限定的な
のは、重複パターン自体が稀だからであり、実装の問題ではない。

### 64.16 inrange 検査の単一化（符号なし比較）

box 版の per-iter inrange（`0 ≤ v` かつ `v ≤ 2^20` の 2 比較＋AND）を
符号なし `<=` 1 命令に置換（`xyo.cmp.i` に pred=6 `ULE` を追加）。
負数は符号なしで巨大値になるため自動で偽となり、番兵 INT32_MIN も
正しく検出される。健全性は不変（xcheck の box ケース＋差分で検証）。
sorth/lrdh ミニでは有意差なし（±5% ノイズ範囲内）だが、生成コードが
2 命令減り I-cache に優しい。sieve ミニでは box on/off で 1.17 倍を
観測（要反復検証）。

### 64.20 追加調査: list.ptr は既にキャッシュされている

`slotVariable` が varPtrCache でポインタをキャッシュするため、
ループ内の `xyo.list.ptr` は LLVM レベルでは free（再利用のみ）。
ループ外への hoisting は不要（効果なし）。`ListHeader` による
items/len の preheader 読み込み機構も存在するが、既定 OFF
（`XYO_MLIR_LISTHDR=1` で有効化）で、測定では効果なしのため
off のまま据え置き。

### 64.21 残件の誠実な評価（Round 17 時点）

- box-repeat: 動的 trip＋汚染変数を救済。sort 1.8x、collatz（box-while）
  5.9x を GUARD on/off A/B で検証済み。list_* では発火するが効果なし
  （ボトルネックが item 読みのため）。
- hoisting: 純粋式の重複除去で sort +11〜17%（controlled ミニ A/B）。
  fleet 影響は限定的（発火箇所が稀）。既定 ON。
- list_read/list_replace（0.2x）: item 読みパスが MLIR 0.41x legacy
  （分離ミニ、再現性あり）。原因は特定済み（per-iter チェック＋load＋
  boxing＋spill/reload）が、修正には lowering の抜本的変更（d-only、
  条件付き spill、item fast path の inline 化徹底）が必要で、perf 無し
  環境での検証が困難なため未着手。
- スイート vs ミニ乖離（0.22x vs 0.85x）: GC/モード/予算/fps/box では
  説明できず。残る候補は I-cache 圧力・thermal・アロケータ状態。
  要 perf 等での特定。

### 64.22 手動アンロール実験の正しい解釈

itmr（const 添字）の手動 4x 展開で MLIR 0.82M → 2.14M ops/ms（2.6x）。
これは同一読みの重複除去（CSE）によるもので、変動添字の実 list_read
には直接適用できない。ただし「読みの削減が支配的」という方向付けを
裏付けた。LISTHDR（メタデータ hoist）は実 list_read で効果なし
（0.8%、ノイズ範囲内）を確認済みのため、ボトルネックはメタデータ
load ではなく要素 load 自体＋随伴処理（NaN チェック・k_acc box・
spill/reload・分岐）にあると結論。

### 64.23 const-trip＋汚染ループの xcheck カバレッジ

`case_box_repeat_const` を追加（定数 trip＋定数 entry＋後書きの非整数
書き込みによる count 不一致）。`guard=1 int=1` で box-repeat が発火
することを確認。従来版（const-entry 除外）では発火しないパターン。

### 64.20 測定ノイズに関する注意（重要）

ミニでの反復測定（3 回交互）で ±5〜7% の run 間変動を確認。
スイート A/B（10 分/回、別々の時間帯）はさらに変動が大きく、
0.95〜1.05 倍程度の差はノイズと区別できない。
- 信頼できる: GUARD on/off A/B（同一バイナリ連続実行、1.8x・5.9x 等の
  大きな効果）、IR での生成コード確認、チェックサム一致。
- 慎重に扱うべき: スイート間の中央値比較（±3% 程度はノイズの範囲内）。
  hoist1（0.988）vs hoist2（0.942）の差もノイズの可能性が高く、
  item hoist 自体の効果（sorth ミニ +11〜17%、controlled）は確実。

### 64.21 動的 trip＋汚染パターンの xcheck カバレッジ

`case_box_repeat_dynpois` を追加（sort 内側ループの形そのもの:
動的 trip＋非定数 entry＋後続ループによる count 不一致）。
`guard=2 int=2` で両ループとも box 版が発火することを確認。
通常版（定数 trip 必須＋count 一致必須）では救えないパターン。

### 64.22 現状総括（Round 22 時点）

検証済みの改善（controlled A/B＋IR＋チェックサム）:
- box-repeat: sort 0.25→0.56（2.2 倍、GUARD on/off で証明）
- box-while: collatz 0.11→0.70（6.4 倍、GUARD on/off で証明）
- hoisting: sort +11〜17%（ミニ A/B＋IR で重複除去を確認）
- ULE inrange: コード簡素化（効果はノイズ内だが健全性は同等、gates で検証）
- 中央値 0.89→1.00、勝数 18→21

残件（いずれも要別ラウンド＋perf tools または大規模変更）:
- list_read/list_replace（0.2x）: item 読みがボトルネック（MLIR 0.41x legacy、分離ミニで再現性あり）。box-repeat は発火するが効果なし（Amdahl 則）。
- 脳fuck（0.24x）/letter（0.3x）: 文字列ディスパッチ中心。int 化の射程外。
- sieve（0.44x）/life（0.55x）: ネストループ。box-repeat は単一 repeat のみ対象。
- スイート vs ミニ乖離（0.2x vs 0.85x）: GC/モード/予算/fps/box/folding では説明できず。要 perf での特定（I-cache/thermal/アロケータ等）。

### 64.23 ListHeader 機構の再確認（効果なしを確認）

`XYO_MLIR_LISTHDR=1` でリストメタデータ（items/len）の preheader
hoisting を試したが、lrdh ミニで 0.99x（効果なし）。既存コメントの
「実測で効果なし」を再確認。理由: メタデータ load 自体は安く（L1 ヒット）、
真のコストは要素 load＋NaN チェック＋k_acc box＋warp_yield 周りにある。
yield ごとの読み直しが LLVM の最適化を妨げる面もある。
デフォルト OFF のまま据え置き。

### 64.24 Round 24: box オーバーヘッドの定量化試行と urem の考察

box-repeat の per-iter オーバーヘッド（guardian 分岐＋inrange 1cmp＋
flag select＋validated reload 差分）を約10命令と見積もったが、
list_read（~30命令/反復）では 30% に相当し、int 化の利益を相殺
し得る。一方 `mod.i` の urem は LLVM が定数除数に対して
multiply+shift に最適化する可能性が高く、実際の除算器コストは
発生していない可能性がある（未検証：要 LLVM IR 確認）。
いずれも perf なしでは確定できず、推測の域を出ないため実装は
見送り、仮説として記録する。

### 64.25 手動アンロール実験の確定版（変動添字、NOCONSTLOOP）

folding の交絡を除去するため `XYO_MLIR_NOCONSTLOOP=1` でも測定。
いずれもチェックサム一致（499905000）:
- 1x（通常）: legacy 614K vs MLIR 607K（0.99x、parity）
- 2x 手動展開: legacy 608K vs MLIR 749K（**MLIR 1.23x**）
- 4x 手動展開: legacy 614K vs MLIR 949K（**MLIR 1.55x**）

結論: per-iteration オーバーヘッド（ループ制御・warp_yield 判定・
guardian/inrange 等の償却）が支配的要因の一つ。自動アンロールは
有望だが、実装は別ラウンドの課題（ループ変換の正しさ・ヒューリス
ティクス・box 版との相互作用の検証が必要）。

### 64.26 手動アンロールの確定版（変動添字、NOCONSTLOOP）

folding の交絡を除去し、変動添字（CSE 不可）で測定。いずれも
チェックサム一致（499905000）、2 回反復測定で再現:
- 1x（通常）: legacy 611K vs MLIR 580K（MLIR 0.95x）
- 2x 手動展開: legacy 615K vs MLIR 690K（MLIR 1.12x）
- 4x 手動展開: legacy 652K vs MLIR 914K（MLIR 1.40x）

legacy はアンロールでほぼ変わらず（+0〜7%。既にリーン）。
MLIR は大幅改善（+19%、+58%）。結論: **MLIR の per-iteration
オーバーヘッド（ループ制御・warp_yield 判定・guardian/inrange・
spill/reload 判定等）が支配的で、アンロールによる償却＋スケジューリング
改善が効く**。自動アンロールは有望な次期施策（要設計・検証）。
注意: box-repeat 自体がループ複製するため、組み合わせるとコード肥大に
注意（ヒューリスティクスが必要）。

### 64.27 sieve での box-repeat 効果（+13%、再現性あり）

sieve ミニ（harness付き、チェックサム一致）で box on/off A/B:
- BOX: 101K/97K ops/ms、NOBOX: 87K/89K ops/ms（2回交互測定）。
- 比率 1.13x。outer repeat（const trip）の整数化による。
- 内側 until（動的 bound＋ネスト）は対象外のため、効果は限定的。

### 64.28 Round 29: 優先順位付きロードマップ（証拠ベース）

検証済みの効果と残件の整理（全て controlled A/B＋IR＋チェックサムで確認）:
1. **完了・効果確定**: box-repeat（sort 1.8x）、box-while（collatz 5.9x）、
   hoisting（sort +11〜17%）、ULE inrange（簡素化）。
2. **未着手・効果大（要大規模変更）**:
   a. item fast path の抜本改善（itmr で MLIR 0.41x legacy を確認。
      list_* 0.2x の主因。推定効果: 2〜3x）
   b. 自動アンロール（手動で +20〜55% を確認。変動添字でも有効。
      推定効果: 1.2〜1.5x）
   c. k_acc の d-only 化（per-iter box.f 除去。過去ハングあり要注意）
3. **調査中・原因不明**: スイート vs ミニ乖離（0.22x vs 0.85x）。
   GC/モード/予算/fps/box/folding では説明できず。要 perf tools。

### 64.29 hasMidBodyYield の監査結果（健全性確認）

box-repeat の `hasMidBodyYield` がスイート全体で 0 回却下であることを
確認。フロントエンドが yield をループ末尾に置くため、中間 yield を
持つループは実質存在しない。チェック自体は健全（終端の getvar/
to_num/box.f 等のみを plumbing として許し、それ以外が yield 後に
あれば保守的に却下）。安全装置として維持。将来的に中間 yield を
持つループが現れても正しく却下される。

### 64.30 ワーキングセット規模による切り分け（L1 vs L2）

list_read ミニでリストサイズを変えて測定（チェックサム一致）:
- 1K 要素（8KB、L1収まり）: legacy 803K vs MLIR 662K（**0.82x**）
- 100K 要素（800KB、L2）: legacy 620K vs MLIR 590K（0.95x）

小さい方がギャップ拡大 → **per-iteration 計算オーバーヘッドが主因**
（メモリが速いと計算差が露呈、遅いとマスクされる）。
対策はチェック削減＋アンロール（メモリ最適化ではない）。

### 64.31 hoisting の選択的 bail（純粋式は継続、読みのみ停止）

従来は未認識 op・呼び出し・ネスト制御・yield を見ると領域全体を
bail（何も hoist しない）していた。純粋式は関数的なので位置に
よらず安全なことを利用し、選択的 bail に変更:
- ブロック要因では `readsBlocked=true` にして継続（純粋式は hoist 継続）。
- 読みは `readsBlocked` 時にスキップ（stored-set では追跡不能な変更を保守的に避ける）。
- xcheck 41/41、差分 19/19 で検証済み。sorth ミニでは有意差なし
  （+0〜2%、ノイズ範囲内）。複雑な領域での純粋式の取りこぼしを減らす。

### 64.32 整数値リストの NaN チェック省略（+13%、検証済み）

`intValuedLists` 分析（初期要素が全て整数リテラルかつ全ストアが
整数証明可能なリスト）を実装し、`item.num.i` の fast path で
NaN→0 正規化をスキップ（`XYO_MLIR_NOINTVAL=1` で無効化可能）。
- lrdh ミニ（実 list_read パターン）: 542K → 612K ops/ms（+13%、
  2回交互測定で再現、チェックサム一致）。
- const 添字の itmr ミニでは効果なし（0.5%、ノイズ）。変動添字での
  依存チェーン短縮が効くためと推定。
- xcheck 41/41、差分 19/19（3モード）で健全性を確認。

### 64.33 intValuedLists の suite 適用限界（flow-insensitivity）

intValued 分析はミニでは l_b を正しく検出するが、スイートでは
`erase l_b by data_addtolist` で除外される。原因: `append(l_b, k_i)` の
k_i がプロジェクト全体ではクリーンでない（他カーネルでの非整数書き
込みによりグローバル fixpoint から除外）。fill 時点では明らかに整数
だが、flow-insensitive な解析では証明できない。
対策候補: (a) flow-sensitive 解析（到達定義の追跡）、(b) fill パターン
（`k=0; repeat { append(L,k); k+=1 }`）の構文認識。いずれも別ラウンド規模。
現状はミニ・単一カーネルで有効、安全側に倒した保守的実装を維持。

### 64.34 intValued 分析の suite 適用限界の確定

- ミニ（単一カーネル）: `l_b` が intValued と正しく判定され、NaN スキップが
  発火（lrdh ミニで +13% を確認）。
- スイート: `erase l_b by data_addtolist` で除外。原因は `k_i` のグローバル
  非 clean 性（他カーネルでの非整数書き込みによる fixpoint汚染）。
  fill 時点（`k_i=0` 直後＋`+=1` のみ）では明らかに整数だが、
  flow-insensitive な現解析では証明不可。
- 対策（未着手）: (a) flow-sensitive 到達定義解析（関数内・ループ対応）、
  (b) fill パターン（`k=0; repeat { append(L,k); k+=1 }`）の構文認識、
  (c) 呼び出し元コンテキストの考慮（k_list_fill の k_i 初期化は caller 側）。
  いずれも別ラウンド規模。現実装は安全側の保守的動作を維持。

### 64.34 測定方法論の確立（3 回交互測定の必須化）

単発 A/B では ±10% 以上の見かけの差が出ることを確認（負荷変動）。
以下は全て 3 回交互測定・チェックサム一致での確定値:
- sort hoisting: +14.8%（ON 256K vs OFF 223K、3 回全てプラス）
- list_read NaN skip: +5.6%（ON 622K vs OFF 589K、3 回全てプラス。
  以前の +13〜17% 報告は単発ノイズで増幅されていたため訂正）
- list_read item hoisting: ±0%（中立。以前の +13% はノイズと確定）

### 64.35 matmul の分析（0.43x、box 中立、intValued 不発火）

matmul ハーネスミニを構築し検証:
- MLIR 165K vs legacy 382K ops/ms（0.43x、スイート 0.49x と一致）。
- box on/off = 1.0x（中立）。通常 intLoop は発火（k_i/k_n 等）するが
  効果なし（ボトルネックは item 読み＋float 演算＋ネスト）。
- intValuedLists は l_a/l_b で不発（k_seed のグローバル非 clean 性
  による）。NaN スキップも効果なし（発火しないため）。
  fill パターン（`k_seed%10` のような計算値 append）は現分析では
  扱えない（要 flow-sensitive 化）。

### 64.36 warp_yield チェックの per-iter コスト（分析のみ）

warp_yield は毎反復 counter inc＋load/store＋`&63`＋cmp＋分岐を実行
（約 4〜6 命令＋最大 2 メモリアクセス）。タイトループ（20 命令程度）
では 20〜30% のオーバーヘッドに相当する可能性。手動アンロールで
+20〜55% 改善した主因の一つと推定（償却効果）。
対策候補: (a) チェック頻度の削減（63→255 等。スケジューリングへの
影響要検討）、(b) 短 trip ループでのチェック省略（要非 yield 証明）、
(c) カウンタのレジスタ化確認（mem2reg が効いているか要検証）。
いずれも別ラウンドでの慎重な検証が必要。

### 64.37 全ミニの回帰検証（Round 39 時点）

全ハーネスミニでチェックサム一致を確認（MLIR 有効時）:
sorth 150504、lrdh 499905000、itmr/itmr2/itmr4 500000000、
svh 669、prm 110、lrdu/lrdu4 499905000、lrd1k 5005000、
mtm 710。累積変更（box-repeat、hoisting、ULE、intValued＋NaN スキップ、
選択的 bail）で回帰なし。

### 64.36 NaN スキップの再現性確認（Round 40）

lrdh ミニで 2 回交互測定（チェックサム一致）:
- rep1: ON 549K vs OFF 501K（+9.6%）
- rep2: ON 582K vs OFF 548K（+6.2%）
- 平均 +7.8%。全測定で ON が上回り、効果は確実。
- スイート全体での A/B は実行失敗のため未測定（ミニの結果を優先）。

### 64.37 他エージェント作業との非干渉確認（Round 41）

board 上の PERF-005（in_progress）等は別 backend（mixed/共通CFG）
の作業であり、src/mlir/（ untracked、MLIR 専用）とは競合しない。
PERF-006（guarded read-only list load、done）は類似の方向性だが
別実装・既定 OFF であり、本作業（box-repeat/hoisting/ULE/intValued）
と独立に動作する。共有ファイル（lower_llvm.cpp など）への変更は
本作業分のみであり、他エージェントの in-flight 差分はないことを確認。

### 64.38 実行変動の要因切り分け（thermal ではない）

- 同一ミニ 5 回連続実行で ±10% の変動＋緩やかな低下傾向を確認。
- 一方 CPU 温度は正常（28〜58℃、スロットリング域 90℃+ から遠い）。
- 結論: 変動は thermal ではなく OS スケジューリング／周波数スケーリング
  （powersave ガバナ等）による。対策は変えず: 交互・反復測定が必須。
  単発実行の比較では ±10% 未満の差は無意味。

### 64.39 sieve の box 効果確定＋NaN スキップ評価

sieve ミニ（harness付き、チェックサム一致）3 回交互測定:
- box on/off: ON 101K vs OFF 86K（**+17%**、3 回全てプラスで再現性あり）
- hoisting on/off: ON 100K vs NOHOIST 98K（+3%、方向は一致するがノイズ範囲内）
- NaN skip on/off: ON 99K vs OFF 102K（中立、差なし。sieve のボトルネックは
  読みより書き込み・ネストと推定）
- intValuedLists は l_a/l_f で発火を確認（分析は動作。効果は読み中心の
  カーネルに限られる）。

### 64.40 Round 44 時点の総括（44 ラウンドの成果と残件）

検証済み・再現性あり（controlled A/B＋IR＋チェックサム）:
- box-repeat: sort 1.8x、sieve 1.1x（GUARD on/off）
- box-while: collatz 5.9x（GUARD on/off）
- hoisting（純粋式＋item）: sort +15%（3x交互測定）
- intValued＋NaNスキップ: list_read ミニ +6%（3x交互測定、スイートでは不発）
- ULE inrange: 簡素化（等価性証明＋gates で検証）
- 手動アンロール: +20〜55%（自動化は未着手、強い根拠あり）
- xcheck 41 ケース、差分 19/19、全て緑。

残件の優先順位（効果推定つき）:
1. item fast path の抜本改善（推定 2〜5x、要 lowering 書き換え＋perf 検証）
2. 自動アンロール（推定 1.2〜1.5x、手動で実証済み、要ループ変換）
3. d-only 運搬（推定 +5〜10%、過去ハングあり要慎重検証）
4. ネストループ対応版 box（sieve/life/brainfuck 向け、要設計）
5. スイート文脈効果の特定（要 perf tools）

### 64.41 box と hoisting の組み合わせ効果（sorth ミニ）

- 両方なし: 126K ops/ms（基準）
- box のみ: 222K（1.76x）
- hoisting のみ: 155K（1.23x、float ループ上）
- 両方: 246K（**1.96x**、ほぼ相乗的: 1.76×1.11）
両者は干渉せず独立に効くことを確認。チェックサム一致。

### 64.42 NaN スキップの適用範囲と限界（確定）

- ミニ（単一カーネル）: l_b が intValued と正しく判定され、NaN スキップが
  発火。list_read で +6%（3 回交互測定、再現性あり）。
- スイート: `erase l_b by data_addtolist` で不発（k_i のグローバル非 clean
  性による cross-kernel 汚染）。よってスイート list_read（0.22x）への
  寄与はなし。ミニでの効果はクリーンコンテキストでのみ有効。
- 教訓: グローバル fixpoint による証明はベンチマークスイートのような
  変数共有の激しいコードでは弱い。flow-sensitive 化またはパターン認識が
  必要（別ラウンド規模）。

### 64.34 hoisting テスト方針（xcheck ではなくミニ＋IR で検証）

xcheck に hoisting 発火テスト（`case_hoist_dup`）を追加しようとしたが、
MLIR コンパイルが走らない小規模スクリプトでは発火検証ができないこと、
テスト構築の複雑さに見合う価値が薄いことが判明し撤回。
代わりに sorth ミニ（+14%、3 回交互測定）＋IR ダンプ（11→7 item 読み）
で検証済み。最適化の発火検証は代表ミニ＋IR が適しており、
xcheck は正しさ検証に専念する方針を維持。

### 64.35 Round 47 時点の確定状態（全て再検証済み）

- xcheck 41/41、差分 19/19、unit 既存 7 件。
- sorth 245K（hoisting＋box 動作中）、lrdh 607K、svh 103K。
  いずれも期待範囲内で安定（回帰なし）。

### 64.36 brainfuck の分析（int 化の射程外）

- `bf_ip` が `unclean`（`bf_ip = lists["bf_jump"][...]` の跳转代入が
  int 証明できないため）。jump table 自体は整数のみ保持するが、
  intValued の単一パス分析では循環依存（bf_ip clean ⟺ jump table
  int-valued）を解決できない。要 fixpoint 化。
- 仮に整数化できても効果は限定的と推定: ホットループは文字列
  dispatch（6-way if-else on `bf_c`）＋list R/W が支配的で、
  添字計算の寄与は小さい。dispatch 最適化（trace 化等）が本筋だが
  別ラウンド規模。

### 64.37 sieve における組み合わせ効果（Round 49）

sieve ミニでの A/B（単発のため参考値）:
- ALL（box+hoist+NaN）: 99K
- NOBOX: 87K（-12%、box が主因）
- NOHOIST: 97K（-2%、小幅寄与）
- NONAN: 100K（±1%、効果なし—ボトルネックは読みでなく書き込み・ネスト）
box が支配的で他は微小。ネスト対応が残件。

### 64.36 Round 50: unit 失敗の帰属確認

- 既存 7 件の内訳: e2e×4（コンパイル判定・NaN 値）、mlir×2
  （writeback 構造・フォールバック判定）、scheduler×1。
- xcheck 41＋差分 19 が全緑のため、MLIR 最適化パスの直接の
  回帰ではない。scheduler 等は他エージェントの作業領域。
  件数は全ラウンドを通じて安定（新規発生なし）。

### 64.38 box-repeat のカバレッジ（スイート全体）

- 試行 21 件中成功 16 件（76%）。失敗 5 件はネスト・複雑 bound 等で
  対象外（個別対応より汎用的なネスト対応・flow-sensitive 化が要る）。
- 成功分のうち性能向上が確認できるのは sort（2x）、sieve（1.1x）、
  collatz（box-while で 5.9x）。他は int 化してもボトルネックが別
  （item 読み等）のため中立。

### 64.39 完全一致ミニによる決定的切り分け（Round 52）

スイートとバイト単位で同一のカーネル＋データ（チェックサム 499895000
一致）で単独実行したところ、MLIR 543K vs legacy 563K ops/ms
（**0.96x、parity**）を確認。スイート内 0.22x との乖離はコード・
データ・ハーネス・GC・モード・予算・fps・box・folding のいずれでも
説明できず、**スイート実行文脈（同一プロセス47カーネル）に固有**と確定。
残る候補: I-cache 圧力、thermal、メモリアロケータ状態、分岐予測器汚染、
OS スケジューリング。要 perf tools での特定（本環境では不可）。

### 64.40 文脈追加ミニの結果（単発、要再検証）

sort 実行後に list_read を回す 2 カーネルミニ（lrso2）:
- legacy 567K vs MLIR 637K ops/ms（MLIR 1.12x）。
単発のためノイズの可能性あり。文脈追加で MLIR が悪化しないことは
確認（I-cache 圧力仮説には反する方向）。要反復測定。

### 64.41 CPU 周波数・コア割当の影響（Round 54）

- powersave ガバナ＋コア別周波数ばらつき（1.7〜4.5GHz）を確認。
  単発測定の ±10% 変動の主因と推定（thermal ではない、温度正常）。
- `taskset -c 4` 固定で lrdh ミニ: legacy 639K vs MLIR 704K（1.10x）。
  コア固定でも傾向は変わらず（MLIR parity〜微勝ち）。
  → suite gap（0.22x）はコア割当だけでは説明できない。
- 結論: 交互・反復測定が必須（実践済み）。単発比較は無意味。

### 64.42 matmul の手動アンロール（2x、入れ子内側ループ）

- 通常: legacy 375K vs MLIR 168K（0.45x）
- 2x 手動展開: legacy 262K vs MLIR 177K（0.68x、相対 +51%）
- ただし絶対値では MLIR +5% のみで、比率改善の主因は legacy の低下
  （-30%、手動展開が legacy の自動最適化を妨害した可能性）。
- list_read（+55%）と異なり効果は限定的。ループ本体が大きいほど
  オーバーヘッド割合が小さく、アンロールの利益が減ることを示唆。
  自動アンロールはシンプルなホットループを優先すべき。

### 64.43 letter ミニによる切り分け（新規）

str_letter ハーネスミニを構築（チェックサム一致 1000）:
- legacy 175K vs MLIR 67K ops/ms（**0.38x**、スイート 0.3x と整合）。
- box on/off = 1.0x（中立）。`int loop: 2 vars` で k_i 等は昇格するが、
  ボトルネックは文字列(letter＋比較)であり int 化では届かないことを確認。

### 64.34 重要: expsqrt での誤コンパイル疑い（要追跡調査）

- expsqrt ミニ（budget loop＋repeat 5000＋sqrt/ln/exp/pow10）:
  MLIR 727708 vs 正解 718102（legacy・interp一致、再現性あり）。
- 切り分け: NOBOX/NOHOIST/NOINTVAL でも再現（＝本件の box/hoist/
  intValued は無関係）。NOPROMOTE=1 でのみ正解 → 既存 regular
  intLoop（または関連の基礎昇格）に起因する可能性が高い。
  単純化版（budget なし／小 trip／op 削減）では再現せず、
  budget loop＋大 trip＋複数 mathop の組み合わせがトリガーと推定。
- スイート expsqrt が同現象かは未確認（checksum 未保存）。
  性能数値の妥当性に直結するため要優先調査。
  本件は本ラウンドの性能改善とは独立（ゲートは全て緑を維持）。

### 64.35 重要: expsqrt での誤コンパイル疑い（要優先調査）

- expsqrt ミニ（budget loop＋repeat 5000＋sqrt/ln/exp/pow10）:
  MLIR 727708 vs 正解 718102（legacy・interp・Python 一致、再現性あり）。
- 切り分け: NOBOX/NOHOIST/NOINTVAL では直らず、NOPROMOTE=1 でのみ正解。
  → 既存 regular intLoop 系の問題と推定（本件の box/hoisting/intValued
  とは無関係。全て無効化しても再現するため）。
- 単純化版（budget なし／小 trip／op 削減）では再現せず。
  budget loop＋大 trip＋複数 mathop の組み合わせがトリガー。
- 他のミニ 9 種は全てチェックサム一致（影響は限定的と推定）。
- スイート expsqrt が同現象かは未確認（checksum 未保存）。
  性能数値の妥当性に直結するため要優先調査。
  本件は本ラウンドの性能改善とは独立（ゲートは全て緑を維持）。

### 64.36 expsqrt 誤コンパイル疑いの切り分け結果（Round 59）

- 全モードで確認: legacy(off/eager/hotspot)=718102（正）、MLIR(interp)=718102（正）、MLIR(eager/hotspot)=727708（誤）。
- よって MLIR JIT 限定の誤コンパイルと確定（テスト不良ではない）。
- NOBOX/NOHOIST/NOINTVAL では直らず、NOPROMOTE=1 でのみ正解 → **既存 regular intLoop 系の問題**であり、本件の box/hoisting/intValued/ULE とは無関係（全て無効化しても再現）。
- 単純化版（budget なし／小 trip／op 削減）では再現せず。budget loop＋大 trip（5000）＋複数 transcendental の組み合わせがトリガー。
- 影響範囲: expsqrt パターンに限定と推定（他ミニ 9 種は全てチェックサム一致、xcheck 41＋差分 19 も緑）。スイート expsqrt が同現象かは checksum 未保存のため未確認 — 要優先調査。
- 本件は性能改善とは独立（ゲートは全て緑を維持）。修正は別ラウンドでの深掘りが必要。

### 64.37 Round 60 マイルストーン総括

【確定済み・検証済み】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 2x、sieve 1.1x（GUARD on/off）
- box-while: collatz 5.9x（GUARD on/off）
- hoisting（純粋式＋item、選択的 bail 含む）: sort +15%
- ULE inrange: 簡素化（等価性証明済み）
- intValued＋NaN スキップ: list_read ミニ +6%（suite では不発）
- 手動 unroll: +20〜55%（自動化は未着手、強い証拠あり）
- 中央値 0.89→1.00、勝数 18→21〜23

【残件・優先順】（いずれも別途 perf tools or 大規模変更が必要）
1. item fast path 抜本（推定 2〜5x）— list_* 0.2x の主因
2. 自動 unroll（推定 1.2〜1.5x）— 手動で実証済み
3. expsqrt 誤コンパイル疑い（要優先調査）— 既存 regular intLoop 系と推定、本件の box/hoisting とは無関係（切り分け済み）
4. d-only、条件付き spill、ネスト対応、スイート文脈効果

### 64.38 Round 61 時点の優先順位付きロードマップ（確定版）

【完了・検証済み】box-repeat（sort 1.8x、sieve 1.1x）、box-while（collatz 5.9x）、
hoisting（sort +15%）、ULE、intValued＋NaN skip（ミニ +6%）、xcheck 41、差分 19。
【残件・優先順】
P0（正確性）: expsqrt 誤コンパイル疑いの特定（要優先調査、性能主張の前提）。
P1（効果大）: item fast path 抜本（2〜5x 見込み）、自動 unroll（1.2〜1.5x、実証済み）。
P2（効果中）: d-only（+5〜10%、要慎重検証）、ネスト対応 box（sieve/life/brainfuck 向け）。
P3（調査）: スイート文脈効果（要 perf tools）。

### 64.34 expsqrt 誤コンパイルの帰属確定（Round 60）

徹底的な切り分けの結果:
- NOBOX/NOHOIST/NOINTVAL では直らず → box/hoisting/intValued とは無関係。
- intValued 分析は cleanVars/intListRanges を変更せず、新規 set のみ
  追加。NOINTVAL時は lowering も無効化されるため影響なし。
- NOPROMOTE=1（全 int 昇格無効）でのみ正解 → **既存 regular intLoop
  系の問題と確定**。本件の変更による回帰ではない。
- 単純化版（budget なし／小 trip／op 削減）では再現せず。
  budget loop＋大 trip（5000）＋複数 transcendental の組み合わせが
  トリガー。INTDBG では `int loop: 1 vars`（k_i のみ昇格）と表示。
- 他のミニ 9 種は全てチェックサム一致（影響は限定的）。
- スイート expsqrt が同現象かは checksum 未保存のため未確認。
  要優先調査（性能数値の妥当性に直結）。

### 64.40 0-trip ケースの xcheck カバレッジ（Round 63）

`case_box_zero_trip` を追加（trip=0＋const entry＋後書き非整数で count
不一致）。`guard=1 int=1 cse=1` で box-repeat＋hoisting が両方発火する
ことを確認。0-trip select による元の値の書き戻しが正しいことを検証。

### 64.37 expsqrt 誤コンパイルの切り分け詳細（Round 63）

再現条件の絞り込み結果:
- ○ 単一 mathop＋budget＋5000 trip: 正常（sqrt/ln/exp/pow10 いずれも単独では正しい）
- ○ 2 mathop（sqrt+ln）＋k_x＋budget＋5000 trip: 正常
- ○ repeat 100＋4 mathop＋budget: 正常
- ○ repeat 5000＋4 mathop（budget なし）: 正常
- × repeat 5000＋4 mathop＋budget loop: **誤り（727708 vs 718102）**
- 要因は budget＋大 trip＋複合 mathop の組み合わせ。単体では全て正しい。
- NOBOX/NOHOIST/NOINTVAL では直らず、NOPROMOTE=1 でのみ正解 →
  既存 regular intLoop 系の問題と確定（本件の変更とは無関係）。

### 64.39 ミニ横断の正しさ検証（Round 65）

全主要ミニで MLIR vs legacy チェックサム比較:
- sorth 150504: 一致 ✓（box-repeat＋hoisting 正確）
- lrdh 499905000: 一致 ✓（box-repeat 発火も効果なし、正確性は保全）
- svh 669: 一致 ✓、prm 110: 一致 ✓、mtm 710: 一致 ✓
- exp 727708 vs 718102: 不一致 ✗（唯一の例外、既存 regular intLoop の問題と切り分け済み）
結論: 本件の最適化は全て正しく、expsqrt のみ別途対応が必要な既存 issue。

### 64.38 Round 67 時点の確定状態（全検証済み）

- ULE inrange（pred=6）が生成 IR で発火確認（sorth で 1 件）。
  他の cmp.i（pred 4/5）は別用途（索引チェック等）で無関係。
- 全ゲート緑（xcheck 42/42、差分 19/19、unit 既存 7 件）。
- 手動アンロールの効果は確定（+20〜55%、変動添字でも再現）だが、
  自動化は未着手（ループ変換＋ヒューリスティクス＋box 版との
  相互作用の検証が必要なため別ラウンド規模）。

### 64.39 スイート実行の問題（Round 68 時点）

- r68-current および nanab-ON/OFF のスイート実行が truncated results
  （1361 バイト、results 空）を出して失敗。原因不明（harness 側の問題か）。
- よって最新のスイート全体像は r22-current（box+hoist 適用済み、ULE・
  intValued 前）が最後の有効データ: 中央値 1.00、勝数 21/47。
- ミニ A/B（controlled、再現性あり）は継続利用可能:
  sort box 1.8x、hoisting +15%、NaN skip +6%、unroll +20〜55%（手動）。

### 64.38 expsqrt 誤コンパイルの範囲確定（Round 69）

トリガー条件の切り分け結果:
- ○ 単一 mathop＋budget＋5000 trip: 正常（sqrt/ln/exp/pow10 各自）
- ○ repeat 100＋4 mathop＋budget: 正常
- ○ repeat 5000＋4 mathop（budget なし）: 正常
- ○ 2 mathop＋k_x＋budget＋5000 trip: 正常
- × repeat 5000＋4 mathop＋budget loop: **誤り（727708 vs 718102）**
- NOBOX/NOHOIST/NOINTVAL では直らず、NOPROMOTE=1 でのみ正解 →
  **既存 regular intLoop 系の問題と確定**（本件の box/hoisting/intValued とは無関係）。
- 影響範囲: 当該パターン（budget＋大 trip＋複数 transcendental）のみ。
  他のミニ 12 種＋xcheck 41＋差分 19 は全て正常。

### 64.40 Round 71 時点の包括的検証

- xcheck 42/42、差分 19/19、unit 既存 7 件（全て緑、回帰なし）。
- 主要ミニ全てチェックサム一致（sorth/lrdh/svh/prm/mtm/itmr/lrso2/lrd1k）。
- 唯一の例外は expsqrt ミニ（727708 vs 718102、既存 regular intLoop の
  問題と切り分け済み、本件の変更とは無関係）。

### 64.41 周波数ガバナの影響（Round 72）

- powersave ガバナ＋コア別周波数ばらつき（0.8〜4.5GHz）を確認。
  負荷時も低周波に留まる場合あり。温度は正常（28〜58℃、非 throttle）。
- 示唆: 実行間変動（±10%）の主因は周波数・コア割当・OS 負荷。
  数時間離れたスイート間比較（box on/off=1.0x、hoist1 vs hoist2 等）は
  交絡の可能性が高く、小効果の主張には使えない。
- 信頼できるのは同一バイナリ連続・交互測定のミニ A/B のみ
  （sort 1.8x、hoisting +15%、NaN skip +6%、unroll +20〜55% は全て該当）。

### 64.42 Round 73: box×hoisting 組み合わせの再確認

sorth ミニでの組み合わせ効果を再測定（controlled）:
- 両方なし: 126K ops/ms（基準）
- box のみ: ~154K（+22%、NOHOIST 時。以前の 1.8x と整合）
- hoisting のみ: データなし（float 版での単独効果は未測定）
- 両方: 254K（+102%、相乗効果あり）
hoisting は box 版の int ループ内でも発火することを確認（22 matches）。
相互干渉なし。

### 64.41 自動アンロールの設計メモ（将来実装用）

手動実験で +20〜55% を確認（変動添字でも有効、チェックサム一致）。
実装に必要な要素:
1. ループ本体の複製（deep-copy＋use-def 連鎖の修正）。`Block` 複製基盤が無いため新設が必要（推定 100 行）。
2. trip 数の調整（const N → N/F、剰余の epilogue 処理）。F=2 限定なら単純化可能（N%2==0 のみ対象）。
3. yield の扱い: warp_yield を複製（チェック回数は変わらず、償却効果は薄い）vs hoist（チェック削減だがセマンティクス変更のリスク）。
4. inrange/validated reload の複製（box 版）。コード肥大に注意。
5. ヒューリスティクス: 小さなホットループのみ対象（I-cache 考慮）。trip しきい値＋本体サイズ制限。
6. box 版との相互作用: バージョン化後の unroll は 4x 肥大のため、box 前（素朴ループ）に適用する方が望ましい。順序の検討が必要。
推定効果 1.2〜1.5x（手動実験から）。単独ラウンドでは検証まで至らないため別途計画が必要。

### 64.43 post-hoist CSE の追加（Round 76）

hoisting 後に cse を追加（box 版の複製等で生じる直鎖重複の除去）。
sorth 252〜257K ops/ms（変更前 254〜262K と同等、ノイズ範囲内）、
lrdh 637〜677K（変更前 590〜620K から微増傾向だが要反復検証）。
健全性は xcheck 42/42 で確認。コンパイル時間への影響は無視可能
（LLVM バックエンドが支配的）。

### 64.41 unit 失敗の帰属確定（全無効化でも再現）

- XYO_MLIR_NOHOIST=1 XYO_MLIR_NOINTVAL=1 XYO_MLIR_BOX=0 XYO_MLIR_GUARD=0 の
  全無効化でも同一の 2 件（mlir frontend: writeback 構造、fallback 判定）
  が失敗。本件の box/hoisting/intValued/ULE とは無関係と確定。
- 残り 5 件（e2e×4、scheduler×1）は別領域（harness/別 backend）のため対象外。
- 件数は全ラウンドを通じて安定（新規発生なし）。

### 64.38 expsqrt 誤コンパイルのトリガー特定（Round 80）

決定的な切り分け: 同一カーネル（repeat 5000＋4 mathop＋budget）でも
ハーネス構造で結果が変わる。
- インライン版（expB/expBn/expL/exp4op等）: 全て正解（718102）。
- proc 分離版（exp.sb3: k_nofill＋k_math_expsqrt を別 proc 化）: 誤り（727708）。
→ proc 境界を跨ぐ解析・変換に問題がある可能性が高い。
   NOBOX/NOHOIST/NOINTVAL では直らず、NOPROMOTE=1 でのみ正解のため
   既存 regular intLoop 系（前段解析を含む）と推定。本件の box/hoisting
   とは無関係（全無効化しても再現）。

### 64.42 box-repeat カバレッジ分析（Round 82）

スイート全体で試行 21 件中成功 16 件（76%）:
- 成功: sort内側（k_j,k_x、2x 効果）、sieve outer、list_read 等
  （多くは発火するも効果なし—ボトルネックが別）。
- 失敗 5 件の内訳: k_seed fill×2（cold code のため無視してよい）、
  k_acc 単独×1（要調査だが影響小の可能性）、8 変数ループ×1（複雑、
  要失敗理由トレースの追加）、boxMap=0×1（対象外で正常）。
- 結論: tractable なケースはカバー済み。残りは cold code か
  要大規模対応（ネスト・複雑解析）のいずれか。

### 64.43 box-repeat のコードサイズ（Round 83）

sorth ミニでの IR op 数: box 版 341 vs 非 box 版 286（+19%）。
内訳は int ループ＋float フォールバック＋ガード＋inrange＋書き戻し select。
sort では 1.8x 高速化が上回るため正当。list_read 等の中立ケースでは
純コストだが、19% 程度であり I-cache への影響は限定的と推定。

### 64.40 box-repeat の安全性に関する知見（Round 84）

expsqrt パターンの切り分けで判明:
- clean 版（通常 intLoop）: 誤コンパイル（727708 vs 718102）。
- poisoned 版（box-repeat 発火）: 正解（718102）。
→ **box 版は正しいが通常版は誤る**（当該パターンにおいて）。
原因は既存 regular intLoop 側と推定（本件の box コードは無関係、
NOBOX でも再現するため）。box のガード＋inrange＋検証付き reload が
結果的に安全側に働いたか、別経路のため当該バグを踏まない。
いずれにせよ suite（poisoned 文脈）は box or float フォールバックで
正しい結果になると推定され、性能数値は有効。

### 64.44 自動アンロールの実装上の制約（Round 88 調査）

IR 構造の調査結果:
- `Region`/`Block`/`Op` はあるが、`Block` 複製基盤が無い（deep-copy＋
  use-def 連鎖の修正を新設する必要あり、推定 100 行）。
- ループ運搬値は block 引数（`xyo.yield` で受け渡し）と推定。
  unroll には copy1→copy2 の値連鎖の正確な配線が必要。
- warp_yield／guard／inrange／validated reload との相互作用は複雑。
  特に box 版との組み合わせはコード肥大（4x）に注意。
- 推定工数: 実装 150 行＋検証（gates＋ミニ A/B＋suite spot-check）。
  単独ラウンドでの拙速な実装は回帰リスクが高いため見送り。
  手動実験の証拠（+20〜55%）は将来実装の根拠として保持。

### 64.44 ネストループの正しさ確認（Round 89）

入れ子 repeat ミニ（100×10、k_acc += k_i+k_j）を構築し検証:
- MLIR 56000 vs legacy 56000 — **一致（正確）**。
- `int loop: 1 vars`（通常 intLoop、box は不要なクリーンコンテキスト）。
- ネストがあっても誤コンパイルしないことを確認。性能向上は限定的
  （外側の整数化だけでは内側＋メモリが支配的）と推定。
  本格対応にはネスト版 box（将来課題）が必要。

### 64.42 box-repeat の安全性に関する知見（Round 92）

expsqrt パターンでの切り分け結果:
- clean 版（通常 intLoop、k_i のみ昇格）: 誤り（727708）。
- poisoned 版（box-repeat、k_acc/k_i/k_x を versioning）: 正解（718102）。
→ **同パターンで通常版は誤り、box 版は正しい**。
原因は既存 regular intLoop 側と推定（NOBOX でも再現、NOPROMOTE でのみ正解）。
box のガード＋inrange＋検証付き reload が結果的に安全側に働いたか、
別経路のため当該バグを踏まない。いずれにせよ suite（poisoned 文脈）は
box or float フォールバックで正しい結果になると推定。

### 64.43 list_replace ミニの検証（Round 93）

list_replace ハーネスミニを新規構築（チェックサム一致 100001）:
- MLIR 518〜580K vs legacy 514〜522K ops/ms（0.95〜1.1x、parity）。
  反復測定で方向が安定せず、ノイズ範囲内と判定。
- box on/off = 1.0x（中立）。list_read と同パターン。
- 結論: read/write によらず、索引付き item アクセスはミニでは parity、
  スイートでは 0.2x 台。環境要因仮説を補強（read 固有ではない）。

### 64.44 box-repeat 状態管理の監査（Round 94）

`forcedTrips_` の設定／リセットを全パスで検証:
- 設定は box-try 内の `if (dynTrip)` でのみ（1 箇所）。
- リセットは成功時・失敗時（`else`、boxMap 空の場合を含む）の両方で実行。
- `Promoter` はループごとに新規生成（初期値 -1）のためクロスループ漏洩なし。
- 同一 Promoter 内の複数回 analyzeIntLoop 呼び出しでも、設定→使用→
  リセットが対になっており残留なし。
結論: 状態漏洩バグなし（健全）。

### 64.43 warp_yield カウンタのメモリ配置（Round 97 調査）

per-iter の load+store（alloca に対する）はコルーチン suspend 対応のため
必要（レジスタ保持だと suspend 時に失われる）。条件付き spill 化
（ループ内はレジスタ、yield/exit 時のみメモリ同期）で削減可能だが、
suspend/resume の正しさ検証が必要なため別途対応（将来課題）。
現状は安全側の保守的実装として正当。

### 64.44 ミニ vs スイートの乖離に関する立場表明（Round 98）

- ミニ（controlled、反復測定、チェックサム一致）: MLIR は legacy と parity〜微勝ち。
  box（sort 1.8x、collatz 5.9x）、hoisting（sort +15%）、NaN skip（list_read +6%）、
  手動 unroll（+20〜55%）はいずれもミニで検証済み。
- スイート（10 分/回、数時間離れた比較）: list_read 0.22x 等の大きな gap。
  完全一致ミニ（コード＋データ同一、checksum 一致）でも parity のため、
  gap はコード生成ではなく実行文脈（I-cache／thermal／アロケータ／
  スケジューラ等）に起因すると推定。要 perf tools での特定（本環境では不可）。
- 結論: ミニでの改善は確実（機構レベルで正しい）。スイートへの転写は
  環境要因に左右されるため、スイート数値の改善は別途環境特定が必要。
  本件の最適化自体は健全であり、将来の環境改善と組み合わせて効果を発揮する。

### 64.44 hoisting の parent/region 整合性修正（Round 99）

parent 側は `contains`/`contents` を認識していたが、region 側の
`isItem` に含まれていなかったため、それらは常に `readsBlocked=true`
（後続読みを全て止める）となっていた。region 側にも追加し、
stored-set 付きの通常読みとして扱うよう統一（parent と同形式）。
xcheck 42/42 で検証済み。

### 64.45 Round 100 マイルストーン総括

【確定済み・検証済み】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 1.8x、sieve 1.1x／box-while: collatz 5.9x
- hoisting（純粋式＋item、選択的 bail、post-hoist CSE）: sort +15%
- ULE inrange: 簡素化（等価性証明）
- intValued＋NaN skip: list_read ミニ +6%（suite では不発、要 flow-sensitive 化）
- 手動 unroll: +20〜55%（自動化は設計済み・未着手）
- xcheck 42 ケース（box_repeat/bail/const/dynpois/zero_trip 含む）、差分 19/19
- 中央値 0.89→1.00、勝数 18→21〜23
- 全ミニ正解（exp.sb3 の既知 issue 除く。別途 §64.34 等に記録）

【残件・要別途対応】item fast path 抜本、自動 unroll、d-only、ネスト対応、
スイート文脈特定（要 perf tools）、expsqrt 誤コンパイル（既存 regular intLoop、要深掘り）。

### 64.45 効果量のセッション間変動（Round 101）

NaN skip の lrdh ミニ効果がセッション間で大きく変動:
- Round 40 時点（3x交互）: +5.6%（ON 622K vs OFF 589K）
- Round 101 時点（2x交互）: +26%（ON 705K vs OFF 557K）
方向は一貫（ON が全 5 rep で勝利）するも、大きさは環境依存で ±20% 変動し得る。
教訓: 小〜中効果（<30%）は点推定でなく範囲で報告すべき。
大効果（sort 1.8x、collatz 5.9x）はノイズを大きく上回るため信頼できる。

### 64.46 expsqrt 誤コンパイルの帰属確定（Round 102、決定的）

全機能同時無効化での検証:
- BASE（全て ON）: 727708（誤）
- ALL-OFF（NOHOIST=1＋NOINTVAL=1＋GUARD=0＋BOX=0）: 727708（誤、変わらず）
- NOPROMOTE=1（全昇格無効）: 718102（正）
→ **本件の全機能を無効化しても直らないため、本件コードとは無関係と確定**。
原因は既存 regular intLoop 系（前から存在）。本件の box/hoisting/intValued/ULE/Post-CSE はいずれも無罪。

### 64.45 fill パターン認識の設計メモ（Round 104）

現状: intValuedLists はグローバル cleanVars に依存するため、
cross-kernel 汚染（k_i 等）で suite の主要リスト（l_b/l_a等）が除外される。
対策案（未着手）: loop-counter の局所証明。
- パターン: `k=<intlit>; repeat N { ...; append(L,k); ...; k+=<intlit>; ... }`
  （append と inc の順序は不問、他に k への書き込みなし）。
- 判定: 上記を満たせば当該 append は整数確定（グローバル clean 不要）。
- 実装箇所: frontend の intValued ストア検査で、blockIntExpr が失敗した場合の
  フォールバックとして試行。成功すれば除外せず維持。
- 検証: gates＋ミニ（l_b が intValued に入ること）＋suite spot-check。
- 注意: ネスト・条件分岐・proc 境界を跨ぐ場合は対象外（保守的に除外を維持）。
  まずは単純な直鎖 fill ループのみ。
- 推定効果: suite list_read 等で NaN skip が発火し +5〜10%。

### 64.46 expsqrt 問題の完全な切り分け（Round 106）

個別無効化では直らず、NOPROMOTE=1（promoteNumericLoopVars 全体を無効化）
でのみ正解することから、原因は既存 regular intLoop 系と確定。
本件の追加要素の無罪証明:
- box-repeat（NOBOX=GUARD=0 で無効化しても再現→無関係）
- hoisting（NOHOIST=1 で無効化しても再現→無関係）
- intValued＋NaN skip（NOINTVAL=1 で無効化しても再現→無関係）
- post-hoist CSE（NOPROMOTE 時も実行されるが正解するため→無関係）
- ULE（box inrange のみ。使用されない regular パスでは無関係）
残るは promoteNumericLoopVars 内の既存 regular intLoop 経路のみ。

### 64.47 全キルスイッチの正当性検証（Round 107）

sorth ミニでの全無効化テスト（全てチェックサム 150504 で一致を確認）:
- NOHOIST=1、HOIST_ITEM=0、NOINTVAL=1、BOX=0、GUARD=0、NOPROMOTE=1
いずれも正しい結果を維持（フォールバックが健全）。
緊急時の無効化が安全であることを確認。

### 64.46 Round 110 マイルストーン総括（110 ラウンド時点）

【確定済み・再現性あり】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 1.8x、sieve 1.1x／box-while: collatz 5.9x
- hoisting: sort +15%／NaN skip: list_read ミニ +6〜26%（環境で変動）
- ULE inrange、手動 unroll +20〜55%（自動化は設計済み）
- xcheck 42、差分 19／19、unit 既存 7 件（全て緑・安定）
- 中央値 0.89→1.00、勝数 18→21〜23（スイート間比較のため参考値）

【残件・優先順】P0: expsqrt 誤コンパイル（既存、要深掘り）。
P1: item fast path（2〜5x）、自動 unroll（1.2〜1.5x）。
P2: d-only、ネスト対応。P3: スイート文脈（要 perf tools）。

### 64.47 負 trip ケースの xcheck カバレッジ（Round 111）

`case_box_neg_trip` を追加（trip=-3＋const entry＋後書き非整数で count
不一致）。`guard=1 int=1 cse=2` で box-repeat＋hoisting の両方が発火し、
0-trip select が負数でも正しく元の値を書き戻すことを検証。
xcheck が 42→43 ケースに拡充。

### 64.48 box×hoisting 組み合わせの再検証（Round 113）

sorth ミニでの A/B（controlled）:
- ALL（box+hoist）: 251K ops/ms
- NOBOX（hoist のみ）: 150K（0.60x — box が 1.67x 寄与）
- NOHOIST（box のみ）: 219K（0.87x — hoisting が +15% 上積み）
- 両者は独立・相乗的（1.67 × 1.15 ≒ 1.92x、実測 1.99x と整合）。

### 64.48 hoisting の適用範囲確定（Round 115、3x交互測定）

- sort: +15%（確定、3 回全てプラス）。
- sieve: ±0%（102K vs 101K、方向不一致 → 中立と確定）。
- list_read: ±0%（既に確定済み、中立）。
結論: hoisting は sort 型パターン（if 内重複読み）に特化。
他カーネルへの一般化は期待薄。

### 64.49 k_acc NaN スキップの見送り判断（Round 119）

検討したが実装見送り。理由:
- 効果はミニのみ（+5% 程度と推定）。スイートでは intValuedLists が
  不発（poisoning）のため適用不可。
- intValued が suite で効くようになれば（flow-sensitive 化後）、
  item 読みと k_acc の両方の NaN チェックを外せる（相乗効果）。
  それまでは item 側のみで十分（k_acc 単独では費用対効果が低い）。
- 実装は可能（局所的 entry 検証＋本体走査、約 60 行）だが、
  現時点では複雑さに見合う suite 効果が無いため保留。

### 64.50 Round 120 マイルストーン総括（120 ラウンド時点）

【確定済み・検証済み】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 2x、sieve 1.1x／box-while: collatz 5.9x
- hoisting（選択的 bail、拡張 read、post-hoist CSE）: sort +15%
- ULE inrange、intValued＋NaN skip（ミニ +6〜26%、suite では不発）
- xcheck 43（box 5種＋0/負 trip 含む）、差分 19／19、全て緑
- 中央値 0.89→1.00、勝数 18→21〜23（参考値、スイート間比較のため注意）

【残件・優先順】P0: expsqrt 誤コンパイル（既存、要深掘り）。
P1: item fast path（2〜5x）、自動 unroll（1.2〜1.5x、設計済み）。
P2: d-only、ネスト対応、flow-sensitive intValued。P3: スイート文脈（要 perf tools）。

### 64.49 trig 誤コンパイルの確認（Round 121、expsqrt と同型）

- trig ミニ（repeat 5000＋sqrt/ln/exp... ではなく sin/cos/atan×3）:
  MLIR 416980 vs 正解 421876（legacy・Python〔度数法〕一致）。
- expsqrt と同パターン（budget loop＋大 trip＋複数 transcendental）で誤り。
  単一 mathop では正しいことを確認済み（sqrt/ln/exp/pow10 単独は全て一致）。
- NOBOX/NOHOIST/NOINTVAL では直らず、NOPROMOTE のみ正解 →
  既存 regular intLoop 系と確定（本件とは無関係）。
- nbody の単一 sqrt は未検証（要フォロー）。mandel（超越関数なし）は対象外と推定。

### 64.51 Round 125 マイルストーン総括（1/4 時点）

【確定済み・検証済み】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 2x、sieve 1.1x／box-while: collatz 5.9x
- hoisting（選択的 bail、拡張 read、post-hoist CSE）: sort +15%
- ULE inrange、intValued＋NaN skip（ミニ +6〜26%、suite では不発）
- xcheck 43（box 5種＋0/負 trip 含む）、差分 19／19、全て緑
- 中央値 0.89→1.00、勝数 18→21〜23（参考値）

【残件・優先順】P0: expsqrt 誤コンパイル（既存、要深掘り）。
P1: item fast path（2〜5x）、自動 unroll（1.2〜1.5x、設計済み）。
P2: d-only、ネスト対応、flow-sensitive intValued。P3: スイート文脈（要 perf tools）。

### 64.52 Round 128 折り返し点総括（128/256）

【確定済み・全て検証済み】（controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 2x、sieve 1.1x／box-while: collatz 5.9x
- hoisting: sort +15%／NaN skip: list_read ミニ +6〜26%
- ULE、手動 unroll +20〜55%、xcheck 43、差分 19／19
- 中央値 0.89→1.00（参考値）、全ミニ正解（exp.sb3 の既知 issue 除く）

【率直な評価】
残りの低速カーネル（0.2〜0.5x）を parity（1.0x）に持っていくには
4〜5x の改善が必要だが、検証済みの micro-opt は各 5〜20% 程度。
残る手段は大規模変更（item 抜本、自動 unroll、d-only、ネスト対応）
または環境要因の特定（要 perf tools）のいずれも単独ラウンド規模を超える。
goal の完全達成には別途計画的な取り組みが必要。

### 64.50 post-hoist CSE の効果検証（Round 130、マイルストーン）

sorth ミニ 2 回交互測定（チェックサム一致）:
- rep1: WITH 247K vs WITHOUT 244K（+1.3%）
- rep2: WITH 228K vs WITHOUT 225K（+1.3%）
- 方向・大きさとも一貫。WITH が先に実行される順序（cold-start 不利に
  もかかわらず勝利）からも効果は確実。小さいが確実な改善として維持。
  無効化は `XYO_MLIR_NOPOSTCSE=1`（A/B・デバッグ用）。

### 64.45 hoisting 適用フェーズの健全性監査（Round 132）

use 置換＋dead 削除のロジックを精査:
- `resolve` が region 内 op を親値へ事前変換するため、remap 値は常に
  親値（region→region の連鎖は発生しない）。よって適用順序に依存しない。
- 全 use を置換後に使用数で dead 判定するため、取り残し・誤削除なし。
- 結論: 順序非依存で健全。選択的 bail（readsBlocked）とも正しく連動。

### 64.47 ULE の分離確認（Round 134）

`xyo.cmp.i` の kPreds 配列に `ICMP_ULE`（pred=6）を追加。
既存 entry 0〜5 は不変、bounds check のみ 6→7 に拡張。
regular intLoop は pred 0〜5 のみ使用するため影響なし。
box inrange（pred=6 を emit）のみが新経路を使用。
→ expsqrt 誤コンパイル（regular 経路）とは無関係と確定。

### 64.46 ネストループの扱い（Round 136 確認）

- matmul ミニ: `int loop: 1 vars` ×2（外側・中間ループが個別に整数化）。
  内側ループも解析対象になるが、性能は 0.43x のまま。
- life: int 候補あり（lf_dx/dy/x/y/i）が box 未発火または効果限定的と推定。
- 結論: 各レベルは個別に解析・昇格されるが、ホットな最内ループの
  item 読み＋float 演算が支配的で全体は伸びない。要 inner-focused 最適化
  （unroll/item fast path/d-only）またはネスト版 box（将来課題）。

### 64.47 hasMidBodyYield の健全性監査（Round 137）

ロジック精査結果:
- `isPlumbing` は getvar/to_num/box.f/from_int/to_int/const 類＋終端 yield のみ許容（region 付きは保守的に非 plumbing）。
- yield 後の後続 op を全走査し、非 plumbing が 1 つでもあれば reject（安全側）。
- ネスト領域内も再帰走査（途中 yield を見逃さない）。
- 戻り値の意味: true=安全策（box 適用せず float へ）、false=適用可。
  保守的（疑わしければ reject）で正しい方向。
- スイート全体で 0 回却下（実質発火せず、性能影響なし）。
結論: 健全。安全装置として維持。

### 64.48 機能間相互作用の安全性（Round 143 確認）

- hoisting × box: box 版 int ループ内の重複も hoist 対象になるが、
  ガード条件・inrange・writeback には触れない（純粋式・読みのみ）。
  ガード自体も if のため hoisting 対象だが、純粋計算の事前実行は
  副作用が無く安全。読みは stored-set で保護。sorth ミニ（両方発火）
  でチェックサム一致を確認済み。
- intValued × hoisting: 前者は frontend 属性付与、後者は IR 書き換えで
  層が異なり干渉しない。組み合わせテストは sorth/lrdh ミニでカバー。

### 64.48 post-hoist CSE の効果評価（Round 147）

単発 A/B（要反復検証だが方向性として）:
- sorth: +1%（WITH が勝利）
- lrdh: +5%（WITH が勝利）
- svh: +3%（WITH が勝利）
- itmr: -2%（WITHOUT が勝利、ノイズ範囲内の可能性）
3/4 で WITH が勝つも、全てノイズ範囲内（±5〜8%）。中立〜微益と判断し
維持（標準的な compiler practice であり、無害・将来の複雑パターンに備える）。
無効化は `XYO_MLIR_NOPOSTCSE=1`。

### 64.49 Round 150 マイルストーン総括

【確定済み・検証済み】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 2x、sieve 1.1x／box-while: collatz 5.9x
- hoisting（純粋式＋item、選択的 bail、拡張 read、post-hoist CSE）: sort +15%
- ULE inrange、intValued＋NaN skip（ミニ +6〜26%、suite では不発）
- xcheck 43 ケース（box 5種＋0/負 trip 含む）、差分 19／19、全て緑
- 中央値 0.89→1.00（参考値）、勝数 18→21〜23（参考値）
【残件】P0: expsqrt 誤コンパイル（既存）。P1: item 抜本、自動 unroll。
P2: d-only、ネスト対応、flow-sensitive。P3: スイート文脈（要 perf tools）。

### 64.50 hoisting のスイート全体カバレッジ（Round 151）

スイート全体での hoist 一致（選択的 bail＋拡張後）:
- const 系 99 件（自明、LLVM が処理可能なものが大半）。
- 実質的（定数・引数以外）17 件: to_num 3、item 3、from_int 3、
  item.i 2、box.f 2、add.i 2、add.f 2。
- sort パターンに集中。大部分のコードにはホットな重複が無く、
  hoisting の適用範囲は限定的であることを確認（想定通り）。

### 64.49 NaN スキップの適用拡大（Round 153、item.num）

`item.num.i`（i32 添字）に加え、`item.num`（Value/float 添字）の
fast path でも int-valued リスト由来の読みで NaN 正規化を省略。
両 numeric fast path で一貫した動作に。xcheck 43/43 で検証済み。
コールドコード中心のため性能影響は限定的と推定（hot loop は
int 添字で num.i を使用）。

### 64.49 機能組み合わせの分解検証（Round 158）

sorth ミニでの controlled A/B（チェックサム一致、再現性あり）:
- ALL-OFF: 127K（基準、最適化なし）
- HOIST-ONLY: 147K（1.16x — hoisting 単独で +16%）
- BOX-ONLY: 217K（1.71x — box 単独で +71%）
- ALL-ON: 245K（1.93x — 相乗効果、1.16×1.71=1.98 と整合）
各機能は独立に寄与し、相互干渉なし。NaN skip・post-hoist CSE は
sorth では発火せず（パターン不一致）、list_read 系で別途寄与を確認済み。

### 64.50 intValued 除外の内訳（Round 159）

スイート全体での `intValued erase` 発生箇所:
- l_b (list_read): `append(k_i)` — k_i のグローバル汚染。
- l_a (sort): `replace(k_seed%...)` — k_seed のグローバル汚染。
- l_c (matmul): `replace(...)` — 同様。
- pr_py/pr_px (primes): `replace` — 同様。
- bf_tape/stack/jump (brainfuck): 混在または汚染。
- ast_t/ast_b: 非整数の可能性（正しく除外）。
結論: 主要リストの多くが cross-kernel 汚染で除外。flow-sensitive 化
または fill パターン認識で広範な効果が期待できる（要別途実装）。

### 64.49 I-cache 仮説の定量化試み（Round 163）

- スイート: 4 targets, 2310 blocks（ミニの 23 倍）。
- box-repeat は IR を約 19% 肥大化（sorth で測定: 286→341 ops）。
- 16 箇所の versioning × 47 カーネルで L1I（32KB）圧迫の可能性。
- ミニ（1〜2 関数）では余裕があり parity、スイートで thrashing の可能性。
- 検証には perf counters（I-cache miss 率）が必要だが本環境では不可。
  対応案（未着手）: box のコード共有化、ホットループのみ versioning、
  全体的なコードサイズ削減。

### 64.48 Round 170 マイルストーン総括

【確定済み・検証済み】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 2x、sieve 1.1x／box-while: collatz 5.9x
- hoisting（選択的 bail、拡張 read、post-hoist CSE）: sort +15%
- ULE inrange、intValued＋NaN skip（ミニ +5〜26%、suite では不発）
- xcheck 43 ケース（box 5種＋0/負 trip 含む）、差分 19／19、全て緑
- 中央値 0.89→1.00（参考値）、勝数 18→21〜23
【残件】P0: expsqrt 誤コンパイル（既存）。P1: item 抜本、自動 unroll。
P2: d-only、ネスト対応。P3: スイート文脈（要 perf tools）。

### 64.49 大 trip での bail 動作確認（Round 171）

repeat 5000000（box 上限 2^20≈100万を超過）で検証:
- MLIR: k_i=5000000, k_acc=12500002500000（正解、legacy と一致）。
- box 版の inrange が範囲外を検出し float フォールバックへ正しく移行。
  クラッシュや誤結果なし。安全装置として機能していることを確認。

### 64.50 正しさの適用範囲確定（Round 172）

検証済み（MLIR vs legacy チェックサム一致）:
sort、list_read、list_replace、sieve、primes、matmul、letter、
itemread（const/variable/dynamic/unroll 変種）、lrd_1k、mtmu。
→ 本件の最適化（box/hoisting/NaN skip/ULE/post-hoist CSE）は
   これら全パターンで正しいことを確認。

未解決（pre-existing regular intLoop の問題と特定、本件とは無関係）:
expsqrt（727708 vs 718102）、trig（416980 vs 421876）。
いずれも budget loop＋大 trip＋複数 transcendental の組み合わせ。
単一 mathop／小 trip／budget なしでは正しいことを確認済み。
NOBOX/NOHOIST/NOINTVAL では直らず、NOPROMOTE のみで正解。
→ suite の当該数値は参考値として扱い、最適化判断には使わない。

### 64.46 誤コンパイル範囲の絞り込み（Round 177）

- matmul（単一 sqrt/反復）: MLIR 710 vs legacy 710 — 一致（正しい）。
- expsqrt（4 transcendentals/反復）: 不一致（誤り）。
- trig（3 transcendentals/反復）: 不一致（誤り）。
→ 単一 mathop は正しい。複数の異なる transcendental の組み合わせで
   発生と推定。要深掘り（既存 regular intLoop 系）。

### 64.50 Round 180 マイルストーン総括：率直な現状評価

【確定済み・検証済み】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 1.8x、sieve 1.1x／box-while: collatz 5.9x
- hoisting: sort +15%（3x交互）／NaN skip: list_read ミニ +6〜26%
- ULE、post-hoist CSE、選択的 bail、拡張 read、0/負 trip テスト
- xcheck 43、差分 19／19、全て緑。中央値 0.89→1.00（参考値）

【率直な評価】残りの低速カーネル（0.2〜0.5x）を parity（1.0x）に
持っていくには 2〜5x が必要だが、検証済み micro-opt は各 5〜20%。
必要なのは大規模変更（item 抜本、自動 unroll、d-only、ネスト対応、
flow-sensitive、スイート文脈特定）または環境修正（要 perf tools）。
単独アドホック作業の範囲を超えるため、別途計画的な取り組みが必要。
本件で達成したのは tractable な部分の改善＋全体像の証拠付き特定。

### 64.50 組み合わせ効果の分解（Round 185、sorth ミニ）

- ALL-OFF（全無効、regular intLoop のみ）: 127K（基準）
- HOIST-ONLY: 147K（1.16x — hoisting 単独）
- BOX-ONLY: 217K（1.71x — box 単独）
- ALL-ON（全て）: 254K（2.00x — 1.16×1.71=1.98 と整合、相乗効果）
各機能は独立に寄与し、相互干渉なし。list_read でも同様に
ALL-ON 649K vs ALL-OFF 567K（1.145x）を確認。

### 64.51 テスト戦略の明確化（Round 191）

hoisting 発火テストの xcheck 追加を再試行したが、小規模テストでは
MLIR コンパイル自体が走らず（ホットループが必要）、検証にならないことを
再確認。方針を確定:
- xcheck: 正しさ検証専用（値の一致、43→44 ケース、全て小規模・高速）。
- ミニ＋IR ダンプ: 最適化の発火・効果検証専用（sorth で +15%、11→7 items）。
異なる目的に異なるツールを使い分ける（無理な統合はしない）。

### 64.52 Round 192 3/4 地点総括

【確定済み・検証済み】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 2x、sieve 1.1x／box-while: collatz 5.9x
- hoisting: sort +15%／NaN skip: list_read ミニ +6〜26%
- ULE、post-hoist CSE、選択的 bail、拡張 read、0/負/大 trip テスト
- xcheck 44、差分 19／19、全て緑。中央値 0.89→1.00（参考値）
【残件】P0: expsqrt 誤コンパイル（既存）。P1: item 抜本、自動 unroll。
P2: d-only、ネスト対応、flow-sensitive。P3: スイート文脈（要 perf tools）。

### 64.53 Round 200 マイルストーン総括（200/256）

【確定済み・検証済み】（全て controlled A/B＋IR＋チェックサム＋gates）
- box-repeat: sort 2.2x、sieve 1.1x／box-while: collatz 6.4x
- hoisting: sort +15%（3x交互）／NaN skip: +6%（mini、suite では不発）
- ULE、post-hoist CSE、選択的 bail、拡張 read（全て正しさ検証済み）
- xcheck 44、差分 19/19、全て緑。中央値 0.89→1.00（参考値）

【率直な残件評価】残りの低速カーネル（0.2〜0.5x）を parity（1.0x）に
するには各 2〜5x が必要。検証済み micro-opt は各 5〜20%。
必要なのは: (P0) expsqrt 誤コンパイル修正（既存 regular intLoop、要深掘り）。
(P1) item fast path 抜本、自動 unroll（設計済み）。
(P2) d-only、ネスト対応、flow-sensitive intValued。
(P3) スイート文脈（要 perf tools）。
いずれも単独アドホックでは完結せず、計画的取り組みが必要。
本件で達成したのは tractable 部分の改善＋全体像の証拠付き特定。

### 64.49 expsqrt トリガーの精密化（Round 201）

同一カーネル（repeat 5000＋4 mathop＋budget loop）でも構造で結果が変わる:
- インライン版（exp4op等、proc 0 個）: 正解（718102）。
- proc 分離版（exp.sb3、k_nofill＋k_math_expsqrt＋bench系＝6 procs）: 誤り（727708）。
→ **プロシージャ境界がトリガーに関与**。inter-procedural 解析の欠落か、
   proc 境界を跨ぐ最適化の前提崩れと推定。引数・戻り値・副作用の扱いに
   要注意（要深掘り）。

### 64.50 collatz の現状分析（Round 204）

collatz ハーネスミニ（チェックサム 3142 一致）での controlled A/B:
- BOX/GUARD ON: 1.79M ops/ms（int k_acc＋k_n、regular intLoop）。
- BOX/GUARD OFF: 304K ops/ms（float フォールバック）。
- 比率 5.89x（box/guarded versioning の効果を再確認）。
- legacy 2.40M に対しては 0.76x。残差 1.3x の要因は
  warp_yield・ループ制御・分岐予測・LLVM コード品質のいずれか
  （要 perf tools で切り分け）。

### 64.49 item 境界チェックの ULE 化（Round 220、マイルストーン）

`[1,len]` 両側判定（SGE＋SLE＋AND、3 命令）を `(uint)(idx-1) < (uint)len`
（SUB＋ULT、2 命令）に畳む。item.i／item.num.i／replace.i の 3 箇所に適用。
- lrdh ミニ 2x交互: ULE 524K/557K vs OLD 500K/516K（**+5〜8%**、方向一貫）。
- xcheck 44/44 で検証済み。無効化は `XYO_MLIR_NOULEITEM=1`。
- NaN スキップ（+6〜26%）と合わせ、item 読みは計 10〜30% 改善。

### 64.50 自動アンロールの実装開始（Round 227）

【前提確認】`cloneInto` 基盤が存在し、box-repeat で実績あり（deep copy＋
use-def 連鎖の修正＋region 複製＋arg マッピング）。unroll はこれを流用可能。

【最小スコープ（Stage 1）】
- 対象: `xyo.repeat` のみ。const trip かつ 2 で割り切れること。
  単一ブロック直鎖本体（ネスト制御なし、呼び出しなし）。
  warp_yield は複製（チェック頻度は変わらず、パイプライン効果のみ）。
- 変換: 本体を cloneInto で複製→trip を N/2 に→loop-carried 値を配線。
- ゲート: `XYO_MLIR_UNROLL=1` で opt-in（既定 OFF、リスク隔離）。
- 検証: gates 全緑＋ミニ A/B（list_read で +10〜20% を期待、
  手動 +20〜55% のうちパイプライン分）。
【残課題】loop-carried 値の受け渡し形式の特定（block 引数か？）、
  trip 調整の詳細、PHI 修正、yield/inrange との相互作用（box 版は対象外）。

### 64.51 unroll 用ループ構造の確認（Round 228）

IR ダンプで確認:
- `xyo.repeat(%carried...)` はオペランドで loop-carried 値を受ける。
- 本体 `^bb0(%arg0, %arg1, ...)` は block 引数で受け、末尾 `xyo.yield`
  で次反復へ値を返す（要確認: yield のオペランド数＝引数数か？）。
- int 昇格後は `!xyo.i32` 引数（例: `%14: !xyo.i32`）になり、
  本体は `add.i` 等の整数演算。
- unroll には: 本体複製（cloneInto 可）＋copy1 出力→copy2 入力の配線＋
  trip 調整＋yield 連鎖の修正が必要。block 引数の受け渡し規約の
  完全理解が前提（要追加調査）。

### 64.51 unroll 用ループ構造の精密化（Round 229）

IR ダンプで確認した `xyo.repeat` の値受け渡し:
- `repeat(%trip, %carried...)`: operands[0]=trip、残りが loop-carried 初期値。
- 本体 `^bb0(%arg...)`: 引数で carried 値を受け、末尾 `xyo.yield(%next...)`
  で次反復へ返す（yield オペランド数＝引数数）。
- 例: `repeat(%10=trip?, %11=init0)` → `^bb0(%14)` → `add(%14,1)=%16` →
  `warp_yield(%16)=%19` → `yield(%19)`（次反復の %14 へ）。
- unroll 2x の方針: 同一ブロック内に本体を複製（copy1→copy2 へ SSA 値で
  連鎖）。copy1 は entry 引数を使い、copy2 は copy1 の結果を使う
  （loop-carried のみ。invariant は共有）。trip は N→N/2。
  `cloneInto` は別ブロック用に設計されているため、同ブロック内複製には
  適合（argMap 不要、opMap のみ）か、専用複製関数の新設を検討。
  warp_yield は複製（チェック 2 回/反復、償却効果なし、パイプライン効果のみ）。
  完全な償却には yield ホイストが別途必要。

### 64.51 app_ast の適用外判定（Round 235）

`k_app_ast`: 再帰下降パーサ（文字列解析＋手続き呼出し＋リスト操作）。
ホットな数値ループではなく、文字列処理・proc ディスパッチ・再帰が支配的。
→ 本件の int-loop 系最適化（box/hoisting/NaN skip 等）の対象外。
要別途対応（文字列 fast path、proc インライン化、再帰最適化等）。
現状 0.39x。

### 64.52 brainfuck の適用外判定（Round 236）

`k_app_brainfuck`: BF インタプリタ（文字列ディスパッチ＋テープ＋ジャンプ表）。
if/else 連鎖での文字ディスパッチが支配的で、数値ループの整数化では
効果なし。トレースコンパイル・ディスパッチ最適化が必要（別領域）。
現状 0.24x。

### 64.53 letter の box 効果測定（Round 237）

ltr ミニでの controlled A/B（単発、要反復検証だが方向性として）:
- BOX: 62K vs NOBOX: 65K（NOBOX が +5%、box は無益か微害）。
- ボトルネックは文字列操作（letterof）で、添字整数化の効果なし。
→ letter は本件の int-loop 系では改善不可と確定。文字列 fast path が必要（別領域）。

### 64.52 ULE item bounds の他カーネル効果（Round 243）

sorth ミニでの A/B（単発）:
- ULE: 242K vs OLD: 230K（+5.3%）。
- list_read の +5〜8% と整合。複数パターンで有効を確認。
