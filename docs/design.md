# xyo-cpp 設計書

Scratch 3 / TurboWarp 完全互換・LLVM JIT 搭載のネイティブランタイム。

対象読み物:
- `refs/scratch-vm/` — TurboWarp/scratch-vm（コンパイラ + エンジン + 全ブロック実装の正本）
- `refs/scratch-editor/packages/scratch-render/` — 公式 scratch-render（レンダラ契約の正本）

この文書は実装に直結するレベルの設計を定める。ファイルパスは refs/ 配下のソースを指す。

---

## 1. 目的と非目的

### 目的
- **完全互換**: TurboWarp/scratch-vm と bit 単位で一致する実行意味論（キャスト・比較・スケジューリング・描画順・乱数まわりの仕様まで）
- **LLVM JIT**: スクリプトをネイティブコードへ JIT コンパイル。計算系スクリプトで TurboWarp コンパイル済み JS を大幅に上回る速度を目指す
- **インタプリタ層との併用**: 全ブロックをカバーするインタプリタ（正確性の基盤）+ JIT（速度の基盤）の 2 層構造。互換レイヤーブロック・拡張・編集中の動的変更はインタプリタ経由
- **埋め込み可能**: GUI/エディタなしの CLI シェル + C API で外部ツールから駆動可能。将来的には scratch-gui 相当のフロントエンドも差し込める構造

### 非目的（初期スコープ外）
- エディタ（blockly 相当のブロック編集 UI）
- sb2 ローダ（旧フォーマット。構造は把握済み、後回し）
- BLE/USB ペリフェラル拡張（wedo2/ev3/boost/microbit/makeymakey 等）
- クラウド変数の実プロバイダ（インターフェースのみ用意しスタブ）

---

## 2. アーキテクチャ概要

```
project.sb3 (zip)
   │
   ▼
[project]  SB3 ローダ ──► BlockModel / Target / Sprite / Variable / Monitor
   │                        （モデル層: TW blocks.js + target.js + rendered-target.js 相当）
   ▼
[ir]       IRGen ──► IntermediateScript (Stack/Input ノード木)
   │            │
   │            ▼
   │         IROptimizer（InputType ビットマスク型解析・定数畳み込み・変数型推論）
   │            │
   ▼            ▼
[interp]  木走りインタプリタ        [jit]  LLVMGen（IR → LLVM IR）
   │                                   │
   │                                   ▼
   │                            ORC JIT (LLJIT) → ネイティブ関数
   │                                   │
   ▼                                   ▼
[rt] ランタイム ABI（キャスト・比較・リスト・ハット・yield・compat 呼び出し …）
   │
   ▼
[engine] Runtime / Sequencer / Thread(Fiber) / IoDevices / Clock
   │
   ▼
[render] IRenderer（Null / GL33 実装）   [audio] サウンドエンジン
   │
   ▼
[shell] xyo CLI・ヘッドレステストドライバ・ベンチ
```

**2 層実行の使い分け**（TurboWarp と同じ方針）:
- 起動時にまずインタプリタで走る実装も可能だが、TW 同様「初回スレッド開始時にコンパイル」が基本
- コンパイル失敗・未対応 opcode・`tw nocompile` コメント → そのスクリプトだけインタプリタへフォールバック（スクリプト単位の混合実行）
- compat ブロック（JIT 未対応の一部 opcode）は JIT コード中からインタプリタ呼び出し —— TW の `executeInCompatibilityLayer` と同じ構造

---

## 3. 最重要決定: スレッド実装 = スタックフル・ファイバ

### 3.1 問題設定
Scratch のスレッドは「イールド」を持つ軽量スレッド。TW ではコンパイル済みスクリプトは JS ジェネレータ関数(`function*`)になり、`yield` でシーケンサへ制御を返す。イールド点:

- 非ワープスクリプトのループ反復ごと（`yieldLoop`）
- `wait`, `wait until`, `broadcast and wait`, エッジハット評価後, `waitThreads`
- ワープ中でも `isStuck()`（500ms 超で抜け出し用イールド）
- 手続きの再帰呼び出し直前（`yieldThenCall`: 引数評価→一度イールド→呼び出し）
- compat レイヤー: プリミティブが `util.yield()` するたびにイールド

### 3.2 選択肢と結論

| 方式 | 判定 |
|---|---|
| OS スレッド + ミューテックス | ✗ 数百スレッドで破綻、起動/切替が重い |
| LLVM コルーチン（stackless） | ✗ **suspend はコルーチン関数自身の中でしか書けない**。compat 層の `util.yield()` は通常の C++ 呼び出しの深いところで起きるため原理的に不可 |
| 手動状態機械化（CPS 分割） | △ IR が木構造なので可能だが、compat 層・手続き越しのイールドで同じ壁に当たる |
| **スタックフル・ファイバ** | ◎ **任意の呼び出し深さからイールド可能**。生成コードは直線 LLVM IR、ローカルはレジスタ常駐、手続きは普通の call。compat 層の任意プリミティブもそのままイールドできる |

→ **ファイバ採用**。これにより「イールド可能なのは文の位置だけ」という TW ジェネレータの制約を越えて、**compat プリミティブを全てネイティブ実装しその内部から直接イールド**できる（TW で compat 化が必要だった `motion_glidesecstoxy`, `sensing_askandwait`, `looks_sayforsecs` 等もネイティブ関数として実装可能）。

### 3.3 ファイバ実装
- コンテキスト = callee-saved レジスタ + SP + 戻り先（x86-64 SysV: `rbx rbp r12-r15 rsp rip`。aarch64 / Win64 も後で追加可能な構造）
- 切り替え: `xyo_fiber_switch(Fiber* from, Fiber* to)`、約 60 行のアセンブリ。コスト ~20-50 cycle
- スタック: `mmap` で仮想 1MiB 予約 + guard page、コミットはタッチ時（実効 メモリ = 使用量のみ）。再帰の深いプロジェクト（quicksort 等）対策として 1MiB
- `ASan` では `__sanitizer_start_switch_fiber` / `finish_switch_fiber` を呼ぶ
- イールド ABI: JIT コードから `xyo_yield(ctx)` を呼ぶと fiber switch でスケジューラへ復帰。次回 resume は `xyo_fiber_switch` で再開
- スレッドの kill（`stop all`/`delete this clone`/`stop this script`）: fiber をそのまま破棄すると「レジスタ内に生きている文字列への参照」が GC 起点から消えるだけで安全（値は GC 管理、§5）。C++ 例外を投げる必要はなく、ファイバを巻き戻さずそのまま廃棄する

---

## 4. スケジューリング意味論（sequencer/runtime の移植）

`refs/scratch-vm/src/engine/sequencer.js`, `runtime.js` の `_step()` を忠実に移植。

### 4.1 スレッド状態（数値は ABI — JIT コードが直接参照）
```
STATUS_RUNNING      = 0
STATUS_PROMISE_WAIT = 1   // 非同期待ち（C++ では futures/コールバックで resume）
STATUS_YIELD        = 2   // 同ティック内で再実行可（warp なら即時）
STATUS_YIELD_TICK   = 3   // 次ティックまで寝る
STATUS_DONE         = 4
```

### 4.2 フレームループ
- `FrameLoop`: 既定 30fps、`setFramerate(0)=requestAnimationFrame 相当(vsync)`、最大 250fps クランプ。補間 (`interpolationEnabled`) 時は step とは別に描画パスを走らせる
- `_step()` の順序（runtime.js:2515〜 を厳密に再現）:
  1. `isKilled` スレッド除去・`threadMap` 再構築
  2. **エッジハット全評価**: `edgeActivated` な hat opcode 全てに `startHats`
  3. `redrawRequested = false`、モニタスレッド投入
  4. `BEFORE_EXECUTE` → `sequencer.stepThreads()` → `AFTER_EXECUTE`
  5. `renderer.draw()`
  6. targets/monitor 更新イベント

- `stepThreads()`: `WORK_TIME = 0.75 * currentStepTime`。`threads` を順に回し、`STATUS_RUNNING|YIELD` → fiber resume。`turboMode || !redrawRequested` なら継続。終了スレッドは除去
- `stepThread`: コンパイル済み → fiber resume（= `generator.next()` 相当）。インタプリタ → TW の `execute` 相当の木走り
- warp: スレッドの `warpTimer` が 500ms (`WARP_TIME`) 超で STATUS_YIELD 同様に次スレッドへ。warp 中は `yield` を経ても同じティック内で即再開（sequencer.js:221-229 の `continue` 相当）

