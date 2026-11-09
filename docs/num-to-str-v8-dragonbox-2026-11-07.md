# xyo-cpp: V8 Dragonbox 数値→文字列変換（2026-11-07）

## 実装

V8の最短浮動小数点変換経路をxyo-cppへ実験的に移植。

- third_party/dragonbox/dragonbox.h: jk-jeon/dragonbox のヘッダ実装。仮数 uint64_t と指数 int32_t を直接取得。
- src/core/v8_decimal.cpp: V8 conversions.cc の SignificandToChars、ConvertUpTo9Digits、Convert8Digits と2桁表を独立C++20へ移植。ECMAScript表記への直接整形を行う。
- src/core/v8_decimal.hpp: 正の有限・非ゼロ double -> 最短10進文字列のAPI。
- src/core/jsnum.cpp: XYO_NUM_TO_STR_DRAGONBOX=ON のときだけ移植処理へ分岐。NaN、Infinity、-0、負数、小整数の高速経路は従来どおり。
- CMakeLists.txt: XYO_NUM_TO_STR_DRAGONBOX (現行の既定ON)。OFFなら移植ソースをリンクせず、従来のstd::to_charsを使う。
- tests/differential/num_to_str_dragonbox_node.mjs, .py と tests/bench/num_to_str_dragonbox_node_data.cpp: 実際のNode.js/V8のString(number)と比較。CTest名はxyo-v8-dragonbox-parity。

以前の方式はstd::to_chars(scientific)で最短文字列を作り、指数と有効数字を再解析していた。この経路ではDragonboxから仮数と指数を直接得て、V8由来の整数→数字列生成とECMAScript配置を行う。

## 取得元とライセンス

- Dragonbox upstream: https://github.com/jk-jeon/dragonbox/blob/master/include/dragonbox/dragonbox.h
  Blob SHA: 84d6e24e3f49c6ad2952bec9ac53cfeabb5ebc2c。著作権 Junekey Jeon。Boost Software License 1.0を採用。
- V8 conversions.cc: https://chromium.googlesource.com/v8/v8/+/refs/heads/main/src/numbers/conversions.cc
  Blob SHA: cfbaf358521c6c5a2d5f858d8063ab44039e6664。V8 BSDライセンス。
- 元ソースの著作権表示は保持。LICENSE-Boost、LICENSE-V8、third_party/dragonbox/README.mdに由来と条件を記載。ビルド時にネット接続は不要。

## 互換性

- 旧jsnum::toStringに対しランダムな浮動小数点50万、整数50万、境界値2,023を含む1,002,023件の出力がバイト単位で一致。
- 実際のNode.js v26.4.0のString(number)と602,023件の入力について完全一致。数値のIEEE754ビット列をNodeのBufferで復元して比較。
- DragonboxとXYO_ASM_ECMA_FORMATの同時ONでも1,002,023件で完全一致。ただし速度はDragonbox単独の方が速い。
- 正負0、NaN、Infinity、非正規数、2^53境界、1e-6と1e-7、1e20と1e21を含む。

## 単独ベンチマーク（g++ -O3、ns/回、3プロセス×5回の中央値）

| 入力 | 従来 C++ | V8 Dragonbox | Dragonbox + ASM |
|---|---:|---:|---:|
| 一般的な小数 | 54.43 | 27.64 | 70.85 |
| 極端なdouble | 54.92 | 34.37 | 59.51 |
| 6桁の整数 | 8.94 | 9.00 | 8.91 |
| 15桁の整数 | 13.02 | 12.72 | 12.76 |

## Scratch Eager JITベンチマーク（起動・JITコンパイル込み）

全てのオブジェクトを共通にし、jsnum.cppだけDragonbox OFFで再コンパイルした比較バイナリ build-v8-dragonbox/xyo-v8-baseline と build-v8-dragonbox/xyo を9試行ずつ交互に実行。出力全変数一致。

| 作品 | 反復 | 従来C++ | Dragonbox | 短縮 |
|---|---:|---:|---:|---:|
| i/7 の文字列化 | 500,000 | 68.06ms | 58.22ms | 14.45% |
| 1/(i × 10^6) の文字列化 | 300,000 | 55.55ms | 48.82ms | 12.12% |
| 小数と微小値の混合 | 300,000 | 54.65ms | 47.85ms | 12.43% |

これは文字列を大量に作る専用作品の結果であり、一般的なScratch作品の改善率を保証しない。

## 再現

cmake -S . -B build-v8-dragonbox -G Ninja -DCMAKE_BUILD_TYPE=Release -DXYO_NUM_TO_STR_DRAGONBOX=ON -DXYO_ASM_NUM_TO_STR=OFF -DXYO_ASM_ECMA_FORMAT=OFF
cmake --build build-v8-dragonbox -j4
ctest --test-dir build-v8-dragonbox --output-on-failure

測定ログは build-to-num-cold/v8-* に保存。初期実験では既定OFFだったが追加検証後に既定ONへ変更（docs/num-to-str-fastest-2026-11-07.md）。未コミットの別作業は保持し、Commit・Pushは実施していない。
