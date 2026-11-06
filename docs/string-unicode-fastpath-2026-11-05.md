# Unicode文字列検索・比較の高速化（2026-11-05）

## 対象

xyo-cpp の Scratch \`operator_contains\` と文字列比較（\`operator_equals\` / \`operator_lt\` / \`operator_gt\` などが内部で使う処理）。

従来の非ASCII経路:

- \`contains\`: 毎回 \`u16ToLower(hay)\` と \`u16ToLower(needle)\` で **2つの文字列を確保**して検索
- 大小文字無視の比較: Unicodeを逐次小文字化して比較。確保はしないが、各文字ごとにUnicodeの変換表を参照
- ASCIIは既に特殊化済みであり、変更しない

## 変更

\`SStr\` は不変なので、最初の検索・比較時に \`lowerCp\` で文字列の性質を証明して、\`flags\` にatomicなビットとして記録する。構造体サイズは増やさない。

1. **lowercase-stable**: JS/ScratchのUnicode小文字化をしても文字列が完全に同一。日本語、ハングル、絵文字、既に小文字の英語など。以後は生のUTF-16 \`find\` / 比較を使う
2. **ASCII-fold-compatible**: 非ASCIIの文字は一切変化しないが、ASCII \`A-Z\` のみ小文字化される。英大文字が混在する日本語文字列など。比較ではASCIIだけfoldし、検索では短い針（16 UTF-16コードユニット以下）に限り、確保しない \`std::search\` を使う
3. **それ以外**: 従来のUnicode完全変換経路。例: \`İ\` の2文字展開、\`K\`、ギリシャ語の文脈依存語末シグマ、Deseret大文字など

UTF-16サロゲートペアはUnicodeコードポイントとして解析する。不正な単独サロゲートは従来どおり変化しない。大小文字変換の証明には実際の \`u16ToLower\` と共通の \`lowerCp\` を使う。

スレッド間でimmutableな \`SStr\` を共有しても、キャッシュ状態は \`atomic_ref.fetch_or\` で公開する。既存のJIT/AOTが使う \`SStr\` レイアウトは不変。追加ビットは \`flags\` の未使用領域のみ使用する。

最適化は**既定ON**。同じバイナリで従来経路を比較したいとき:

~~~sh
XYO_STRING_UNICODE_FAST=0 build/xyo run project.sb3 --jit eager
~~~

## 検証

~~~sh
cmake --build build --target xyo xyo-tests -j4
ctest --test-dir build --output-on-failure
python3 tests/differential/string_unicode_fast.py build/xyo
python3 tests/differential/string_unicode_fast.py build/xyo --bench

# 文字列関数だけのマイクロベンチマーク
c++ -O3 -DNDEBUG -std=c++20 -Isrc \
  tests/bench/string_unicode_case_bench.cpp build/libxyo.a \
  -lz -lLLVM-23 -ldl -lSDL2 -o build/string_unicode_case_bench
build/string_unicode_case_bench
~~~

\`tests/unit/test_sstr_cons.cpp\` に日本語、ハングル、ギリシャ語、İ、K、Deseret、絵文字、不正なサロゲートを含むケース群の、従来のUnicode小文字化結果との照合を追加した。

\`tests/differential/string_unicode_fast.py\` は4件の実SB3構造を生成し、インタプリタ・Eager JIT・HotSpotで、経路ON/OFFの変数状態の完全一致を検証する。各作品は1,000反復。

## 実SB3の性能

同じビルドで \`XYO_STRING_UNICODE_FAST=0/1\` を切り替えた。CPU 0固定、OFF/ONをランダム順に各7回、プロセス起動・JITコンパイル・変数出力を含むwall-clock中央値。

| ケース | 実行経路 | OFF | ON | 速度比 |
|---|---|---:|---:|---:|
| 長い日本語 | interpreter | 63.96ms | 18.32ms | 3.49x |
| 長い日本語 | eager JIT | 58.94ms | 44.10ms | 1.34x |
| 長い日本語 | hotspot | 61.58ms | 45.44ms | 1.36x |
| 日本語＋英大文字 | interpreter | 60.61ms | 17.29ms | 3.50x |
| 日本語＋英大文字 | eager JIT | 85.74ms | 41.87ms | 2.05x |
| 日本語＋英大文字 | hotspot | 86.13ms | 41.57ms | 2.07x |
| Unicode特殊大小文字 | interpreter | 16.84ms | 16.63ms | 1.01x |
| Unicode特殊大小文字 | eager JIT | 41.81ms | 42.03ms | 0.99x |
| Unicode特殊大小文字 | hotspot | 41.94ms | 41.79ms | 1.00x |
| 日本語＋絵文字 | interpreter | 16.39ms | 15.74ms | 1.04x |
| 日本語＋絵文字 | eager JIT | 41.55ms | 41.88ms | 0.99x |
| 日本語＋絵文字 | hotspot | 42.49ms | 41.15ms | 1.03x |

**これは文字列検索・比較を意図的に繰り返す合成SB3の計測であり、一般のScratch作品全体の高速化率ではない。** 文字列長・実行数・JIT編成に依存する。

## マイクロベンチマーク

約768文字の日本語文字列、繰り返し同じ \`SStr\` オブジェクトを検索・比較。旧Unicode小文字化・逐次比較とキャッシュ済み高速経路を比較すると、おおむね以下。

- CJK \`contains\`: 25µs程度 → 0.19µs程度（約130倍）
- CJK 同内容の比較: 49µs程度 → 0.19µs程度（約260倍）
- 日本語＋\`HELLO\` の \`contains\`: 0.21µs程度 → 0.009µs程度（約24倍）
- 特殊な大小文字変換が必要な入力には高速経路を適用しない

これは関数単体の極端なワークロードであり、コンパイル時間・スケジューラ・描画・GCなどを含まない。

## 制限と次の調査

- 初回の判定にはO(length)のコストがかかる。**繰り返し利用されるSStrが主要対象**。
- \`std::search\` を使うASCII-fold-compatible混在検索は針が16コードユニット以下の場合に限定し、長い場合は従来のUnicode経路を使う。
- 数値文字列比較の意味論は \`cast::compare\` に任せる。今回の高速化は文字列比較に入った場合のみ。
- CPU・メモリとScratchBenchの実作品については、別途ホールドアウトセットで性能とパリティを検証する必要がある。

CTest 15/15通過（2026-11-05、既存全テストを含む）。

未コミット、未push。他の実験中の未コミット変更は保持。
