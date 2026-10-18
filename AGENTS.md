# xyo-cpp

Scratch/TurboWarp 完全互換の LLVM JIT 対応高速ランタイム（C++20）。
設計の正典は `docs/design.md`。参照実装の移植元は `refs/scratch-vm`（TurboWarp）。

## 状態

- M0/M1 完了: 値・キャスト・数値変換（jsnum、node 差分 4 万件一致）・文字列・GC・リスト・ランタイムヘルパ。
- M2 完了: モデル層・SB3 ローダ・インタプリタ・コアブロック全実装。
- M3 完了: Runtime/Sequencer（hats/clone/warp/broadcast/stopAll detach）移植。
- SB2 ローダ完了（specmap 機械生成）。
- M4 完了: LLVM ORC JIT（`src/jit/`）。execute フィクスチャ **116/116** が両モードでパス（assert: JIT 336/336、インタプリタ 339/339）。ユニット 231 + e2e JIT 85 / interp 80 + pen 31 + renderer JIT 879 / interp 878 パス。renderer の ASan 検証もパス（`detect_leaks=0`、後述の既知リークを除く）。
- JIT カバレッジ拡大（M4b）: operator/list/motion/looks/sensing/control/sound の全コア opcode を native helper 経由で lowering。ベンチ（warp 演算ループ）で interp 比 **3.4×**。
- 並列スケジューラ完了（design.md §4.6）: スレッド間メモリ依存を静的解析して衝突グラフを構築し、独立成分を worker pool で並列実行（`src/engine/deps.*`, `pool.hpp`, `sequencer.cpp`）。独立 8 スプライトの変数ループで **5.4×**。検証フィクスチャは `tests/fixtures/par/`（生成器: `gen_par_fixture.py`）。
- SDL2 ウィンドウレンダラ完了（design.md §10.4）: `xyo play <sb3>` でスプライト/ペン/モニタ/バブル表示 + キー・マウス入力 + green flag/stop UI。`src/render/`（SDL2 + librsvg/Cairo/Pango、stb_image/stb_truetype vendored）。**renderer 接続中は並列スケジューラが自動で無効になる**（`parallelEnabled` ガード — レンダラコールバック非スレッドセーフのため）。
- 完全対応完了: 全7視覚エフェクト（`src/render/effects.*`、scratch-render シェーダ準拠の CPU 実装、描画と pick/タッチングで同一ビットマップ）、スライダモニタ操作（ドラッグで変数書換・離散丸め・min/max クランプ）、スプライトドラッグ（`draggable && visible`、掴みオフセット保持、`wasDragged` でクリックハット抑制）、ask and wait 入力バー（テキスト入力・サロゲートペア編集・`emitAnswer`→`resolveWait`）、オーディオ（`src/audio/sdl_audio.*`、WAV/SBV/MP3 デコード・`playSoundAndWait` 完了で `resolveWait`・pitch/pan エフェクト・loudness、デバイス無しは即解決）。バブル状態は `Target::setSay` で一元化（interp/JIT/ask 全経路共通）。`Runtime::onProjectStopped`/`onQuestion` コールバック追加。検証用 `XYO_INJECT`（`SdlRenderer::handleEvent` 直接注入）で対話操作を自動検証済み。
- ペン拡張完了（`src/blocks/pen.cpp`）: scratch3_pen 全 opcode（色/サイズ/レガシー hue/shade）。`Target::penState` + `onTargetMoved` フック（penDown 中のみセット、clone に複製）。`RendererAPI` に `createPenSkin/penLine/penPoint/penClear/penStamp` を追加（`Runtime::penSkinId`）。
- **AOT コンパイル（実験的、`src/aot/`）**: `xyo aot <project.sb3> -o <app>` は全スクリプトを 1 モジュールへ emit → O2 → オブジェクト化し、実行中バイナリの末尾へ `[sb3][object][footer]` を追記した単体実行ファイルを作る（Linux。`./app` で再生、`./app run [--frames N]` でヘッドレス）。生成コードはホストの絶対アドレスを埋め込まず `xyo_aot_slot_N` グローバル経由で参照する（`src/jit/aot_emit.hpp`）— 変数 id C 文字列・BlockCached*・SStr* リテラル・ASCII キャッシュ・pen 移動フック。`JitEngine::loadAot` がロード時に slot へ実アドレスを書き込み、通常のコンパイルキャッシュへ登録するので以降は JIT と同じ経路。block id はターゲット間で一意とは限らないため slot キーはターゲット index を前置する。**GPU 並列ループも slot 経由**（`AotSlotKind::GpuProgram`、キーは target index + loop block id）: ロード時に同じループを `gpu::compileParallelLoop` で再コンパイルし、`Program*` を slot へ束縛する（決定論的なので AOT 時と同一のプログラムになる。再コンパイルに失敗した場合は nullptr のまま → 生成コードは CPU ループへ）。メタに sb3 の FNV-1a ハッシュを持ち、不一致・AOT 不能 script は JIT/インタプリタへフォールバック（クラッシュしない）。検証用に `--obj <file>`（オブジェクトのみ出力）と `xyo run/play/test ... --aot <obj>` を用意。検証: execute フィクスチャ 116 件中 115 件で `test` 結果が JIT と一致（残り 1 件は AOT 対象 script が無く skip）、mandelbrot は SDL dummy の PPM が byte 一致、3D2（56 scripts/275 slots）と TFG（70/327）も AOT 実行を確認。
- **ランチャー GUI（`src/shell/gui.cpp`）**: 引数なしの `xyo`（ダブルクリック起動）または `xyo gui [dir]` で SDL2 のランチャーが開く。`*.sb3`/`*.sb2` とディレクトリを一覧し、クリック / ↑↓ / Enter で実行、`Backspace` で親フォルダ、`Esc` で終了。[AOT 実行] は事前コンパイルしたオブジェクトで再生、[AOT 書き出し] は同じ場所へ `<名前>` の単体実行ファイルを生成する（既存なら `<名前>-aot`）。書き出しに対応できない script は JIT/インタプリタへフォールバック（GUI からも失敗しない）。再生を閉じるとランチャーへ戻る。文字は `FontRenderer`（`XYO_FONT`）で、フォントが無い環境では矩形フォールバックでも操作は可能。起動フォルダは `XYO_PROJECT_DIR` でも指定できる（未指定は実行ディレクトリ、`.sb3` が無ければ `$HOME`）。自動検証は `XYO_GUI_ACTIONS="down,export,quit"`（1 フレーム 1 アクション: down/up/home/end/enter/back/run/aotrun/export/quit。指定時は 1 回の再生でプロセス終了）、`XYO_GUI_DEBUG=1` で選択・書き出しログ。
- 次の候補: さらなるパフォーマンス最適化（var_get のインライン化・SList 直接操作等）。

