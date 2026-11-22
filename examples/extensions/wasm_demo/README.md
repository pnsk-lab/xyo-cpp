# WASM Demo（ネイティブ wasm3 プロバイダ）

`ext.wasm` を wasm3 でロードし、3 つの reporter を実行するサンプルです。

| opcode | 動作 |
|---|---|
| `wasmdemo_double` | 引数を 2 倍した数値を返す |
| `wasmdemo_add` | 2 引数の和を返す |
| `wasmdemo_hello` | `"hello from wasm"` を返す |
| `wasmdemo_echo` | 文字列引数をそのまま返す |
| `wasmdemo_sumcount` | 引数 0 の数値 + `xyo_arg_count()` を返す |

## 試す

```sh
# 1) ビルド（clang + wasm-ld）
examples/extensions/wasm_demo/build.sh

# 2) xyo から見える場所に置く
XYO_EXTENSION_PATH=examples/extensions XYO_EXTENSION_DEBUG=1 \
  ./build/xyo run your_project.sb3
```

`XYO_EXTENSION_PATH` の直下に `wasm_demo/` があり、その中に
`manifest.json` と `ext.wasm` があれば読み込まれます。

## ABI（抜粋）

`ext.wasm` は次の export を持ちます。

- `xyo_ext_abi() -> i32` — 1 を返す
- `xyo_ext_alloc(n: i32) -> i32` — 拡張メモリのオフセット
- `xyo_ext_call(op: i32, args: i32, nargs: i32, ret: i32) -> i32`
  - `args` / `ret` は `{ i32 tag; f64 num; i32 str_ptr; i32 str_len; }` の配列
  - 戻り値 0 = 成功、非 0 = エラー

`wasmdemo_sumcount` は `xyo_arg_num` / `xyo_arg_count` を使うため、
`ext.wasm` の import セクションに `env.xyo_arg_num` と
`env.xyo_arg_count` が入ります。ホスト関数の一覧:

```
xyo_arg_count() -> i32
xyo_arg_tag(i32) -> i32
xyo_arg_num(i32) -> f64
xyo_arg_str_copy(i32 index, i32 dst, i32 cap) -> i32
xyo_log(i32 ptr, i32 len)
```

`docs/extensions.md` §5.2 に全体の設計があります。

## 制約

- wasm3 はネイティブビルド専用です（Emscripten では `XYO_WASM_EXT=OFF`）。
- 1 呼び出しあたり 1 億 gas、メモリ 64 MiB の上限を掛けています。
- 拡張インスタンスはプロセスに 1 個で、呼び出しは mutex で直列化されます。
- 無限ループは gas で trap しますが、`xyo_ext_alloc` を呼ばない巨大な
  単一関数などはコンパイル時の gas 上限に依存します。
