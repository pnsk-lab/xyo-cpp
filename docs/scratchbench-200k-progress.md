# ScratchBench総合200kへの進捗：実測基準、混在型、readonly list

## 現状：200kは未達

正規の全47項目で測定した。最近の保存結果は約104〜109k、今回の安定した対照測定は約107k。
新しい最適化を段階的に足したが、**現時点の試作は約107.7kで、200kには届いていない**。

合成ベンチの4〜7倍という結果を、総合点の倍率へ外挿しない。
実際、共通CFGを単純に全面優先した最初の全体測定では退行が見つかり、その修正から始めた。

今回の対照測定で確認したNumericRegion経路は既定ONにした（`XYO_JIT_NUMERIC_REGION=0`で従来経路へ戻せる）。混在キャリア・FloatFacts・既存fast path優先のコスト選択・readonly list読みと有限値ガードも既定ONで、個別の`=0`指定で無効化できる。`--opt-all`、ベンチ本体、計測時間、重み、時計の精度は変更していない。

## 計測条件

- 正規ScratchBench `performance/project`、source hash `e8bd5be3e7345088`。
- 公式 `measureSuite`、`performance.parse`、`summarize`、`scoreRuntime` をそのまま使用。
- scoring version **2**、47/47の正の有限rateを必須とした。
- i7-12700、Linux、CPU12固定、HotSpot、`--fps 30`。dangerは使わない。
- v1.2.3 `d567ae8ab58d6480a8af855735d68dbd03de4635` の隔離ソースを基底に必要パッチだけを適用。
- executableとSB3を測定ディレクトリへコピーして固定し、SHA-256を開始・終了で確認。
- プロファイルの実行順を固定seedで並べ替え、次ブロックでは逆順。各設定3 run。
- 決定論的な39項目のchecksumを全設定・全runで一致させた。時間依存のscheduler8項目は一致対象外。
- 全体スコアは、各項目のrun中央値から公式関数で集計した。単発TOTALの最大値ではない。

[対照測定driver](<../tools/scratchbench-paired.ts>)は公式suiteを一度ビルドし、全設定で同じSB3を使う。
[目標分析tool](<../tools/scratchbench-target.ts>)は公式の重みと幾何平均を再利用する。
benchmark repositoryのSHAはruntimeのソース版ではないため、runtimeの来歴とは分離して記録した。

## 第1段階：合成ベンチの改善が全体では退行した理由

最初のv1.2.1基底の単発測定は、既定89,524、共通CFG83,833だった。
この2回は順序・ホスト負荷の影響がある（後の同一既定の再測定は104,407）。
ただし、`math_arith` の約0.28倍は大きく、実装にも説明できる原因があった。

同ケースは整数の剰余漸化式と、小数の累積値が混在する。
初期の共通CFGは一つでも非整数の演算があるとグラフ全体をf64化し、
既存の高速な整数modキャリアまで失っていた。

### 修正したもの

1. **per-SSA-valueの混在キャリア**
   [mixed adapter](<../src/jit/numeric_region_mixed_ssa.inc>)で、整数証明された値・slotだけをi32/i64にする。
   他はf64を維持する。Phiの入力変換は実際のpredecessor末尾へ挿入する。
2. **実ガードdomainに基づく浮動事実**
   [float facts](<../src/ir/numeric_region_float_facts.hpp>)で有限・非ゼロ・非NaNを証明。
   Float入力へ仮の整数範囲を適用せず、証明できる演算だけORDガードを省く。
   範囲証明された整数にはnsw、証明された浮動演算には局所nnanを伝える。
   nsz、reassoc、ninfや包括的fast-mathは有効化していない。
3. **既存の強い経路を失わないコスト選択**
   実在する既存affine経路を保持し、[共通CFGのpostdom構造](<../src/ir/numeric_region_cost.hpp>)から
   入れ子分岐には広い整数表現を選ぶ。ベンチIDによる判定はない。
   flatな比較のi32化は維持する。既存conditional parser自体は変更していない。

