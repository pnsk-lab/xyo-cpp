# 最適化の構造依存性：v1.2.1実測とスカラーコピーSSA試作

## 目的

「個別の高速経路は多いが、組み合わせたときに適用されなくなるのではないか」を検証する。
単に最適化一覧から推測せず、同じ漸化式の書き方を変えたScratchプロジェクトを生成し、
正しさ・生成IR・同一バイナリの対照実験を照合した。

実装したもの：

- [構造比較ハーネス](<../tests/bench/algos/bench_structure_cliffs.py>)
- [ハーネス単体テスト](<../tests/bench/algos/test_structure_cliffs.py>)
- [スカラーコピー差分テスト](<../tests/differential/jit_numeric_repeat_copy.py>)
- [数値repeat SSAの受理条件拡張](<../src/jit/numeric_repeat_ssa.inc>)：**現在は既定ON**。`XYO_JIT_NUMERIC_REPEAT_COPY=0` で無効化できる。

ユーザー指示により、初期試作の検証後に既定ONへ変更した。以下のリリース基準・初期試作の表は変更前に保存した測定結果であり、過去のmanifestや集計は書き換えていない。

既定ON化後の再検証：環境変数未設定・明示0・明示1の3通りで357フィクスチャ、計1,071比較がインタプリタと一致。未設定で数値SSAが生成され、明示0では生成されないことも確認した。ハーネス単体テスト5/5、既存execute 116/116・assert 338/338。
[既定ON検証記録](<../scratch-bench/results/structure-cliffs-v121/default-on/validation.json>)、
[回帰ログ](<../scratch-bench/results/structure-cliffs-v121/default-on/execute.log>)。
ビルド検証は引き続き隔離したv1.2.1＋変更パッチで実施し、共有dirtyツリー全体の統合ビルドは行っていない。

## 測定対象と条件

- リリース基準：v1.2.1、`d6654a6ba9d8de81fe6b4cb2f69ffdb56833b0bf`。
- 共有作業ツリーには他エージェントの変更があるため、タグを `git archive` で別領域へ展開してビルドした。
- GCC 16.2.1 / LLVM 23.1.4、Release、ネイティブLLVM、SDL2 OFFのヘッドレスビルド。
- CPU 12へ固定。CPU型式・実行ファイルSHA-256・環境設定は各manifestに記録。
- `--jit eager`、2回ウォームアップ後に3回のカーネル時間を計測。
- 12,000,000反復、独立プロセス6ブロック。プロセス内中央値を取り、ブロック対応の比率を集計。
- ケースとプロファイルの順をシャッフルし、次のブロックでは逆順で実行。
- 継承した `XYO_*` を除去。GPU・並列スケジューラ・ParForはOFF。
- danger、厳密ブロック計数、プロファイラは使用しない。IR出力は別プロセスで取得。
- 時間はScratch内の `days since 2000` の差から算出。実質1msの分解能があるため、微小差を有意な改善と扱わない。

これはウォームアップ後のカーネル実験であり、HotSpotの起動速度、実作品のFPS、全47ワークロードの改善を測ったものではない。
CPU固定は他プロセスからのCPU隔離を意味しない。信頼区間や他機種での再現性は未評価。

## ワークロード

初期値 `x=1, acc=0` から、各反復で次を順に実行する。

```text
x   = (x * 17 + 23) mod 65521
acc = (acc + x) mod 65521
```

全変種で最終的な `x, acc, aux` を独立したPython実装と照合する。
ただし、**最終状態が同じであることと、実行ブロック数・副作用・中断機会が同じであることは別**。
分岐、呼び出し、コピー、入れ子の追加そのものにも費用がある。