### 4.3 startHats / イベント
- `runtime.startHats(opcode, matchFields, optTarget)`:
  - matchFields は `toUpperCase` で正規化してから比較（hatFields 側はキャッシュ時に事前大文字化 — `blocks-runtime-cache.js`）
  - `hatMeta.restartExistingThreads` → 同 (target, topBlock) の既存スレッドを `_restartThread`。実装は**新 Thread を生成して threads 配列の同じ位置に差し替え**る（実行順序を維持するための Scratch 2 互換動作 — runtime.js:2045-2066）
  - restart でないハット → 既に動いていたら追加しない（stackClick は例外）
  - **生成したスレッドのエッジハット本体を直ちに 1 回評価**（Scratch 2 互換: `newThreads.forEach(compilerExecute)` の箇所 runtime.js:2263-2275）
  - ハットの restart フラグ一覧: flag/thisspriteclicked/stageclicked/backdropswitch/broadcastreceived は restart=true、keypressed は false（連打で多重起動）、touchingobject/whengreaterthan は edge-activated+no-restart
- `_pushThread`: 新 Thread を threads 末尾へ push → `compilerOptions.enabled` なら `tryCompile`（monitor スレッドは除く）。コンパイルはスレッド生成時に同期的に行う（キャッシュはブロックコンテナ側、クローン間で共有）
- `greenFlag()`: stopAll → PROJECT_START → currentMSecs 更新 → projectTimer リセット → 全ターゲット `clearEdgeActivatedValues` → `onGreenFlag` → `startHats('event_whenflagclicked')`
- ブロードキャスト: `event_broadcast` は `startHats` するだけ（即応答しない）。`broadcast and wait` は開始したスレッド群を capture して `waitThreads` で全終了を待つ（待つ間 `allWaiting` なら STATUS_YIELD_TICK 化）。**収集は実行前に確定** —— 「途中参加した受信者を待たない」意味論を維持

### 4.4 Thread
`thread.js` の移植:
- `topBlock`, `stack[]`(blockId スタック), `stackFrames[]`(isLoop/warpMode/justReported/params/waitingReporter/executionContext/op), `status`, `isKilled`, `target`, `warpTimer`
- JIT 追加フィールド: `fiber`（スタック+レジスタ）, `jitCtx`（§7.1）, `compiledEntry`, `procedures`（variant→関数テーブル）, `executableHat`, `timer`
- `stopThisScript()`: `waitingReporter` または `procedures_call` のフレームまでポップしてから再開位置を設定（TW の gh-201 修正を含む意味論 —— コマンド形の呼び出しは `goToNextBlock` 済みにする）
- `isRecursiveCall`: 直近 5 フレームを遡って同じ proccode の `procedures_call` があるか検査（非 warp 再帰呼び出し時の yield 判定に使用）

### 4.5 インタプリタ層（execute.js 移植）
- `BlockCached`: ブロックごとのキャッシュ（opcode/fields/inputs/mutation/branch 構造）
- プリミティブシグネチャ: `Value prim(Runtime&, Target&, ArgsView, BlockUtility&)`
- Promise 戻り → 我々の場合は `Async` マーカ: `std::variant`/`Result` で「非同期継続待ち」を表し `STATUS_PROMISE_WAIT` に遷移、完了コールバックで `handleReport` 相当 → `STATUS_RUNNING`
- `handleReport` の分岐（hat→edge/predicate 判定, conditional/loop→`stepToBranch`, stackClick→visualReport, monitor 更新）を忠実に

### 4.6 並列実行（依存グラフスケジューラ）

Scratch/TurboWarp の逐次パス意味論を保存しつつ、独立したスレッドを
ワーカースレッドで並列実行する。実装: `src/engine/deps.{hpp,cpp}`,
`src/engine/pool.hpp`, `Sequencer::stepThreads`/`flushParBatch`。

- **解析**: `(topBlock, Target)` 単位でスクリプト全体を静的走査し、
  読み/書き `MemKey` 集合を求める。キーは Var（解決済み `Variable*`）、
  Field（`(Target*, TF)` ターゲット属性）、Global（`GK` ランタイム共有
  状態）の 3 種。`procedures_call` は定義本体を再帰展開。
- **衝突**: 書き∧書き・書き∧読み・読み∧書きのキー交差で無向辺を張る
  （読み同士は衝突しない）。connected components を union-find で
  求め、成分内はバッチ順（= spawn 順）に逐次、独立成分は並列。
- **安全性**: `spawn`（スレッド生成・waitQueue・共有構造変更）と
  `unknown`（未解析 op・動的ターゲット解決・実行時変数生成を含む）
  を持つスクリプトはバッチ不適格 → 従来経路で逐次実行。
- **順序保存**: バッチは連続する適格スレッドのみを集め、不適格
  スレッドの直前に必ず flush する — 逐次実行と同じ可視順序になる。
- **並列条件**: JIT 済みスレッドのみ（インタプリタ経路は対象外）。
  `runtime.renderer != nullptr` 時は無効（renderer コールバックは
  スレッドセーフ前提にできない）。
- **共有状態対策**: `Gc::alloc` は mutex 保護 + 並列区間は collect
  遅延（worker 上でスタックスキャンしない）。`BlockUtility`（interp
  `g_util`・JIT `jitUtil`）と `Rng::global` は thread_local。
  `FiberPool`/`JitCtx` プールは mutex 保護。
- **環境変数**: `XYO_PAR=0` で無効化、`XYO_DEPGRAPH=<dir>` で
  バッチ毎の衝突グラフを DOT 出力（`-` で stderr）、
  `XYO_PROF=1` で並列バッチ統計（threads/groups/batches）。

---

## 5. 値表現とメモリ管理

### 5.1 `Value`: NaN-boxing u64

Scratch の値 = number | string | boolean（リストは値ではなく変数の中身、色は `[r,g,b]`）。8 バイトで表現するため NaN-boxing を採用:

```
上位 16bit (v >> 48):
  < 0x7FF8           → IEEE754 double そのまま（±Inf, -NaN も含む）
  0x7FF8             → タグ空間（payload 解釈）:
       payload 0     → 数値としての NaN（Scratch "NaN" 値）
       payload 1     → false
       payload 2     → true
       payload 3     → 空文字列シングルトン（intern 済み）
       payload 4-255 → 予約
  0x7FF9             → ヒープ文字列ポインタ（下位 48bit、x86-64/aarch64 のユーザー空間内）
  0x7FFA             → 色 [r,g,b] を下位 24bit に内包（ヒープ不要）
  その他             → 予約（小文字列インライン化の拡張余地）

isNumber(v)  = ((v >> 48) & 0xFFF8) != 0x7FF8      // 全 double + -NaN
                || v == kNan
isString(v)  = (v >> 48) == 0x7FF9
isBool(v)    = v == kFalse || v == kTrue
asPtr(v)     = v & 0x0000'FFFF'FFFF'FFFF
```

- **NaN 正規化規則**: 演算が生の NaN f64 を生成し得る箇所（`0/0`, `inf-inf`, `inf*0`, `sqrt(負)`, `log(負)` 等）は、型解析で `NUMBER_NAN` ビットが立つ場合に限り `xyo_canonicalize` を挿入して `kNaN` に写像。実数 NaN 演算が生む `0x7FF8`/`0xFFF8` パターンをタグと誤認しないようにする
- `toNumber(kNaN)` → 0（Scratch は NaN を数値扱いのとき 0 にする）
- `toString(kNaN)` → `"NaN"`、`toString(±Inf)` → `"Infinity"`/`"-Infinity"`

### 5.2 文字列 — UTF-16
- JS の文字列セマンティクスを保つため **UTF-16 (`char16_t`)** 内部表現。`letter of`, `length`, `contains` の添字・部分一致が UTF-16 コードユニット基準でそのまま一致する
- `SStr { u32 rc_or_mark; u32 len; u32 hash_cache; char16_t data[]; }`、GC 管理
- 小文字列（≤3 char16）は将来インライン NaN-box 化の余地あり
- `toLowerCase` は Unicode 既定の全小写化（簡易マッピングテーブルを持つ。ICU に依存しない範囲で実装、最悪の差異はドキュメント化）
- JS `Number→String` / `String→Number` の厳密な相互変換: **double-conversion ライブラリ**（V8 の Grisu/Ryu 相当）を vendored 依存に入れる。`String(1e21)`→`"1e+21"`, `String(-0)`→`"0"`, 最短往復表現, `Number("0x10")=16`, `Number("")=0`, `Number(" ")=0`, `Number("Infinity")=Inf`, `Number("1e-324")=0` 等の ES 準拠をこれに委譲。`Number()` の前置/後置空白・`0b`/`0o`/`0x` 接頭辞パーサも移植

