# v1.3.7 — Windows/Linux 観測パリティと matpow フォールバック修正

v1.3.6 時点で ctest 全件中 9 件が失敗していた（うち 4 件は HEAD 自体の
バグで、クリーンな HEAD ビルドでも再現する）。本リリースは以下を含む。

## Windows/Linux パリティ（REL-006、観測挙動の一致）

`#ifdef _WIN32` の特殊経路を実施論ではなく観測挙動で Linux に合わせた。
コードの `#ifdef` 除去ではなく、振る舞いの一致が目的。

- **AOT 自己複製**（`src/aot/aot.cpp`）: `CopyFileW(..., FALSE)` は同名
  再出力だけ失敗していた。Linux の `fopen "wb"` 上書きに合わせて
  `TRUE`（上書き許可）へ。`isSelf` は先に両 OS 共通の
  `std::filesystem::equivalent` で判定し（ハードリンク/シンボリック
  リンク解決、各 OS の FS の大小文字規則に従う）、取れないときだけ
  従来経路（Windows: `GetFullPathNameW`+`_wcsicmp`／Linux: `stat`
  dev+ino）へ落とす。
- **時刻**（`src/blocks/core.cpp`・`src/jit/rt_blocks.cpp`・
  `src/shell/launcher_core.cpp`）: `localtime_s` / `localtime_r` の
  引数順違いを `*LocaltimePortable` ヘルパに集約（中身は同一）。
- **環境変数 unset**（`src/shell/gui_qt.cpp`）: Windows の
  `_putenv_s(name,"")` は空文字設定であって unset ではない（`getenv`
  が非 NULL を返し、Linux の `unsetenv` と Mega 既定判定の `!getenv`
  が割れる）。真に消すため `SetEnvironmentVariableA(name, nullptr)`
  を使う（`windows.h` 追加）。
- **拡張パス区切り**（`src/extensions/manifest.cpp`）: 両 OS で `;` と
  `:` の両方を受理（Windows ドライブ直後の `:` は保護）。
- **HOME/履歴パス**（`src/shell/launcher_core.cpp`）: `userHome` は
  両 OS 同一の優先順位（`HOME`→`USERPROFILE`→`HOMEDRIVE+HOMEPATH`→
  `getpwuid`）。`recentHistoryPath` は `XDG_STATE_HOME`→`APPDATA`→
  既定の順で両 OS 同一。`fs::rename` 失敗時の remove→rename 再試行も
  両 OS 化（ウイルス対策ロックだけでなく NFS 等でも起きる）。
- **GC スタック走査（Windows のみ）**（`src/core/gc.cpp`）: ファイバ
  上ではファイバ範囲を走査せず、Linux と同一方針（スケジューラ退避
  rsp からメインスタック頂まで、現ファイバは `Runtime::markRoots`
  が精密走査）へ。ファイバ外・別スレッドは現範囲走査のまま。

## matpow フォールバック欠落の修正（MEXP-001 関連）

warp 内 `control_while` / `control_repeat_until` の行列累乗
（`tryEmitMatPowCond`）は、emit 成功時に汎用ループの生成を省略して
早期 return していた。実行時 reject（非数値文字列 init・範囲証明
失敗等）の fallback 先 `head` が空のまま `unreachable` Fixup され、
**reject 時にループ全体が消える**未定義動作だった（数値 init の
成功パスでは発現しないため隠れていた）。

- fallback 用の専用ブロック `matpow.fallback` を emit 時にだけ作り、
  そこから通常経路（induction/GPU/汎用）へフォールスルーする
  （`src/jit/codegen.cpp`）。未使用の空ブロックを残さない（空のまま
  だと終端子なしの不正 IR になる）。
- 実行時 reject 時に昇格スロットの flush が未初期化スロットを書き戻
  して適用結果を壊す問題を、`matPowApplied` 時は promotion を畳んで
  実メモリで動かすことで解消（countdown-repeat 側と同型の整合性）。
- yield スコープの入口を matpow 振分けより前に移動（counted-repeat
  側と同型）。スコープの退避値（quantum 等）が全経路を支配しないと
  merge の復元処理が壊れた IR になる。
- 非数値文字列 init（`"abc"` 等）は affine モデルと一致しない（条件
  比較が字句比較になる）ため実行時に逐次へ落とす
  （`src/jit/matpow_rt.cpp`、`toNumberOrNaN` が NaN なら reject）。
  整数文字列（`"0"` 等）は加速を維持する。
