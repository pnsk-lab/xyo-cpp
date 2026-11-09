# xyo-cpp Number→String：高速な既定バックエンド（2026-11-07）

## 最終構成

Dragonbox (V8採用) → V8由来 SignificandToChars → ECMAScript固定/指数表記へ直接整形 → スタック上のASCIIバッファ → GC所有のSStr。

- `XYO_NUM_TO_STR_DRAGONBOX=ON` を**既定有効**に変更。`std::to_chars(double, scientific)`の生成と科学表記テキストの再解析を省略する。`OFF`で完全な旧C++経路へ切り替えられる。
- `XYO_NUM_TO_STR_DIRECT_BUFFER=ON` をDragonbox有効時の**既定有効**に設定。`jsnum::toChars(double,char[64])`が例外値、負数、正確な整数、一般の浮動小数点をASCIIバッファへ直接出力する。`jsnum::toString`もこの関数を経由する。
- `v8_decimal::toCharsPositive(double,char*)`がDragonboxの仮数・指数から、文字列の中間確保なしで最短10進のASCIIを出力する。
- `cast::numberToString`は固定長バッファから直接`SStr`を生成し、長い数値文字列での`std::string`生成・コピーを省く。
- `smallIntTable`は小整数（-128..1023）のキャッシュを生成するとき、数値キャッシュも`numState=1`で公開する。既知の数字文字列を初回から再パース不要にする。
- 以前のx86-64アセンブリフォーマッタは独立オプションとして残すが、Dragonboxとの併用は実測で遅いため両方とも既定OFF。macOS、Windows、ARM、WASM向けのバックエンドはC++20で完結する。

## 互換性とテスト

- DragonboxでIEEE754 binary64 1,002,023値の旧C++版に対する出力が完全一致。
- Node.js/V8の`String(number)`と602,023入力で完全一致。
- `tests/bench/num_to_str_direct_sstr_parity.cpp`を追加。100,020個の入力で最終SStrのUTF-16内容、バッファ境界番兵、数値キャッシュ状態、-0を確認する。
- 直接出力を有効にした後、旧実装と1,002,023入力の数値文字列が完全一致。
- 直接バッファ・V8数字生成・小整数キャッシュを統合したRelease構成にて**全CTest 62/62成功（167.59秒）**。Node.js/V8差分、100,020個のSStr実体チェック、LLVM JIT、OSR、Binaryen、GC、GPUを含む。
- clang++ -O3でも1,002,023個の数値→文字列結果が旧実装とバイト単位で一致。

## 計測結果

単独フォーマッタの基準：Dragonbox導入前の`std::to_chars(scientific)`版では小数54.43ns、Dragonbox+V8整数数字生成は27.64ns（約1.97倍）。非常に大きい/小さいdoubleでは54.92→34.37ns（約1.60倍）。

Dragonboxだけの旧実装と、`SStr`への直接バッファ経路を追加した新実装のScratch Eager JIT比較（9回中央値、起動とJITコンパイル含む）：

| 作品 | Dragonboxのみ | 直接バッファ付き | 短縮 |
|---|---:|---:|---:|
| 小数文字列化 500,000反復 | 51.69ms | 51.28ms | 約0.8% |
| 指数文字列化 300,000反復 | 43.48ms | 42.91ms | 約1.3% |
| 混合文字列化 300,000反復 | 43.21ms | 42.55ms | 約1.5% |

元の`std::to_chars(scientific)`版からDragonboxへ置き換えると、同種の専用Scratch作品で12〜14%程度短縮した。後段の直接バッファ化でさらに小さな追加効果がある。ただし専用作品での総実行時間であり、全Scratch作品での同等の改善を保証しない。

## 追加の独立したGC文字列ベンチ

直接バッファの効果は小さく、起動・JITコンパイルを含む測定では実行環境の負荷により符号が反転した。`tests/bench/num_to_str_sstr_micro.cpp`を用意し、同じReleaseの`libxyo.a`に対して`cast.cpp`だけ直接バッファON/OFFでコンパイルして比較した。5回の交互実行中央値。

| 入力 | `std::string`経由 | `SStr`直接出力 | 比率 |
|---|---:|---:|---:|
| 一般的な小数 | 71.26ns | 70.47ns | 1.01倍 |
| 極端なdouble | 75.70ns | 74.72ns | 1.01倍 |
| 長い整数 | 60.54ns | 63.29ns | 0.96倍 |
| 短い数字 | 40.36ns | 41.12ns | 0.98倍 |

`jsnum::toString`単独では、負数を含む極端な浮動小数点が34.25→30.52ns、一般的小数が27.49→27.18ns。ただし整数単独処理は従来構成が同等または速い。実際の`cast::toString`は-128〜1023の整数文字列をキャッシュする。

11回試行の追加測定では、JIT込みの時間差は小数0.7%改善、指数0.5%悪化、混合3.6%悪化と不安定だった。**直接バッファ化の総実行時間への改善は未確証**で、マイクロベンチマークの小数経路の僅かな改善のみ支持する。このため`XYO_NUM_TO_STR_DIRECT_BUFFER=OFF`でA/Bテスト可能。

## 採用しなかった試行

- Dragonbox仮数の数字生成をV8由来の`SignificandToChars`からC++整数`std::to_chars`へ変更：小数で約27.60→31.72nsとなり撤回。
- 既存の浮動小数点→整数→浮動小数点の判定をIEEE754ビットマスクへ変更：整数で遅くなり、小数の差はノイズ程度のため撤回。
- Dragonboxの後ろで既存のx86-64アセンブリ整形を使用：小数27.64→70.85nsと逆効果のため既定OFF。

## ビルド

```sh
cmake -S . -B build-v8-dragonbox -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DXYO_NUM_TO_STR_DRAGONBOX=ON \
  -DXYO_NUM_TO_STR_DIRECT_BUFFER=ON \
  -DXYO_ASM_ECMA_FORMAT=OFF -DXYO_ASM_NUM_TO_STR=OFF
cmake --build build-v8-dragonbox -j4
ctest --test-dir build-v8-dragonbox --output-on-failure
```

今後の比較では、文字列GC確保（および文字列連結そのもの）など数値変換以外のコストが支配的になりやすい。最速という表現は検証した候補間でのことであり、全CPUや全種類のScratch作品での最速を保証するものではない。

変更はローカルに保存済み。別作業の未コミット差分には触れず、Commit/Pushしていない。