### 5.3 キャスト・比較 — `cast.js`/`jsexecute` 互換の核心
`xyo_rt` として C++ で以下を**bit-for-bit**移植（`refs/scratch-vm/src/util/cast.js`, `jsexecute.js` runtimeFunctions）:

| 関数 | 意味論の要点 |
|---|---|
| `toNumber` | NaN→0, 文字列 `Number()` 準拠（空白のみ→0、hex/bin/oct 接頭辞、Infinity、小数）、bool→0/1 |
| `toNumberOrNaN` | NaN 保持 |
| `toBoolean` | `''`,`'0'`,`'false'`(大小無視)→false。他文字列→true。数値→JS 流 |
| `toString` | JS `String()` 準拠（§5.2） |
| `toListIndex` | 1-based。`'all'`→LIST_ALL（acceptAll 時）、`'last'`→末尾、`'random'/'any'`→乱択、範囲外→INVALID |
| `compare` | `Number()` 双方 NaN なら小文字化文字列比較。`n===0 && isNotActuallyZero(v)` の特例（数字/タブを含まない `""` や空白のみは数値 0 扱いしない）。∞==∞→0 |
| `isInt` | 文字列は `'.'` を含まなければ整数扱い |
| `compareEqual/Lt/Gt` | `jsexecute` の fast/slow 2 段構成を移植（型確定時はインライン化） |
| `mod` | 床型剰余 `n % m` が負なら `+m` |
| `tan` | 度数法、`±90/270` でちょうど `±Infinity` |
| `randomInt/randomFloat` | TW の定義を踏襲（内部 RNG は PCG 系。シード同一性は求めないが分布は一致させる） |
| `listIndex/listGet/listInsert/listReplace/listDelete/listContains/listIndexOf/listContents` | インデックス解釈と「`item` が 1 文字なら join('')、そうでなければ join(' ')」ルールまで一致 |
| `limitPrecision` | `|v-round(v)| < 1e-9` なら丸める（`x position` 等が整数表示を保つため） |
| `colorToList` | `toRgbColorList`（`#hex` 文字列 / 10 進色値） |

### 5.4 メモリ管理 — 自前 mark-sweep GC（小規模）
- 対象オブジェクト: `SStr`（文字列）、`SList`（リスト実体: `capacity,len,Value[]`）。スキン・音声データ等は別経路
- **GC が保守的に走査する根**: 各 `Target::variables`、モニタ値、`runtime._answer` 等、**そして全 live ファイバのスタック領域 + 保存済みレジスタ領域**
  - 走査はワード単位、`(w>>48)==0x7FF9` → `asPtr` をヒープ所属に照合して mark。タグ付きポインタも原生ポインタも捕捉できる（自前 GC だから実現できる点）
  - 中断中ファイバは「SP〜スタック底」を走査。呼び出し規約上 suspend 時の live 値は必ず callee-saved または spill 済み
  - 実行中ファイバ上で collect が走った場合も同様に「現 SP〜そのファイバ底」を走査し、切替中のスケジューラ側退避レジスタ（`detail::gSchedulerRegs`）も対象に含める（`Runtime::markRoots` 実装）
- **ロード中は collect を遅延**（`Gc::DeferCollect`）: `loadTargetObject` 中の Target は `runtime.targets` 未登録でルート走査から不可視。変数・リスト・ブロック field 等の GC オブジェクトがスタック残滓だけで生き延びるのは非決定的なため、区間中は回収自体を止める（3D2.sb3 のクラッシュ原因）
- 割当: bump allocator + フリーリスト、マークは非移動。トリガは割当バイト数閾値（経験的に 1-2MiB 起点、ヒープ拡大係数 2.0）
  - マークフェーズ中の割当は**黒色割当**（`collecting_` 中は mark=1 で生成）— ルート走査が `Mutation::parseArgs` 等で SStr を遅延生成するため、同 sweep で即解放されるのを防ぐ
  - 上記の再入割当のため `allocMtx_` は `std::recursive_mutex`（`collect()` が mutex 保持下でルート走査を呼ぶ）
- 停止時の平均ヒープは小さいため一時停止は通常 1ms 未満。文字列結合が多いプロジェクトでも安全に回収される
- **ファイバ kill 後の後片付けは GC が自動処理**: fiber を捨てるだけで、そのスタックにだけあった一時値は次回 GC で回収される（イールド中の一貫した参照を壊さない）

### 5.5 変数・リスト
- `Variable { id, name, type, isCloud, value(Value|SList*) }`。`create()` の JS 版「変数ごとに別 hidden class」は我々には不要 —— JIT で `Variable*` 直接埋め込み
- **クローンでの変数解決**: コンパイル済みコードはスレッド単位で `varSlots` に `Variable*` を束縛（`irgen.js` の `descendVariable` → スレッド開始時に `target.variables[id]` / `stage.variables[id]` を決め打ち。クローンは同じコード・別 ctx で各 clone の変数を指す → TW の「ファクトリ関数内で `evaluateOnce` により各スレッドの変数を閉じ込める」の同値実装）
- `LIST_TYPE` の値 = `SList*`。`_monitorUpToDate` 相当の dirty フラグを保持（モニタ用）

---

## 6. IR — TurboWarp の中間表現をそのまま採用

`refs/scratch-vm/src/compiler/enums.js` / `intermediate.js` を C++ に移植。

### 6.1 型（InputType ビットマスク）
`enums.js:21-92` の lattice をそのまま `enum class Type : u16` で移植:
- `NUMBER_POS_INF/POS_INT/POS_FRACT/ZERO/NEG_ZERO/NEG_INT/NEG_FRACT/NEG_INF/NAN`（下位 9bit）
- `STRING_NUM/STRING_NAN/STRING_BOOLEAN`（中位 3bit）
- `BOOLEAN`（0x1000）
- `COLOR`（0x2000）
- 合成型 `NUMBER`, `NUMBER_OR_NAN`, `STRING`, `ANY`, `NUMBER_INTERPRETABLE`, `BOOLEAN_INTERPRETABLE` も同じ値で定義

この lattice は「その値が実行時に取りうる型の集合」。`isAlwaysType`/`isSometimesType` で条件分岐・キャスト除去を導く。

### 6.2 ノード
- `IntermediateStackBlock { StackOpcode opcode; inputs; bool yields; TypeState *entryState,*exitState; }`
- `IntermediateInput { InputOpcode opcode; Type type; inputs; bool yields; }` + `toType`（定数なら畳み込み、非定数なら `CAST_*` ノード挿入）
- `IntermediateStack/Script/Representation` — `stack`, `isProcedure`, `procedureVariant`, `arguments`, `isWarp`, `yields`, `warpTimer`, `dependedProcedures`, `executableHat`, `topBlockId`

### 6.3 IRGen（irgen.js の移植）
- `ScriptTreeGenerator::descendInput` / `descendStackedBlock` / `walkStack` / `walkHat` / `descendSubstack` / `descendVariable` / `descendCompatLayerInput`/`descendCompatLayerStack` / `getProcedureInfo` / `analyzeLoop` / `readTopBlockComment` を C++ 化
- ハット処理: `edgeActivated` → `HAT_EDGE`、predicate → `HAT_PREDICATE`、`stackClick` → compat 層へ
- 手続き variant: `"W"+code`（warp）`"Z"+code`（非warp）。`dependedProcedures` 推移閉包を `IRGenerator::generate` で処理、`yields` 固定点反復（`analyzeScript`）

### 6.4 IROptimizer（iroptimizer.js の移植）
- `TypeState { varId → InputType }` の前方向解析。`clear/clone/mutate/or/after/overwrite/setVariableType/getVariableType` を移植
- 全 `OP_*` の結果型規則（`getInputType`）と、スタックブロック後の state 伝播（`optimize` エントリ側）を移植
- `entryState/exitState` を各 `IntermediateStackBlock` に保存 —— JIT 側で「その入力が実行時に取りうる型」を参照

---

## 7. LLVM JIT — コード生成設計

### 7.1 ABI とスレッドコンテキスト

```cpp
struct JitCtx {
    Thread*         thread;     // status, warpTimer, fiber 等
    RenderedTarget* target;     // self
    RenderedTarget* stage;
    Runtime*        runtime;
    Variable**      varSlots;   // descendVariable 解決済み（スレッドごとに束縛）
    Value*          procArgs;   // PROCEDURE_ARGUMENT 用
    u32             stuckCounter;
    // 必要に応じて padding / キャッシュライン整列
};
using ScriptFn = void (*)(JitCtx*);
```

