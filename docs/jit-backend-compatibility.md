# JIT バックエンド互換性

## 判定基準

ここで実行互換は、同じ Scratch プロジェクトの結果、スケジューラ順序、ブロック副作用、型変換が一致することを指す。これは、全 opcode を WASM で JIT コンパイルできることとは区別する。WASM 側が対応しない命令は、共通 Scratch IR から生成されたコンパイル済み区間の間でも正確なインタプリタ実行へ切り替える（hybrid regions）。スクリプト全体が未対応である必要はない。

| 対象 | LLVM ネイティブ JIT | WASM HotSpot |
|---|---|---|
| HotSpot / Eager | 対応 | 対応 |
| 数値変数、算術、比較、if/if-else | 対応 | WASM へ変換 |
| operator_mathop（14演算） | 対応 | 4種はWASM命令、10種は既存数値ヘルパを共有 |
| 剰余・NaN 型変換 | ランタイム準拠 | ホスト fmod ヘルパ・NaN 正規化 |
| トップレベル repeat / while / until / forever（最大128個） | 対応 | 順次 WASM 再開 |
| 入れ子ループ、トップレベル129個以上 | 対応または互換ヘルパ | warp 手続き内の数値入れ子ループは **既定で nested WASM**（1呼出し1024反復、超過時 deopt）。それ以外の入れ子ループは Interpreter。129個以上は hybrid（数値 region のみコンパイル） |
| 静的ハット（グリーンフラグ・受信イベントなど） | 対応 | WASM 化 |
| edge-activated ハット | ランタイム判定 | Interpreter（ランタイムが毎フレーム条件評価。headless `run` も 2026-11-16 からスレッド終了後もポーリング） |
| 文字列・リスト・一般レポータ | 対応または互換ヘルパ | Valueタグ付き代入・join・letter_of・contains・listContents・文字列長・数値/文字列比較・混合型リストの検索・読取・追加・挿入・置換・削除をWASM化。random添字や未対応効果はInterpreter区間 |
| 手続き・再帰・引数・warp | 対応または互換ヘルパ | 型付きValue引数を持つwarp手続きと、yieldを含まない非warp手続きの一部をインライン展開。依存自己再帰（tail / prefix+if / 最小形、数値引数≤8・戻り先≤8）は1024フレームの燃料付きでWASM化し、超過時はInterpreterへ復帰。**非warpでもループ/待機を含まない同期再帰は同じ機構でコンパイル**。再帰引数は実行時に数値タグを検査し、非数値なら frame に格納せず bail する。yield を含む再帰は Interpreter |
| motion / pen / sound / looks / sensing | 対応または互換ヘルパ | 無副作用の `sensing_timer`・`sensing_dayssince2000`・`sensing_mousex`・`sensing_mousey`・`sensing_mousedown`・`sensing_loudness`・`sensing_username` はコンパイル対象。Runtime は **invoke ごとに publish/restore** され（greenFlag の即時ハット評価も含む）、import 27-32 が timer/mouse/loudness/username を読む。username は Runtime が所有・root する SStr を返す。`motion_setx` / `motion_sety` / `motion_changexby` / `motion_changeyby` /
`motion_pointindirection` / `motion_turnright` / `motion_turnleft` / `looks_setsizeto` / `looks_changesizeby` / `sound_setvolumeto` / `sound_changevolumeby` / `pen_clear` / `pen_penDown` / `pen_penUp` / `pen_setPenSizeTo` / `pen_changePenSizeBy` / `pen_setPenColorParamTo` / `pen_changePenColorParamBy` / `pen_stamp` / `pen_setPenColorToColor` / `looks_seteffectto` / `looks_changeeffectby` / `looks_cleargraphiceffects` / `looks_show` / `looks_hide` は **commit-only 端末効果** としてコンパイル（region の最後でのみ実行）。数値引数は primitive 境界の `ToNumber` と同じく NaN→0 に正規化する。その他の motion / pen / sound / looks 効果は Interpreter |
| wait / その他の yield | 対応 | Interpreter 区間で実行し、yield 後のトップレベル継続点から WASM 再開 |
| OSR（インタプリタ途中昇格） | 一部ループに対応 | 未対応 |
| 非同期コンパイル | 対応構成あり | 未対応 |
| ネイティブ AOT | 対応 | 未対応 |
| LLVM なしのビルド | 不可 | 対応（Emscripten） |
| ネイティブ Linux/Windows での WASM 実行 | 非該当 | 未対応（JS WebAssembly ホストが必要） |

