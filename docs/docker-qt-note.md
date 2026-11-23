# Docker / bench での Qt 依存について（調査メモ）

## 現象

scratch-bench (docker) で xyo-cpp の全 variant が exit 127 で失敗：

```
xyo: error while loading shared libraries: libQt6Quick.so.6: cannot open shared object file
```

## 原因

`xyo-linux-x86_64.tar.gz`（動的リンク release）は Qt6 Quick に動的リンク
している（LGPL のため。release.yml で明示）。Qt の無い環境では
`main()` 前に loader が失敗する。`xyo run/test` は Qt を使わないが、
`DT_NEEDED` で要求されるため起動できない。xyo-cpp のコード不良ではない。

## 対策

### bench/docker 側（推奨）

`xyo-linux-x86_64-static.tar.gz` を使う。`XYO_STATIC=ON` でビルドされ
`readelf -d` の `NEEDED` が 0（完全静的）。headless の run/test/aot に
必要十分。`src/runtimes/xyo.ts` の `archiveName` を static に変えるか、
docker image に Qt6 を入れる（重いので static 推奨）。

### xyo-cpp 側

- `cmake -DXYO_QT=OFF` で Qt なしビルド可（`xyo gui` はスタブ）。
- release は dynamic（desktop 向け、Qt あり）と static（container 向け、
  Qt なし）の両方を出している。使い分けを明記する。