- エントリ: `void fn(JitCtx*)` で、fiber の開始点。`thread->fiber` にこの関数の開始ポイントを仕込む
- 手続き: `void fn(JitCtx*, const Value* args)` だが、`PROCEDURE_ARGUMENT` は `procArgs[i]` で参照するのではなく、**引数を通常の関数引数**に落としてもよい（warp 手続きは非 yield 通常関数）。yield する手続きも fiber 内では通常呼び出しでよい
- reporter 手続き（`procedures_return`）は `Value` を返す関数にして呼び出し側で利用

### 7.2 `yield` の lowering
```
xyo_yield(ctx):
    ctx->stuckCounter = 0
    fiber_switch(ctx->thread->fiber, scheduler_fiber)
```
- JIT からは extern "C" 関数 `xyo_yield` を呼ぶだけ（引数は ctx のみ、常に `attributes(noinline)` で LLVM に最適化させない）
- `yieldNotWarp` → `if (!isWarp) xyo_yield(ctx)`、IR の `isWarp` はコンパイル時定数なので warp 関数では命令自体を出さない
- `yieldStuckOrNotWarp` → `if (isStuck(ctx)) xyo_yield(ctx)`。`isStuck` はインライン: `++stuckCounter==100` で `sequencer.timer > 500ms` を見る（jsexecute と同一ロジック）

### 7.3 型別 lowering の指針（最重要の性能領域）
`descendInput` で IR `InputType` を見て LLVM 型を選択:

| InputType が含むもの | 生成する LLVM 値 |
|---|---|
| `NUMBER` 確定（`isAlwaysType(NUMBER)`） | `double`（生の f64、ボックス化しない） |
| `BOOLEAN` 確定 | `i1` |
| `STRING` 確定 | `SStr*`（`ptr`、GC 管理） |
| `COLOR` 確定 | `[3 x double]` or タグ化カラー（最終は `u64` boxed） |
| 混合/不明 | `u64` NaN-boxed `Value` |

`toType` で必要になった `CAST_*` も同じく:
- `CAST_NUMBER` on NUMBER → no-op
- `CAST_NUMBER` on BOOLEAN → `select`
- `CAST_NUMBER` on STRING/ANY → `xyo_rt_to_number` 呼び出し（中に fast-path インライン化可）
- `CAST_BOOLEAN` on BOOLEAN → no-op
- `CAST_NUMBER_OR_NAN` on NUMBER_OR_NAN → no-op（raw double でも NaN を許す）
- `CAST_NUMBER_INDEX` on NUMBER → `fptosi`/丸め
- 定数なら irgen の `toType` がコンパイル時に畳み込み済み

演算:
- `OP_ADD/SUBTRACT/MULTIPLY/DIVIDE` on NUMBER → `fadd`/`fsub`/`fmul`/`fdiv`。結果型は `iroptimizer` の規則で `NUMBER`/`NUMBER_OR_NAN` を推論し、NaN 生成可能性があるときのみ `xyo_canonicalize` 挿入
- `OP_MOD` → `fmod`+`fadd` 条件分岐（Scratch は floored）
- `OP_ABS/FLOOR/.../POW_10` → `fabs`, `floor`, `ceil`, `sqrt`, `sin`, `cos`, `tan`（`xyo_rt_tan` で ±90/270 の ∞ 処理）, `asin`, `acos`, `atan`, `log`, `log10`, `exp`, `pow(10,x)` の libm 呼び出し（`llvm.*` intrinsic または `xyo_rt_*`）
- `OP_AND/OR/NOT` on BOOLEAN → `and`/`or`/`not` + 必要に応じ `toBoolean` キャスト
- `OP_EQUALS/GREATER/LESS`:
  - 両側 `NUMBER_INTERPRETABLE` → 数値比較 `fcmp`（NaN 片側のケースは `xyo_rt_compare_*`）
  - 片側が絶対に数値解釈不可能（`!isSometimesType(NUMBER_INTERPRETABLE)`）→ 文字列小文字比較
  - 安全な定数最適化（jsgen の `isSafeInputForEqualsOptimization`）を同様に適用
  - それ以外は `xyo_rt_compare_eq/lt/gt` 呼び出し
- `OP_JOIN` → `xyo_rt_join`（UTF-16 結合、SStr 生成）
- `OP_LENGTH` → `len` フィールドロード
- `OP_LETTER_OF` → 範囲チェック+UTF-16 code unit 取得（`""` 既定）
- `OP_CONTAINS` → `xyo_rt_contains`（小文字化 find）
- `OP_RANDOM` → int/float 版に分岐（`useInts`/`useFloats` ヒントをそのまま使う）

### 7.4 スタックブロック lowering（jsgen の対応表）
- `CONTROL_IF_ELSE` → `br` + 各 substack を `descendStack`、マージ点は PHI
- `CONTROL_REPEAT`/`CONTROL_FOR`/`CONTROL_WHILE`/`forever` → ループ `br`。非 warp で末尾 `yield`（`yieldLoop`）、warpTimer フラグなら `yieldStuckOrNotWarp`
- `CONTROL_WAIT` → `thread->timer = timer();` → `runtime.requestRedraw()` → `while (elapsed < ms) yield*;`
- `CONTROL_WAIT_UNTIL` → `while (!cond) yieldStuckOrNotWarp`
- `CONTROL_STOP_*` → `runtime.stopAll()` / `runtime.stopForTarget(target, thread)` / `retire()`
- `CONTROL_CLONE_CREATE` → `runtime.ext_control._createClone(name, target)` 相当の rt 関数
- `CONTROL_CLONE_DELETE` → `if (!target->isOriginal) { disposeTarget; stopForTarget; retire(); }`
- `EVENT_BROADCAST` → `xyo_rt_startHats(...)`、`EVENT_BROADCAST_AND_WAIT` → `waitThreads` ループ（全生き残り検査→yield、全待機→YIELD_TICK）
- `VAR_SET/SHOW/HIDE`, `LIST_*` → rt ヘルパー + `runtime.ioDevices.cloud.requestUpdateVariable` の呼び出し（cloud 変数のみ）
- `LOOKS_*`/`MOTION_*`/`PEN_*` → `target->setXY` 等の直呼び出し or rt ヘルパー。`MOTION_STEP`/`_ifOnEdgeBounce` 等は rt 側に実装
- `SENSING_*` → `runtime.ioDevices.*` 直接呼び出し
- `PROCEDURE_CALL` → 直接 `call`（引数評価→warp ならそのまま、再帰 warp なら `yieldThenCall` パターンで一度 `xyo_yield` してから `call`）。`procedures[variant]` テーブル引きではなく**コンパイル時に静的に解決された関数シンボル**を呼ぶ（IR では `dependedProcedures` が既に集まっている）
- `PROCEDURE_RETURN` → 値を返して return
- `COMPATIBILITY_LAYER` → `xyo_rt_execute_in_compat_layer(ctx, blockId, argsStruct, branchInfo)` を呼び、内部でインタプリタを駆動。`yield*` 相当はファイバの中で自然に起きる
- `HAT_EDGE`/`HAT_PREDICATE` → ハット評価 + `retire`/`yield` を組み込んだコード
- `VISUAL_REPORT` → `runtime.visualReport` 呼び出し
- `ADDON_CALL` → `runtime.getAddonBlock(...).callback` 相当の rt 呼び出し
- `DEBUGGER` → no-op or trap

### 7.5 変数/リスト参照の lowering
`descendVariable` が返した `{scope, id, name, isCloud}` に基づき:
- `scope == 'target'` → `ctx->varSlots[i]` 経由で `Variable*` をロード（スレッド作成時に `target->variables[id]` を解決済み）
- `scope == 'stage'` → `ctx->stage->variables[id]` を同様にスレッド作成時に解決してスロットへ
- 変数が存在しない場合（TW はここで**新規生成**して target/clones にも作る —— `_descendVariable` の末尾ロジックを移植）

`VAR_GET` → `varSlots[i]->value` ロード、`VAR_SET` → `varSlots[i]->value = v` + cloud/dirty フラグ。

