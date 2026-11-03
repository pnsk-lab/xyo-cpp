# JITのwarp中断点と数値SSAの整合性（2026-11-03）

## 変更したこと

### 1. 危険な剰余・等値の融合を撤回

`src/jit/codegen.cpp`から、`(a % b) == 0`の時だけ`emitModEqZero`へ置き換えるコードを削除。これはwarp手続きだけに限定しても、ScratchBenchを全体実行するとSIGSEGV（終了コード139）を発生させた。一方、`XYO_JIT_DISABLE_FUSED_MOD_EQ=1`で無効化すると完走した。コードと特化条件のどちらに根本原因があるかは未確定なので、未検証の融合を残したまま通常経路に使わない。

通常の`operator_mod`および`operator_equals`の汎用数値演算と型ガードは保持。剰余の検証用ケースは`tests/bench/algos/mod_eq_zero_probe.py`へ残した。

### 2. タイマーポーリングで数値SSAを不要に壊さない

以前のwarpループ処理:

```text
64反復 -> spill-promoted -> xyo_loop_yield (大抵は中断しない)
                               -> reload-promoted (型・値の再確認)
```

変更後:

```text
64反復 -> xyo_warp_yield_due (時計・dangerMode判定のみ)
                 |
                 +-- false -> 型付きSSAを保持してループへ
                 |
                 +-- true  -> spill-promoted -> xyo_yield -> reload-promoted
```

`xyo_warp_yield_due`は`src/jit/rt.cpp`で宣言・実装し、`src/jit/jit.cpp`のJITシンボル表へ登録。専用warp経路でのみ使用する。非warpループは引き続き毎反復yieldする。昇格変数やリストがないwarpループは従来の`xyo_loop_yield`を維持する。

`isStuck(c)`は`runtime->dangerMode`と`sequencer->timer.timeElapsed()`を読むだけで、ファイバ切替・Scratch変数書込み・GC実行をしない。したがってfalseの場合は数値の影（bits, double, numeric compare predicate）およびリストのlen/itemsを再読込する必要がない。trueなら**yield前に必ずspill**し、再開後には**必ずreload**することで、他スクリプトからの変更を観測する。

## 正当性確認

- CTest 6/6 pass: `build/structural-warp-test-ctest.log`
- ScratchBench parity 2122/2123: `build/structural-parity.log`。既存の`Motion: If On Edge Bounce` 15/16のみ
- 47ワークロードのScratchBench本体は`DONE`まで到達し、終了コード0: `build/structural-full-run.out`。融合ありはSIGSEGVで完走できなかった
- 39個の非schedulerワークロードは、融合なしの旧バイナリと全て同一チェックサム。時間依存のscheduler項目はこの照合から除外
- `tests/unit/test_danger.cpp`へ、`xyo_warp_yield_due`のタイマー判定・danger判定、およびwarpの実yield中に別スクリプト相当の変数書き換えを行う回帰ケースを追加。JITとインタプリタの双方で成功
- 実JIT最適化後IR `build/structural-primes-ir.ll`は、`xyo_warp_yield_due`の結果が0の場合に直接backedgeへ入り、`yield.handoff`のspill/reloadブロックを通らない

なお、シンボル表やAOT実装との整合性についてはCTest内で実行する既存のAOTテストを利用。新たなOSR、型推論そのものの拡張、世代別GC化は今回含めない。大量のワークロードの速度比較は、ビルドと同時に動いたログを採用しない（CPU干渉があるため）。

## 次に改善するべき構造

1. ループ内の変数型・値域をAST断片ごとではなくSSAのデータフローとして表現し、`knownNumD / knownNumOk`の寿命を基本ブロック境界と中断点の契約に一致させる。
2. 型が変わったときのdeopt / 汎用バージョンへの移行をループ入口でまとめる。ただしScratchの文字列比較、NaN、-0、他スクリプトの変更を保持する。
3. 長い文字列連結の確保・GCは別の根本問題。小型ノードと割当方式の変更は専用の安全な段階で実装する。