## JIT ブロックヘルパ層（src/jit/rt_blocks.cpp）

- `codegen.cpp` の `directInputs()`/`directStack()` テーブルが opcode→C ABI ヘルパを解決。引数は inputs→fields の順で渡す。`ctx=false` エントリは `JitCtx*` を取らない純粋関数（join/strlen 等）。
- ヘルパの意味論は `src/blocks/core.cpp` 冒頭の共有関数（`updateBubble`/`setCostumeArg`/`setBackdropArg`/`clampEffect`/`moveSteps`/`getTargetXY`/`getSoundIndex`）か `src/rt/runtime_helpers.hpp` の `rt::list*` を再利用 — **意味論を重複実装しない**。
- リスト系は TW jsexecute の `listIndex` 意味論（`rt::list*`）に従う — interp の `toListIndexD` とは 'all'/NaN 扱いが意図的に異なる（TW 自体が interp/JIT でこう分かれている）。
- `procedures_return` は `exitBB`（関数単位の遅延生成終了ブロック）へ分岐 — warp 手続きでは `isWarp` 復元をエピローグに集約。
- 新規ヘルパ追加時の手順: rt_blocks.cpp に extern "C" 定義 → jit.cpp に宣言+`kRtSymbols` 登録 → codegen のテーブルにエントリ。**引数順不一致が最大のバグ源**（emit は inputs→fields 順）。
- **入力評価順**: `call2("f", emitInputOf(A), emitInputOf(B))` は C++ 引数評価順（未規定・GCC は右→左）に IR emit 順が依存し、reporter 副作用が逆順実行される。複数入力は必ず `callOp`/`callOpCtx`（左→右逐次 emit）。`callN` の braced init-list は左→右保証で安全。
- **reporter 内 yield**: `stepThreadBound` は `u.yield`・非 warp ループ境界で `status=STATUS_RUNNING` に戻して早期 return する（STATUS_YIELD は呼出し側に見えない）。`xyo_rt_reporter` は「`stack.size()>depth && status==RUNNING` で return」を yield 境界として `xyo_yield` する。
- **手続き呼出しのスタックフレーム**: `emitProcedureCall` はネイティブ関数呼出しだが、呼出し中は `xyo_proc_push`/`pop` で interpreter スタックにも `procedures_call` フレームを残す。yield 中の warpMode 参照・compat op が読む `Thread::stack` 形状・interp `stepToProcedure` の `isRecursiveCall` がこれを見る。**leaf 手続きでは省略可**: `elideLeafProcFrames`（codegen 末尾）が「自身のコードで yield/compat なヘルパを呼ばない（fnDirect が空）」proc 呼出しの push/pop を IR から消す — 上記の観測者は全て yield/compat 経由でしか到達しない。再帰 proc も対象（JIT の再帰はネイティブ再帰でフレーム非依存、interp 側手続き呼出しは def ブロックを直接実行する）。`fnDirect` に載る yield 可能ヘルパのリスト（`helperMayYield`）と同期必須。`Thread::framePool` が push/pop の StackFrame をプール再利用する（pop 時に reset — GC 参照も即解放、pop 後のフレーム内容を読む JIT 観測者は存在しない）。
- **手続き warpMode は継承+昇格のみ**: `isWarp = caller.isWarp || def.warp`（JS `stepToProcedure` / TW irgen 準拠）。warp 文脈から呼ばれた非 warp 手続きは **warp のまま**動きループ yield しない — 定義側フラグで 0 に降格してはならない（"The fastest tri fill engine" で非 warp `fill.draw` が毎反復 yield し、fill がフレームを跨いで破壊されていた）。`xyo_proc_push` が積むフレームの warpMode も同じ合成値。
- **手続き呼出し ABI はレジスタ引数**: proc fn は `(JitCtx*, a0..aN)` の固定シグネチャ。実引数はレジスタ直渡しで `ctx->args`/`pargs` の save/set/restore は存在しない。callee 側 `emitArgGet` は仮引数（`argVals`）を直接返す。`cs->procedures` への外部呼出し経路は無いため ABI 変更は codegen 内に閉じる。
- **小 proc の強制 inline**: `inlineSmallProcCalls`（emit 後・verify 前）が ≤220 inst の proc を `llvm::InlineFunction` で callsite に展開 — O2 が大関数への inline を渋って残していた call/引数準備/ret を消す。再帰・不可分は LLVM が拒否して残る（安全側）。fnDirect/callEdges/elide の意味論判定は emit 時点のグラフで済んでいるため IR inline は観測不能。
- **`operator_letterof` ASCII fast path**: `emitLetterOf` が (num index && str && 1<=idx<=len && unit<0x80) のとき GC ルート済み共有キャッシュ `g_asciiCache`（`xyo_ascii_cache()`）を直接返す。それ以外は `xyo_letterof`。interp 側は `rt::letterOf` 内の `letterCached` が同じキャッシュを使う（単一実装）。
- **list ヘルパは `Variable*` 直渡し**: `xyo_list_*` の第2引数は `Variable*`（`varSlot` 由来）。`data_addtolist`/`itemoflist`/`replaceitemoflist` は JIT inline fast path（num index・numCache 有効な str index・cap 有り add）— helper 呼出しは非 num index・cap 不足・null のみ。
- **`rt::join` (str,num) fast path**: `sstrConcatAscii` が jsnum 表記（ASCII のみ）を直接書き込み中間 SStr を省く（linux.sb3 では join の ~90%）。`jsnum::toString` は |v|<9.2e18 の i64 往復整数を `to_chars` で直出し（ES では |v|<1e21 まで指数にならず、往復探索を省略可）。
- **GC scanRange のページフィルタ**: `Gc::pageSet_`（ヘッダ/ペイロード開始のページ）で非マッチワードの objSet ルックアップ3回を1回に削減。sweep 毎に生存オブジェクトから再構築。
- フィールド欠落（LIST/VARIABLE 無し）は interp の `obj()==nullptr` 経路 — codegen で早期 return/デフォルト値を emit し、実行時の空変数自動生成を避ける。
- **未定義 `procedures_call` は NOP**: prototype または definition が存在しない呼び出し（削除済み手続きの残骸 — linux.sb3 の `'​​log​​ %s'` ゼロ幅スペース入り proccode 等）は TW irgen と同じく**引数も評価しない純粋な no-op**。scanChain で bail するとスクリプト全体が interp へ落ちて致命的に遅くなる（エミュレーションコアが毎フレーム warpTimer まで回る）。emitProcedureCall も同じ条件で何も emit しない。interp の procCall も早期 return で no-op。
- **メニュー shadow のフィールド名は親入力名と違い得る**: `pen_menu_colorParam` の実 SB3 フィールド名は `colorParam`（小文字）。プリミティブ登録済み menu は `isShadowBlock` にならず独立 op として走るため、自身の `argValues` には**自身のフィールド名**で値が入り、戻り値が親の `parentValues->vals[親入力名]` へ書き戻される。親入力名で読むと undefined になる（interp が transparency=50 を無視して alpha=1.0 になった件）。JIT 側は `fields.begin()` で名前非依存 — interp も第一フィールドへフォールバックさせて揃えた。
- **`sensing_dayssince2000` は ms 精度必須**: 値は `(Date.now() - Date.UTC(2000,0,1))/86400000`（scratch-vm の local/DST 補正は代数的に相殺され UTC 起点に帰着）。`std::time` は秒精度のため `days*86400` で差分を取るプロジェクト（"tri fill engine" の fps.time 計測）が整数秒に量子化される。`rt::daysSince2000()`（chrono ms）に interp/JIT 双方を統一した — 再重複させない。

