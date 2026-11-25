# LLVM JIT: 未キャッシュ文字列の数値変換を直接呼び出す（2026-11-07）

## 対象と実装

従来の `emitToNumF64` は、タグが文字列であり `SStr::numState==0` と確認したあとも、汎用の `xyo_to_num(bits)` を呼んでいた。`xyo_to_num` 内では再びNaN-boxタグを分類し、`numState` を読むため、判定が重複していた。

以下を実装した。

- `src/jit/rt.cpp`: `extern "C" double xyo_to_num_uncached(SStr*)`。既存の `cast::parseNumberStringUncached` に直接委譲する。文字列状態の分類・NaNの0への変換・原子的キャッシュ公開は既存の処理を利用し、意味論を変えない。
- `src/jit/backend_llvm.cpp`: `xyo_to_num_uncached` の外部シンボルを登録。LLVMのJIT/AOTどちらの解決でも利用できるようにする。
- `src/jit/codegen.cpp`: `XYO_JIT_TO_NUM_COLD_HELPER=1` のときのみ、JITが既に検証した文字列ポインタを直接渡す。**既定OFF**、他の型・キャッシュ済み文字列・数値変換には変更なし。

`numState=0` の観測後に別スレッドが同じ不変の`SStr`を解析しても、既存の`publishNumCache`の原子的公開規則を保ったまま同じ値を再解析できる。

## テスト

`tests/differential/jit_to_num_fastpath.py` を、ヘルパーON／OFF×Eager／HotSpot×warp／非warpの**8構成**に拡張。新しく結合生成した数字文字列（整数、小数、指数、非数値、空白、16進接頭辞など）の出力がインタプリタと一致した。ONのEagerコンパイルでは最適化後IRに`xyo_to_num_uncached`が含まれることもチェックする。

関連CTestは**6/6成功**（lazy型付きSSA、型フロー生成、if-PHI、duplicate scalar、to-num、OSR）。

## 速度の測定結果

Releaseビルド、512種の整数文字列×3000回を、毎回`numState=0`に戻して処理するJIT条件模擬ベンチマーク。15回の中央値：

| 経路 | 中央値 |
|---|---:|
| 汎用 `xyo_to_num(bits)` | 11.12 ns/変換 |
| 専用 `xyo_to_num_uncached(SStr*)` | 10.08 ns/変換 |

**約9.4%短縮**。チェックサムは全回同一であった。

本物のScratchスクリプト（`join`で文字列生成後に数値加算）を実行してプロセスのウォール時間（JITコンパイル込み）を交互に測定した。値は各モードの中央値。

| 反復回数 | 汎用 | 専用 | 結果 |
|---|---:|---:|---|
| 50,000 × 7種類 | 77.38 ms | 79.59 ms | 専用が約2.9%遅い |
| 250,000 × 7種類 | 201.75 ms | 199.11 ms | 専用が約1.3%速い |

**Scratch作品全体の性能改善は、計測誤差を超えるほど安定していない**。実装は既定OFFの実験機能に留める。

最適化後LLVM IRの静的な呼び出し箇所数：
- 変更前: `xyo_to_num` 13件、専用0件。
- 変更後: `xyo_to_num` 0件、専用13件。

上の専用作品のORC出力`.text`サイズ: **5,962 → 5,901バイト**（61バイト減）。

再現用ファイル:
- `tests/bench/to_num_runtime_micro.cpp` （従来のJIT条件と専用ヘルパー経路を計測する関数を追加）
- `tests/bench/to_num_cold_e2e.py`
- `build/to-num-cold-micro.log`、`build/to-num-cold-e2e.json`、`build/to-num-cold-e2e-250k.json`
- `build/to-num-cold-0.ll`、`build/to-num-cold-1.ll`
- 専用ビルドディレクトリ `build-to-num-cold`（他の作業による`build/`の再作成に干渉しない）。

Commit・Pushは未実施。

## 追加: 短い小数・指数表記の重複走査を削減

同じ作業で src/core/jsnum.cpp に短いASCII十進文字列（64文字未満）の高速経路を追加。従来はUTF-16構文検証→ASCIIコピー→std::from_charsの順序だったが、新経路はASCII確認後にfrom_charsの全入力消費を要求することで重複する構文検証走査を省略した。nan/infの誤受理を防ぐ先頭文字検査を含む。

Infinity、0x/0b/0o、符号・空白は既存の規則のまま。結果が範囲外の場合はCロケール固定strtod_lにフォールバックする。64文字以上は既存の文法検証を維持。

| 文字列 | 従来 | 変更後 |
|---|---:|---:|
| 小数 | 25.58ns | 23.17ns |
| 指数表記 | 22.07ns | 18.49ns |
| 短い整数 | 10.12ns | 9.78ns |

テストは tests/bench/to_num_parity.cpp の既存240,041文字列に、20文字種×長さ1〜5の3,368,420通りを追加。合計3,608,461種類で旧実装と新実装のIEEE754ビット表現が完全一致。

この数値は単独パーサの性能であり、Scratch作品全体の高速化率ではない。

## 最終ビルド・回帰テスト

専用ディレクトリ build-to-num-cold をRelease構成で生成し、すべてのターゲットをビルドした後にCTestを実施。既定設定で56/56成功（167.87秒）。最初の全CTestで発生した23件の「未実行」は単に一部のテスト用バイナリをまだビルドしていなかったためで、すべてのターゲットをビルドした後は解消。

最後にXYO_JIT_TO_NUM_COLD_HELPERの判定をスクリプトのJITコンパイル時に1回だけ行うよう変更し、再ビルド。XYO_JIT_TO_NUM_COLD_HELPER=1の設定でxyo-tests、JIT型変化生成、型付きSSA、if-PHI、OSR、重複算術、to-num-fastpathの関連CTest 7/7成功（47.47秒）。

字句・数値互換性：短い十進の重複検証走査を削減した新パーサは3,608,461文字列で旧実装とビット一致。差分テストの8構成（warp／非warp×Eager／HotSpot×flag ON／OFF）も通過。

追加の小数／指数パーサ高速化は既定有効。JIT冷文字列専用ヘルパーは専用作品でのウォール時間が計測ノイズ程度しか変わらないため、既定OFFを維持する。

Commit・Pushはしていない。その他の未コミット変更に触れず、作業ディレクトリで実装・テスト済み。
