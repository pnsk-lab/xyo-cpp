# 実行時依存と使い方

## Linux x86-64

Linux 版は 2 種類あります。どちらも **LLVM は静的リンク**なので
`libLLVM` のインストールは不要です。

### 1. `xyo-linux-x86_64-static.tar.gz` — 完全静的（推奨）

同梱: `xyo`（LLVM JIT + ランチャー以外の CLI。**動的依存ゼロ**）

```sh
./xyo run <project.sb3> --frames N   # ヘッドレス実行（HotSpot JIT）
./xyo test <fixture.sb3|dir>         # フィクスチャ検証（既定は eager JIT）
./xyo aot <project.sb3> -o <app>     # 単体実行ファイルを書き出し（明示的な AOT）
./xyo minify <project.sb3>           # sb3 を小さくする（→ project.min.sb3）
```

- `xyo minify [-o out.sb3]` は既定で可逆な削減だけを行います: zip の
  deflate 圧縮（`.json/.svg/.wav/.ttf/.otf`）、ブロック/コメント ID の短縮、
  未参照アセットと到達不能ブロックの削除。`--aggressive` で未使用の
  変数/リスト/ブロードキャストも削除、`--strip-comments` でコメントを削除、
  `--level N`（0〜9、既定 9）で圧縮レベルを指定できます。
- AOT の既定ターゲットは移植性を優先したCPU基本命令セットです。別のマシンで実行する場合も、コンパイル元の拡張命令に依存しません。特定CPU向けに調整する場合だけ `XYO_AOT_TARGET_CPU=native` を指定してください。この場合、異なるCPUで動作する保証はありません。
- JIT の実行モードは `--jit=hotspot|eager|off`（環境変数 `XYO_JIT_MODE`）。
  `hotspot`（既定）はインタプリタで開始し、ホットになったスクリプトだけ
  JIT コンパイルします。`eager` は全スクリプトを即コンパイル（従来動作）、
  `off` はインタプリタのみ。AOT は `xyo aot` / `--aot <obj>` を明示した
  ときだけ使われます。

- `--danger`（環境変数 `XYO_DANGER=1`）は **スレッドごとの実行時間予算
  （warp 500ms）を外す危険モード**です。既定では warp スクリプトは 500ms
  走ると他のスレッド/描画へ譲りますが、`--danger` では 1 ティックで終わらない
  warp ループが譲らずに走り切ります（長い計算を 1 フレームで回す計測用）。
  代償としてアプリが固まり得ます（止まらないのは純粋な計算ループだけ —
  `wait` / `wait until` / `broadcast and wait` などの待機は danger でも他
  スレッドへ譲るので停止します）。`run`/`play`/`test`/`edit` と AOT 実行
  ファイル（`./app run --danger`）で使えます。
- `XYO_LIST_STORAGE=flat|sbo4|sbo8|flat15|deque|gap|chunked|adaptive` で SList の格納方式を選択できます。
  既定は `flat`（従来の malloc/realloc、容量 2 倍）。
  `sbo4` / `sbo8` は 4 / 8 要素まで SList の GC オブジェクト末尾に
  インライン格納し、拡張時に従来の連続配列へ移行します。
  `flat15` は連続配列の成長率を 1.5 倍にします。
  実験的な非連続方式 `deque`（std::deque）、`gap`（gap buffer）、
  `chunked`（128要素ブロック + 累積要素数による rank 検索）も選べます。
  これらの方式は JIT の `items` 直接参照を無効にし、既存の helper
  を通すため、連続配列の JIT fast path に比べて遅くなる場合があります。
  連続配列の4方式は従来どおり JIT/AOT の直接アクセスと互換です。
  `adaptive` は平常時に連続配列の JIT fast path を使い、長いリストへの
  途中挿入・削除が続けば `gap` または `chunked` に移ります。読取りが
  優勢になった場合や `delete all` で連続配列へ戻ります。切替え時は
  元の要素順・文字列の GC 参照・数値特化フラグを保持します。
  JIT のループ内ヘッダキャッシュは、yield 後と非連続格納からの復帰時に
  格納領域の世代番号を使って更新します。ループ内の通常の連続配列参照は、
  再配置を行う操作が存在しないことを確認したうえでキャッシュを利用します。
  環境変数はプロセス起動前に設定してください。

- `--list-limit N`（環境変数 `XYO_LIST_LIMIT=N`）は **リスト長の上限**です。
  既定は 0 = 無制限（TurboWarp と同じ）。Scratch の 200000 を再現するなら
  `--list-limit 200000`。上限に達すると `add` は追加を落とし、`insert` は
  末尾を落として長さを保ちます（Scratch/Mistwarp 準拠）。JIT のインライン
  追加経路・AOT 実行ファイルでも同じ設定が効きます。

- `ldd`/`readelf -d` で NEEDED が 0（glibc も静的）。どの x86-64 Linux でも動きます。
- `play` / `gui`（ウィンドウ表示）は **含みません** — SDL2 を使うためです。
- Vulkan compute は `libvulkan.so.1` があれば実行時に読み込んで使います（無くても動作）。