## ビルド

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build        # libxyo.a + xyo シェル + tests/xyo-tests
# ASan:
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXYO_JIT=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build-asan
```

- LLVM は `llvm-config` で検出（`llvm orcjit native passes`、現在 LLVM 23）。`XYO_JIT` は既定 ON。`XYO_JIT=OFF` では `src/jit/nojit_stub.cpp`（JitEngine 空実装 + C ABI スタブ）が入り、`->jit` 参照に ifdef を散らさない — interp のみ。
- SDL2 は Linux では pkg-config、それ以外では `find_package(SDL2 CONFIG)`（`-DSDL2_DIR=` / `-DCMAKE_PREFIX_PATH=`）でも探す。`SDL_MAIN_HANDLED` を定義して SDL に main を奪わせない。
- **librsvg は任意**（`option(XYO_RSVG ...)`、既定 ON）: 見つからなければ SVG コスチュームを失敗キャッシュし、ビットマップコスチュームのみ描画する（headless / Windows はこれで動く）。Cairo/Pango/Fontconfig は librsvg のリンク依存で、SVG の text/tspan とフォント選択・Unicode シェーピングを担う。SVG 内の文字は OS のフォントを使う。モニタ/バブルの文字は同梱フォント（後述）と `XYO_FONT` で描く。
- **静的リンク（リリース用）**: `-DXYO_STATIC_LLVM=ON`（静的 LLVM + 静的 libstdc++/libgcc）、
  `-DXYO_STATIC=ON`（完全静的 `-static`）、`-DXYO_SDL2=OFF`（headless）、
  `-DXYO_LLVM_MODE=auto|config|llvm-config`（既定 auto = llvm-config 優先、
  無ければ LLVMConfig.cmake）。Ubuntu の **`llvm-18-dev` は静的アーカイブを同梱**する
  （apt.llvm.org の `llvm-23-dev` は同梱しない）ため、静的リンクは LLVM 18 +
  `llvm-config-18 --link-static` で行う。LLVM 18〜23 の API 差は
  `src/jit/llvm_compat.hpp` が吸収する（`lookupTarget`/`createTargetMachine`/
  `setTargetTriple` の Triple vs StringRef、`ThreadSafeContext::withContextDo`）。
  完全静的は SDL2/librsvg を外した headless のみ（librsvg の静的版は無い）。
  `-static` 時は `ZLIB::ZLIB`（.so を指す）ではなく `libz.a` を明示的にリンクする。
- **静的リンクでは ORC の dlsym が効かない**: バックエンドが `frem`→`fmod`、
  `llvm.log`→`log`、memcmp→`bcmp` の libcall を生成する。動的リンクでは
  `DynamicLibrarySearchGenerator` が解決するが、`-static` では自プロセスの
  静的シンボルが見えず `JIT session error: Symbols not found: [ fmod ]` に
  なる（interp へフォールバックするので気付きにくい）。`jit.cpp` の
  `hostLibcallSymbols()` を `kRtSymbols` と同じ absoluteSymbols として
  JITDylib へ define して解決する — **新たな libcall が必要になったらこの表へ追加**。
  CI/Release の静的ジョブは stderr の "JIT session error" を検出したら失敗する。
- `-fno-exceptions -fno-rtti`。例外は使わず error コード/`std::optional`。
- アセンブリ: `src/engine/fiber_x86_64_sysv.S`（FiberRegs レイアウトは fiber.hpp と厳密一致）。aarch64 は未実装。Windows x86-64 は `fiber_x86_64_win64.S`（Win64 ABI: rdi/rsi/xmm6-15 も callee-saved、`this` は r15→rcx）。

### Windows .exe（MSYS2 ネイティブ = JIT / zig クロス = interp）

```sh
# 1) zlib を Windows 向けにビルド（zig toolchain を再利用）
mkdir -p .windeps && curl -sL -o .windeps/zlib.tar.gz \
  https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
