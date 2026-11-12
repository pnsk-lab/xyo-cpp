# START-005: 任意の高速JSONパーサーでSB3起動を短縮

2026-11-10。現行xyo-cppのSB3ロードでは、ScratchBench Parityの
`project.json` だけで5,132,327バイトある。従来の再帰下降パーサーは
`std::map` のJSON DOMへ直接書き込んでいた。

## 実装

`-DXYO_JSON_YYJSON=ON` で、システムにインストールされた
MITライセンスの**yyjson**を利用できるようにした。
**既定はOFF**。ONでも従来のJSON DOM型
`JsonValue` / `JsonArray` / `JsonObject` へ変換するため、
SB3ローダーや実行エンジンの公開ABIには変更がない。

CMakeは`yyjson.h`と共有ライブラリを明示的に検出する。
見つからない場合はONのビルド構成を失敗させる。
WASM、完全静的リンク、クロスビルドでは指定を拒否する。
ライブラリは同梱しない。

```bash
cmake -S . -B build-fast -DCMAKE_BUILD_TYPE=Release \
  -DXYO_JIT=ON -DXYO_LINK_JEMALLOC=ON -DXYO_JSON_YYJSON=ON
cmake --build build-fast --target xyo-cli -j4

# 繰り返し起動では前回実装済みのAOTキャッシュも利用できる
bun tools/xyo-aot-cache.ts prepare project.sb3 --bin build-fast/xyo
bun tools/xyo-aot-cache.ts play project.sb3 --bin build-fast/xyo

# 同じバイナリで旧JSONパーサーとA/B比較可能
XYO_JSON_YYJSON=0 build-fast/xyo play project.sb3
XYO_JSON_YYJSON=1 build-fast/xyo play project.sb3
```

## 符号付きゼロと意味論

yyjsonの通常の数値読み込みはJSONリテラル `-0` を
符号なしゼロとして格納することがある。
Scratchでは負のゼロを区別するため、
そのまま高速化すると意味論の退行になる。

今回、文字列やエスケープされた引用符を飛ばしながら
**JSON文字列の外にある裸の整数リテラル`-0`** を検出する。
該当する場合だけyyjsonの `YYJSON_READ_NUMBER_AS_RAW` を利用し、
`std::from_chars`（範囲外は従来の `strtod`）で値を復元する。
yyjsonが拒否した入力は従来パーサーで改めて判定する。
これにより、旧実装が許容していた不正Unicodeなどの扱いも維持する。

新規 `tests/platform/json_yyjson_parity.cpp` は整数、
境界値、指数、負のゼロ、サロゲートペア、エスケープ、
重複キー、不正JSONなどの**29例**に加え、
実際の5.13MBのJSONファイルのDOM一致を検証する。
独立バイナリでは30/30 PASS。
`XYO_JSON_YYJSON=ON` のCMake構成では
`xyo-json-yyjson-parity` CTestを登録し、実行済み。
通常ビルドではこのテスト／リンク依存は増えない。

## 性能（CPU10固定、交互A/B）

AOTオブジェクトの生成時間は含まず、
実プロジェクトが最初の出力を行うまでを計測。
システムは他の開発作業と共有しており実測値には変動がある。

| 条件 | 従来JSON | yyjson | 備考 |
| --- | ---: | ---: | --- |
| 独立Release・glibc、交互16組 | 288.7ms | 285.2ms | JSON解析39.6→36.9ms |
| 独立Release・jemalloc、交互16組 | 252.7ms | 248.5ms | JSON解析32.6→31.7ms |
| 現行共有Release・jemalloc、交互20組 | 246.6ms | 245.1ms | yyjson 14/20組勝利 |

現行共有Releaseでの20ペア速度比の幾何平均は **1.0122倍**。
**大規模な起動改善ではなく、数msの選択的最適化**である。

同じ現行共有ReleaseバイナリのAOT実行（479スクリプト事前生成、
8423スロット）で、両パーサーともScratch Parity
**2656/2656件 PASS**。

| 1回ずつの全量Parity完走 | 従来JSON | yyjson |
| --- | ---: | ---: |
| 壁時計時間 | 17.40秒 | 17.49秒 |
| 最大RSS | 419.3MB | 411.2MB |

完走時間・メモリ使用量は各1回のみで有意差を主張しない。
起動時間削減効果が小さく外部ライブラリへの依存が増えるため、
配布版の既定は従来パーサーのまま。

## 採用しなかった実験

yyjsonで生成した大きなJSONオブジェクトを一旦ベクターに蓄え、
キーを整列してから`std::map::emplace_hint`する案は、
単体プロトタイプで通常挿入より遅かったため取り込まなかった。

作業内容は未コミット・未push。GitHub Actionsの実CIは未実行。