## 実装済みの互換性保護

1. 未対応の opcode や不正なブロックグラフを WASM に誤変換しない。
2. 変数変更は一時領域で行い、WASM セグメントが正常終了してから反映する。
3. 型ガード失敗・WASM trap では書き戻さずInterpreterへ移行する。NaN/文字列比較そのものはホスト意味論で実行できる。
4. 順次トップレベルループの現在の添字・継続状態を Thread が保持し、途中の bailout 時に対象ループの StackFrame とカウンタを復元する。
5. round(NaN) は Scratch 同様に数値変換で 0 にしてから丸める。
6. 浮動小数点の剰余は既存ランタイムと同じ fmod を env.h 経由で呼ぶ。独自の近似剰余式は使用しない。
7. standalone WASM の剰余で精度を保証できない入力は bailout する。
8. 三角関数・対数・指数関数は LLVM JIT の既存数値実装を共有し、ホストヘルパのない standalone WASM では bailout する。

## 再現可能な検証

Emscripten の WASM HotSpot、ネイティブ LLVM JIT、Interpreter に同一 SB3 を実行させ、最終的な変数値を比較する。

~~~sh
python3 tests/differential/wasm_hotspot_integration.py \
  build-wasm-hotspot/xyo.js --out build-wasm-hotspot \
  --llvm build/xyo

python3 tests/differential/wasm_compat_fuzz.py \
  build-wasm-hotspot/xyo.js --llvm build/xyo \
  --out build-wasm-hotspot --cases 128
~~~

乱数生成試験の seed=42 で NaN 数値変換漏れ、seed=52 で剰余演算精度の不一致、複数ループ対応後の seed=6 でホスト剰余の一時領域が Scratch 変数に重なる不具合を検出・修正した。現在の試験では 512 シードで3方式の最終結果が一致した。

## 完全互換までの残作業

既存 LLVM codegen は Scratch ブロックから LLVM IR を直接構築する。WASM は数値・分岐・トップレベルに並ぶ最大128個のループに加えて、限定的な文字列・リスト・warp 手続きの対応を進めている。全機能を Binaryen へ移植するには以下が必要。

- GCルートと長寿命continuationを含む任意の副作用付きreporterをサポートする完全な共通Value ABI。文字列・混合型リストの基本操作は対応したが、レンダリングなどの複雑な処理は未対応。
- 任意深さの yield / procedure / warp / clone / stop を扱う共通継続状態。
- Interpreter と WASM の OSR、非同期コンパイル、guard / deopt 契約。
- LLVM と WASM 共通の型プロファイル、最適化、広範な ScratchBench 差分テスト。

現状は「未対応機能を安全に Interpreter 区間へ切り替え、後続の対応区間で JIT を再開する」段階である。「LLVM JIT と全く同じ機能を Binaryen でコンパイルする完全互換」には至っていない。


### ネストした手続き・反復

`XYO_NESTED_WASM` は **未設定なら warp-only で有効**（2026-11-16〜）。
インタプリタの手続き・反復スタックを保持したまま、対応する数値命令区間
だけをWASMに委譲する。`XYO_NESTED_WASM=0` で完全に無効化できる。
`XYO_NESTED_WASM=1` を明示した場合は従来どおり全候補が対象で、
`XYO_NESTED_WASM_WARP_ONLY=1` で副作用のない数値warp反復に限定できる
（未設定時の既定はこの warp-only）。このモードでは