### 7.6 ORC JIT 基盤
- `LLJIT`（`llvm::orc::LLJITBuilder`）+ `IRCompileLayer` + `ObjectLinkingLayer`（`RTDyldObjectLinkingLayer`）
- モジュール: 1 スレッドの全スクリプト+手続きを 1 `LLVMModule` にまとめ、`ThreadLocalStorage`的に共有コンテキスト構造体でアクセス
- シンボル解決: `DynamicLibrarySearchGenerator::GetForCurrentProcess` + `absoluteSymbols` で `xyo_rt_*` を公開。あるいは `LLJIT::getExecutionSession().intern()` で明示的に登録
- 最適化: `PassBuilder` + `OptimizationLevel::O2`/`O3`（デフォルト O2、ベンチで検証）。`TargetMachine` = ホスト CPU 最適化（`EngineBuilder().selectTarget()`）
- コードモデル: `Small`、再配置は `RTDyld` が処理
- `.eh_frame` 登録は LLJIT が自動。ただし我々は C++ 例外で unwind しない（fiber 捨てるだけ）ので、eh_frame は「`xyo_yield` 中に C++ throw しない」運用で事実上不要だが、`compat` 層の例外安全のため保持しておく
- **コンパイル失敗のフォールバック**: `tryCompile` で例外 → そのスクリプトは永続的に interpreter へ（`cachedCompileError` と同等の機構）

### 7.7 エッジハットと「直ちに一度実行」
`startHats` が作ったスレッドはコンパイル済みなら `compilerExecute(thread)` をその場で一度呼ぶ（runtime.js:2263-2275）。これは `HAT_EDGE`/`HAT_PREDICATE` の評価まで進めて旧値を更新するためで、fiber resume 1 回で再現できる（fiber は初回 resume で entry から hat 部分を実行して `yield` で止まる）。

---

## 8. インタプリタ層

JIT で直接コンパイルしないブロック（= compat 層対象）を含め、**全オペコードを解釈できる木走りインタプリタ**を実装。用途:

1. JIT コンパイル失敗時のフォールバック
2. エディタ的な「スタッククリック」「モニタ評価」用の小さい実行
3. compat 層呼び出し先（JIT から `xyo_rt_execute_in_compat_layer` 経由で入る）
4. `tw nocompile` 指定スクリプト

構造:
- `engine/execute.cpp`（TW の `execute.js` 相当）: `BlockCached` キャッシュ + `handleReport` + promise/非同期待ち
- **`BlockCached._ops` のフラット化を採用**（execute.js:289-387）: 各ブロックについて、子 reporter を前置順に展開した平坦なオペ列を事前構築しておき、実行時は配列を回すだけ。引数は `argValues` テーブル（field は構築時に焼き込み、reporter の結果は `_parentKey`/`_parentValues` リンク経由で書き戻す）。Promise 中断時は `currentStackFrame.reported`（実行済み op の値一覧）と `reporting`（中断した op id）を保存し、resume 時に argValues へ thaw + op インデックス復帰する
- `BROADCAST_INPUT` は `{id, name}` 構造を持つ特殊ケース（reporter 挿入時は name に toString して id=null）
- `engine/block-utility.cpp`: `BlockUtility {target, sequencer, timer(), yield(), yieldTick(), startBranch, initParams/params, startHats, ioQuery}` を C++ 化。`ioQuery` は `runtime.ioDevices[device][func](args...)` のディスパッチ
- `Status`/`StackFrame`/`Thread` の意味論は JIT 側と共有（同じ Thread 構造体）
- `broadcastAndWait` のインタプリタ版は `stackFrame.startedThreads` にスレッド集合を保持し再実行時に `runtime.threads` 所属を検査。`isWaitingThread` = PROMISE_WAIT|YIELD_TICK|非アクティブ。コンパイル版 `waitThreads` と同じ意味論で実装する

`compat` 層のサブスタック（`BlockType.CONDITIONAL`/`LOOP`）は `branchInfo` を使い、`isLoop`/`branch` を runtime 側で管理してインタプリタに「次のブランチ番号」を返させる（jsgen の `createBranchInfo` 相当）。

---

## 9. ブロック実装の構成

`refs/scratch-vm/src/blocks/*.js` を 1:1 C++ 移植。プリミティブシグネチャは JIT/インタプリタ共通で使える形:

```cpp
using ArgsView = llvm::ArrayRef<Value>; // または名前付きマップ
Value prim_motion_movesteps(Runtime&, Target&, Value steps, ExecCtx&);
```

ただしブロック引数は名前付きが多いので、実際は `std::initializer_list` 的なアクセスでよい。**重要: 「yield できるプリミティブ」はファイバ前提なので、内部で `yield()` や `waitThreads(...)` を普通に呼んでよい**。

compat-blocks.js の一覧は「JIT が直接 lowering しない opcode」リストとして使い、インタプリタ側では全部実装する。

---

## 10. レンダラ境界

### 10.1 インターフェース（VM が呼ぶもののみ）
`IRenderer` 抽象クラス。呼ばれるメソッド（§レポート参照、主なもの）:
- ライフサイクル: `createDrawable/destroyDrawable`, `setLayerGroupOrdering`, `setStageSize`, `draw`, `resize`
- 変形・見た目: `updateDrawable{Position,DirectionScale,Visible,SkinId,Effect}`, `setDrawableOrder/getDrawableOrder`, `updateDrawableSkinId`
- スキン: `createBitmapSkin/createSVGSkin/createPenSkin/createTextBubbleSkin`, `update{Bitmap,SVG}Skin`, `getSkinSize/getCurrentSkinSize`, `getSkinRotationCenter`, `getSkinMightBeMissing/updateSkinMightBeMissing`, `setPrivateSkinAccess`
- クエリ: `getBounds/getBoundsForBubble`, `drawableTouching`, `isTouchingDrawables`, `isTouchingColor`, `pick`, `extractDrawable`, `getFencedPositionOfDrawable`, `clientSpaceToScratchBounds`
- ペン: `penClear/penPoint/penLine/penStamp`
- 動画レイヤ: `updateVideoCamera`, `videoLayerSkin`（必要なら）
- `offscreenTouching`, `useHighQualityPen`, `snapshot`

### 10.2 実装方針
- `NullRenderer`: ヘッドレス・テスト用。CPU でシルエット/タッチングを完全に実装（GPU 不要）。ピクセルはスキン生成時に CPU 側にも保持
- `GL33Renderer`: scratch-render の構造を OpenGL 3.3 core に移植。GLSL ES → GLSL 330 変換（`texture2D`→`texture` 等）、premultiplied alpha、EFFECT_INFO の uniform 対応、MIP スキンキャッシュ、pen FBO、text-bubble スキン
- **シルエット/タッチングは CPU で統一実装**し、Null/GL で共有（正確な互換性には GPU readback より CPU ウォークのほうが簡単で移植性が高い）
- SVG スキン: 自前ラスタライズ or `lunasvg` + `scratch-svg-renderer` の quirk 修正を C++ に移植

### 10.3 座標系
Scratch 座標 x ∈ [-240,240], y ∈ [-180,180]（480×360 ステージ）。`Runtime.STAGE_WIDTH/HEIGHT` 480/360。`bitmapResolution`（スキン実解像度倍率）はコスチューム毎に保持。

### 10.4 実装済み: SDL2 レンダラ（`src/render/`）

GL33Renderer の先行実装として SDL2 ベースの表示レンダラを実装（`xyo play`）。
SDL2 が無い環境では `XYO_HAVE_SDL2` 未定義でヘッドレスのみビルドされる。

- **`SdlRenderer`**: `RendererAPI` 実装。スプライト描画は SDL_Texture
  （PNG= stb_image / SVG= nanosvg 2x ラスタ化でテクスチャ px 保持）、
  `RotationStyle`（all around/left-right/none）・`visible`・layer order。
- **エフェクト**: `src/render/effects.cpp` に全7種の CPU 実装
  （color/fisheye/whirl/pixelate/mosaic/brightness/ghost、scratch-render
  シェーダ準拠）。スキン毎にエフェクトキーで変換済みバリアントをキャッシュ
  （24 エントリ上限で全破棄）。描画と `pick`/タッチング系サンプルで同一
  ビットマップを使用するため見た目と衝突判定が一致する。
- **座標変換**: scratch→stage論理px→window（レターボックス等倍）。
  コスチュームアンカーは `rotationCenter`（論理 px）×`skinScale`。
- **ペン**: stage 2x 解像度の CPU RGBA バッファ。`penLine/penPoint/penClear`
  はソフトウェアラスタ、`penStamp` はスキン画素の逆変換合成。
  変更時のみテクスチャへアップロード。層順は backdrop < pen < sprite。
- **タッチング系**: `drawableTouching`/`isTouchingDrawables` はスキン画素の
  AABB+alpha 判定、`isTouchingColor`/`colorTouchingColor` は「背面の合成色」を
  drawable→pen→backdrop の順にサンプリング（±5 許容）。
