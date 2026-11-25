# Docker / bench での Qt 依存について

## 解決済み（2026-11-23）

`xyo-linux-x86_64.tar.gz` に Qt/SDL2/rsvg 等の `.so` を `lib/` として同梱し、
rpath `$ORIGIN/lib` で参照するようにした（`release.yml` + `bundle_linux_libs.sh`）。
Qt の無い docker でも動的バイナリがそのまま起動する。
LGPL 対応として `lib/` 置換可能である旨を `THIRD-PARTY-NOTICES.txt` に明記。

## 背景

動的リンク版のリリースは Qt6 Quick にリンクしているが、bench の docker には
Qt が無く、`libQt6Quick.so.6: cannot open shared object file` (exit 127) で
`main()` 前にローダーが起動に失敗していた。`xyo run/test` は Qt を使わないものの、
`DT_NEEDED` でライブラリが要求されるためである。

## 代替案（残る選択肢）

- `xyo-linux-x86_64-static.tar.gz`（完全静的、依存ゼロ）。headless 専用なら
  こちらでも可。
- `cmake -DXYO_QT=OFF` で Qt なしビルド（`xyo gui` はスタブ）。
