# Epoch-Guarded Loop (実験: 2026-11-05)

## 適用範囲

xyo-cpp の既存ループ変数昇格 (src/jit/codegen.cpp) を改良したもの。

- **既定ONの正当な最適化**: ループ内で一度も書き込まない変数は、yield前・ループ終了時の不要な書き戻しを行わない。再開後は従来どおり実メモリから値を読む。
- **既定OFFの実験的ガード**: XYO_JIT_EPOCH_GUARD=1 を指定すると、初期値が文字列で、ループ内では読み取り専用で、数値シャドウを持つ変数に限り、再開後の数値変換・比較可否判定をキャッシュできる。
- この実装は Variable 全書込箇所に世代番号を置くものではない。すべての値変更経路が共通の世代番号を更新するという仮定を導入せず、常に実メモリのValue bitsを読み直してガードする。
- 文字列ポインタのABAに備え、yield前にGc::collectCountを記録。再開後に同じポインタ値が観測された場合、GC世代が一致したときだけ以前の数値シャドウを使用する。GCが走ったら常に通常経路で再計算する。
- 即値(double/boolean/NaN等)は同一ビットなら同じ値。違う場合は従来どおり変換する。
- 文字列は不変のScratch SStr値であることが前提。GCは停止世界方式であり、ヘルパはJIT実行中の非GC区間でのみ世代を観測する。
- ループを跨ぐ型推測の無条件な固定、opaque手続き内部の昇格、ループの意味論的書き換えは実施していない。

### ガード

~~~text
beginLoopPromotion:
  promotedVar <- Variable::value
  dslot <- Scratch.toNumber(promotedVar)   // 初期化
loop body:
  既存の値を使ったネイティブ算術
  before yield:
    if written: Variable::value <- promotedVar
    savedGcEpoch <- Gc.collectCount()     // 実験ガード対象がある場合のみ
  after yield:
    observed <- volatile load Variable::value
    if observed.bits == promotedVar.bits and
       (not heapString(observed) or Gc.collectCount() == savedGcEpoch):
      keep existing dslot + comparison fact
    else:
      promotedVar <- observed
      dslot <- Scratch.toNumber(observed)
      comparison fact <- Scratch-compatible check
loop exit:
  if written: Variable::value <- promotedVar
~~~

**注**: 実装はJIT生成時にターゲットの初期値を読むため、文字列になり得るが初期値が数値だった変数はガード候補から外れる。これは効果の範囲に関する保守的制約で、正当性には影響しない。

## コマンド

~~~sh
cmake --build build --target xyo xyo-tests -j4
ctest --test-dir build --output-on-failure
python3 tests/differential/epoch_guard.py
XYO_JIT_EPOCH_GUARD=1 build/xyo test refs/scratch-vm/test/fixtures/execute --jit eager
~~~

実験ON/OFFは環境変数で選択。既定はOFF。

## 差分検証

- 6つのScratch SB3フィクスチャ (numeric, string, whitespace, nonnumeric, negative_zero, other_script) を新規追加。
- 同じSB3でガードON/OFFの最終変数値一致を検証。
- 他スクリプトによる変更: 値7のループに途中で文字列"9"→数値3を代入。ガードON/OFFで最終結果一致 (sum=2404、800反復)。
- CTestに xyo-jit-epoch-guard を登録した。
- 新テストを含む CTest **13/13成功**。
- Scratch VM execute フィクスチャ **116/116**、assert **338/338** 成功（ガードON）。
- AOTオブジェクト生成と外部オブジェクトを使った実行成功（ガードON、文字列ケースの最終値一致）。

## 性能（前述のパターンを抽出した新規SB3）

CPU 0固定、同じReleaseバイナリ、別プロセスをランダム順で各8回。20,000反復、ガードOFF/ON、プロセス起動・JIT時間を**含む** wall-clock 中央値。最終値はすべてON/OFF一致。

| ケース | OFF | ON | OFF/ON (1超なら改善) |
| --- | ---: | ---: | ---: |
| numeric | 27.659ms | 27.746ms | 0.997 |
| string | 27.323ms | 28.417ms | 0.961 |
| whitespace | 27.262ms | 28.606ms | 0.953 |
| other_script | 32.072ms | 33.081ms | 0.970 |

**今回のガードは改善を示していない。実際、文字列では遅い。既定OFFが妥当。** 測定はコーパス中の代表性能ではなく、フィクスチャ用の対照実験である。

## 今後の方針

本格的なEpoch-Guarded Loopを作るには、インタプリタ/JIT/GPU/モニタ/スライダ/一時文字列等の**すべてのscalar write**をwrite barrierに集約し、Variable単位の更新世代とJITのdeopt境界を整える必要がある。

その場合、yield再開時に値そのものを読むことなく「更新なし」を証明できる。ただし書き込み側に世代番号更新の追加コストが発生する。負荷の高い通常JITの性能とバランスを取って設計・測定すること。

今回の変更はコミット・push・tag操作を行っていない。