ネストした repeat / repeat until / while / for each、条件分岐と
引数なし・非再帰warp補助手続き、末尾の自己再帰も共通の最大1,024反復予算で
単一WASM呼び出しにまとめる。末尾再帰は初期値が数値定数、再帰引数が数値演算、
最大8引数・8戻り先、最後のIF内から自己呼び出しする場合に限定する。非warp反復は各ティックのyieldを保持する。
数値if/if-elseの本体は軽量WASMで処理可能だが、非末尾再帰も、引数なしwarp手続きで再帰呼び出し箇所が1つ、数値処理のみの
「前処理・条件付き自己呼び出し・後処理」形に限り、戻り先を圧縮継続深度として
保持する。複数の戻り先、引数を持つ非末尾再帰、非warp再帰・任意の副作用や
待機はまだフォールバックする。文字列・リストを含む実験モードは `XYO_NESTED_WASM_ALL=1` を追加する。
warp ループは yield しないため、反復をまとめてもスケジューリングは変わらない。
100k 反復の数値 warp 入れ子ループでは WASM interpreter 0.099 秒 →
nested 0.087 秒（nested off 0.113 秒）を実測。小さいループでは昇格判定の
オーバーヘッドが勝つことがあるため、`XYO_NESTED_WASM=0` の逃げ道を残す。

詳細: [ネストしたWASM継続実行](nested-wasm-continuation-2026-11-07.md)

## 2026-11-16 再検証（LLVM 互換スイープ）

LLVM ネイティブ JIT と Emscripten WASM/Binaryen を同じ SB3 で実行し、
最終変数ダンプ・副作用回数・フォールバック境界を一括で突き合わせる
ハーネスを追加した。

~~~sh
python3 tests/differential/llvm_wasm_compat_sweep.py \
  build-wasm-be/xyo.js --llvm build/xyo \
  --addon build-wasm-be/binaryen-host.node \
  --out /tmp/compat --json /tmp/compat.json
~~~

`--quick` で fuzz seeds を縮小、`--only NAME` で単発実行できる。
Emscripten のビルドではネイティブ LLVM バイナリを指定して
`cmake --build <build> --target check-wasm-compat` からも実行できる:

~~~sh
emcmake cmake -B build-wasm-be ... \
  -DXYO_NATIVE_LLVM_XYO=$PWD/build/xyo
cmake --build build-wasm-be --target check-wasm-compat
~~~

addon 未指定時は Binaryen 固有スイートを SKIP する。

### 実測結果（このツリー、LLVM 23、Binaryen 130）

| スイート | 結果 |
|---|---|
| `wasm_hotspot_integration.py`（simple/conditionals/list/string/loops/broadcast/wait/unsupported） | PASS |
| `wasm_compat_fuzz.py --cases 128` | 128 seeds PASS |
| `wasm_string_semantics.py` / `wasm_tagged_list_semantics.py` | PASS |
| `wasm_list_extensions.py` / `wasm_string_clock_extensions.py` | PASS |
| `wasm_warp_procedure.py`（warp inline / nested repeat / list rollback / nonwarp leaf / loop fallback / recursion） | 8/8 |
| `wasm_return_pc_stack.py`（2-return-PC + 引数フレーム） | 10/10 |
| `wasm_nested_continuation.py`（opt-in nested） | PASS |
| `wasm_return_stack_fuzz.py --seeds 32` / `wasm_return_pc_fuzz.py --seeds 32` | PASS |
| `wasm_tail_recursion_fuzz.py --seeds 32` / `wasm_nested_tree_fuzz.py --seeds 48` | PASS |
| `runtime_{broadcast,broadcast_wait,looks,pen,clone}_journal.py` | 5/5 |
| `--jit eager`（simple/conditionals/numeric_list/string/wait） | LLVM と一致 |

合計 **18 スイート・0 不一致**。`startBroadcast` 系の副作用回数も
Interpreter/LLVM/builtin/Binaryen で同一だった。

### ハーネスの修正

`xyo-wasm-hot: selecting emitter=...` は lowering の**前**に出るため、
これだけで「コンパイルされた」と判定すると compile 失敗時も成功と
誤認する。`wasm_warp_procedure.py` と `wasm_list_extensions.py` は
実際の `compiled script=` / `hybrid script=` を要求するよう修正した。
この誤検出は「フォールバックしたが結果は一致」のテストを、コンパイル
経路の検証と誤って数えるため、互換性の証拠として不適切だった。

### 結果は一致するがコンパイル被覆が違う箇所

