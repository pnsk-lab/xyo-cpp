# Scratchコーパスに基づくQuickening候補調査（2026-11-05）

## 入力と取得の仕組み

入力: https://huggingface.co/datasets/KangpingJP/scratch-ff/tree/main の projects.csv。

- CSVサイズ: 12,834,779,133 bytes（12.83GB）。
- 列: username, project_id, project_title, project_url, project_public_date。
- ダウンロードせず、HTTP Rangeで64箇所×64KiBを分散読み出し。36,201行を観測。
- 行数は約1.11億と推定（**全件計数ではない**。バイト長に基づく標本なので完全な無作為行抽出でもない）。
- 2020〜2026年から20作品ずつ選択し、合計140 ID（seed=1618）。
- 公開作品用のScratch APIメタデータで公開状態を確認し、同メタデータにあるproject_tokenで **project.jsonだけ** を取得。画像・音声は取得しない。
- 140件中134件を取得、6件がHTTP 404。134件中24件は構造的類似により重複除外、110件を集計。
- この構造的類似判定は、リテラル値やランダムなブロックIDを捨てた近似的なフィンガープリント。意味論的な同一性の証明ではない。
- 110件の集計対象ブロックは51,239個。大きい2作品だけで28,213個（約55.1%）。
- 取得物と解析JSONはgitignoredな .scratch/quickening-corpus/ に保存。追跡対象としてアップロードしない。

## 再現

リポジトリ直下で:

~~~sh
bun test ./tests/bench/algos/quickening_corpus.test.ts

# CSVを全量DLせず、全体に分散したレンジを抽出
bun tools/quickening_corpus.ts sample \
  --windows 64 --window-bytes 65536 --count 140 --min-year 2020 --seed 1618

# ネットワークアクセスが必要。取得は一度に対象を絞り、節度あるレートで実行する
bun tools/quickening_corpus.ts fetch --limit 140 --delay-ms 300

# オフラインで静的集計
bun tools/quickening_corpus.ts analyze
~~~

sample/fetchだけがネットワークを使う。大量アクセスの前に取得元の利用条件を確認すること。

成果物:

- .scratch/quickening-corpus/selected.jsonl — 選択ID・公開日など
- .scratch/quickening-corpus/sample_report.json — HTTP Rangeの観測数
- .scratch/quickening-corpus/projects/*.json — 公開作品のproject.json
- .scratch/quickening-corpus/fetch_report.json — 取得成否
- .scratch/quickening-corpus/analysis.json — 命令頻度・2〜5命令連鎖・入力依存構造・作品単位の分布

## 初回の集計結果（静的出現回数）

| パターン | 生の出現数 | 出現作品数（110件中） |
| --- | ---: | ---: |
| data_setvariableto > data_setvariableto | 784 | 24 |
| control_if <- operator_equals | 1001 | 25 |
| control_if_else <- operator_equals | 938 | 18 |
| control_if <- operator_or | — | 19 |
| data_setvariableto <- operator_multiply | — | 13 |

出現数はデータ量の大きい作品に強く影響されるため、analysis.json の byProjectCoverage と cappedPerProject も確認すること。

## 候補の解釈

1. **条件比較の型特化**: operator_equals が条件式の根になっているパターンは多い。両オペランドが生のdoubleで非NaNなら、Scratchの比較意味論と一致する数値比較を行い、それ以外は厳密な cast::compare に戻すQuickening候補。
2. **連続代入**: data_setvariableto の連続はよくあるが、2命令融合する前に値の依存関係、途中の副作用、監視、yield、同一変数への書き込みを考慮する必要がある。
3. **単純な出現数を最適化目的にしない**: procedures_call > control_wait は1938回出現したが一部の作品に偏りがあり、wait操作は意味論上のスケジューラ境界。安易な融合対象ではない。
4. **演算木の融合**: operator_equals <- data_itemoflist、operator_add <- operator_multiply などは候補になるが、出現頻度より動的実行頻度を重視すべき。

## 実際のQuickeningに向けた追加計測

静的頻度だけでは、ループ1億回実行する1個の命令と、一度だけ実行する1万個の命令を区別できない。

次の段階では src/engine/execute.cpp の BlockCached::ops 実行に、低オーバーヘッドのランダムサンプルカウンタを加える。サンプル毎に (opcode、入力型、スクリプトID、実行済命令数、yield有無) を集計し、**動的頻度 × A/Bで測った専用命令の節約時間**で優先順位を決める。

正しい比較には、同じ入力で interpreter OFF/ON、HotSpot JIT、TurboWarp の結果を比較し、各々について起動時間、処理速度、メモリ、パリティを別々に評価すること。今回の変更はコーパス解析ツールだけであり、エンジンのQuickening自体はまだ実装していない。