- **モニタ**: `monitorState.records` の visible を描画。default/slider/large/list
  各モード対応（角丸ボックス+ピル型値チップ、リストは target 解決して行表示）。
  スライダはドラッグ可: 丸角トラック+白縁円ノブ、ノブ可動スパン
  （sliderTravel_）を min/max へ線形マップ・isDiscrete は丸め・
  cloud 変数は拒否し、変数へ直接書き込む。
- **バブル**: `bubbleText` を角丸白ボックス＋尾で描画（say/think）。
- **UI**: ステージ右上に green flag/stop ボタン（角丸・ホバー強調・
  旗はペナント/ストップは八角形、Shift+旗 = turbo で旗が橙色変化）。
  操作対象（ボタン/スライダ/draggable スプライト/入力欄）に応じて
  システムカーソルを hand/ibeam/arrow に切替（`updateCursor`）。
- **入力**: SDL キー/マウス → `ioDevices.keyboard/mouse` へ変換
  （クリックハットは `Mouse::pickTarget`→`pick` 経路）。
- **スプライトドラッグ**: `draggable && visible` のみ掴み可能。
  掴み時のオフセットを保持して追従、移動時は `wasDragged` で
  クリックハットを抑制。非移動の離脱は TW 通り mouse-up でハット発火。
- **ask and wait**: `onQuestion` で下端に入力バーを表示
  （`SDL_StartTextInput`）。丸角入力フィールド+緑の✓送信ボタン
  （クリック送信可）+ 質問テキスト表示。Enter 送信/BS/Delete/←→/Home/End、
  UTF-16 サロゲートペア対応の編集、回答で `emitAnswer` → `resolveWait`。
  質問中はキー入力をランタイムへ流さず UI が消費。
- **オーディオ**: `src/audio/sdl_audio.cpp`（`SoundBankAPI`）。
  WAV/SBV/MP3(dr_mp3) デコード→SDL オーディオコールバックでミックス。
  `playSoundAndWait` はボイス完了時 `resolveWait` でスレッド再開
  （`update()` をフレーム毎に呼ぶ）。デバイス無し/デコード失敗時は即解決。
  `setEffects` は pitch(=2^(v/120) リサンプル)+pan(等ラウドネス)、
  `getLoudness` をモニタ用途に提供。
- **テキスト**: `FontRenderer`（stb_truetype + グリフキャッシュ + TTC 対応）。
  フォントは `$XYO_FONT` → NotoSansCJK/DejaVu 等の既知パス順に探索。
  見つからなければテキスト描画をスキップ（警告のみ）。
- 検証用: `XYO_EXIT_FRAMES=N`（N フレームで終了）、`XYO_SHOT=path.ppm`
  （最終フレームを PPM 保存）。`SDL_VIDEODRIVER=dummy` でヘッドレス動作可能。
  `XYO_INJECT="F:op:args|..."` でフレーム F に合成イベントを注入
  （`md/mu/mm:x,y`・`txt:utf8`・`key:name`）— `SdlRenderer::handleEvent`
  を直接呼ぶため SDL イベントキュー非依存。
- **依存スケジューラとの関係**: `runtime->renderer != nullptr` の間、
  並列バッチ実行は自動無効（`parallelEnabled` ガード）。レンダラコールバックが
  ワーカースレッドから呼ばれないための安全弁。

---

## 11. I/O デバイス

`runtime.ioDevices` テーブルを C++ 実装:

| device | メソッド | 実装ノート |
|---|---|---|
| `clock` | `projectTimer`, `resetProjectTimer`, `pause/resume` | ステップ毎に `currentMSecs` から算出 |
| `keyboard` | `postData`, `getKeyIsDown`, `getLastKeyPressed` | キー正規化（数字→ASCII、`KEY_NAME` マッピング、TW 拡張キー）を移植 |
| `mouse` | `postData`, `getClientX/Y`, `getScratchX/Y`, `getIsDown`, `getButtonIsDown` | `miscLimits` で int 丸め。クリックで hat 起動 |
| `mouseWheel` | `postData` | `event_whenkeypressed` に `up/down arrow` を送る |
| `userData` | `postData`, `getUsername` | 簡易 string |
| `video` | `postData`, `enableVideo`, `getFrame`, `setPreviewGhost`, `mirror` | 初期はスタブ（videoSensing 拡張対応時に実装） |
| `cloud` | `setProvider`, `postData`, `request{Create,Update,Rename,Delete}Variable`, `clear` | 外部プロバイダへのコールバック IF のみ |

`postIOData(device, json)` で外部から入力を注入（GUI シェル・テストハーネス用）。

---

## 12. プロジェクトローダ（sb3）

`refs/scratch-vm/src/serialization/sb3.js` のデシリアライズ側を移植:

- ZIP 展開（`minizip-ng` or `libzip`）→ `project.json` パース（`nlohmann/json` or `simdjson`）
- `targets[]`: `isStage`, `variables`, `lists`, `broadcasts`, `blocks`, `comments`, `currentCostume`, `costumes`, `sounds`, `layerOrder`, stage-only `tempo/videoTransparency/videoState/textToSpeechLanguage`
- `blocks` map: `id → blockObj|primitiveArray`。
  - **プリミティブ配列** (`[typeId, fieldValue, fieldId?, x?, y?]`): typeId 4-13 → `math_number|math_positive_number|math_whole_number|math_integer|math_angle|colour_picker|text|event_broadcast_menu|data_variable|data_listcontents` にマップ
  - `inputs`: `[rel, blockId, shadowId?]`、rel 1/2/3 を `{block, shadow}` に復元
  - `fields`: `[value, id?]` → `{value, id, variableType?}`
  - `mutation` そのまま保持（`proccode`, `argumentids`, `warp` 等）
- モニタ: `monitors[]` を MonitorRecord へ。`data_variable`/`data_listcontents` は変数 ID を保持、他はブロック ID に再バインド
- ロード順序: platform チェック → extensionURLs → **customFonts** → layerOrder ソート → costumes/sounds デシリアライズ（ZIP から `assetId.dataFormat` 読み出し → `loadCostume`/`loadSound` → renderer skin / audio bank）→ targets 構築 → `fixUpVariableReferences` → monitors → extensionStorage
- `tw-compress-sb3` の ID 圧縮は復号ルールに従って処理（id が短いだけで構造は同じ）
- `meta.platform` 不一致 → `PLATFORM_MISMATCH` イベント（ログのみ）

### アセットパイプライン
- `loadCostume`: `dataFormat` `svg` → `createSVGSkin`、`png/jpg/bmp/gif` → `createBitmapSkin`。`bitmapResolution`（既定 2）、`rotationCenter` を適用。失敗時はデフォルトコスチューム
- `loadSound`: `mp3|wav` → オーディオエンジンで decode → `soundBank` へ。失敗時はデフォルト音

---

## 13. 拡張機構

- `extension-support/*`（BlockType/ArgumentType/menu/hat 登録）の最小限を C++ 実装
- 組み込み拡張の初期対応: `pen`（ペンは必須・レンダラと直結）、`tw`（lastKeyPressed/buttonIsDown）、`music`（後で）、他はスタブ
- 外部拡張（URL ロード）は**初期スコープ外**。`canLoadExtensionFromProject` を stub-deny、セキュリティマネージャは既定値で実装
- `runtime.addonBlocks`（add-on の手続き的ブロック）はインターフェースだけ用意

---

## 14. ディレクトリ構成