- 依存自己再帰は 2026-11-16 に WASM でもコンパイルするようになった。
  `SK::WarpTailRecurse` / `WarpReturnStack` / `WarpReturnPcStack` を
  HotSpot の whole-script / hybrid 双方で許可し、共通の1024フレーム燃料で
  実行する。超過時は `wasm return=1` で全 staged 書き込みを破棄して
  Interpreter に復帰する（`recursive_deep` で 2000 段を検証）。
  最小形（本体が `if (n>0) call self(n-1)` のみ）もコンパイルする。
- 残る非等価（結果は一致、コンパイル被覆の差）: ループ/待機を含む
  非warp再帰（yieldするため）、9戻り先以上、9引数以上の戻り先スタック。
  非数値の再帰引数は、コンパイル済みモジュールが実行時に数値タグを検査して
  bail し、Interpreter が同じ結果を出す（`recursive_string_arg`）。
- ホスト側は PC-stack 再帰のために `slotIds + 3 + recursionFrameWords`
  ワードを確保する（`HotCode::recursionFrameWords`、
  `AtomicWarpExecutionAbi::frameWords(実引数数)`）。以前はスカラー
  slot のみを resize しており、フレーム書き込みが領域外になっていた。
- したがって性能特性は同一ではない。LLVM は OSR・非同期コンパイル・
  ネイティブ AOT・並列スケジューラ・ネイティブ機械語を持ち、WASM は
  持たない。これらは従来どおり未対応として扱う。

### 観察: headless 全体パリティでの frame 進行差（2026-11-16）

`tests/fixtures/scratch-parity-test.sb3` を headless で `--fps 30
--frames 2400 --print-list output` 実行した場合:

- native Interpreter と WASM (Interpreter/builtin/Binaryen) は 2400 frame で
  完走し、いずれも **1899/2123** で出力も byte 一致（視覚系 224 件が
  renderer 無しで失敗する既知の headless プロファイル）。
- 同じ native バイナリの `--jit hotspot` / `--jit eager` は 2400 frame
  （hotspot は 9000 frame でも）で frame limit に達し、出力は 8 行のまま
  だった。SDL renderer 接続時の GUI パリティは 2123/2123 と記録されている
  ため、headless の compiled-mode 進行差として別途切り分けが必要。
  本スイートの 18 ケースはこの差の影響を受けない範囲で一致している。

WASM 側はこの差の影響を受けず、Interpreter と同じ frame 数で完走した。
原因（compiled loop の yield 回数か、headless の redraw/wait 境界か）は
未特定であり、LLVM 側の調査項目として残す。

### 2026-11-16: headless edge-activated hat

`when timer >` / `when touching` のような edge-activated ハットは
`Runtime::step()` が毎フレーム `startHats` して `Target` の前回値と比較する
実装だったが、headless の `run` ループは「アクティブスレッドが無い」と
`step()` を呼ばずに終了していたため、**フラグハット等が無いプロジェクトでは
一度も発火しなかった**。

`runLoaded` は実行対象に edge-activated ハットのスクリプトがある場合、
スレッドが終了してもフレーム上限まで `step()` を続けるようにした。
フレーム上限到達時にアクティブスレッドが無ければ `done`（終了コード0）と
して扱う。`tests/differential/edge_hat_activation.py` が
native interpreter/LLVM と WASM interpreter/builtin/Binaryen で
発火・非発火の両方を検証する。

### 2026-11-16: `sensing_timer`

`sensing_timer` を数値式としてコンパイルできるようにした。モジュールには
Runtime ポインタが無いため、`Runtime::step()` の先頭で
`ioDevices.clock.projectTimer()` をスナップショットし、host import 27 が
その f64 を scratch へ書く。ステップ中 `currentMSecs` は一定なので、
インタプリタが同じステップ中に読む値と一致する。`sensing_timer` は
`numericAssignmentSource`/host-math 判定にも追加済み。
`tests/differential/wasm_timer_reporter.py` が
`if timer > -1` / `if timer > 100000` / `repeat until timer > -1` /
`7 + timer*0` を LLVM/interp/builtin/Binaryen で比較する。

### 2026-11-16: mouse reporters

