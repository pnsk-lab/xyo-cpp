# String cast / scan 高速化（2026-11-05）

## 実装

今回は Unicode の `contains` に加えて、`src/core/cast.cpp` の一般的な文字列変換と文字列走査を改善した。

- `cast::toString`: 空文字列・真偽値・`undefined` に対して、都度生成ではなく共有の `SStr` を返す。
- `cast::toStringValue`: `true` / `false` / `undefined` の文字列値も同じ共有 `SStr` を参照する。コンパクトな `Value::emptyStr()` タグは変更しない。
- 共有文字列は4個のみ。GCルートプロバイダを**確保前に**登録し、GC後も生存する。
- `strIsNotActuallyZero`、`cast::isInt`、`cast::isWhiteSpace`、`cast::toBoolean` は、走査中の `s->data()` 呼び出しをループ外に移動した。とくに連結 `SStr` では毎文字の複合文字列判定と間接アクセスを省ける。
- 数値の `toString`、`NaN`・`Infinity`、数値比較、Unicodeの大小文字変換規則は変更しない。

## 再現

~~~sh
cmake --build build --target xyo xyo-tests -j4
ctest --test-dir build --output-on-failure -j4

c++ -O3 -std=c++20 -Isrc tests/bench/string_common_cast_bench.cpp \
  build/libxyo.a -lz -lLLVM-23 -ldl -lSDL2 \
  -o build/string_common_cast_bench
build/string_common_cast_bench

c++ -O3 -std=c++20 -Isrc tests/bench/string_cast_scan_bench.cpp \
  build/libxyo.a -lz -lLLVM-23 -ldl -lSDL2 \
  -o build/string_cast_scan_bench
build/string_cast_scan_bench
~~~

## 定数文字列への変換

一つのバイナリをそれぞれの実装でビルドして実行。GC回収を試験中は遅延し、100,000回のループで測定。

| 変換 | 最適化前 | 最適化後 | 生成GCオブジェクト数（前→後） |
|---|---:|---:|---:|
| `Value::emptyStr()` | 30.5 ns/call | 2.2 ns/call | 100,000 → 4（共有文字列の初期化） |
| `Value::boolean(true)` | 33.3 ns/call | 3.6 ns/call | 100,000 → 0 |
| `Value::boolean(false)` | 34.4 ns/call | 3.4 ns/call | 100,000 → 0 |
| `Value::undefined()` | 36.1 ns/call | 3.5 ns/call | 100,000 → 0 |

この測定はGCが回収動作を実行するコストを含まない。GCプレッシャー減少により実作品では副次的な効果があり得るが、定量的な全体効果は別途調べる。

## 文字走査

4096 UTF-16コードユニット（または2048+2048の連結文字列）を5000回走査するマイクロベンチマーク。単発の概数であり、CPUのばらつきはある。

| ケース | 変更前 | 変更後 | 速度比 |
|---|---:|---:|---:|
| 数値文字列 `isInt` | 3049.75ns | 940.66ns | 3.2倍 |
| 連結数値文字列 `isInt` | 4502.82ns | 926.36ns | 4.9倍 |
| 空白文字列 `isWhiteSpace` | 5038.41ns | 4185.02ns | 1.2倍 |
| 日本語文字列 `isNotActuallyZero` | 2279.93ns | 1423.60ns | 1.6倍 |

## 正当性

`tests/unit/test_cast.cpp` に、4つの共有文字列の内容・ポインタ同一性・GC後生存と、連結文字列の `isInt`、`isWhiteSpace`、`isNotActuallyZero`、`toBoolean` の回帰検証を追加。

検証: 全CTest 16/16成功（連結文字列の追加テスト前）、その後の追加テストを含む xyo-tests も成功。

これらは新たな意味論や型特化を加えるものではなく、キャッシュと不要なアクセスの削減。実作品全体での高速化倍率は未計測。

コミット・pushは未実施。他の進行中の実験は保持した。