tar xzf .windeps/zlib.tar.gz -C .windeps
cmake -B .windeps/zlib-build -S .windeps/zlib-1.3.1 \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/zig-windows-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .windeps/zlib-build --target zlibstatic
mkdir -p .windeps/zlib/lib .windeps/zlib/include
cp .windeps/zlib-build/libzlibstatic.a .windeps/zlib/lib/
cp .windeps/zlib-1.3.1/zlib.h .windeps/zlib-build/zconf.h .windeps/zlib/include/

# 2) xyo.exe（JIT なし — LLVM for Windows は zig に無い）
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/zig-windows-x86_64.cmake \
  -DXYO_JIT=OFF -DXYO_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
  -DZLIB_LIBRARY=$PWD/.windeps/zlib/lib/libzlibstatic.a \
  -DZLIB_INCLUDE_DIR=$PWD/.windeps/zlib/include
cmake --build build-win    # → build-win/xyo.exe（PE32+ x86-64、約 1.5MB）
```

- ツールチェーン: `tools/zig-{cc,cxx,ar,ranlib}` + `cmake/zig-windows-x86_64.cmake`（`x86_64-windows-gnu`、pkg-config 無効化 — ホストの Linux SDL2 誤検出防止）。fiber switch は `src/engine/fiber_x86_64_win64.S`（Win64 ABI: rdi/rsi/xmm6-15 も callee-saved）。
- **JIT 対応（MSYS2 mingw LLVM）**: `windows-latest` + MSYS2 の `mingw-w64-x86_64-llvm`
  （**静的アーカイブ同梱**）で JIT 有効ビルドができる:
  ```sh
  cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DXYO_JIT=ON -DXYO_LLVM_MODE=llvm-config \
    -DXYO_STATIC=ON -DXYO_STATIC_LLVM=ON \
    -DXYO_SDL2=OFF -DXYO_BUILD_TESTS=OFF
  ```
  `XYO_STATIC=ON` で LLVM/libstdc++/libgcc を静的リンクした単体 `xyo.exe` になる
  （`LLVM-*.dll` 不要）。**AOT 書き出しも Windows で動作**する（COFF オブジェクトを
  末尾に追記し、ORC でロード）。JIT 有効の Windows ビルドは初めてコンパイルされる
  コード（`src/jit/`）を含むため、POSIX 依存（`localtime_r`、`M_PI`）は
  `_WIN32` ガードが必要 — `windows-jit` CI ジョブが検証する。
- **zig クロスは interp 専用**（LLVM for Windows が無いため）。`run` / `test` と
  **Vulkan compute による headless GPU オフロード**（`LoadLibraryA("vulkan-1.dll")`）、
  **UTF-8 パスとコンソール**は従来どおり。
- **UTF-8**: コマンドラインは `GetCommandLineW` + `CommandLineToArgvW` で UTF-16 から UTF-8 へ取り直し（`main.cpp` の `winInit`）、`SetConsoleOutputCP(CP_UTF8)` で日本語出力も化けない。ファイルは `src/core/fileio.*` の `fopenUtf8` / `pathFromUtf8` が UTF-8 → UTF-16 へ変換して wide API に渡すので、日本語ファイル名の sb3 も開ける（`XYO_FONT` / `XYO_SHOT` も同様）。
- **`play` / `gui` を有効にする**: Windows 版 SDL2 を用意して `-DSDL2_DIR=<SDL2-devel-*-mingw>` を渡す（クロスビルドでホストの Linux SDL2 を拾わないよう、`SDL2_DIR`/`CMAKE_PREFIX_PATH` の明示が無ければ config モード探索をしない。Windows 以外の SDL2 を掴んだ場合は `IMPORTED_IMPLIB` 検査で拒否）。`SDL_MAIN_HANDLED` を定義して SDL に main を奪わせない。**librsvg は任意**: 無ければ SVG コスチュームを失敗キャッシュしてビットマップのみ描画する（`-DXYO_RSVG=OFF` で明示的に切れる）。同梱フォントは SDL2 があるときだけ埋め込む（テキスト描画が無いビルドでは死荷重になるため）。
- **AOT**: `writeExecutable`/`readSelf` は Windows でも動く（`GetModuleFileNameW` + `CopyFileW` + 追記）。MSYS2 JIT ビルドでは `xyo aot` が実際に動く（COFF オブジェクトを追記 → ORC でロード、CI で検証済み）。
- **zig クロスビルドは interp のみ**（LLVM for Windows が無いため高速化は Vulkan compute 頼み）。JIT が要る場合は MSYS2 ビルドを使う。
- 移植済み shim: `M_PI`/`M_LN10`（#ifndef 定義 — ISO 外）、`localtime_s`、`_strtod_l`/`_create_locale`、`GetCurrentThreadStackLimits` + メインスタック記録（fiber 上の collect 対策）、VirtualAlloc/VirtualProtect/VirtualFree。`windows.h` を直接 include する箇所は `NOMINMAX` + `WIN32_LEAN_AND_MEAN` を定義する。
- デバッグ用環境変数: `XYO_JIT_DUMP=1`（生成 IR ダンプ）、`XYO_JIT_DUMP2=1`（O2 後 IR ダンプ）、`XYO_JIT_DISABLE=1`（JIT 無効・インタプリタのみ）、`XYO_JIT_VERBOSE=1`（コンパイルログ）、`XYO_PROF=1`（sequencer/compile の内訳プロファイルを終了時に stderr 出力）、`XYO_PAR=0`（並列スケジューラ無効化）、`XYO_DEPGRAPH=<dir|->`（衝突グラフの DOT 出力、`-` で stderr）、`XYO_SEED=<n>`（`Rng::global` を固定シードに — AOT/JIT/interp の一致検査用。並列時は順序で揺れるため `XYO_PAR=0` 併用）、`XYO_DUMPVARS=1`（`xyo run`/AOT 実行の後に全変数・リストを決定的順で出力）、`XYO_AOT_VERBOSE=1`（AOT slot 束縛ログ）、`XYO_GUI_ACTIONS=<a,b,...>`/`XYO_GUI_DEBUG=1`（ランチャー GUI の自動操作・ログ）、`XYO_PROJECT_DIR=<dir>`（GUI の開始フォルダ）。
- `xyo play` 用: `XYO_FONT=<path>`（フォント指定）、`XYO_EXIT_FRAMES=N`（N フレームで終了）、`XYO_SHOT=path.ppm`（最終フレーム保存）、`XYO_INJECT="F:op:args|..."`（合成イベント注入: `md/mu/mm:x,y`、`txt:utf8`、`key:name`）、`XYO_DEBUG=1`（UI/オーディオのデバッグログ）、`XYO_PENLOG=1`（各ペンストロークの直径・端点・alpha を stderr に出力 — JIT/interp の描画差分切り分け用）、`XYO_PENPROF=1`（ペンラスタ/バッチ集計を終了時に stderr）、`XYO_PENSKIP=1`（ラスタ/バッチをスキップ — 計測分離用）、`XYO_PEN_CPU=1`（GPU ペン層を無効化して CPU ラスタへ）、`XYO_SET="name=value;..."`（緑旗直後に変数セット — ベンチ用）。`SDL_VIDEODRIVER=dummy`/`SDL_AUDIODRIVER=dummy` でヘッドレス検証可能。デモ: `python3 tests/fixtures/gen_play_demo.py examples/play_demo.sb3` → `./build/xyo play examples/play_demo.sb3`、全機能検証用は `tests/fixtures/gen_fx_demo.py`。

## JIT パフォーマンス構造（ベンチマーク実測から）

- **FiberPool / JitCtx pool**（fiber.cpp / sequencer.cpp）: スクリプト完了時に fiber と JitCtx を即座にプールへ返却（Thread 自体は `ownedThreads` で ~Runtime まで生存するため ~Thread では遅すぎる）。**実行中の fiber は絶対に release しない**。
- **`CompiledScript::mayYield`**: 生成コード内に yield 可能ヘルパの呼出しが無い場合 false → `jitStepThread` は fiber を介さず `entry(ctx)` を同期実行（broadcast 受信等の短命スレッドの切替コストを排除）。**codegen の `helperMayYield` リストは rt.cpp/rt_blocks.cpp の `xyo_yield` 使用箇所と対応 — 新規 yield 追加時は必ず更新**（漏れると `c->fiber==nullptr` で yield → SEGV）。
- **compile キャッシュ**: トップ `Block*` キー（`Blocks::blocks_` は unordered_map でポインタ安定）。失敗/対象外は `failed_` に入れ再試行しない。`RuntimeScriptCache::block` が hint として `pushThread`→`tryCompile`→`compile` に渡り getBlock を省略。
- **broadcast 系ワークロードはスレッド spawn 支配**: 送信1回毎に pushThread（Thread 構築 ~600ns、両モード共通）+ compile ヒント（JIT のみ ~150ns）。mayYield 同期実行・compile ヒントで JIT も interp 並み（実測 ~1.08×）。fiber switch 自体は往復 ~11ns で律速ではない。
- **hot loop 向け direct lowering（3D2 系で 24.5s→3.0s/60f）**: 変数スロット（`JitCtx::varSlotData` + `xyo_varslot_init` でエントリ時一括解決・proc 呼出しでは再解決無し）・算術/比較/round/mathop の inline 化・`SStr::numCache/numState`・`rgbCache/rgbState`（`toRgbColorObject` パースキャッシュ）・`PenState::lastSetColor` メモ + `colorDirty` 遅延 rgba 計算・`sensing_mousedown`/`motion XY`/`pen` の直接フィールドアクセス（Layout はオフセットを**実インスタンスのアドレス差**から算出 — `offsetof` は非標準レイアウト警告になる）。
- **`knownNum` 追跡**: `emitCanon` 戻り値の SSA 値は num タグ保証だが、NaN でない保証ではない。関数境界で clear。数値演算の入力変換 `emitToNumF64` は定数・既知 num・動的 num の全経路で `emitNotNaN` 相当の NaN→0 を行う（Scratch Cast.toNumber / TW toNotNaN）。演算結果そのものの NaN は `emitCanon` で保持し、比較用 `emitCmpNumF64` の NaN 扱いとは分ける。
- **JIT の固定サイズ alloca**: ループカウンタ `rep_i` / `fe_i` と手続き引数配列 `pargs` は `entryAlloca` で現在の native 関数の entry block に配置する。ループ本体の `CreateAlloca` は反復ごとに rsp を下げ続け、3DOcean では Scratch スタック深さ 5 のまま 1MiB の fiber guard に達していた。値の初期化・引数評価は元の実行位置を維持する。各 callsite の領域は native 呼出し単位で独立し、再帰・clone・yield 中の引数も共有しない。reserve を増やして隠さない。
- **失敗した最適化（再試行禁止）**: 「yield スキップ」（`xyo_yield` を他実行可能スレッド無し時に早期 return する案）は **waitThreads/compat reporter の yield がスケジューリングを駆動する設計**と衝突し ~6× 劣化。yield のスキップは不可 — fiber switch が唯一の進行保証。
- warp 中のインタプリタ手続き（`xyo_rt_reporter`→`stepThreadBound`）は `warpTimer>500ms` まで無中断実行 → 1 step が 300-600ms になり得る（TW の isStuck と同モデル・仕様）。`passes` 少数は異常ではない。

## 描画性能と回帰テスト

- **空コスチュームの再デコードを防ぐ**: `SdlRenderer::skins_` は成功した Skin と、決定的な失敗の `nullptr` を両方キャッシュする。空データ・サイズ 0 の SVG・不正画像を移動のたびにデコードしない。`getFencedPosition` はペンの各画素を描く移動でも呼ばれるため、失敗キャッシュの欠落は 3D2 で 30 描画フレームあたり約 520 万回の SVG 再解析になっていた。Cairo surface/context の生成失敗と `SDL_CreateTexture` のリソース失敗はキャッシュせず再試行可能に保つ。デストラクタは null エントリをスキップする。
- **移動フェンスはスプライト全体を画面内へ収めない**: `getFencedPosition` は参照 RenderWebGL と同じ AABB から `inset=floor(min(width,height)/2)`、各半ステージから `min(15,inset)` を引いた境界を使い、一部が見えるまで移動を許す。回転・左右反転・回転中心・サイズを既存の bounds 計算に反映し、境界越えだけ ceil/floor で補正する。空/不正/スキン無しは現在位置の点 bounds とし、標準ステージでは x=±240、y=±180 に到達する。仮の30×30を使うとペンが±225/±165の450×330へ縮み、3D描画の端が圧縮される。四隅と全172800画素のペン移動経由テストを維持する。
- **モニタ表示は scratch-gui 準拠**: `drawMonitors()` は `refs/scratch-editor/packages/scratch-gui/src/components/monitor/monitor.css` と `monitor.jsx`（light color mode）に合わせる。コンテナ `#E5F0FF` + 1px `rgba(0,0,0,.15)` + radius 4 + padding 3、フォント 12px（large のみ 16px）。default/slider は太字ラベル `#575E75` + 値チップ `#FF8C1A`（白文字・min-width 40・radius 4）、large はカテゴリ色の箱 + 白文字の値のみ、list は白ヘッダ（下線・中央太字）/ 本体 `#E5F0FF` / 行 24px（太字 index + 値チップ `#FF661A` + 白文字 + 1px 枠）/ 白フッタ（`+`・`length N`・`=`）。リストの幅高さは sb3 の monitor `width`/`height`（`sb3.cpp loadMonitor`、0 は既定 110×200）、はみ出す行はクリップしてスクロールバーを描く。値は `monitor-adapter.js` と同じく**数値のみ `toFixed(6)` → Number → 文字列**（リスト項目は丸めない）。太字はフォント側の合成ボールド（`FontRenderer::render(..., bold)`）。モニタはステージ canvas 上の HTML オーバーレイ相当なので **green flag/stop・ask バーより後に描く**（`draw()` の順序）。
- **同梱フォント（`third_party/fonts/`）**: Scratch の font-family は `"Helvetica Neue", Helvetica, Arial` で Linux では Arial 系メトリクスの Liberation Sans に解決されるため、**Liberation Sans Regular/Bold（OFL、`LICENSE-Liberation.txt` 同梱）を埋め込む**。`tools/embed_font.py` がビルド時に TTF を C++ 配列（`xyo_fonts.inc`）へ変換し、`text.cpp` が `#include` する（CMake の `XYO_HAVE_EMBEDDED_FONT`）。`FontRenderer` は 3 フェイス — 0=regular（`$XYO_FONT` があればそれを優先）、1=bold（同梱 Bold）、2=fallback（OS のシステムフォント。日本語など同梱に無いグリフ用）— を **コードポイント単位で順に試す**。太字は Bold フェイスがあればそれ（本物の太字）、無ければアルファを横へ grow px 膨張する合成ボールド（テクスチャ 2 回重ねは二重像になるので使わない）。AOT の単体実行ファイルにもフォントが入るため、Latin は OS フォント無しでも表示できる。
- **ペンの画素モデル**: ペン層はステージの native 解像度、内部は premultiplied RGBA8。幅が元の double で厳密に 1 または 3 の場合のみ Scratch 座標へ +0.5 の補正を行う。座標は整数へ丸めず画素中心 (x+0.5,y+0.5) で評価し、参照 line シェーダと同じ `clamp(width/2+0.5-distance,0,1)` を合成する。幅≤8 の点の被覆率は 8 個の有界カーネルにキャッシュする。SDL アップロードと色サンプリングでは straight RGBA に変換する。以前の 2 倍解像度・二値円/5 texel cross は参照と異なるため使わない。
- **GPU ペン層（XYO_PEN_GL）**: libGL 検出時に有効化。SDL_Renderer の backend が opengl 系のとき、自前の `SDL_GL_CreateContext` + 480×360 FBO + フラグメントシェーダ（カプセル距離を解析評価、CPU と同じ `clamp(r+0.5−dist)` 式）でペンを描画する。ストロークは quad として CPU 側バッファ `GlPen::v/ix`（生配列、realloc 成長）に蓄積され、`penSyncCpu` が1回の `glDrawElements` で FBO へ描画し `penPx_` へ行反転 readback する。**readback は CPU 読者（`stageColorAt`/`penStamp`/テクスチャ upload/`savePPM`）の前だけ** — ストローク毎には行わない。`penClear` は未描画バッチを破棄して FBO を直接クリア、`penStamp` は sync→CPU ブレンド→行反転 `glTexSubImage2D` で FBO へ戻す。`XYO_PEN_CPU=1` で CPU ラスタへ強制フォールバック。コンテキストは 4.3 core→3.3 core→デフォルト互換の順で試し、失敗時は CPU。The fastest tri fill engine: @tps 1,506 → **166,664**（CPU ラスタ最適化込みの110倍、wayland/opengl）。
- **GPU 並列スクリプト実行（src/gpu/）**: warp 中の `control_repeat`・`control_for_each`・回数を証明できる `control_while` / `control_repeat_until` を `ParallelLoop` へまとめる。構文だけで独立性を決めず、生成した f32 VM バイトコードを `gpu_analysis.cpp` の CFG 固定点解析で検査する。JIT は従来の ABI 名 `xyo_gpu_repeat` を使い、失敗時は元の CPU ループを実行する。内側のループ・分岐・`control_all_at_once`・安全な手続きのインライン展開にも対応する。
- **GPU 化の証明条件**: スタック高さとジャンプ先、全経路での代入、位置・ペン状態・属性の読み書きを検査する。反復内で読む前に代入する変数は lane-local、本体で書かない変数は有限数値のスナップショット。反復をまたぐ変数は定数整数ステップの affine recurrence のみ受理し、開始値・反復番号との積・中間値・終了値を f32 の厳密整数範囲 ±2^24 に制限する。f32 への丸めで整数に見えるソース小数を、整数ステップと誤認しない。while/until はカウンタと不変境界を分離し、方向と終了回数を実行時にも検査する。境界の数値文字列は比較用キャッシュを初期化するが、生の NaN は 0 に置き換えず CPU へ戻す。手続きの引数は呼出し側スコープで先に評価する。変数は `[12]` shadow 経由も含め ID で解決し、一時スロットの番号はユーザの変数 ID と分離する。
- **64bit ペン順序 ID**: 上位32bit が論理反復（lane）、下位32bit が反復内 stroke。VM 完了後に lane ごとの count を読み戻して prefix offsets を作り、GPU が順序付き `uvec2` ID 列を生成して、その順でレコードを描画する。一般ソートや GPU の完了順には依存しない。ID は同期 dispatch 内のもの。dispatch 間や CPU ペン描画との順序は先行バッチの flush と GL の描画順で保ち、プロジェクト全体のグローバルな遅延描画 ID ではない。lane0 は実際のペン状態、後続 lane は penUp で開始するため、状態の観測前リセットなどを証明できない本体は拒否する。全経路 penUp 終了は必須ではない。位置・ペン属性・変数は成分ごとの最後の論理 writer を書き戻し、冗長な penDown では点を重ねない。
- **GPU の実行時ガード**: 非warp・stage・dragging・対象外の型・変数の解決失敗や書込み alias・小さいループ・数値やハードウェアの制限により CPU へ戻す。GL 経路は code/slot/wb/SSBO/workgroup と確保サイズを検査し、レコード超過・未知 op・20万命令上限などの失敗はレコード描画前に判定する。失敗時に変数・ターゲット・RNG を書き戻さない。乱数は既存の lane 別 xorshift 系列を維持し、dispatch ごとの seed は RNG のコピーから取り、成功時だけ更新を反映する。CPU の共有 RNG とは別系列（design.md §5.3）。`XYO_GPU=0` で無効化、`XYO_GPU_MIN=N`（既定16）、`XYO_GPU_DEBUG` / `XYO_GPU_DUMP` で計装する。
- **GPU 回帰テスト**: `xyo-gpu-tests --parallel` が共通解析とヘルパを検査する。GL4.3 以上を利用できる環境では `SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy XYO_TEST_GPU=1 ./build/tests/xyo-gpu-tests --parallel` で4種類のループの実 dispatch、半透明赤/青の重なり、CPU 描画を挟む2 dispatch、超過時の非破壊フォールバックを検査する。画像比較は整数座標・同じ GL ペン層での CPU 実行との byte 一致であり、任意の f32 計算や CPU ラスタとの完全一致を主張しない。今回の検証は通常 CTest 3/3、実 GL の377チェック、`XYO_JIT=OFF` の共通解析113チェックが失敗0。
- **旧 repeat 専用経路の実測記録**: The fastest tri fill engine（test→fill→fill.draw の入れ子 proc 呼出し+内側 repeat を含む 10000 lanes）: @tps **4,999,933**（CPU JIT 499,997 の 10×、GPU ペンのみ時 166,664 の 30×）。今回の共通解析追加後の速度として再計測した値ではない。
- **GPU ループが受理する text リテラル**: `literalOpcode` は数値系のみだが、`text` shadow でも「数値化 → ES 表記に戻して元文字列と完全一致」するもの（例 `"1"`）は数値と観測等価なので受理する（`textLiteralIsNumber`）。`"01"`/`"1.0"`/`"-0"`/`"abc"` は従来どおり拒否。これが無いと "The fastest tri fill engine" の `fill.draw`（`set var to (text "1")`）が `expr op: text` で落ち、GPU プログラムが 1 つも生成されず CPU 実行になる（`XYO_JIT_VERBOSE=1` の `gpu loop skip:` で確認できる）。
- **Vulkan デバイス選択はハードウェア優先**: DISCRETE > INTEGRATED > VIRTUAL の順に選び、CPU 実装（llvmpipe/lavapipe）は既定では使わない — `XYO_VK_DEV=<名前部分列>` を明示したときだけ許可する。ソフトウェア Vulkan は同じ計算を CPU JIT より遅く行う（この環境の実測: llvmpipe で @tps 1,111,116 / CPU JIT 2,500,065、"The fastest tri fill engine" 120 フレーム）。`XYO_GPU_DEBUG=1` で選択デバイス名と type を stderr に出す。
- **現行ビルドの実測（headless `xyo run`、"The fastest tri fill engine" 120 フレーム）**: text 修正前は GPU ループが受理されず @tps ≈ 2.0–2.5M（CPU JIT、**GPU プログラムが 1 つも生成されない**）。text 修正後は Vulkan のハードウェアデバイス（RTX 3070、type=2）を選び 10000 lanes を dispatch する: **@tps 9,999,866 / 498ms**（CPU JIT 2,499,966 / 3239ms の 4.0×、llvmpipe 1,111,116 / 1557ms の 9.0×）。実 GL の live テストも `DISPLAY=:0 SDL_VIDEODRIVER=x11 XYO_TEST_GPU=1 xyo-gpu-tests --parallel` で 377 checks 0 failures。`fill.draw` のループは単体 entry としては `outer-scope argument` で拒否されるが、flagship 経路（`test` のループ）では同一プログラム内へ inline されるため GPU 化に影響しない。**AOT でも GPU ループは動く**: `GpuProgram` slot をロード時に再コンパイルして束縛するため、`xyo aot` の単体実行ファイルも `--aot` 実行も同じ @tps **9,999,866**（非 AOT と同一）。
- **TurboWarp `_twconfig_`（カスタムステージサイズ等）**: sb3 のステージ/スプライト comment で ` // _twconfig_` で終わる行の直前に ExtendedJSON のプロジェクトオプションが入る（`runtime.js parseStoredProjectOptions`）。`sb3.cpp applyTwProjectOptions` がターゲット/モニタロード**前**に適用 — `width`/`height`→`runtime.stageWidth/stageHeight`（0/非数値は既定 480×360）、`framerate`→`currentStepTime`（>250→250、<0→1、0=vsync 相当で 60fps 扱い）、`turbo`/`interpolation`/`runtimeOptions`（maxClones/miscLimits/fencing）。`xyo play` は `--fps` 未指定時にプロジェクトの framerate をペーシングに使い、`--turbo` 未指定でも config の turbo を消さない。fence/bounds/io_devices/pen 層/GPU dispatch は全て `stageWidth/stageHeight` 経由 — 固定値を残さない。
- **ビットマップコスチュームの rotationCenter は生画像 px**: sb3 の `rotationCenterX/Y` は logical×bitmapResolution（load-costume.js で `*2` 保存）。`Skin::rcx/rcy` は論理単位で持つ約束（描画・pick・bounds が `rcx*scale` でテクセルへ戻す）なので、ビットマップ経路は `/bitmapResolution` で変換してから格納する。raw のまま掛けると `bitmapResolution>1` の背景がずれて半分しか覆わない（linux.sb3 の 1920×1440@res=2 背景が左上 320×240 に潰れていた件）。SVG の rotationCenter は元々論理 px — 変換不要。
- SDL2 がある場合は `xyo-render-tests` をビルドする。`ctest --test-dir build -R xyo-render-tests --output-on-failure` は dummy SDL ドライバで実行し、空/不正/正常コスチューム・切替・クローン共有・合成の byte 一致・点の境界と丸めを検証する。
- **FPS は描画ありで測る**: renderer 不在の `Runtime::step()` 回数は実際の描画フレーム数ではない。起動フェード中の早すぎるクリックも空画面だけを計測する原因になる。3D2 では開始待ちを解除し、最初のペン描画とウォームアップを終えてから計時する。同じ 30 active frames で penPoint=5,184,000、fence=5,205,630、penClear=60、draw=30 を照合する。性能比較は `XYO_PROF` を無効にし、描画解像度・入力・ペーシング・ドライバを揃える。画面比較では実時間に依存する FPS 表示を別扱いにする。
- 空コスチュームの失敗キャッシュ・旧ペン高速化時点の A/B 実測（後述の native AA / NaN 修正より前、3D2、scale=2、30 active frames、ロード/JIT 起動/ウォームアップ除外、CLI の 33ms sleep なし）: dummy/software の交互 3 回の中央値は 9109.584ms → 1206.111ms（3.293 → 24.873 FPS、7.55×）。実 Wayland/OpenGL ウィンドウの 1 回の比較では 8929.543ms → 1139.433ms（3.360 → 26.329 FPS、7.84×、vsync 維持）。描画量は全回同一、dummy の 3 組の PPM は FPS 数字表示（x=582,y=42,w=176,h=42）以外の全 683,808 pixel が一致。これらは XYO の変更前後の比較であり、TurboWarp との直接比較ではない。