```
xyo-cpp/
├── CMakeLists.txt
├── cmake/
│   └── FindLLVM.cmake      (llvm-config 優先、LLVM>=18)
├── docs/
│   └── design.md           （本書）
├── refs/                   （git submodule/depth1 clone。参照のみ）
│   ├── scratch-editor/
│   └── scratch-vm/
├── src/
│   ├── core/
│   │   ├── value.hpp/cpp       Value(NaN-box), SStr, SList, Color
│   │   ├── cast.hpp/cpp        toNumber/toString/toBoolean/toListIndex/compare/isInt
│   │   ├── jsnum.hpp/cpp       double-conversion ラッパ（ES Number()/String() 準拠）
│   │   ├── gc.hpp/cpp          miniGC（ヒープ・mark-sweep・root 走査）
│   │   ├── string_util.*       UTF-16 操作・小文字化・contains
│   │   ├── uid.hpp             ブロック/ターゲット ID
│   │   └── log.hpp
│   ├── model/
│   │   ├── block.hpp           ブロックノード（id,opcode,inputs,fields,next,parent,topLevel,x,y,shadow,mutation,comment）
│   │   ├── blocks.hpp          Blocks コンテナ＋キャッシュ
│   │   ├── variable.hpp
│   │   ├── target.hpp          Target / RenderedTarget / Sprite
│   │   ├── monitor.hpp
│   │   └── comment.hpp
│   ├── engine/
│   │   ├── runtime.hpp/cpp     Runtime（_step, startHats, greenFlag, events, options）
│   │   ├── sequencer.hpp/cpp   stepThreads/stepThread/stepToBranch/stepToProcedure/retireThread
│   │   ├── thread.hpp/cpp      Thread / StackFrame / 状態定数
│   │   ├── fiber.hpp/cpp       ファイバ切替（アセンブリ: x86_64 SysV/aarch64/Win64）
│   │   ├── frame_loop.hpp/cpp  30fps/rAF/補間ループ
│   │   ├── execute.cpp         インタプリタ（BlockCached/handleReport）
│   │   ├── block_utility.*
│   │   └── io_devices.hpp/cpp  clock/keyboard/mouse/mouseWheel/userData/video/cloud
│   ├── ir/
│   │   ├── enums.hpp           InputType/StackOpcode/InputOpcode（TW 値一致）
│   │   ├── intermediate.hpp    ノードクラス
│   │   ├── irgen.cpp           ScriptTreeGenerator/IRGenerator
│   │   └── iroptimizer.cpp     TypeState/getInputType/optimize
│   ├── jit/
│   │   ├── jit.hpp/cpp         LLJIT ラッパ、シンボル登録、compile thread
│   │   ├── codegen.hpp/cpp     LLVMGen（IR → LLVM IR）
│   │   └── rt_api.hpp          JIT から呼ぶ extern "C" ヘルパーの宣言集
│   ├── rt/
│   │   ├── runtime_helpers.cpp  cast/compare/list/string/mod/tan/random/colorToList/limitPrecision
│   │   ├── thread_rt.cpp        yield/isStuck/retire/waitThreads/startHats/createBranchInfo
│   │   ├── compat_layer.cpp     executeInCompatibilityLayer
│   │   └── target_rt.cpp        target/setXY/setDirection/touching/distance 等のブリッジ
│   ├── blocks/                 （プリミティブ実装、scratch3_* を C++ 移植）
│   │   ├── operators.cpp control.cpp motion.cpp looks.cpp sound.cpp event.cpp sensing.cpp data.cpp procedures.cpp pen.cpp music.cpp tw.cpp ...
│   ├── project/
│   │   ├── sb3.hpp/cpp         ローダ
│   │   ├── zip.hpp/cpp         （minizip-ng or 自前）
│   │   ├── json.hpp            （nlohmann or simdjson ラッパ）
│   │   └── asset_load.*        costume/sound decode
│   ├── render/
│   │   ├── irenderer.hpp       IRenderer IF
│   │   ├── null_renderer.*     CPU silhouette 全対応
│   │   ├── gl33_renderer.*     scratch-render 移植
│   │   ├── skin.* drawable.* silhouette.* shader.*
│   │   └── svg_raster.*        lunasvg ラッパ
│   ├── audio/
│   │   └── miniaudio_engine.*  wav/mp3 decode + effects + loudness
│   └── shell/
│       ├── main.cpp            xyo CLI（run/test/bench）
│       ├── sdl_shell.*         SDL2 + GL ウィンドウシェル（後で）
│       └── headless_shell.*    テスト/ベンチ用
└── tests/
    ├── unit/                   cast/compare/list/gc/jit-unit
    ├── fixtures/               refs/scratch-vm/test/fixtures のミラー
    ├── differential/           node scratch-vm との差分ランナー
    └── snapshots/              JIT 出力・IR スナップショット
```

---

## 15. 依存・ビルド

- C++20、CMake ≥ 3.24、LLVM ≥ 18（開発機は 23.1.1）
- `llvm-config --libs core orcjit native` でリンク。`libLLVM.so` 共有リンクを許可
- vendored 依存:
  - `double-conversion`（ES Number/String 準拠）— 必須
  - `minizip-ng` or `zlib`+自前 zip reader — sb3 展開
  - `nlohmann/json` or `simdjson` — project.json パース（simdjson 推奨・高速）
  - `lunasvg` — SVG ラスタライズ
  - `miniaudio` — 音声 decode/再生/loudness
  - `stb_image`/`stb_image_write` — PNG/JPG decode、スクリーンショット
  - `stb_truetype` or 直接フォント — text bubble
  - `SDL2`（推奨、既に存在 2.32.72）— shell ウィンドウ/入力。GLFW3 もあるので選択可

---

## 16. 互換性・テスト戦略

1. **差分テスト（最重要）**: 同じ `.sb3` を Node 上の scratch-vm と xyo-cpp のヘッドレスで走らせ、フレーム毎に可視状態（sprite 座標/向き/size/visibility/effects/costume/say/answer/変数値/モニタ）を JSON ダンプして比較
   - `refs/scratch-vm/test/fixtures/execute/*.sb3`（118 件）は既に実行差分検証に使えるフィクスチャ群
   - `tw-comparison-matrix-inline/runtime`、`tw-NaN`、`tw-add-can-return-nan`、`tw-forkphorus-515-*`、`tw-counter` 等が cast/compare/warp の重要ケースを網羅
2. **ユニットテスト**: `cast.cpp`/`compare.cpp`/`jsnum.cpp`/`list.cpp` の table-driven テスト（cast.js のエッジケース全網羅）
3. **JIT スナップショット**: IR/生成 LLVM IR を fixture 化（`ll` ファイルを比較。型推論と lowering が退行しないことを確認）
4. **統合**: interpreter tier と JIT tier を同じ fixture で走らせ、差異がないことを確認
5. **ベンチ**: `mandelbrot`/`raycaster`/`physics`/`sort`/`particles` など計算密集プロジェクトで `ops/sec` またはフレームタイムを TW と比較

---

## 17. ロードマップ

| Phase | 内容 | 完了条件 |
|---|---|---|
| M0 | ビルド骨組み + Value/SStr/SList + GC | 全 unit test 通過 |
| M1 | cast/jsnum（double-conversion 統合）+ 比較行列 | `tw-comparison-matrix` 的 fixture を自前で全パス |
| M2 | sb3 ローダ + BlockModel + インタプリタ（全コアブロック） | `test/fixtures/execute` の interpreter run で一致 |
| M3 | sequencer/thread/fiber + ハット/イベント/クローン/warp 意味論 | 同上 + スケジューリング fixture 一致 |
| M4 | NullRenderer（CPU シルエット/タッチング/pen CPU）| ヘッドレスで touching が動く |
| M5 | IRGen + IROptimizer 移植 | スナップショット fixture で IR 一致 |
| M6 | LLVMGen + LLJIT + rt ABI | コンパイル済みスクリプトがインタプリタと一致 |
| M7 | GL33Renderer + audio + SDL shell | 実プロジェクトが動く |
| M8 | 差分ハーネス整備・ファズ・ベンチ | ベンチで TW 比 2x+ |

---

## 18. リスク・注意点（実装者向け）