### 2. `xyo-linux-x86_64.tar.gz` — ウィンドウ表示つき

同梱: `xyo`（JIT + SDL2 レンダラ + ランチャー GUI + Vulkan compute）

動的リンクの依存（**LLVM は静的リンク済み**なので `libLLVM` は不要）:

| ライブラリ | パッケージ例（Ubuntu 24.04） |
|---|---|
| `libSDL2-2.0.so.0` | `libsdl2-2.0-0` |
| `librsvg-2.so.2` / `libcairo.so.2` / `libglib-2.0.so.0` / `libgobject-2.0.so.0` | `librsvg2-2` |
| `libGL.so.1`（GPU ペン層） | `libgl1` |
| `libz.so.1` / `libzstd.so.1` / `libtinfo.so.6`（LLVM 静的ライブラリが参照） | `zlib1g` / `libzstd1` / `libtinfo6` |

`ldd` で実際に必要なのは上記のみです（`libxml2` は不要）。

```sh
./xyo play <project.sb3>             # ウィンドウで再生
./xyo                                # （引数なし）ランチャー GUI
```

## macOS (Apple Silicon arm64 / Intel x86-64)

Homebrew を用いた LLVM ORC JIT + SDL2 のネイティブビルドに対応。
ビルド手順、依存ライブラリ、GPU バックエンドの制約は
[macOS ビルドガイド](docs/macos.md) を参照してください。

## Windows x86-64（xyo-windows-x86_64.zip）

同梱: `xyo.exe`（**JIT + SDL2 レンダラ + ランチャー GUI**、LLVM・libstdc++・SDL2 は
静的リンクなので **DLL の同梱は不要**です）

```sh
xyo.exe play <project.sb3>             # ウィンドウで再生
xyo.exe                                # （引数なし）ランチャー GUI
xyo.exe run <project.sb3> --frames N   # ヘッドレス実行（LLVM JIT）
xyo.exe test <fixture.sb3|dir>         # フィクスチャ検証
xyo.exe aot <project.sb3> -o <app.exe> # 単体実行ファイルを書き出し
xyo.exe minify <project.sb3>           # sb3 を小さくする
```

- **LLVM JIT が動きます**（MSYS2 の mingw-w64 LLVM でビルド。LLVM は静的リンクなので
  `LLVM-*.dll` は不要）。ホットなスクリプトはネイティブコードにコンパイルされます。
- **AOT 書き出しも Windows で動作**します（COFF オブジェクトを exe 末尾へ追記）。
- **ウィンドウ表示（`play` / `gui`）も動きます**。SDL2 は静的リンク（`SDL2.dll` 不要）で、
  モニタ・バブルの文字用フォント（Liberation Sans）も exe に埋め込まれています。
- **SVG コスチュームは nanosvg でラスタ化**します（Windows には librsvg が無いため）。
  `<text>` / `<tspan>` の文字だけは描画されません（ビットマップコスチュームは完全対応）。
  GPU ペン層（OpenGL FBO/シェーダ）も Windows では無効で、CPU ラスタで描画します。
- Vulkan（`vulkan-1.dll`）があれば GPU 並列ループが有効になります。
- UTF-8 のパス・コンソール出力に対応（日本語ファイル名の sb3 も可）。

## HotSpot JITと遅延手続きコンパイル

ネイティブ版では **HotSpotと遅延JITが統合されている**。既定の
`--jit=hotspot --jit-lazy=auto` では、まずHotSpotがスクリプト単位の
コンパイル対象を選ぶ。その後、LLVMの関数数と、スクリプト本体にある
直接の手続き呼び出し数を見て、手続きごとの遅延最適化・機械語生成を
自動選択する。

- `--jit-lazy=auto`（既定）: HotSpotでのみ自動選択。32個以上のLLVM
  関数を含み、本体の直接呼び出し箇所が関数数の1/4以下なら遅延を選ぶ。
  それ以外、またEager JITでは通常の一括O3コンパイルになる。
- `--jit-lazy=on`: HotSpot/Eagerのいずれでも、複数関数を含むスクリプトの
  手続きを初回呼び出し時にLLVM O3最適化・機械語生成する。
- `--jit-lazy=off`: 従来の一括O3コンパイルを使用する。

```sh
./xyo run project.sb3 --jit=hotspot                   # 自動選択
./xyo run project.sb3 --jit=hotspot --jit-lazy=on     # 遅延を明示
./xyo run project.sb3 --jit=hotspot --jit-lazy=off    # 従来の一括コンパイル
XYO_HOT_STATS=1 ./xyo run project.sb3                 # スクリプトごとのjit/lazy統計
XYO_JIT_TIMING=1 ./xyo run project.sb3                # compile段階とtier=eager/lazy
```

環境変数は `XYO_JIT_LAZY_MODE=auto|on|off`。旧設定
`XYO_JIT_LAZY_PROCEDURES=0|1`も互換入力として使えるが、
新しい設定またはCLIが優先される。

**遅延JITは初回起動時間を短縮するための機能**であり、コンパイル作業を
後から発生させる。ほとんどの手続きを実行するプロジェクトでは、むしろ
全体の終了が遅くなる場合がある。`auto`は静的な推定であり、全ての条件分岐の
実際の実行結果を予測するものではない。実行時間重視なら`off`で比較できる。
AOTは別のコンパイル方式のため、この設定の対象外。

