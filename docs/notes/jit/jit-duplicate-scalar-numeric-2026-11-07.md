# LLVM JIT: 同一スカラーの算術変換共通化（2026-11-07）

## 狙い

`emitToNumF64`で生成されるScratch型タグ分岐や`xyo_to_num`は、LLVM最適化後にも残る場合がある。特に`x + x`のように「直接の変数レポータ」が同じスカラーを2回読む式で、後段のLLVMが安全に2回の変換を統合できない場合があった。

## 実装

`src/jit/codegen.cpp`の実験フラグ：

```sh
XYO_JIT_DUPLICATE_SCALAR_NUM=1 build/xyo run project.sb3 --jit eager
XYO_JIT_DUPLICATE_SCALAR_STATS=1 build/xyo run project.sb3 --jit eager
```

`XYO_JIT_DUPLICATE_SCALAR_NUM`は**既定OFF**。

1. 算術`operator_add`、`operator_subtract`、`operator_multiply`、`operator_divide`の両入力が、互いに直接の`data_variable`レポータかどうか確認する。
2. 両レポータの非空変数IDが一致し、そのIDで現在のTargetの**同じスカラー変数**が解決できる場合だけ、一度読み出したboxed値と`emitToNumF64`の結果を共有する。
3. 同様に`data_changevariableby`の`VALUE`が変更対象のスカラー自身を直接読む場合、既に評価済みのデルタを変更前の値として使い、`toNumber(x)+toNumber(x)`を1回の変換で生成する。
4. 数値文字列、非数値文字列、空白のみの文字列、NaN、±Infinity、-0などのScratchの型・数値変換規則は従来の`emitToNumF64`／`emitCanon`をそのまま用いる。
5. 同名だが別ID、ネストされた式、list reporter、未知の拡張、副作用を持つ式、解決できないIDには適用しない。投機的な型決め打ち・OSR境界越えのキャッシュは行わない。

この最適化は型情報の保存期間を長くするのではなく、**副作用を持たない直接スカラーレポータの重複読み出し**を同一のScratch演算内でまとめる。変数が外部から変更され得る環境も考慮し、既定OFFで評価する。

統計フラグ`XYO_JIT_DUPLICATE_SCALAR_STATS=1`による出力例：

```
[jit-duplicate-scalar] SCRIPT pairs=49 self_changes=12
```

`pairs`は二項算術の最適化候補、`self_changes`は`x += x`の候補数（いずれも静的コンパイル時の数）。

## 検証

- `tests/differential/jit_duplicate_scalar_numeric.py`：直接スカラーxを両オペランドに取る加算・減算・乗算・除算、変数自身によるchange-by、文字列から数値への変化を実施。インタプリタ、Eager JIT、HotSpot JITの結果一致を検証。
- `tests/differential/jit_type_flow_fuzz.py`にもEager／HotSpotの最適化ONを追加。固定乱数24作品×8構成の192差分比較。
- 新しいCTest`xyo-jit-duplicate-scalar-numeric`を登録。
- 最適化後LLVM IRの`call double @xyo_to_num`数を直接比較し、候補がある場合だけ実際に減ることを検証。

## LLVM O3実測（各方式5回の中央値）

`tests/bench/phi_demand_metrics.py --duplicate-scalar`、GPU/並列OFF、JIT=Eager。
測定バイナリを固定するため`build/xyo`とSHA-256が同じ`build/metric-stable/xyo-final`を別途用いた。

| SB3 | モード | xyo_to_num箇所 | LLVM .text | JITコンパイル | 総ウォール |
|---|---|---:|---:|---:|---:|
| 専用mixed.sb3 | OFF | 184 | 13,705 B | 47.77ms | 61.38ms |
| 専用mixed.sb3 | ON | 123 | 11,480 B | 43.82ms | 58.85ms |
| quicksort_4000 | OFF / ON | 46 / 46 | 28,355 B / 28,355 B | 145.09 / 143.92ms | 161.07 / 159.14ms |
| mergesort_4000 | OFF / ON | 41 / 41 | 24,307 B / 24,307 B | 114.39 / 114.34ms | 131.81 / 131.47ms |
| sieve_100000 | OFF / ON | 29 / 29 | 16,818 B / 16,818 B | 85.80 / 85.70ms | 103.36 / 103.73ms |
| micro_read_1000000 | OFF / ON | 10 / 10 | 6,086 B / 6,086 B | 31.42 / 31.16ms | 48.13 / 48.52ms |

専用作品では、最適化後呼び出し61箇所減（約33%）、機械語2,225バイト減（約16%）。O3コンパイル込みのウォール時間中央値も2.53ms短縮した。ただしこれは専用作品であり、一般的な高速化率ではない。ほかの4作品では適用機会が0〜2箇所で、機械語サイズは変化しなかった。

## LLVM O1実測（各方式5回）

| SB3 | OFF/ON xyo_to_num | OFF/ON .text | OFF/ON JITコンパイル | OFF/ON総ウォール |
|---|---|---|---|---|
| mixed.sb3 | 184 / 123 | 16,129 / 11,309 B | 49.74 / 40.26ms | 64.60 / 56.55ms |
| quicksort_4000 | 45 / 45 | 27,574 / 27,574 B | 123.92 / 123.80ms | 141.22 / 142.90ms |
| mergesort_4000 | 40 / 40 | 23,030 / 23,030 B | 97.97 / 97.92ms | 115.18 / 115.64ms |

計測ファイル：`build/jit-duplicate-scalar-stable-O3.json`（O3）、`build/jit-duplicate-scalar-O1.json`（O1）。`PermissionError`を避けるため測定時のバイナリを別の固定ファイルへコピー。コーパスでの適用候補が少ないため、今は**既定OFF**を維持する。

## 今後

- 実SB3で適用箇所を増やすには、同一変数だけでなく、純粋なレポータ木の構造的共通部分式除去を安全に行う必要がある。途中でyieldや外部副作用がある場合は必ず無効化する。
- `data_changevariableby`の複合式やリスト読みなどでは、alias・GC・副作用を証明する追加解析なしに読み出し回数を省略しない。

## 最終回帰テスト

ビルド後の完全なCTestは、最初の試行でbuild/xyo起動時に
PermissionError (Errno 13) が再発し、45件中30件が実行失敗となった。
これはスクリプトによる判定結果の不一致ではない。

build/xyoの安定したコピー build/metric-stable/xyo-final へ
**生成済みCTest設定の実行パスだけ**を一時的に差し替えて
全テストを再実行し、**45/45成功**（169.80秒）。
OSR、スケジューラ、各種JIT SSA、Binaryen、GPU、UI、
インタプリタ等も含む。終了後、生成済みCTestファイルの
実行パスは build/xyo へ復元した。Git管理のCMake設定は変更していない。

一般的なScratch作品での高速化を保証するものではないため、
本最適化は既定OFFの実験機能として維持する。
