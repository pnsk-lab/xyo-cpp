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
- JIT の実行モードは `--jit=hotspot|eager|off`（環境変数 `XYO_JIT_MODE`）。
  `hotspot`（既定）はインタプリタで開始し、ホットになったスクリプトだけ
  JIT コンパイルします。`eager` は全スクリプトを即コンパイル（従来動作）、
  `off` はインタプリタのみ。AOT は `xyo aot` / `--aot <obj>` を明示した
  ときだけ使われます。
- `--danger`（環境変数 `XYO_DANGER=1`）は **スレッドごとの実行時間予算
  （warp 500ms）を外す危険モード**です。既定では warp スクリプトは 500ms
  走ると他のスレッド/描画へ譲りますが、`--danger` では 1 ティックで終わらない
  warp ループが譲らずに走り切ります（長い計算を 1 フレームで回す計測用）。
  代償としてアプリが固まり、warp 内の `wait until` / `broadcast and wait` は
  ハングし得ます。`run`/`play`/`test`/`edit` と AOT 実行ファイル
  （`./app run --danger`）で使えます。

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
