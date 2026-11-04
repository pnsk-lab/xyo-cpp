---
title: プロジェクトの最適化
---
## まず解析する

```sh
./xyo analyze project.sb3 --top 20
./xyo analyze project.sb3 --dot dependencies.dot
```

解析は実行を伴わず、構造的なボトルネックと並列化の制約を表示します。

## サイズの削減

```sh
./xyo minify project.sb3
./xyo minify project.sb3 --aggressive --strip-comments
```

既定の minify は可逆な削減を目指します。`--aggressive` は未使用と判断したデータも削除するため、変換結果の動作確認を推奨します。

## 意味論を保存する書き換え

```sh
./xyo rewrite project.sb3 -o project.opt.sb3 --report
```

このコマンドは条件を満たした手続きの warp 化や一時変数のインライン化を試みます。すべてのプロジェクトで高速化されるわけではありません。実際の出力を比較し、[ベンチマーク](/xyo-cpp/development/testing/)で評価してください。
