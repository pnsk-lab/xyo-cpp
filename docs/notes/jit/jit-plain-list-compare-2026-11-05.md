# リスト比較の型ガード付きLLVM高速経路 — `algo_sort` 改善（2026-11-05）

## 原因と対象

ScratchBench `algo_sort` は、100個の疑似乱数をバブルソートする。内側のループで `data_itemoflist` を2回評価して `operator_gt` で比較し、必要なら要素を2回書き換える。

従来JITの比較は、値の数値・文字列解釈を個別に確認する複数の分岐を通る。今回、**2つの入力レポータが直接 `data_itemoflist` である場合のみ**、値のNaN-boxedビットを検査して通常の浮動小数点値であればLLVMの`fcmp`で比較する。

- 値の絶対値部分のビット列がIEEE-754 `+Infinity` 以下の場合に限る（負数、±0、±Infinityを含む）。
- NaN（負のペイロードも含む）、文字列、真偽値、色、undefined、範囲外のリスト要素には、従来のScratch比較ヘルパを使用する。
- 型推測やリスト単位の`numericOnly`フラグに依存しない。実行中に別スクリプトがリスト要素を書き換えても、比較のたびに型を確かめる。
- 手続きやソートそのものを認識して特別な実装に置き換えたわけではない。一般の「リスト要素どうしの比較」で効果がある。

ファイル: `src/jit/codegen.cpp` の `emitCmp`。既定ON。切替は`XYO_JIT_PLAIN_NUMBER_CMP=0`でOFF、`=1`で直接リスト要素間以外の動的比較へも拡張（診断用）。

## ベンチマーク

独立したScratchBenchの`algo_sort.sb3`を使用。**同一固定バイナリ**の環境変数だけを変更し、CPU 12に固定し、OFF/ONをランダム順に16ペア実行した。ScratchBenchが出力した`ops_per_ms`比のペア中央値。起動／JITコンパイル時間を含まないベンチマーク内部の数値。チェックサムはON/OFF一致。

| ケース | ON/OFF ops/ms（中央値） | 備考 |
|---|---:|---|
| `algo_sort` | **1.0671倍** | 16ペアすべて改善、checksum `150504` |
| `math_compare` | 1.0065倍 | ほぼ同等 |
| `math_strcompare` | 1.0021倍 | ほぼ同等 |
| `list_read` | 0.9867倍 | 少量の計測変動、比較経路を使用しない |
| `algo_matmul` | 0.9934倍 | ほぼ同等 |

元データ: `build/algo-sort-eval/paired.json`。再現:

```sh
python3 tests/bench/algos/bench_plain_numeric_cmp.py \
  --binary build/xyo --cases algo_sort math_compare math_strcompare list_read algo_matmul \
  --trials 16 --out build/algo-sort-eval/paired.json
```

**注意:** 性能はJITのソート内側ループに特化して観測された。一般的なScratch作品全体の改善率ではない。並行ビルドが計測に混ざる事例があったため、報告値は固定コピーしたバイナリを使う最終計測に限定した。

## 採用しなかった代替案

- リスト`numericOnly`のガードを比較に使用: `algo_sort`中央値0.9963倍（6ペア）。既存の実験機能はOFFのまま。
- 数値書き込み時の`numericOnly`更新を条件分岐で省略: `list_replace`は1.051倍だが`algo_sort`は0.9719倍（各8ペア）。コード生成の追加試作は削除。
- 絶対値ビット列2つをORした単一比較ガード: `algo_sort`は中央値0.9960倍（20ペア）。元の二重ガードに戻した。

## 正当性

- `tests/differential/plain_numeric_cmp.py`: 数値、空文字、空白、数値文字列、`Infinity`、英語、日本語、真偽値を含むリストを構築。直接 `item(i)>item(j)` / `item(i)<item(j)` を評価し、変数経由の比較結果と照合。途中でリストを異種型に書き換える。インタプリタ／Eager JIT／HotSpotで全比較出力一致。
- `tests/differential/plain_numeric_sort_algos.py`: 独立生成したバブルソート100要素、クイックソート500要素、マージソート500要素を、同じ3経路かつ高速経路OFF/ONの全18組で実行。全要素一致、昇順判定OK、チェックサム一致。
- CTest: `xyo-jit-plain-list-compare`、`xyo-jit-plain-list-sort` に登録。実行は `ctest --test-dir build -R 'xyo-jit-plain-list' --output-on-failure`。
- AOT専用のフル互換性回帰、非canonical NaNをビット単位で注入するテストは今後の課題。

元のほかの変更は保持し、本件のみのcommit/pushは行っていない。