| ケース | 直接ループからの変更 |
|---|---|
| inline | 基準。係数17を式へ直接記述 |
| scalar_coeff | 係数を数値型スカラーへ格納して参照 |
| scalar_text_coeff | 同じ係数を数値文字列として格納して参照 |
| list_coeff | 数値リストの第1要素から係数を参照 |
| helper_body | 反復本体を引数なしwarp手続きへ切り出す |
| empty_call | 同じ本体の後に空warp手続き呼び出しを追加 |
| redundant_branch | 両分岐が同じ本体のif/elseで包む（負の対照） |
| unrelated_write | 各反復の最後に `aux=x` を追加。終了後にも全変種で `aux=x` |
| nested_repeat | 内側8回のrepeatへ分割。余りは末尾で実行 |

### 係数のタグを分離した理由

Scratchの数値shadowは、代入後も数値文字列として保持される場合がある。
一方、数値SSAの入口はplain Numberを要求する。
予備実験でこの交絡を見つけ、`scalar_coeff` は `0+17` で数値型を作り、
文字列のままのものを `scalar_text_coeff` として独立させた。
最初のpilotと中断した測定は結論の集計に使用しない。

## 1. 未変更v1.2.1の結果

[リリース基準の集計](<../scratch-bench/results/structure-cliffs-v121/baseline/summary.json>)。
時間は6プロセス中央値の中央値。比率はブロックごとの比率の中央値。

| ケース | 既定 ms | 直接ループ比 | Numeric Repeat SSAを無効化 ms |
|---|---:|---:|---:|
| inline | 31 | 1.00× | 82 |
| scalar_coeff | 43 | 1.39× | 99 |
| scalar_text_coeff | 100 | 3.23× | 99 |
| list_coeff | 97 | 3.13× | 97 |
| helper_body | 115 | 3.71× | 115 |
| empty_call | 109 | 3.52× | 108.5 |
| redundant_branch | 108.5 | 3.50× | 109 |
| unrelated_write | 82 | 2.65× | 82 |
| nested_repeat | 39 | 1.26× | 82 |

対照プロファイルは `XYO_JIT_NUMERIC_REPEAT_SSA=0` と `XYO_JIT_NUMERIC_REPEAT_FLOAT=0`。
この漸化式では他のCopy/Affine/Conditional専用経路には適合しないため、数値repeatの効果を比較できる。
汎用のループ内変数昇格まで無効化した実験ではない。

### 何が分かったか

1. 直接ループは数値SSAにより約2.65倍高速化されている。
2. コピーを1個追加したケースではその効果が失われ、既定とSSA無効がともに82msになる。
3. 手続き・リスト・冗長分岐にも同様の適用境界がある。ただし全遅延をSSAの欠落だけに帰属させてはいけない。
4. 入れ子は全体が汎用へ落ちるわけではない。外側は汎用でも内側の8回ループに数値SSAが使われ、39msに収まる。
5. 数値型係数と数値文字列係数にも43ms対100msの差がある。IRに高速経路が存在しても、実行時ガードが通るとは限らない。

## 2. コードとIRによる原因の照合

以下のリンクは**v1.2.1タグ**のソースを指す。共有作業ツリーの行番号とは異なる。

