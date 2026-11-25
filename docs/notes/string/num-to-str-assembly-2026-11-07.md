# xyo-cpp: x86-64 アセンブリ版 Number → String（2026-11-07）

## 対象と制約

`jsnum::toString(double)`で使う**正確な整数の10進整形**を、手書きのx86-64 SysVアセンブリで実装した。使用するファイルは`src/core/num_to_str_x86_64_sysv.S`。

最短往復表現が必要な一般の`double`（小数・指数・非正規数・2^53超）については、**従来の`std::to_chars(double, std::chars_format::scientific)`＋ECMAScript形式への再整形を維持**している。これは浮動小数点数全体をアセンブリで実装したという意味ではない。

`CMakeLists.txt`で`XYO_ASM_NUM_TO_STR`を追加した。Linux x86-64限定、既定OFF。macOS/Windows/ARMなどは既存の`std::to_chars`を使う。ONを非対応環境で指定した場合は構成時にエラーとする。

```sh
cmake -S . -B build-to-num-asm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DXYO_ASM_NUM_TO_STR=ON
cmake --build build-to-num-asm --target xyo-cli xyo-tests xyo-num-to-str-asm-parity -j4
ctest --test-dir build-to-num-asm --output-on-failure \
  -R 'xyo-num-to-str-asm-parity|xyo-tests|xyo-jit-to-num'
```

## アセンブリ実装

`extern "C" size_t xyo_i64_to_ascii_asm(int64_t value, char out[32])`。

- 引数: SysV AMD64 ABIの`rdi`（整数）・`rsi`（出力32バイトバッファ）。戻り値は`rax`の生成文字数で、NUL終端は書かない。
- 正負の1〜2桁は、2桁ルックアップ表を使って`out`へ直接書く。カーネル呼び出し、ヒープ確保なし。
- 3桁以上は2桁ずつ末尾側へ書き、最後に前方へコピーする。`|n| ≤ 2^53`の範囲では`ceil(2^68/100)`による逆数乗算で100除算の商を計算。`INT64_MIN`を含む大きな値は通常の64ビット符号なし除算を使う。
- 呼び出し規約のcallee-savedレジスタを破壊せず、SSE/AVXの使用・動的確保・実行時関数呼び出しを行わない。
- **実際の`jsnum::toString`では、計測で速かった符号付き2桁整数（-99〜-10、10〜99）に限定してアセンブリを選択**。3桁以上はstd::to_charsへフォールバックする。0は既存の特殊処理、1桁はstd::to_charsを維持する。

## 正確性

- `tests/bench/num_to_str_asm_parity.cpp`: 最小・最大の符号付き整数、十進桁境界、乱数100万個、さらに符号付き2^53以下の乱数200万個。全**3,000,025整数**について`std::to_chars`とバイト列・桁数を比較し、範囲外のメモリ書き込みを検査。全件一致。
- `tests/bench/num_to_str_asm_full_parity.cpp`: `jsnum::toString(double)`を従来経路とアセンブリONで別々にビルドし、浮動小数点ランダム50万個・正確な整数50万個・境界値・-999〜999を含む**1,002,023値**の実際のECMAScript出力をバイナリ比較。完全一致。
- `tests/CMakeLists.txt`: `XYO_ASM_NUM_TO_STR=ON`時に`xyo-num-to-str-asm-parity`をCTest登録。

## 速度

`tests/bench/num_to_str_asm_micro.cpp`はアセンブリの純粋な整数整形を、`tests/bench/num_to_str_asm_full_micro.cpp`は`jsnum::toString(double)`全体を測定する。各データセット256値の反復、5回の結果の中央値（ns/変換）。

| 値の種類 | 標準実装 | アセンブリON | 評価 |
|---|---:|---:|---|
| 2桁整数の`toString` | 約9.94ns | 約6.09ns | 約1.63倍 |
| 1桁整数の`toString` | 約5.10ns | 約5.50ns | 逆効果 |
| 6桁整数の`toString` | 約11.13ns | 約11.48ns | ほぼ同等 |
| 15桁整数の`toString` | 約15.69ns | 約15.88ns | ほぼ同等 |
| 小数・指数表記 | 約66〜67ns | 約69ns | ほぼ同等 |

単体の整数整形では3桁以上で`std::to_chars`が手書き版より速かった。そのため、最終のJIT/インタプリタ経路では2桁だけを対象にしている。

**Scratchでの実効果は限定的**。`cast::toString`には**-128〜1023の小整数共有キャッシュ**があり、2桁整数は通常、文字列整形自体が呼ばれない。したがってこの改善をランタイム全体の高速化とは主張できず、既定OFFとした。

## 追加検証

独立したRelease構成の`build-to-num-asm`でアセンブリONの全ターゲットをビルドし、**CTest 60/60件成功（243.13秒）**。LLVM JIT、AOT、OSR、Binaryen、GC、GPU、エディタ、文字列関連も含む。その後、拡張済み`xyo-num-to-str-asm-parity`を再ビルドして**追加で1/1成功**（3,000,025整数）。

アセンブリ関数は`build-to-num-asm/xyo`に実際にリンクされていることも確認した（`nm -C`に`xyo_i64_to_ascii_asm`が存在）。

小ベンチマークでは、直接整数整形について正の2桁が`std::to_chars`約2.19ns→アセンブリ約1.09ns、負の2桁が約2.69ns→1.05ns。一方、6桁程度からアセンブリの長整数経路は`std::to_chars`より遅いため、通常の`jsnum::toString`からは呼ばない。

## Git

ユーザーからコミットの指定はないため、他の並行作業の未コミット差分は保持し、Commit・Pushは実行していない。