1. **NaN-boxing の NaN 正規化漏れ**: NaN を生む全ての演算で canonicalize を忘れるとタグ誤認。IROptimizer の `NUMBER_NAN` ビットを厳密に使う
2. **`isNotActuallyZero` 罠**: `""`/`" "`（空白のみ）/`"\t"` は `Number()` で 0 だが比較では 0 扱いしない。`compare` のこの分岐を 1 文字でも誤ると互換性が崩れる
3. **UTF-16 基準**: `letter_of`/`length`/`contains`/`toLowerCase` を UTF-8 で実装すると絵文字・サロゲートで壊れる
4. **手続きの `yields` 推移閉包**: 「この手続きを呼ぶ側も yield しうる」を固定点反復で求めないと、warp 内の呼び出しを誤って yield 不可能にコンパイルする
5. **エッジハットの「直ちに一度実行」**: `startHats` で作ったスレッドを次ティックまで待たせると旧値がおかしくなる（runtime.js:2261-2275）
6. **broadcast and wait のスレッド集合は開始時点で確定**: 開始済みスレッドのみを待つ
7. **クローンの変数解決はスレッド生成時**: `descendVariable` の「見つからなければ作る」「全クローンにも作る」副作用を JIT 側で再現
8. **`stopThisScript` の復帰位置**: `waitingReporter`/`procedures_call` 境界まで pop する挙動を再現しないと再帰プロシージャの引数評価がずれる（gh-201）
9. **`warpTimer`/`isStuck`**: warp 内 `repeat until timer>0.5` 等を検出して 500ms イールドを入れないとハングする（irgen の `usesTimer`→`warpTimer` の自動有効化を移植）
10. **JIT での変数スロット**: `varSlots` は `compile` 時ではなく **スレッド生成時** に解決する（クローン対応）。`evaluateOnce` のセットアップ変数と同じ仕組み
11. **compat 層の `yield` 内部呼び出し**: 我々のファイバでは「どこでも yield できる」ので、compat プリミティブ内部で `thread.yield()` を呼ぶ実装にできる。ただし Promise 相当の非同期は `STATUS_PROMISE_WAIT` 管理にする
12. **レンダラの `draw` タイミング**: `interpolationEnabled`/`document.hidden` 同等の条件で draw を制御しないと余計な GPU コスト（`_step` 側にこの判断を置く）
13. **`getFencedPositionOfDrawable`**: `fencing` runtime option が ON のとき全ての `setXY` は必ずこのクランプを通す。interpreter/JIT 両方で同じパス
14. **モニタ dirty フラグ**: リスト変更後に `_monitorUpToDate=false` を立てるパスを忘れると monitor が古い値を表示する
15. **プリミティブ配列ブロック**: sb3 の `[4-13,...]` 形式をパース時に展開し忘れると JIT 側で `math_number` 等が見つからない

---

## 19. 未決・要確認項目（実装時に詰める）

1. **x86-64 以外の fiber**: 初期は x86-64 SysV のみ。aarch64/Win64 は後で追加（fiber.hpp の ifdef 構成）
2. **音声エンジン詳細**: `scratch3_sound` の `pitch/pan/robotic/fuzz` エフェクト実装深度は実測で調整
3. **video sensing**: カメラ取得はプラットフォーム依存。`ioDevices.video` をスタブ実装し、`scratch3_video_sensing` 拡張は load 時に無効化する選択肢
4. **sb2 インポート**: 後回し。`sb2.js`/`specMap` の移植量が大きいため M8 以降に検討
5. **拡張の動的ロード**: `extension-manager.js` の unsandboxed 拡張は C++ への FFI 層を用意するか、同期 C++ 拡張のみに絞るか。初期は絞る
6. **モニタの再配置アルゴリズム**: `monitorBlocks`/`tw-monitor-state.js` の auto-position を完全に移植するか簡易化するか（GUI 編集用途でなければ簡易で十分）
7. **`tw` コメント `stuck`/`nocompile`**: `readTopBlockComment` で `warpTimer`/`compile 拒否` を処理する点は実装済み方針だが、フラグの完全な一覧は `irgen.js` と docs を再確認
8. **`compat` の `promise` 意味論**: C++ 側では `std::future`/`callback` で `STATUS_PROMISE_WAIT` を再現。IO 系（fetch, ask）で利用

---

## 20. 名前空間と API スケッチ（最初のコードで使う型）

```cpp
namespace xyo {

// core
class Value;            // NaN-boxed u64
struct SStr;            // GC'd UTF-16 string
struct SList;           // GC'd list storage
class Variable;
class MiniGC;

// model
struct Block;
class Blocks;           // block container + caches
class Comment;
class MonitorRecord;
class Target;
class RenderedTarget;
class Sprite;

// engine
class Runtime;
class Sequencer;
class Thread;
class Fiber;
class FrameLoop;
class BlockUtility;
struct IoDevices { Clock keyboard mouse mouseWheel userData video cloud; };

// ir
enum class Type : uint16_t;      // InputType lattice
enum class StackOp : uint8_t;    // StackOpcode
enum class InputOp : uint8_t;    // InputOpcode
struct IrInput;
struct IrStackBlock;
struct IrScript;
struct IrRep;

// jit
class LlvmJit;
class Codegen;
struct JitCtx;
using ScriptFn = void(*)(JitCtx*);

// rt (extern "C" symbols callable from JIT)
extern "C" {
    Value   xyo_rt_to_number(Value);
    Value   xyo_rt_to_not_nan(Value);
    bool    xyo_rt_to_bool(Value);
    SStr*   xyo_rt_to_string(Value);
    Value   xyo_rt_compare_eq(Value, Value);
    bool    xyo_rt_compare_lt(Value, Value);
    bool    xyo_rt_compare_gt(Value, Value);
    double  xyo_rt_mod(double, double);
    double  xyo_rt_tan(double);
    Value   xyo_rt_random_int(Value, Value);
    Value   xyo_rt_random_float(Value, Value);
    SStr*   xyo_rt_join(SStr*, SStr*);
    Value   xyo_rt_letter_of(Value, SStr*);
    Value   xyo_rt_length(SStr*);
    bool    xyo_rt_contains(SStr*, SStr*);
    // lists
    Value   xyo_rt_list_get(SList*, Value);
    void    xyo_rt_list_add(Variable*, Value);
    void    xyo_rt_list_insert(Variable*, Value, Value);
    void    xyo_rt_list_replace(Variable*, Value, Value);
    void    xyo_rt_list_delete(Variable*, Value);
    void    xyo_rt_list_delete_all(Variable*);
    bool    xyo_rt_list_contains(Variable*, Value);
    Value   xyo_rt_list_index_of(Variable*, Value);
    SStr*   xyo_rt_list_contents(Variable*);
    // threads
    void    xyo_yield(JitCtx*);
    bool    xyo_rt_is_stuck(JitCtx*);
    void    xyo_rt_retire(JitCtx*);
    void    xyo_rt_wait_threads(JitCtx*, Thread** list, size_t n);
    void    xyo_rt_start_hats(JitCtx*, const char* opcode, MatchFields*);
    void    xyo_rt_visual_report(JitCtx*, Value);
    Value   xyo_rt_compat_block(JitCtx*, const char* opcode, ArgsView, BranchInfo*);
    // target ops
    void    xyo_rt_set_xy(Target*, double, double);
    void    xyo_rt_set_direction(Target*, double);
    void    xyo_rt_set_size(Target*, double);
    bool    xyo_rt_touching_object(Target*, SStr*);
    bool    xyo_rt_touching_color(Target*, Value);
    bool    xyo_rt_color_touching_color(Target*, Value, Value);
    double  xyo_rt_distance_to(JitCtx*, SStr*);
    // clock/io
    double  xyo_rt_project_timer(Runtime*);
    void    xyo_rt_reset_project_timer(Runtime*);
    bool    xyo_rt_key_is_down(Runtime*, Value);
    double  xyo_rt_mouse_x(Runtime*);
    double  xyo_rt_mouse_y(Runtime*);
    bool    xyo_rt_mouse_down(Runtime*);
    SStr*   xyo_rt_answer(Runtime*);
    SStr*   xyo_rt_username(Runtime*);
    SStr*   xyo_rt_last_key_pressed(Runtime*);
    // var/monitor
    void    xyo_rt_monitor_change(Runtime*, const char* varId, bool visible);
    void    xyo_rt_request_redraw(Runtime*);
    void    xyo_rt_cloud_update_var(Runtime*, const char* name, Value);
    // ...
}

} // namespace xyo
```

---

## 21. 参考文献

- `refs/scratch-vm/src/compiler/{enums,intermediate,irgen,iroptimizer,jsgen,jsexecute,compile,compat-blocks}.js`
- `refs/scratch-vm/src/engine/{runtime,sequencer,thread,execute,block-utility,target,variable,stage-layering,tw-frame-loop,tw-interpolate,tw-monitor-state,monitor-record,bits}.js`
- `refs/scratch-vm/src/blocks/scratch3_*.js`, `refs/scratch-vm/src/sprites/{rendered-target,sprite}.js`
- `refs/scratch-vm/src/util/{cast,math-util,string-util,timer,log,uid}.js`
- `refs/scratch-vm/src/io/{clock,keyboard,mouse,mouseWheel,userData,video,cloud}.js`
- `refs/scratch-vm/src/serialization/{sb3,sb2,deserialize-assets}.js`, `refs/scratch-vm/src/import/load-{costume,sound}.js`
- `refs/scratch-editor/packages/scratch-render/src/{RenderWebGL,Drawable,Skin,BitmapSkin,SVGSkin,PenSkin,TextBubbleSkin,ShaderManager,Silhouette,EffectTransform,Rectangle,twgl}.js`
- `refs/scratch-vm/src/extension-support/*`, `refs/scratch-vm/src/extensions/*`
- `refs/scratch-vm/test/fixtures/execute/*`（差分テストフィクスチャ）
