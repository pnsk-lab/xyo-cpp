# Clock（サンプル拡張）

`manifest.json` だけでブロックを定義するサンプルです。Phase 1 の現時点では
**ブロックの見た目とエディタへの配置だけ**が動き、実行時の動作は no-op の
スタブです（WASM / JS / native プロバイダは今後のフェーズ）。

## 試す

```sh
XYO_EXTENSION_PATH=examples/extensions ./build/xyo run tests/fixtures/basic.sb3
```

ツールボックスに **Clock** カテゴリが現れ、`now` / `wait %1 seconds` /
`mode %1` / `when ready` をワークスペースへドラッグできます。保存すると
`extensions` に `clock` が入り、次回も同じ拡張を要求します。

## マニフェスト

- `id` は opcode の prefix（`clock_now` の `clock`）と一致させる。
- `blocks[].shape` は `stack` / `reporter` / `boolean` / `hat` / `end`。
- `args[].type` は `number` / `string` / `boolean` / `angle` / `dropdown`。
- `dropdown` は `options` が必須。
- `hat` は `edge` / `restart`、C ブロックは `conditional` / `loop` を指定する。
