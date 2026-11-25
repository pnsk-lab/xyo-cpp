# algo_sort：順序付き浮動小数点ガードによる高速化（2026-11-05）

## 概要

ScratchBench の algo_sort は、100要素のリストをバブルソートする。内側のループで2件の data_itemoflist を取得し、operator_gt で比較して要素を交換する。

本件では特定のソートアルゴリズムを認識することなく、リストから取り出した2値を直接比較する汎用JIT経路を改善した。

### 仕組み

Scratch Value は64ビットNaN-boxed形式。文字列、真偽値、空文字、色、undefined、数値NaNは、IEEE-754上ではNaNに対応するビット列を持つ。

そのため、2つの値を f64 として fcmp ord で比較し、両方が非NaNなら同じオペランドを使って fcmp ogt / fcmp olt で直接判定する。どちらかがNaNの場合は従来のScratch比較ヘルパに戻る。

- 常に実際の値を検査するため、他スクリプトによるリストの型変更にも対応する
- ±0、正負の有限値、±Infinityは直接比較できる
- 非数値、数値NaN、特殊な文字列比較は従来ヘルパが処理する
- 通常は直接 data_itemoflist どうしの比較だけに適用する
- ソート固有の関数・専用パターンを導入していない

実装：src/jit/codegen.cpp の Codegen::emitCmp。

既定ON。環境変数で比較を切り替えられる。

    XYO_JIT_PLAIN_NUMBER_CMP=0   # 専用経路をOFF
    XYO_JIT_FP_ORDERED_CMP=0     # 旧整数ビット型ガードを使用
    XYO_JIT_FP_ORDERED_CMP=1     # 浮動小数点の順序付きガード

## 最終ベンチマーク（専用経路 OFF vs 新しい既定経路 ON）

固定コピーした同一xyoバイナリを使用、CPU12に固定。各ケースでOFF/ONをランダム順に16ペア実行。元のScratchBenchプロジェクトを使用。結果は全ペアでチェックサム一致。値はScratchBench内部計測のops/msであり、起動・JITコンパイル時間は含まない。

| テスト | OFF ops/ms中央値 | ON ops/ms中央値 | ペア速度比中央値 |
| --- | ---: | ---: | ---: |
| algo_sort | 296,114 | 351,462 | **1.1882倍** |
| math_compare | 239,619 | 240,137 | 1.0043倍 |
| math_strcompare | 26,462 | 26,493 | 0.9999倍 |
| list_read | 301,631 | 302,759 | 1.0089倍 |
| algo_matmul | 279,824 | 279,837 | 0.9998倍 |

algo_sort は16ペア全てで改善。速度比は1.1788〜1.1973倍、チェックサム150504一致。生データは build/algo-sort-eval/ordered-final-vs-off.json。測定対象バイナリのSHA256は 587c18c86444df484e2d149747c1c0b0ba27b4ba7f33a9268b7588b32fbea4ec。

再計測：

    python3 tests/bench/algos/bench_plain_numeric_cmp.py --binary build/xyo --cases algo_sort math_compare math_strcompare list_read algo_matmul --trials 16 --out build/algo-sort-eval/ordered-final-vs-off.json

## 旧整数型ガードとの比較

同じバイナリのXYO_JIT_FP_ORDERED_CMP=0/1を切り替えた14ペアのA/Bでは、algo_sortの中央値は1.1236倍で全14ペア改善。ほかの代表ケースはほぼ横ばい。

生データ：build/algo-sort-eval/fpordered.json。

## 試したが採用しなかったもの

- リストnumericOnlyフラグを利用する追加の読み取りガード：algo_sort 0.996倍（6ペア）
- リストへの数値書き込み時、フラグ更新を条件分岐で省く方式：algo_sort 0.972倍（8ペア）
- 数値と確定したリスト書き込みでフラグ更新を完全に省く方式：algo_sort 0.9719倍（12ペア全て悪化）
- 整数の絶対値ビット列2つをORして1回の整数比較でガードする方式：algo_sort 0.9960倍（20ペア）

いずれもソート性能を改善しなかったので実験用コードは削除した。旧整数ビット型ガードは診断用スイッチで残した。

## 正当性

- tests/differential/plain_numeric_cmp.py：混在型リスト、NaN(0/0)、±Infinity、負のゼロ、真偽値、空白、数値文字列、Unicode文字列、途中の書き換えを対象。インタプリタ・Eager JIT・HotSpot、専用経路OFF/整数ガード/orderedガードで一致
- tests/differential/plain_numeric_sort_algos.py：バブルソート100件、クイックソート500件、マージソート500件を3実行経路×3モードの合計27ケースで検証。全要素・昇順判定・チェックサムが一致
- ScratchBench algo_sortの85スクリプトをAOTコンパイルし、OFF/orderedの両方で実行成功。チェックサム150504一致
- 最終変更後のCTestは18/18成功（今回変更していない長時間のイベントスケジューラ回帰を除外）。その直前の全CTestは19/19成功

本結果はソート中心のマイクロベンチマークであり、ScratchBench全体の改善率ではない。コードと結果はローカルに保存し、commit/pushは実施していない。
