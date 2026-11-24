# v1.3.6 — macOS バンドルの 2 つの同梱漏れを修正

v1.3.5 の macOS 版 `xyo.app` は、そのままでは GUI が起動しなかった。
原因は 2 つとも同梱（パッケージング）側の問題で、コード侧の不具合ではない。

## 修正

1. **SDL3 が未同梱だった**
   Homebrew の `sdl2` は **sdl2-compat**（SDL2 API を SDL3 へ橋渡しする実装）で、
   実行時に `@loader_path/libSDL3.dylib` / `@executable_path/libSDL3.dylib` を
   `dlopen` する。v1.3.5 は `otool -L` に見える依存だけを同梱していたため
   SDL3 が入らず、`Failed loading SDL3 library` で即終了していた。
   → 同梱した SDL2 の文字列に `libSDL3.dylib` が含まれる場合（= sdl2-compat）、
   `brew --prefix sdl3` から SDL3 を `Frameworks/libSDL3.dylib` と
   `MacOS/libSDL3.dylib`（symlink）の両方へ入れ、`@rpath` の install name と
   `@loader_path` rpath を設定する。SDL3 自身の依存も続けて取り込む。

2. **`qt.conf` が効いていなかった**
   v1.3.5 は `Contents/Resources/qt.conf` にだけ書いていたが、Qt が読むのは
   **実行ファイルと同じディレクトリ**（`Contents/MacOS/qt.conf`）で、実際には
   `Contents/MacOS/platforms` と `Contents/MacOS/qml` を探しに行き、実体の
   `Contents/PlugIns` / `Contents/Resources/qml` を見つけられず cocoa プラグイン
   の初期化に失敗していた。
   → `Contents/MacOS/qt.conf` を追加（`PlugIns = ../PlugIns`,
   `Imports = ../Resources/qml`）。加えて `qt.conf` が効かない起動経路
   （バンドル外から素のバイナリとして実行する場合や Qt のバージョン差）でも
   動くよう、`Contents/MacOS/platforms` → `../PlugIns/platforms` と
   `Contents/MacOS/qml` → `../Resources/qml` の相対シンボリックリンクを張る。

## 検証

- バンドラー（`tools/ci/bundle_macos_app.sh`）に検証を追加:
  `MacOS/qt.conf` の存在、`MacOS/platforms/libqcocoa.dylib` と
  `MacOS/qml/QtQuick/Controls/qmldir` が解決できること、sdl2-compat の場合は
  `Frameworks/libSDL3.dylib` と `MacOS/libSDL3.dylib` の両方が存在すること。
- リリースワークフローに **パッケージ済み `.app` の GUI スモーク**を追加:
  `QT_DEBUG_PLUGINS=1` で起動し、ログに `xyo.app/Contents/...platforms` と
  `xyo.app/Contents/...qml` が現れること（= バンドル内を探索していること）と、
  `Failed loading SDL3` が出ないことを検証する。ビルド直後のバイナリだけを
  スモークしていた従来の検査では、この 2 件は検出できなかった。

## 影響範囲

- macOS のみ。Linux / Windows の成果物と、実行時意味論の変更はない。
- v1.3.5 の macOS 版を使っている場合、`xyo.app` をこの版で置き換えること。
