# v1.3.5 — macOS を Qt 同梱の xyo.app で配布

v1.3.4 までは macOS の成果物を `-DXYO_QT=OFF` でビルドした素の CLI にして
いたため、`xyo gui`（Qt Quick ランチャー）と `xyo edit` はスタブだった。
本リリースで **Qt を同梱した自己完結の `xyo.app`** を配布する。

## macOS

- `brew install qt` を追加し、`-DXYO_QT=ON` でビルド。Qt は **LGPL の
  動的リンクのまま** `xyo.app/Contents/Frameworks` へ同梱する
  （静的リンクにはしない）。`libLLVM.dylib` は巨大なので同梱せず、
  `-DXYO_STATIC_LLVM=ON` で静的アーカイブとしてリンクする。
- `tools/ci/bundle_macos_app.sh`（新規）:
  - `macdeployqt` で framework・QML モジュール・`qt.conf`・`@rpath` を整える
  - 残りの Homebrew dylib（SDL2 等）を `Frameworks/` へ集めて `@rpath` 参照に
    書き換える
  - 全依存が `@rpath` / システムの範囲に収まったこと、`qml/QtQuick/Layouts`
    と `qml/QtQuick/Controls` が存在することを検証
  - `Info.plist.in`（新規）から最小の `Info.plist` を生成し、ad-hoc 署名
- 成果物 `xyo-macos-{x86_64,arm64}.tar.gz` には `xyo.app` を含める。CLI は
  `xyo.app/Contents/MacOS/xyo` で実行する（同梱 dylib を `@rpath` で解決）。
- CI で `smoke.sh gui`（SDL dummy + Qt offscreen）と、ビルド直後バイナリが
  Qt framework に動的リンクし `libLLVM` を同梱していないことを検証する。

## Windows

- `windeployqt` 後に `qml/QtQuick/{Layouts,Controls}` が実際にデプロイされた
  ことを検証する。`qmlimportscanner` 不在などでスキャンがスキップされると
  `Qt6*.dll` だけがコピーされ、起動時に型が見つからず失敗する事故を防ぐ。

## Qt ランチャー / エディタ

- `xyo gui` / `xyo edit` は実行ファイル同階層の `qml/` を QML import path へ
  明示追加する。`qt.conf` が無い配布物でも `QtQuick.Controls`/`Layouts` を
  見つけられる。

## 検証

- macOS: Release ワークフローの macOS ジョブ（x86_64 / arm64）で、Qt への
  動的リンク・libLLVM 非同梱・バンドル後の依存閉包・QML モジュールの存在・
  GUI スモークを検証。成果物は `xyo.app` を含む tarball。
- Linux / Windows: 従来どおり（Windows は QML デプロイ検証を追加）。
- コード側の変更は import path の追加のみで、実行時意味論の変更はない。