### 対照測定の推移

各行は同一バイナリ内の対照。異なる行の絶対値の小差を改善とは扱わない。ここに示した約107.7kのreadonly-list+finite候補を現在の既定として採用したが、実プロジェクトでは適格性ガードと通常経路へのfallbackが常に残る。

| バッチ | 既定TOTAL | 候補TOTAL | 結論 |
|---|---:|---:|---|
| 混在キャリア | 107,102 | 106,584 | math退行は解消したが、loopsの既存経路に劣る |
| 浮動事実ガード | 107,102 | 106,703 | 局所改善だけでは総合を上げない |
| nsw/nnan事実伝達 | 107,073 | 106,740 | 単独では総合差が小さい |
| コスト選択併用 | 107,115 | **107,771** | 約0.61%増。loopの退行をほぼ回避 |

`math_arith` は約247,740→276,410 ops/ms、`vars_setchange` は約11.62M→12.54Mになった。
一方、既存のaffine/conditionalで既に速かったrepeat/nested/ifelseを一般経路へ強制すると遅かった。
一般化した経路を常に優先すればよい、という結果ではなかった。

[混在型バッチ](<../scratch-bench/results/target-200k-20261109/mixed/summary.json>)、
[事実属性バッチ](<../scratch-bench/results/target-200k-20261109/proof/summary.json>)、
[コスト選択バッチ](<../scratch-bench/results/target-200k-20261109/hybrid/summary.json>)。

## 第2段階：readonly listを数値区間へ入れる

前回はリストアクセスが一つ入るだけで数値CFG全体を拒否していた。
今回、[共通IR](<../src/ir/numeric_region.hpp>)に `ListRead` と独立したlist bindingを追加し、
[frontend](<../src/analysis/numeric_region_lower.hpp>)とmixed adapterへ接続した。

### 安全性の条件

- リストは読み取り専用。append/replace/delete等の書き込み、待機、opaqueな処理は引き続き拒否する。
- チャンクごとにSListの存在、`extra==null` の連続配置、len/itemsを確認する。
  非連続・adaptiveの外部形式は通常経路へ戻る。
- 数値indexの境界を確認してから変換・GEP・loadする。正のfractional indexもScratchの切り捨てに合わせる。
- list全体のnumericOnlyフラグは信用せず、実際の要素を確認する。
- exact publication/Phiへ流れる値はNumberタグを要求し、String/Boolタグを勝手に消さない。
- 算術だけなら既存ToNum、数値比較/数値indexだけなら既存の数値比較互換helperを使える。
  空白・不正文字列の比較は元の反復へ戻す。特殊indexのRNGはfast側で実行しない。
- 部分反復の書き込みは公開せず、失敗した反復を元のJITで実行する。
- 16未満の短い定数repeatは入口ガードを償却しにくいため通常経路を保持する。

有限要素のオプションは、投影後の値に実際のisfiniteガードを置く。
その継続上だけ、加算等の非NaN事実を使う。Infinityを黙って有限と扱うわけではない。

### 全体測定

| プロファイル | TOTAL |
|---|---:|
| 既定 | 106,707 |
| 混在型・事実・コスト選択 | 107,199 |
| readonly list | 107,537 |
| readonly list＋有限要素ガード | **107,684** |

最後の設定は同じバッチの既定比で約**0.92%増**。

| 項目 | 既定 ops/ms | 最終候補 ops/ms | 観測 |
|---|---:|---:|---|
| list_read | 321,808 | 414,735 | 約28.9%増 |
| algo_matmul | 290,993 | 310,066 | 約6.6%増 |
| algo_sieve | 201,109 | 203,046 | 約1%増、小差 |
| app_life | 15,777 | 15,748 | 実質変わらない |
| app_raycast | 31,287 | 31,224 | 実質変わらない |