- [一般のループ昇格allowlist](<https://github.com/pnsk-lab/xyo-cpp/blob/v1.2.1/src/jit/codegen.cpp#L1975-L1995>)にはリスト読み・分岐・入れ子があるが、手続き呼び出しはない。
- [数値repeatの式・文のパーサ](<https://github.com/pnsk-lab/xyo-cpp/blob/v1.2.1/src/jit/numeric_repeat_ssa.inc#L64-L152>)は、リスト・呼び出し・分岐・入れ子を受理しない。通常repeatのbare variable代入も拒否する。
- [別のConditional SSAパーサ](<https://github.com/pnsk-lab/xyo-cpp/blob/v1.2.1/src/jit/conditional_ssa.inc#L153-L230>)も今回のmod代入を含む分岐本体は受理しない。
- [手続きインライン化](<https://github.com/pnsk-lab/xyo-cpp/blob/v1.2.1/src/jit/codegen.cpp#L8910-L8929>)はLLVM IR生成後なので、先に拒否されたScratchブロック単位のSSA計画を復活させない。
- [数値SSA入口ガード](<https://github.com/pnsk-lab/xyo-cpp/blob/v1.2.1/src/jit/numeric_repeat_ssa.inc#L411-L435>)はplain Numberと整数範囲などを検証する。

別プロセスのIRでは、直接ループ・数値型係数・内側repeatに `rep.numeric.*` を確認。
コピー追加・呼び出し・リスト係数・冗長分岐では、当該カーネルの数値SSAがないことを確認した。
これは静的な経路選択の証拠であり、ガード通過率の直接計数ではない。

## 3. 実装した試作：スカラーコピーと数値SSAの合成

通常の数値repeatでも `set a to b` を既定で受理する。
`XYO_JIT_NUMERIC_REPEAT_COPY=0` を指定した場合だけ通常repeatでのコピー受理を無効化する。
ベンチ名・プロジェクトIDによる特殊扱いはない。

- 既存のVariable式は、コピー元のboxed bits・数値影・整数キャリアの組をそのまま返す。
- 文の順に状態を更新する。読み取り前後のコピー順序を変えない。
- 整数性の固定点解析と64反復の値域証明をそのまま使用する。
- 数値・整数入口ガード、符号付きゼロの扱い、中断時の状態公開を変更しない。
- bare literal代入は受理しない。元の文字列タグを勝手に数値へ変換しない。
- Numeric Untilは元からコピーを受理しており、今回その既定動作は変えない。
- 手続き・リスト・分岐を数値repeatへ取り込む実装ではない。

### 隔離版と作業ツリーの差

v1.2.1には整数キャリアの変換命令をPHI間に生成する既存の配置問題があり、
現在の作業ツリーにはすでに「全PHIを作成してから数値ビューを生成する」修正がある。
最終試作ビルドにはこの既存修正も反映した。
**試作ON/OFFはどちらも同じPHI修正を含む同一バイナリ**なので、コピー受理条件だけを比較できる。
初回の試作測定は最終結果として採用しない。

最終パッチ：[タグからの全差分](<../scratch-bench/results/structure-cliffs-v121/scalar-copy-with-phi.patch>)。
共有作業ツリーに今回加えたのはコピー受理条件だけで、他担当のPHI修正を変更していない。

### 最終試作の結果

[最終集計](<../scratch-bench/results/structure-cliffs-v121/copy/summary.json>)、
[生の測定値](<../scratch-bench/results/structure-cliffs-v121/copy/results.json>)、
[実行条件とSHA-256](<../scratch-bench/results/structure-cliffs-v121/copy/manifest.json>)。
試作バイナリのSHA-256は `fcdf834471075122c86467025c00ac5e341f7283ba42599cdb91b43230a80874`。

| ケース | 試作OFF ms | 試作ON ms | 対応ある高速化倍率 |
|---|---:|---:|---:|
| inline | 31 | 31 | 1.00× |
| unrelated_write（`aux=x` を追加） | 82 | 31 | **2.65×** |
| scalar_coeff | 43 | 43 | 1.00× |
| scalar_text_coeff | 100 | 100 | 約1.00× |
| list_coeff | 97 | 96.5 | 約1.00× |
| helper_body | 115 | 115 | 1.00× |
| empty_call | 108 | 108 | 1.00× |
| redundant_branch | 108 | 108 | 1.00× |
| nested_repeat | 39 | 39 | 約1.00× |

コピー追加ケースの6ブロックの高速化倍率は約2.64515〜2.64517倍。
時間短縮率は約62%。ほかのケースの1ms程度の差は改善とは判定しない。
コピーケースにはON時だけ数値SSAのIRが現れ、測定結果も直接ループの水準へ戻った。
これは当該パターンの改善であって、実作品全体が2.65倍速くなるという意味ではない。

最終の試作ON/OFF双方で、元のリリース基準とおおむね同じ傾向が再現された。
初回のコピー試作結果ではなく、PHI配置修正後の再測定をこの表の根拠としている。

## 正しさの検証範囲

- ハーネス単体テスト：生成グラフ、既知の参照値、XYO環境隔離、欠損・不正結果の拒否、対応ある集計。
- 各ベンチ：0/1/7/8/63/64/65/129反復でインタプリタ・JIT・Python参照値を比較。
- 本測定の12M反復：全測定の最終値をPython参照値と比較。12Mをインタプリタで実行したわけではない。
- コピー差分テスト：文字列タグを `listcontents`、負のゼロを逆数で観測する。中断チャンク境界、非warp、コピー順序を含む。
- 追加でboxed SSA強制、sqrt付き浮動SSA、NaNを生成する途中復帰、実際の数値NaN/±Infinityを検証した。

実行結果：

| 検証 | 結果 |
|---|---|
| ハーネス単体テスト | 5/5 PASS |
| ベンチ境界・参照値照合 | 各実験216/216 PASS |
| コピー専用差分 | 357フィクスチャ、ON/OFF計714比較がインタプリタと一致 |
| 専用経路の生成 | 整数SSA・boxed SSA・浮動SSAのIR生成を確認 |
| 既存executeフィクスチャ、試作ON | 116/116、assert 338/338 |
| 既存executeフィクスチャ、試作OFF | 116/116、assert 338/338 |
| コピーケースのAOT | 2 scripts / 18 slotsをオブジェクト化、実行結果がJITと一致 |

[コピー差分の期待値一覧](<../scratch-bench/results/structure-cliffs-v121/copy-parity.json>)、
[execute ONログ](<../scratch-bench/results/structure-cliffs-v121/execute-copy-on.log>)、
[execute OFFログ](<../scratch-bench/results/structure-cliffs-v121/execute-copy-off.log>)、
[AOT生成ログ](<../scratch-bench/results/structure-cliffs-v121/copy-aot.log>)、
[AOT実行ログ](<../scratch-bench/results/structure-cliffs-v121/copy-aot-run.log>)。

専用テストは自動CMake登録していない。他担当が編集しているテスト定義へは変更を加えず、下記の独立コマンドで実行した。

これは互換性の完全証明ではない。実作品・他CPU・全体性能・現行の変更済みツリー全体との統合検証は別途必要。
初期試作では既定OFFとして検証し、その後ユーザー指示でこのコピー機能だけを既定ONへ変更した。
`--opt-all` の対象や他の最適化の既定値は変更していない。

## 再実行

```sh
# 任意のRelease LLVMバイナリで構造比較
python3 tests/bench/algos/bench_structure_cliffs.py \
  --binary build/structure-cliffs-v121/native/xyo-v121-baseline \
  --out build/structure-cliffs-repeat \
  --count 12000000 --blocks 6 --samples 3 --cpu 12 --diagnostics

# 現在の既定ON入りバイナリの同一バイナリ既定/OFF
python3 tests/bench/algos/bench_structure_cliffs.py \
  --binary build/structure-cliffs-v121/native/xyo \
  --out build/structure-copy-repeat \
  --count 12000000 --blocks 6 --samples 3 --cpu 12 \
  --profiles default,scalar_copy_off --diagnostics

python3 tests/bench/algos/test_structure_cliffs.py
python3 tests/differential/jit_numeric_repeat_copy.py \
  --binary build/structure-cliffs-v121/native/xyo \
  --out build/structure-copy-parity-repeat
```

出力先は新規ディレクトリが必要。既存結果を上書きしない。
CPU番号は `os.sched_getaffinity(0)` の範囲から選ぶ。

## 結論

この実験では「最適化の合成・適用範囲が次の課題」という仮説を支持する結果が出た。
特に単純なスカラーコピーだけで大きな性能差が生じるため、
新しい算術命令の短縮より、既存の解析結果を保持して適用境界を減らす改善に価値がある。
ただし、今回の小さな受理条件の拡張は共通IRの一般化そのものではない。
次は実作品で手続き境界・不変のリスト読み・型ガード拒否が占める時間を確認してから、
副作用要約・型情報の伝播・復帰機構の一般化へ進む。