`Runtime::step()` は有効期間中だけ `Runtime*` を host に公開し（`xyo_wasm_set_host_runtime`）、
step 終了時に nullptr へ戻す。`sensing_mousex`/`sensing_mousey` は
`ioDevices.mouse.getScratchX/Y()`、`sensing_mousedown` は
`getIsDown()` を host import 28-30 経由で読む。mousedown は数値文脈では
0/1 の f64、タグが必要な文脈（join・boolean 代入）では `emitTaggedExpr` が
kTrue/kFalse へ変換する。`tests/differential/wasm_mouse_reporters.py` が
`set x to mouse x`、`mouse x + 7`、`if mouse down ... else`、
`if not mouse down` を4エンジンで比較する。

### 2026-11-16: `sensing_loudness` と nested センサー

`sensing_loudness` を host import 31 でコンパイル可能にした。オーディオ無しは
-1、有りはインタプリタと同じ `cachedLoudness`/`loudnessTimer` キャッシュを
共有する。また nested 数値プログラムの式ホワイトリストに Timer / MouseX /
MouseY / MouseDown / Loudness を追加し、入れ子 warp ループ内のセンサー条件も
（step 中に公開される Runtime* 経由で）nested WASM で評価できるようにした。
`tests/differential/wasm_loudness.py` が `set x to loudness`=-1、
`loudness+2`=1、`if loudness < 0`=42、`repeat until loudness < 0`=0回 を
4エンジンで比較する。

### 2026-11-16: commit-only 端末効果（motion / looks size）

数値のみの直線 region の**最後の文**に限り、副作用ブロックを WASM から
host import 40 で実行する。規則:

- 対象は `motion_setx/sety/changexby/changeyby`、
  `looks_setsizeto/changesizeby`、`sound_setvolumeto/changevolumeby`、
  motion の point in direction / turn right / turn left（`setDirection` 共有、
  direction reporter で検証可能）、
  pen の clear/down/up/stamp/size set・change/color param set・change/
  color-to-color（colour_picker の tagged color）、
  looks の effect set・change/clear・show・hide。
  オペランドは allocation-free な数値式のみ。pen は `penrt` の共有コア
  （`clear/down/up/stamp/size/colorParamNum`）を呼ぶ。looks effect は
  GraphicEffects の 7 キー（color/fisheye/whirl/pixelate/mosaic/
  brightness/ghost、ghost 0..100・brightness -100..100 クランプ）を共有する。
  数値は primitive の `num()` と同じく NaN→0 に正規化する（これを欠くと
  `change size by (0/0)` が interpreter と 1 件ずれる。2123 の size テストで検出）。
- 効果は region の最終文でのみ許可（`safeRegion`）。効果の後に bail し得る
  命令を置かないため、rollback による再実行で二重適用しない。
- region 内の書き込みが数値のみであることを `safeRegion` で確認する。
  非数値書き込みがあると commit 検査が効果の後に失敗し、再実行で
  `change x by` が二重適用されるため（`string_prefix` で検証）。
- host 呼び出しは `Target*` を invoke ごとに公開して実行。効果の直後は
  Completed で終了し、変数は commit される。helper が失敗した場合のみ
  rollback（効果は未適用）。
- テスト: `tests/differential/wasm_motion_effects.py`（18ケース、
  set/change x/y、変数オペランド、size set/change、NaN の size 変更、
  volume set/change（クランプ・NaN 含む）、direction 3効果（実際の方向を
  比較）、pen 7効果・color-to-color と looks 6効果（trace で実行確認）、非数値オペランドのフォールバック、
  非数値 prefix の安全性）。

### 2026-11-16: `sensing_username` と invoke ごとの Runtime publish

`sensing_username` を host import 32 でコンパイルする。headless は空文字列。
返す SStr は `Runtime::usernameRoot` が所有し、`markRoots` で常に root される
ため、monodule がタグ付き値を保持したまま別の host 呼び出しを跨いでも GC で
無効化されない。比較は tagged compare、join は tagged join 経由。

この実装中に、`greenFlag()` の**即時ハット評価**が `Runtime::step()` の外で
compiled module を呼ぶことを発見した。それまでは Runtime ポインタが null で、
timer/mouse/loudness の helper が静かに -1 を返して bail していた。
`invokeSegment` が `thread.target->runtime` を publish/restore するようにし、
Immediate hat 経路でもセンサー/username が動くようにした。
`tests/differential/wasm_username.py` が空/非空の比較・文字列順序・join を
4エンジンで比較する。