[全体・カテゴリ集計](<../scratch-bench/results/target-200k-20261109/lists/summary.json>)、
[全runとchecksum](<../scratch-bench/results/target-200k-20261109/lists/runs.json>)。

Lifeの短い内側ループ、Raycastのuntil、Projectionのlist writeと入れ子、ASTの文字列/再帰等は、
この拡張では主たるホットパスを取り込めない。リスト読みを追加するだけでapps全体が数倍になるわけではない。

## 互換性検証

- scalar/混在型/PHI/NaN/負のゼロ：1,017フィクスチャ、2,034比較。
- readonly list：229フィクスチャ、3設定で687比較。
- ListRead・FloatFacts・CFGコストのstandalone C++ smokes。
  FloatFactsはNaN/Inf/overflow/underflow等500個のIEEE演算サンプルを含む。
- scoring tool：8 test、42 assertion。
- 既存execute：既定・list有限要素・同HotSpotの3設定で各116/116、assert 338/338。
- readonly list AOT：sum/copy/compare/nonfiniteの4ケースで生成・ロード・実行結果が一致。
- 実native yield：BASE/LIST/FINITE×flat/deque/adaptiveの9設定。
  各設定16回の実中断・1,025反復で状態を独立計算と照合。
  clear、len変更、再確保、storageEpoch変更、adaptiveの非連続化、
  numericOnlyを更新しないraw String書き込みを行い、タグ・綴り・値を確認した。

[scalar差分](<../scratch-bench/results/target-200k-20261109/scalar-parity.json>)、
[list差分](<../scratch-bench/results/target-200k-20261109/list-parity.json>)、
[AOT結果](<../scratch-bench/results/target-200k-20261109/list-aot.json>)、
[実中断ログ](<../scratch-bench/results/target-200k-20261109/list-checkpoint.log>)。

検証は隔離したv1.2.3基底＋パッチで実施。現在の共有dirtyツリー全体の統合ビルドや、
portable CallFrame/OSR/実行中編集・bindingの再解決との統合は未検証。

## なぜ200kにはまだ遠いか

TOTALはカテゴリscoreの重み付き幾何平均で、重みの合計は9。
loops/variables/math/strings/listsが各1、algorithmsとschedulerが1.25、applicationsが1.5。
項目の低い絶対値は、時間の支配率を表すものではない。カテゴリごとに単位も異なる。

約107.7kから200kには、さらに**約1.86倍のTOTAL**が必要。
例えばapplicationsカテゴリだけ2倍でもTOTALは約12.2%増で、200kには届かない。
1つのmath項目だけ2倍にしてもTOTALの増加は約0.86%にとどまる。

寄与は `effective_weight * ln(candidate_rate / baseline_rate)` で評価する。
正規score関数を呼ぶ[分析toolとテスト](<../tools/scratchbench-target.test.ts>)を追加し、
raw scoreの差を足し算して改善量を見積もらないようにした。

次の大きな課題は、readonly loadだけでなく、**list write/未知処理の前後を正確に継続できる区間分割**、
そしてuntil/while・文字列dispatch・再帰を含む実アプリの適用範囲である。
型タグや副作用を省略してスコアだけ上げる方法は採らない。

## 再実行

```sh
bun tools/scratchbench-paired.ts \
  --binary build/scratchbench-200k-v123/native/xyo \
  --out build/scratchbench-200k-repeat --blocks 3 \
  --profiles default,hybrid,lists,list_finite --cpu 12 \
  --source-revision v1.2.3-plus-saved-patch

bun tools/scratchbench-target.ts \
  build/scratchbench-200k-repeat/default/performance-xyo-cpp.json \
  --compare build/scratchbench-200k-repeat/list_finite/performance-xyo-cpp.json \
  --target 200000 --out build/scratchbench-200k-repeat/target-analysis.json
```

[最新パッチ](<../scratch-bench/results/target-200k-20261109/latest-source.patch>)と各段階のパッチ・
実行条件・公式JSON・凍結SB3を保存した。結果ディレクトリは新規指定が必要。
