---
title: AOT コンパイル
---

AOT は SB3 を事前コンパイルし、単体実行ファイルとして書き出します。

```sh
./xyo aot project.sb3 -o app
./app run --frames 300
```

CPU の移植性を優先した設定が既定です。`XYO_AOT_TARGET_CPU=native` はビルド元 CPU への最適化に使えますが、異なる CPU での動作は保証されません。詳細は[実行ガイド](/xyo-cpp/reference/running/)を参照してください。
