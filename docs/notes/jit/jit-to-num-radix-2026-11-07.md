# `jsnum::toNumber` の基数付き整数の短い入力高速化（2026-11-07）

## 対象

Scratchの数値変換（`cast::toNumber`・LLVM JITの`xyo_to_num`）で使用される共通の`jsnum::toNumber`を高速化。ECMAScriptの`0x`・`0o`・`0b`接頭辞を持つUTF-16文字列が対象。

旧実装は全ての桁を`double v = v * base + digit`で処理していた。桁の多い入力では浮動小数点演算の依存が直列化する。

## 実装

`src/core/jsnum.cpp`で、**桁数だけで2^53以下と証明できる入力**を整数レジスタで解析し、最後に`double`に1回だけ変換する。

| 接頭辞 | 直接整数解析できる最大桁数 | ビット精度 |
|---|---:|---:|
| `0x` / `0X` | 13 | 最大52ビット |
| `0o` / `0O` | 17 | 最大51ビット |
| `0b` / `0B` | 53 | 最大53ビット |

数値が正確に`double`へ表現できることを入力長だけで判断するため、**ループ内に追加のオーバーフロー検査はない**。桁ごとの累積には`uint64_t`左シフトとORを用いる。

この桁数を超える場合には、以前と同一の`double v = v * base + digit`経路を実行する。これにより非常に大きい整数で既存の丸め動作を変更しない。全ての桁について既存と同じ文字検証と範囲検証を行い、無効な接頭辞・不正な数字を許可しない。先頭ゼロ一括除去案も試験したが8進・不正入力の処理を遅くしたため**撤回した**。

前の候補で試した「各桁の後で2^53上限を判定し、必要時にdoubleへ切り替える方式」は全カテゴリで遅くなったため採用しなかった。

## マイクロベンチマーク

`tests/bench/to_num_radix_micro.cpp`を、変更直前の同一ソースのスナップショット
`build-to-num-cold/jsnum-pre-radix.cpp`と新版にそれぞれg++ `-O3`でリンク。各カテゴリ512入力×2000反復を3回測定。数値はns/変換の中央値。

| 入力 | 変更前 | 変更後 | 変化 |
|---|---:|---:|---:|
| 有効な2進数・44桁 | 45.50 | 29.20 | 約35.8%短縮 |
| 有効な8進数・16桁 | 14.77 | 12.71 | 約13.9%短縮 |
| 有効な16進数・8桁 | 11.77 | 10.73 | 約8.8%短縮 |
| 長い16進数・25桁 | 25.53 | 25.13 | ほぼ同等 |
| 先頭ゼロを含む16進数・36桁 | 38.12 | 37.92 | ほぼ同等 |
| 不正な16進数・8桁 | 9.18 | 8.60 | 計測揺れ込み |

この数値はパーサ単独の実測であり、一般的なScratch作品の性能改善率ではない。

## 互換性

`tests/bench/to_num_parity.cpp`を拡張。既存の固定乱数・指数表記・Unicode空白など3,608,461ケースに、0x/0o/0bの有効／不正入力（1〜145桁、先頭ゼロ、2^53境界、末尾の不正文字、接頭辞の符号など）250,000ケースを追加。

**合計3,858,461ケースで旧版と新版の`double`の64ビット表現が一致**。

`tests/unit/test_cast.cpp`には0x/0o/0bの短い値、境界値、無効な桁に関する明示的なテストを追加。

`tests/differential/jit_to_num_radix.py`を追加し、Scratchの`join`で毎回構築した基数付き文字列について、インタプリタとEager/HotSpot JIT（非warp／warp、冷文字列ヘルパーON／OFF）で結果が一致することを検証。CTest名は`xyo-jit-to-num-radix`。

## 再現

```sh
g++ -std=c++20 -O3 -Isrc -Isrc/core \
  tests/bench/to_num_radix_micro.cpp src/core/jsnum.cpp \
  -o build-to-num-cold/to-num-radix-after
build-to-num-cold/to-num-radix-after

python3 tests/differential/jit_to_num_radix.py build-to-num-cold/xyo
ctest --test-dir build-to-num-cold --output-on-failure -R 'xyo-jit-to-num-radix'
```

Git作業ツリーには他の機能の多数の未コミット変更があるため、変更のリセットやコミット・Pushは実施していない。

## 最終検証

Release構成の独立した build-to-num-cold で全ターゲットを再ビルドし、CTestを実行。
**57件中57件成功（167.40秒）**。追加した xyo-jit-to-num-radix（非warp／warp×Eager／HotSpot×cold helper ON／OFF）は成功し、OSR・Binaryen・イベントスケジューラ・GPU・UI・文字列・単体テストも通過。

ビット単位の基準比較は、変更前のソースを `build-to-num-cold/jsnum-pre-radix.cpp` に保存して同一コンパイラ・最適化フラグでビルド。3,858,461ケースの出力バイナリが `cmp` で完全一致した。

桁ごとに2^53を判定する案と、基数付き文字列の先頭ゼロを別走査で除去する案は、実測で遅くなるカテゴリがあったため撤回。最終実装は短い入力の整数シフト処理と長い入力の従来FP経路のみ。

**Commit・Pushはしていない。**