## SVG コスチュームの文字

- NanoSVG は `<text>` / `<tspan>` を描画しない。コスチュームは librsvg でパースし、96DPI・元の論理寸法の viewport に `rs=2*max(1,bitmapResolution)` の Cairo scale を適用する。整数に切り上げた texture 寸法を viewport として使わない。
- Cairo ARGB32 は native endian の premultiplied 値。`Skin::px` は straight RGBA8 に戻してから SDL・effects・pick/touchingColor・penStamp に共通で渡す。色成分の alpha 二重掛けを防ぐ。
- SVG handle に base URI を設定しない（外部ファイル/ネットワークを解決しない）。XML の UNLIMITED flag は使わない。0/負/非有限の寸法、壊れた XML は失敗キャッシュにし、positive viewBox-only の寸法は viewBox から解決する。texture は各辺≤8192、≤64Mi pixels に制限する。
- 回帰テストは英字・日本語・tspan の改行/色・opacity・anchor・group transform・viewBox・stroke・失敗キャッシュ・文字の当たり判定/スタンプを検証する。OS のフォントに依存するため文字幅を固定 byte golden として扱わない。
- `3DOcean.sb3` は entryAlloca 修正後、実 Wayland/OpenGL で初期化を完了して海と波の描画を継続し、SVG の生成中メッセージと操作説明も表示できた。診断実行では初期化開始から最初の描画まで14.173s、以降6 step=2.183s。warp 500ms yield により部分描画の step が含まれるため、6 step を6完成フレームとしてFPS報告しない。起動不具合の修正であり、3DOcean の高速化完了ではない。

