# ScratchBench broadcast-and-wait 継続点のソース監査（2026-11-14）

## 概要

Mega Scheduler β の「固定repeat + 固定broadcast + 後続scalar」
フラグ（単独sender、fast latch、STATUS_YIELD正規化）は、
既存の小規模合成テストでは通常Fiberと同等以上になったが、
**実ScratchBenchの sched_pingpong/fanwait/restartにはまだ送信者継続PCが適用されない**。

実行中のLLVMコードを無理に再開させるのではなく、まず実際の
Scratchプロジェクトの lexical call chain と control ancestry を
正確に可視化する診断ツールを追加した。

- `tests/bench/algos/analyze_broadcast_continuations.py`
- `tests/bench/algos/test_analyze_broadcast_continuations.py` （4/4 PASS）
- 出力: `build-scheduler-rewrite-20261111/bcast-reuse-20261114/real-broadcast-source-report.json`

## 実ScratchBenchから静的に確認した実行経路

元の `sched-8.sb3` の Stage で以下を確認した。

| ケース | 緑旗からの手続き経路 | 送信位置の祖先ループ | ソース検証上の不足 |
| --- | --- | --- | --- |
| sched_pingpong | event_whenflagclicked → suite_sched → sched_pingpong | control_repeat_until → control_repeat | 動的停止条件、時計レポーター、2段ネスト、手続き復帰 |
| sched_fanwait | event_whenflagclicked → suite_sched → sched_fanwait | control_repeat_until → control_repeat | 上記と同様。受信側は101個のclone |
| sched_restart | event_whenflagclicked → suite_sched → sched_restart | control_repeat_until → control_repeat | 上記に加え、broadcastの直後にも後続命令がある |

元のScratchBench自体には一切変更していない。

### 継続PCを構成する実Block ID

ソース経路の追跡を拡張し、呼び出し元の**正確なreturn block ID**
と、ループbody/after-loop block IDも診断に含めた。
これらは **SourceレベルのPC候補**であり、
既存のLLVMにそのまま安全に復帰できるという証明ではない。

| ケース | 手続きcall → return | 内側until body → after | 外側repeat body → after |
| --- | --- | --- | --- |
| pingpong | `~N5→~N6`, `~WI→~WJ` | `~Tc→~Td` | `~TT→~Th` |
| fanwait | `~N5→~N6`, `~WO→~WP` | `~Vd→~Ve` | `~VU→~Vi` |
| restart | `~N5→~N6`, `~WJ→~WK` | `~Tw→~Ty` | `~Tn→~U3` |

この再開点の抽出は実SB3から行い、先行の試験4/4件に加えて、
明示的なreturn PCとloop body/afterのIDを照合する。


`sched_pingpong` のループは、外側repeat(5)と、内側の
`operator_not` → `operator_lt` → `sensing_dayssince2000`
に依存する条件付き繰り返しから構成される。
現在の `mega_yield::prove` は固定数値repeatと限定的な
引数なし手続き呼び出ししか証明できない。
既存のSource::Kind::BroadcastRepeatを名乗らせるだけでは、
毎回評価する停止条件、ループ前後の測定変数操作、
実行済み手続きreturn PCを再現できず誤実行になる。

## 推奨する次のLLVM継続設計

ソースの静的ダイジェストとrevを根拠に、次の実行点を
Thread所有の継続状態として定義する必要がある。

1. `Entry`: 未実行緑旗／自作ブロック呼び出しチェーン
2. `UntilCondition`: レポーターを**再評価**する境界（時刻・変数の変化を観測）
3. `BroadcastStart`: 固定または動的メッセージをちょうど1回配送
4. `JoinWait`: 実際に選択された受信Thread*をGC rootとして保持
5. `AfterJoin`: 完了後のみ、送信Threadの次のScratch効果へ進む
6. `AfterBody`: 内側until条件へ復帰（タイマーを再評価）
7. `AfterLoop` / `Return`: 外側固定repeatラッチとネスト手続き戻り先

さらに、戻り先を含む命令PCだけでは不十分で、
動的条件の一時値・Scratch引数・評価済みreporter・
GC-managed文字列参照の保存/再計算可否、variable binding stamp、
broadcast restart identity、source revision の検証が必須。

特に同一フレーム内で多数の送信・受信を処理するため、
`STATUS_YIELD` を通常のJIT Fiberと同じくパス末で
RUNNINGに戻し、`STATUS_YIELD_TICK`はそのまま保つ。
この正規化は前ターンで追加済み。

この段階では一般的な動的untilを自動的にLLVM Megaへ載せない。
任意の時計・変数・手続き条件に既存の固定repeat用ラッチを
流用しないことを安全上の制約とする。

## 棄却した「join一覧再利用」試験

Fiber経路を維持しながらメモリ確保を減らすため、
呼出しごとに作成するjoin vectorをThreadへ所有させる
`XYO_BCAST_JOIN_REUSE`経路を試験した。
JitCtx ABIはtrivially-copyableを維持し、
再利用用vectorをThreadに置き、完了後にclearする設計。

- 集中回帰: OFF 13/13、ON 14/14 PASS
- CPU16固定、Eager、元のScratchBench、16組の交互測定：

| ケース | OFF中央値 ops/ms | ON中央値 ops/ms | 対応ペア幾何平均 | ON勝 |
| --- | ---: | ---: | ---: | ---: |
| sched_pingpong | 431.2195 | 431.6665 | 0.99670x | 8/16 |
| sched_fanwait | 878.8510 | 875.2075 | 0.99445x | 6/16 |
| sched_restart | 1369.4165 | 1360.3285 | 1.00006x | 8/16 |

実ベンチで改善なし。別の合成負荷でも：

| 条件 | OFF中央値 | ON中央値 | 対応ペア比 | ON勝 |
| --- | ---: | ---: | ---: | ---: |
| 100k送信、受信repeat1 | 256.336ms | 255.975ms | 1.01526x | 6/12 |
| 100k送信、受信repeat8 | 419.594ms | 418.649ms | 1.00451x | 8/12 |

大きな改善がないうえ、Threadごとの追加vectorはメモリと
保守コストを増やす。この試験コードは**すべて取り消した**。
共有作業ツリーの他エージェント差分はreset/stashせずそのまま。

生データ：
- `build-scheduler-rewrite-20261111/bcast-reuse-20261114/bench-real-eager-16/results.json`
- `build-scheduler-rewrite-20261111/bcast-reuse-20261114/bench-synthetic-r1-12/results.json`
- `build-scheduler-rewrite-20261111/bcast-reuse-20261114/bench-synthetic-r8-12/results.json`

**結論:** 少数のjoinポインタを運ぶvectorのmallocが主要なボトルネックという仮説は実測で支持されない。実負荷改善には受信Threadの開始・終了と、呼び出し元側の一般継続状態を統合する必要がある。

診断スクリプトはソース調査用であり、Scratch互換の安全性を
証明する認定器ではない。未コミットのまま保存。
