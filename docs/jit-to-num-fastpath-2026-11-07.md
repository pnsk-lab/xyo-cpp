# `xyo_to_num` 高速化（2026-11-07）

## 変更点

- `src/core/jsnum.cpp`: ECMAScript数値文字列の前後空白除去・符号・接頭辞を処理した後、**絶対値が2^53以下の短い整数文字列**をuint64整数として直接読み、doubleへ正確に変換する。長大な数値文字列と表現範囲外は従来の正確な10進解析へフォールバックする。負のゼロも符号を保存。
- `src/core/jsnum.cpp`: 構文検証後の小数・指数表記を、従来の`strtod_l`より軽い`std::from_chars`で解析。範囲外・解析失敗はロケール固定の`strtod_l`へフォールバックし、Infinity・非正規数・underflowを維持。
- `src/jit/rt.cpp`: `xyo_to_num`の即値をNaN-boxの符号付き順序で高速判定。true/false/空文字列/色/undefined、canonical NaNの意味は不変。ヒープ文字列の`numState`を直接読み、キャッシュ済みの場合は`numCache`から値を返す。初回のみ共通の解析・キャッシュ登録経路へ進む。
- `src/core/cast.cpp` / `cast.hpp`: 初回の文字列解析を`parseNumberStringUncached`として共通化し、JITが既に`numState==0`を確認済みなら二重タグ判定・二重状態読出しを避けられるようにした。既存`numState`分類（1, 2, 3, 4）は維持し、Atomic release/acquireの公開規則を変更していない。

## 単独パーサ性能

`tests/bench/to_num_micro.cpp`を、**変更前の同じリポジトリの`jsnum.cpp`**と変更後の`jsnum.cpp`へそれぞれリンクし、各カテゴリ512入力×2000回＝102.4万変換を3セット測定した。単位は1変換のナノ秒。環境・CPU周波数の影響に注意。

| 変換 | 旧版の概数 | 改善後の概数 | 備考 |
|---|---:|---:|---|
| 短い整数文字列 | 36.6〜37.5ns | 9.8〜10.5ns | 約3.7倍 |
| 小数文字列 | 60.4〜61.7ns | 25.6〜25.7ns | 約2.4倍 |
| 指数表記 | 93.2〜95.5ns | 21.9〜23.3ns | 約4.1倍 |
| 0x/0b接頭辞 | 5.7〜5.9ns | 5.1〜5.2ns | 既存経路でほぼ変化なし |

再現：

```sh
g++ -std=c++20 -O3 -Isrc tests/bench/to_num_micro.cpp \
    src/core/jsnum.cpp -o build/to_num_micro_fast
build/to_num_micro_fast
```

旧版の原文は`git show HEAD:src/core/jsnum.cpp`で取り出し、同じ最適化フラグで比較した。生データ：`build/to_num_micro_before.txt`、`build/to_num_micro_fast.txt`。

## ビット単位の互換性

`tests/bench/to_num_parity.cpp`が生成する**240,041種類**の文字列について、旧版と新版でIEEE754の64ビット表現がすべて一致した。固定乱数の長整数、小数、指数、極端なオーバーフロー・アンダーフロー、±0、2^53境界、Unicode空白、0x/0b/0o、Infinity、非数値などを含む。

```sh
git show HEAD:src/core/jsnum.cpp > build/to_num_jsnum_head.cpp
g++ -std=c++20 -O3 -Isrc -Isrc/core tests/bench/to_num_parity.cpp \
    build/to_num_jsnum_head.cpp -o build/to_num_parity_baseline
g++ -std=c++20 -O3 -Isrc tests/bench/to_num_parity.cpp \
    src/core/jsnum.cpp -o build/to_num_parity_fast
build/to_num_parity_baseline > build/to_num_ref.bin
build/to_num_parity_fast > build/to_num_new.bin
cmp build/to_num_ref.bin build/to_num_new.bin
```

`tests/unit/test_cast.cpp`にも、JITヘルパーをまず未キャッシュの状態で呼び、その後のキャッシュ済み変換が`cast::toNumber`と**符号付きゼロを含めてビット一致**するユニットテストを追加。

`tests/differential/jit_to_num_fastpath.py`は、Scratchの`join`で毎回新たな数値文字列を生成し、インタプリタ／Eager JIT／HotSpot JITの出力をwarp・非warpで比較する。CTest名は`xyo-jit-to-num-fastpath`。

## `xyo_to_num`本体のランタイムマイクロベンチマーク

`tests/bench/to_num_runtime_micro.cpp`を`xyo-to-num-runtime-micro`（通常ビルド対象外）として登録。同一ビルド内の`cast::toNumber`経路と、JIT専用`xyo_to_num`エントリのコストを比較する。3回測定した中央値（ns/回）は次のとおり。

| Value種別 | cast::toNumber（旧ヘルパ相当） | 新xyo_to_num | 高速化 |
|---|---:|---:|---:|
| number即値 | 1.92ns | 1.13ns | 約1.70倍 |
| キャッシュ済みヒープ文字列 | 6.31ns | 1.65ns | 約3.82倍 |
| 未キャッシュヒープ文字列（毎回状態初期化） | 11.95ns | 11.63ns | 約1.03倍 |

xyo_to_numの旧実装はcast::toNumberへの委譲だったため、現行ビルド内の比較はヘルパー入口の変更を評価する。チェックサムはすべて一致した。保存先：build/to_num_runtime_final.txt。

```sh
cmake --build build --target xyo-to-num-runtime-micro
build/tests/xyo-to-num-runtime-micro
```

## 解釈と注意

上記の数値は**パーサやJITヘルパー単体**のマイクロベンチマークであり、一般のScratchプロジェクトが3〜4倍高速化するという意味ではない。すでに数値へ型特化したLLVM JITでは変換ヘルパーが呼ばれない。文字列キャッシュのある反復実行では初回のみ解析が必要なため、作品全体の改善幅は入力の型・文字列生成頻度に依存する。

Commit・Pushは行っていない。ほかの作業由来の未コミット変更は保持している。

## 最終ビルド・回帰テスト

- CMake再構成後、xyo-tests、xyo-cli、xyo-to-num-runtime-micro のビルドが成功。
- 新規Scratch差分テスト（毎回新たに生成する数値文字列）：warp／非warp × Eager／HotSpotの4構成で結果一致。
- xyo-tests冒頭のcast数値・文字列テスト：784チェック、0失敗。
- 全CTestは50件中49件成功。残るxyo-testsは、既存E2EのJITコンパイル判定が4件失敗したほか、GC関連の単体テストでSegmentation faultが発生した。
- Segfaultの回収したスタックは Gc::drainPending() → SList::get0()、tests/unit/test_gc_pool.cpp の手動リスト初期化に由来する。別の作業で追加されたSListのextraフィールドが手動初期化されていない可能性があり、数値変換処理の内部ではない。関連する別作業のコードは今回変更していない。
- 他の既存JIT差分テスト、Binaryen、OSR、文字列比較テストは通過。
- CTestの実行先だけを一時的に固定バイナリに向け、終了後に生成済み設定を復元した。

ビット一致を確認する build/to_num_ref.bin と build/to_num_new.bin も最終再実行で一致。Commit・Pushはしていない。
