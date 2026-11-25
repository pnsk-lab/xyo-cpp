# Number → String: scientific formatter x86-64 ASM (2026-11-07)

## 実装

Scratch/ECMAScriptの数値→文字列変換を高速化するため、正の有限非ゼロのdoubleを科学表記へ変換した後の処理をアセンブリ化した。

- src/core/ecma_format_x86_64_sysv.S
  - xyo_ecma_format_asm(digits, k, n, out): 1〜17桁の最短数字列と小数点位置をECMAScript形式へ変換。NUL終端はせず長さを返す。
  - xyo_ecma_from_scientific_asm(sci, len, out): std::to_chars(double, scientific)が返す正規の科学表記を走査し、仮数・指数を取り出して上の整形ルーチンを呼び出す。
- src/core/jsnum.cpp: XYO_ASM_ECMA_FORMATがONなら、最短数字列の生成後にASMへ直接移る。NaN、±Infinity、-0、負数の符号処理と正確な最短数字列の生成は従来のC++のまま。
- CMakeLists.txt: XYO_ASM_ECMA_FORMATは独立した実験オプションで既定OFF。Linux x86-64 SysVのみでONにできる。
- tests/CMakeLists.txt: xyo-ecma-format-asm-parityをCTestへ登録。

これはRyuやDragonboxの浮動小数点→最短数字列生成をアセンブリ化した実装ではない。桁生成はstd::to_charsを使い続け、科学表記の解析、指数のパースと小数点配置をアセンブリ化した。

## 表記規則

k=最短数字列の桁数、n=科学表記の指数+1。

| 条件 | 出力 |
|---|---|
| k <= n <= 21 | digits + (n-k)個の0 |
| 0 < n < k、n<=21 | n桁目に小数点 |
| -5 <= n <= 0 | 0. + (-n)個の0 + digits |
| その他 | 先頭1桁 + 小数部 + e±(n-1) |

0.000001は固定表記、0.0000001は1e-7、1e20は固定表記、1e21は1e+21。指数は0埋めなし。

## 正確性と安全性

- tests/bench/ecma_format_asm_parity.cpp: k=1〜17、n=-327〜310の系統試験とランダム入力で合計716,920件。数字列からの整形と科学表記からの直接整形を両方検証。C++参照実装とバイト単位で一致、出力バッファの番兵も保全。
- tests/bench/num_to_str_asm_full_parity.cpp: 元のC++ jsnum::toStringとASM版で、ランダムな浮動小数点50万件、整数50万件、特殊値など2,023件の合計1,002,023件をバイト単位で比較し、完全一致。±0、Infinity、NaN、非正規数、1e-6/1e-7、1e20/1e21を含む。
- ASM ONのRelease全ビルド: CTest 61/61成功、171.33秒。LLVM JIT、AOT、OSR、Binaryen、GPU、UI、GC、イベントスケジューラと既存差分テストを含む。
- 非対応OS/CPUではビルド時に明示的に拒否し、既定OFFでは従来のC++経路を利用する。

## 単独ベンチマーク

tests/bench/num_to_str_asm_full_micro.cpp、各256入力の反復、中央値。

| 種類 | C++ | ASM | 高速化 |
|---|---:|---:|---:|
| 小数 | 55.39ns | 52.09ns | 約6% |
| 極端な浮動小数点 | 54.89ns | 50.11ns | 約9% |
| 6桁整数 | 9.01ns | 9.03ns | 同等 |
| 15桁整数 | 13.16ns | 13.09ns | 同等 |

## 実Scratch作品での比較

tests/bench/num_to_str_scratch_e2e.pyで生成したScratch作品のjoin演算を用い、JIT Eagerで実行。C++比較バイナリはASM ON版と全オブジェクトを共用し、jsnum.cppだけを同じ最適化フラグでASM OFFに再コンパイルしてリンクした。試行順は交互、初回はウォームアップ、7〜9試行の中央値。出力された変数は全て一致した。

| 処理 | 反復 | C++ | ASM | 短縮 |
|---|---:|---:|---:|---:|
| i/7の文字列化 | 100,000 | 38.67ms | 37.92ms | 2.0% |
| i/7の文字列化 | 500,000 | 76.09ms | 74.02ms | 2.7% |
| 1/(i × 10^6) | 300,000 | 60.54ms | 58.60ms | 3.2% |
| 小数と微小値の混合 | 300,000 | 66.15ms | 64.25ms | 2.9% |

総実行時間にはプロセス起動・JITコンパイルを含む。タイミングの変動幅は数msあり、一般的なScratch作品に同じ改善を保証しない。既定OFFを維持。

再現用ログ:
- build-to-num-cold/ecma_cpp_micro.log
- build-to-num-cold/ecma_asm_micro.log
- build-to-num-cold/ecma_full_ctest.log
- build-to-num-cold/ecma-scratch-fraction-100k.json
- build-to-num-cold/ecma-scratch-fraction-500k.json
- build-to-num-cold/ecma-scratch-exponent-300k.json
- build-to-num-cold/ecma-scratch-mixed-300k.json

ビルド手順:

    cmake -S . -B build-to-num-asm -G Ninja -DCMAKE_BUILD_TYPE=Release -DXYO_ASM_NUM_TO_STR=ON -DXYO_ASM_ECMA_FORMAT=ON
    cmake --build build-to-num-asm -j4
    ctest --test-dir build-to-num-asm --output-on-failure

Gitの他の未コミット変更は保持した。Commit・Pushは未実施。