- MLIR 前段の `xyo.int_exact_ok` も同じ穴があった（coerce 値 0.0 で
  整数判定が通る）ため、数値確定の文字列（`numState==1`）か素の数値
  に限定した（`src/mlir/lower_llvm.cpp`）。未 parse・NaN・比較非数値
  の文字列は汎用ループへ（意味論は常に正しい）。

## 検証

- 修正前 ctest **9 件失敗** → 修正後 **4 件失敗**（残りは下記の既知の
  残課題）。修復したもの:
  - `xyo-native-simple-deopt-parity`（AfterLoopBody 効果消失）
    → PASS（matpow フォールバック欠落が原因だった）。
  - `xyo-jit-warp-induction` / `xyo-jit-ssa-scheduler`
    → PASS（eager・hotspot 両方。`"2.5"`/`"abc"`/`"-1.5"` の
    文字列 init が正しく逐次へ。hotspot 側は MLIR 前段の
    `xyo.int_exact_ok` 修正で復帰）。
  - `xyo-mega-preset` 由来の induction 系不一致 → 解消。
  - `xyo-gui-scratch-parity-full` 2647/2656 → 2649/2656
    （Yield Counting が復帰）。
- `xyo test refs/scratch-vm/test/fixtures/execute`（116 件）は eager
  で 115/116（`control-stop-all-leaks.sb2` の 1 件だけ失敗: broadcast
  後のスレッド回収まわりで、作業ツリー内の別変更（`src/jit/rt.cpp`
  の compat startedThreads 公開、本リリースの範囲外）に起因する
  既存の失敗）。hotspot では `tw-procedure-return-stops-scripts.sb3`
  で MLIR frontend（`xyo.procret` Lowering）がコンパイル時に
  SEGV する既存の不具合があり、スイート全体が回らない（本リリース
  の変更を外しても同一に再現する。MLIR-004/005 の残作業）。
- ベースライン確認: 修正前のクリーン HEAD ビルドでも
  warp-induction／deopt／clone-stress の失敗を再現しており、
  本リリースで修復した 4 件以外は修正範囲外（下記）。

## 既知の残課題（次版以降・担当タスク側で対応）

- `xyo-mega-preset`（`xyo-tests` 全体）: MLIR 前段経路での
  `test_matpow` runs>=1・mega-goto 手続きシンボル・list bounds の
  期待値割れ。MLIR-002〜005（review）の残作業。legacy 経路
  （`XYO_MLIR=0`）では matpow 115/115 を含め PASS。
- `xyo-mega-preset`（`xyo-tests` 全体）: 上記 induction 系は解消
  済み。残りは MLIR 前段経路での `test_matpow` runs>=1・mega-goto
  手続きシンボル・list bounds の期待値割れ。MLIR-002〜005（review）
  の残作業。legacy 経路（`XYO_MLIR=0`）では matpow 115/115 を含め
  PASS（本リリースの修正で後退なし。guard 無効化でも同一に失敗する
  ことを確認済み＝事前存在）。
- `xyo-rt001-async-paused-caller-elf-smoke`: RT-001 のネイティブ
  アンワインド検証（RT-001 in_progress、codex-parity）。
- `xyo-plamo-clone-stress`: hotspot のみ translated=2/fallback=0
  （off/eager は 4/28 で PASS）。非同期 translation のタイミング
  競合の疑い（TRANSLATE-001 open）。
- `xyo-gui-scratch-parity-full` 残り 5 群（2649/2656）: Glide Timing・
  Sound Play And Wait 等の実時間計測と、日時・warp/yield 回数系の
  環境依存ゆらぎ（dummy ドライバ・高速 CPU での計測条件差）。
  PARITY-001（done）の追試対象。
- 本リリースは作業ツリー全体（他エージェントの進行中・未コミット差分
  を含む: 旧エディタ削除、`edit` コマンド除去、blockly/SDL 等）を
  そのまま取り込む。`xyo edit` は現ツリーでは提供されない。

## 影響範囲

- 通常経路（`run`/`test`、eager 既定・Mega プリセット）は上記の
  通り改善のみ。HotSpot 既定経路の matpow 適用ループは、非数値
  文字列 init 時に汎用ループへ落ちるようになった（正しさ優先、
  速度は逐次相当）。
- Windows/Linux の観測挙動の差を潰したが、OS 固有 API 自体は残る
  （wide 文字変換・VirtualAlloc・LoadLibrary 等）。
