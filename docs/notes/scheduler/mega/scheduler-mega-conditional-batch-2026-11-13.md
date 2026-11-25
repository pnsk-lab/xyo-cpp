# Mega Scheduler β — LLVM数値条件分岐（if / if-else）

2026-11-13 / opt-in・既定OFF・未コミット

## 対応するScratchソース

`event_whenflagclicked`または`event_whenbroadcastreceived`から、
`control_if`/`control_if_else`、またはその文を唯一の本体とする
固定非負整数回の`control_repeat`を証明し、最大8本のThreadを1つのLLVM関数で処理する。

- `operator_gt`、`operator_lt`、`operator_equals`
- 片方のオペランドが`data_variable`、もう片方が有限な数値リテラル
- `変数 > 定数`、`定数 < 変数`の両配置を維持。等号も左右に対応
- 真の枝では固定値の`data_changevariableby`をちょうど1命令
- 偽の枝は`control_if_else`なら固定値の加算1回、`control_if`なら完全なno-op
- 命令の`next`連鎖、入れ子の分岐、変数を引数とする比較演算、
  汎用procedure return suffix、リスト/文字列処理、GC、waitは対象外

## 実装

`src/jit/mega_yield_source.hpp`に限定ソース証明`numericIfConditional`を追加。
Sourceは`hasConditional`、`hasElseBranch`、`comparison`、
`conditionLiteral`、`conditionLiteralLeft`、真偽枝の変数IDを保持。

`src/jit/backend_llvm.cpp`は、`MultiPcBatchEntry`の`switch(pc)`内で
浮動小数点`fcmp`を生成し、選んだ枝のみ`load → fadd → store`を実行。
`control_if`の偽枝にはストアも通知も生成しない。
`pc`と`remaining`はThread所有で、各scheduler passに各Threadが
最大1回のrepeat本体を実行する。

非数値の条件変数は`src/engine/sequencer.cpp`側で拒否し、
従来のMega JIT `yieldEntry`を使う。こちらはScratch比較ヘルパー
`xyo_gt` / `xyo_lt` / `xyo_eq`を呼び出すため、文字列や真偽値に
`cast::compare`の意味が適用される。元の比較順序も保存する。

`src/jit/jit.hpp`、`src/jit/jit.cpp`では、
各sourceの真の書込変数、任意の偽枝書込変数、条件変数を個別に追跡。
`control_if`のバッチbindingは2本、`control_if_else`は3本。
C++側は、すべてが存在し、`Variable::kScalarType`で、有限IEEE754数値であることを
検証し、型やソースrevisionが変化すると通常JITへ戻す。

待機通知レジストリ、renderer、並列キュー、OSR、実際の待機Threadなどの
従来の一括実行ガードを維持。

## フラグ

`XYO_JIT_MEGA_IF_ELSE_BATCH=1`を、既存の次の条件と併用する。

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_BCAST_RECEIVERS=1 \
XYO_JIT_MEGA_DIRECT_SCALAR=1 \
XYO_JIT_MEGA_SCALAR_BATCH=1 \
XYO_JIT_MEGA_MULTI_PC_BATCH=1 \
XYO_JIT_MEGA_IF_ELSE_BATCH=1 \
./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 0
```

任意で`XYO_JIT_MEGA_BULK_STATS=1`を指定すると、
実行したLLVMバッチ回数と対象Thread数をstderrに集計表示できる。

## 回帰

`tests/unit/test_scheduler.cpp`の
`testMegaIfElseBatch`、`testMegaIfNoElseBatch`により次を確認。

- 複数Threadが同じ数値条件を逐次変更して条件分岐が切り替わる
- 変数と数値定数の左右両方の並び、gt/lt/equals
- repeat(0)、repeat(2〜4)と単発スクリプトの混在
- no-elseの偽枝は変数を一切変更せず、残反復カウンタを正しく進める
- 数値文字列、非数値文字列による通常JITへの戻り
- 同じIDの変数を削除・再作成しても正しい再束縛とコード再利用

if/elseの左右逆配置まで：Scheduler 551/551 PASS（全テスト群PASS）。
if/elseとifの集中テスト：26/26 PASS。
ifのみの条件分岐も含む全体テストはScheduler **562/562件PASS**、その他のテスト群も全件PASS。
ログ：build-scheduler-rewrite-20261111/mega-ifelse/tests-final-full-on.out。

最終ソースで、同じCLI・テスト実行ファイルを使った3条件の全体回帰：

| 条件 | Scheduler | 終了コード |
|---|---:|---:|
| すべてのMega数値フラグ＋if/else・if ON | **562/562 PASS** | 0 |
| 条件分岐フラグOFF（従来Mega） | **536/536 PASS** | 0 |
| 条件分岐要求ON＋変数待機通知レジストリON | **509/509 PASS** | 0 |

OFF/Wakeログ：tests-final-feature-off.out、tests-final-wait-registry.out（上記mega-ifelseディレクトリ）。

## 合成SB3実測

同じCLI、同じSB3、CPU16固定、交互ペア。8本の緑旗スクリプトが
それぞれ40,000回、数値変数を閾値0と比較。全変数の最終値が
通常MegaとLLVM一括実行で一致することを検査。
プロセス全体の壁時計時間（ms、小さいほど高速）。

| ケース | OFF中央値 | ON中央値 | 対応ペア速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| if/else、v > 0、6組 | 448.78 | 378.25 | 1.2037倍 | 5/6 |
| if/else、0 < v、12組・単独測定 | 475.53 | 394.54 | 1.1276倍 | 10/12 |
| if、0 < v、8組・単独測定 | 436.69 | 353.21 | 1.1986倍 | 8/8 |

元データ：
- `build-scheduler-rewrite-20261111/mega-ifelse/bench-40000x8/results.json`
- `build-scheduler-rewrite-20261111/mega-ifelse/bench-reversed-40000x8-isolated/results.json`
- `build-scheduler-rewrite-20261111/mega-ifelse/bench-if-only-40000x8/results.json`

再現コード：
- `tests/bench/algos/build_mega_ifelse_fixture.py`（`--reverse`、`--no-else`を指定可）
- `tests/bench/algos/bench_mega_ifelse.py`

合成テストの結果から、実プロジェクトやScratchBench全体の効果は推定しない。
他エージェントの未コミット変更はreset/commit/pushしない。
