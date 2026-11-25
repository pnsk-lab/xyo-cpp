# JIT: 非数値文字列の等値比較を直接実行（2026-11-03）

## 変更

`src/jit/rt.cpp` の `xyo_eq` が、すでに双方の `SStr::numState` が `2`（文字列の数値解釈がNaN）だと分かっていても、`cast::compare` に戻って再度数値化の経路をたどっていた。

両方の文字列の型・キャッシュ状態が非数値文字列と判明している場合のみ、`compareStringsCaseInsensitive(SStr*, SStr*)` を直接呼ぶ。双方が文字列であることは既存のガード済み。UTF-16/Unicodeの大小文字処理も既存の比較関数へ委譲する。数字として解釈できる文字列、空白のみの文字列、`Infinity`、キャッシュ未計算（state 0）は従来の `cast::compare` を使用する。値の型を推測してショートカットしてはいない。

診断用のオプトアウトは `XYO_JIT_NONNUMERIC_STR_EQ=0`（プロセス起動時に確定、コンパイル済みScratchループには条件分岐を追加しない）。

## 実測

ローカル Linux / i7-12700 / 同一Release実行ファイル `build/jit-eq-nonnumeric-frozen-xyo`。CPU12固定、ScratchBenchの既存ソースを複製して単独SB3へした5カテゴリから4カテゴリを選択。8セットの交互ON/OFF試験（セット間で実行順を反転）。各カテゴリの中央値、ops/ms（大きいほどよい）。

| ワークロード | OFF | ON | ON/OFF |
| --- | ---: | ---: | ---: |
| `math_strcompare` | 2,666.666 | 3,819.040 | **1.4321倍** |
| `math_compare` | 29,156.409 | 29,028.851 | 0.9956倍 |
| `algo_primes` | 58,519.376 | 58,720.535 | 1.0034倍 |
| `app_raycast` | 31,372.713 | 31,160.268 | 0.9932倍 |

チェックサムは全8回×4カテゴリ×2条件で一致。負の差0.4～0.7%はこの反復数ではノイズと区別できない。元のScratchBenchは改変していない。

生ログ: `build/jit-eq-nonnumeric-same-binary-20261103.log` / `build/jit-eq-nonnumeric-same-binary-20261103.json`。

別バイナリ間の予備6回比較では `math_strcompare` が1.481倍だったが、最終判断には同一バイナリの1.432倍を採用した。

## 正しさ

`tests/unit/test_type_profile.cpp` に、キャッシュ作成前後のJIT `xyo_eq` 直接比較を追加: `Hello World` / `hello world`、異なる文字列、大小文字、`Infinity`、`0` / `00`、空白、Unicode `Ä` / `ä`。Scratchの文字列/数値比較を保持。

変更前から残るインタプリタ・Eager JITのすべての型変更差分テストも維持。

フルScratchBench performance.sb3は全47カーネル完走・`DONE`、非scheduler39カーネルのチェックサム一致: `build/jit-eq-nonnumeric-full-20261103.log`。他のプロセスが走る時間帯ではCPU負荷による約2倍の変動を観測したため、全47項目の速度全体平均は**効果として主張しない**。

最終回帰テスト:
- **CTest 7/7成功**: `build/jit-eq-nonnumeric-final-ctest-20261103.log`
- **HotSpot Parity 2123/2123**: `build/jit-eq-nonnumeric-final-hotspot-parity-20261103.log`
- **Eager Parity 2123/2123**: `build/jit-eq-nonnumeric-frozen-eager-parity-20261103.log`
- 最初のEager再実行では共有ワークツリーの実行ファイル更新と競合して終了126（起動失敗）。性能測定時に保存した固定バイナリで再検証し、2123/2123を確認。

## 不採用の試行

`emitTruthyV` に文字列長からtrue/falseを直接決める追加の試験経路を設けたが、全体の交互測定でCPU負荷が不均一で有効性を判定できず、標準実装へは採用せずソースから完全に撤回した。

## 状態

修正とテストはローカルの未コミットワークツリーに適用。既存の未コミット変更は保持。commit / push は実施していない。
