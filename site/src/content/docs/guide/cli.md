---
title: CLI リファレンス
---
`xyo` の主要コマンドを示します。細かなオプションは使用中のビルドの `xyo --help` と [実行手順](/xyo-cpp/reference/running/)で確認してください。

| コマンド | 用途 |
| --- | --- |
| `xyo run project.sb3 --frames 300` | ヘッドレス実行 |
| `xyo play project.sb3` | ウィンドウで実行（SDL2 対応版） |
| `xyo test fixtures/` | フィクスチャを検証 |
| `xyo aot project.sb3 -o app` | AOT 単体実行ファイルを生成 |
| `xyo minify project.sb3` | SB3 のサイズを削減 |
| `xyo analyze project.sb3` | 実行せず構造を解析 |
| `xyo rewrite project.sb3 -o optimized.sb3` | 安全条件を満たす構造の書き換え |

## 実行の制御

```sh
xyo run project.sb3 --jit=hotspot --frames 300
xyo run project.sb3 --jit=off --print-list output
xyo run project.sb3 --fps 30
```

`--print-list` は特定のリストへの追記を stdout に出し、計測基盤との連携に使えます。

`--danger` は warp スレッドの時間予算を外す検証用の危険な設定です。無限ループにより応答しなくなる可能性があるため、通常の実行には使用しないでください。