## WASM / WebAssembly（Emscripten）

`xyo` を WebAssembly へクロスコンパイルできます（**interp のみ**。ネイティブ
LLVM JIT はブラウザでは動きません）。詳細は [docs/wasm.md](docs/wasm.md)。

事前準備: emsdk を入れて `source $HOME/.emsdk/emsdk_env.sh`。

### 1. Node の headless CLI（`run` / `dump` / `test`）

```sh
emcmake cmake -B build-wasm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/emscripten.cmake \
  -DCMAKE_BUILD_TYPE=Release -DXYO_SDL2=OFF -DXYO_BUILD_TESTS=OFF
cmake --build build-wasm                    # → build-wasm/xyo.js + xyo.wasm

node build-wasm/xyo.js run  <project.sb3> --frames N
node build-wasm/xyo.js dump <project.sb3>
node build-wasm/xyo.js test <fixture.sb3|dir>
```

### 2. ブラウザで `play`（SDL2 canvas）

```sh
emcmake cmake -B build-wasm-web -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/emscripten.cmake \
  -DCMAKE_BUILD_TYPE=Release -DXYO_SDL2=ON \
  -DXYO_WASM_ASYNCIFY=ON -DXYO_WASM_BROWSER=ON -DXYO_BUILD_TESTS=OFF
cmake --build build-wasm-web                # → xyo.html + xyo.js + xyo.wasm

python3 -m http.server -d build-wasm-web 8000
# → http://localhost:8000/xyo.html に sb3 をドロップ
```

- `XYO_WASM_ASYNCIFY=ON` は `play` ループの sleep/poll をブラウザへ譲るため
  （Node 単体でも有効化して動作検証できる）。
- `XYO_WASM_BROWSER=ON` で HTML シェル（`web/shell.html`）を出力。
- JIT 無効 / GPU compute・GPU ペン層は CPU フォールバック / SVG コスチュームは
  nanosvg でラスタ化（図形のみ。<text> は描かれない）。

### String storage A/B selection

Scratch string joins can be compared without rebuilding the runtime:

```sh
XYO_STRING_STORAGE=rope       ./build/xyo run project.sb3
XYO_STRING_STORAGE=flat       ./build/xyo run project.sb3
XYO_STRING_STORAGE=append-log ./build/xyo run project.sb3

./build/xyo run project.sb3 --string-storage=flat
./build/xyo test project.sb3 --string-storage append-log
```

- `rope` (default): flat for short joins (<16 UTF-16 code units), otherwise
  immutable cons nodes with lazy flattening.
- `flat`: eagerly copies every join into a contiguous UTF-16 allocation.
- `append-log`: flat for short joins; long joins with a suffix of at most 256
  UTF-16 units use immutable snapshots of chunked append logs. Larger suffixes
  fall back to rope. A fork from an earlier snapshot preserves immutability.

The setting is process-wide. Existing strings keep their representation when
the setting changes. All representations use the same `SStr`/JIT ABI, and the
setting affects both interpreter and JIT join helpers. Flat mode also disables
the JIT's specialized transient append-log loop optimization to keep actual
flat storage for A/B tests. Rope mode retains the existing specialized JIT
transient optimization where its escape proof applies; to compare strictly
Rope-only joins, also set `XYO_JIT_TRANSIENT_STRINGS=0`.
`XYO_STRING_APPEND_LOG=1` is retained as a legacy alias when
`XYO_STRING_STORAGE` is unset.


### Experimental 8-bit string leaves (Latin-1)

`XYO_STRING_LEAF=latin1` (or `--string-leaf latin1`) packs individual
non-composite leaves with all UTF-16 code units <= U+00FF into **one byte
per code unit**. Any higher code unit keeps the existing UTF-16 leaf.

```sh
./build/xyo run project.sb3 --string-storage rope --string-leaf latin1
XYO_STRING_LEAF=utf16 ./build/xyo run project.sb3
```

This switch is independent from `XYO_STRING_STORAGE`. The default is
`utf16`. All lengths and indices remain UTF-16 code units, and the unchanged
`SStr` ABI exposes a lazily widened UTF-16 buffer through `data()/view()`
for legacy consumers. `unitAt()` and Rope flattening read compact bytes
without materializing each leaf. A materialized UTF-16 buffer is freed with the
GC object. JIT paths that directly load inline UTF-16 now fall back to helpers
for compact leaves. Compact storage can reduce GC bytes but may increase
latency if code frequently requests `view()`; compare workloads before
changing the default.


### Experimental short-string interning

XYO_STRING_SHORT_INTERN=1 (or --string-short-intern=on) enables a bounded
strongly rooted cache for 2..4-unit ASCII strings created during runtime.
The default is off: repeated tiny joins save GC allocations, whereas
high-cardinality strings pay hashing overhead without reuse. The option
is independent of string storage and leaf encoding. Results and caveats:
docs/scratch-string-short-intern-study-2026-11-07.md.