## 3D2 の計算差分検証

- 謎の線を追うときは描画済み画像だけでなく、実 TurboWarp VM の変数・法線・座標・色/明るさリストと比較する。`Res=1` の JIT 明るさバッファ差分 475 pixel、`Res=4` の 239 pixel は `emitToNumF64` の NaN→0 欠落で発生していた。スキャンラインの `x/abs(x)` が x=0 で NaN になり、次の乗算に無変換で伝播していた。
- 修正後は両 Res の計算リストが TW compiled と一致（数値比較 abs=1e-9、rel=1e-10、色文字列は厳密一致）。実時間の FPS/fps step と最適化で数値化された scalar の文字列型差は別扱い。中断中の interpreter 状態は同じ描画フェーズになるまで比較しない。
- native AA と NaN 修正後の Wayland/OpenGL 実測は 30 active frames=1330.518ms、22.548 FPS（1 回、vsync 維持、計測器の CLI sleep 無し）。修正前の 26.329 FPS とは画素モデル・NaN 演算の条件が異なるため、現在の値として混同しない。
- librsvg / entryAlloca 追加後の dummy/software 診断は30 active frames=1452.073ms、20.660 FPS（単発）。penPoint/fence/clear/draw の量は既存値と同じで、draw 合計159.988ms。前述の Wayland/OpenGL は別ドライバなので FPS の直接A/B比較には使わない。
- 移動フェンス修正後の dummy/software 診断は30 active frames=1497.751ms、20.030 FPS。描画量は同じ（penPoint=5184000、fence=5205630、clear=60、draw=30）。3DOcean の並行検証も行っていた単発値で、厳密な速度A/B比較には使わない。3DOcean のペン API 座標も x=[-240,239]、y=[-180,180]、pen buffer は480×360、最終部分描画の画素 bounds は x=[0,479] に到達していた。
- ペンは実シェーダを実行した GPU golden の単点テストで検証する。固定 172802-command trace の replay では premult RGBA の mean error は各 channel 0.85–1.02 byte、max 13–15 byte。CPU の幾何計算と GPU mediump の誤差を含むため、ペン framebuffer の完全な byte 一致はまだ主張しない。

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
  - GC オブジェクトを「ルート未接続のヒープコンテナ」に格納する区間では `Gc::DeferCollect` を必須とする（sb3 ロードはこれに該当 — `loadTargetObject` 中の Target は `runtime.targets` 未登録）。スタック残滓が守ってくれるのは非決定的で、大規模プロジェクトで UAF になる（3D2.sb3 実績）。
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
