# 共通typed numeric CFG：形別の検出から性質ベースの最適化へ

## 実装した範囲

数値ループを個別の形として認識するのではなく、**対応する命令を共通のSSA CFGへlowerし、
その型・値域・読み書きの情報から最適化する実験経路**を追加した。

```text
Scratchの命令・分岐・純粋手続き
    → backend-neutral CFG / SSA / Phi
    → 検証器：型・def/use・支配関係・制御フロー
    → 64反復分の区間解析と分岐合流
    → 証明できればi32/i64、そうでなければf64
    → 既存のwarpチェックポイントと通常経路への復帰
```

既定OFF。明示的に有効化する。

```sh
XYO_JIT_NUMERIC_REGION=1
```

既存の専用経路は削除していない。正確なブロック計数・`XYO_PROF`・SSA全体の無効化は従来どおり尊重する。
前回既定ONにしたスカラーコピーもそのまま維持した。新フラグはコード生成時の選択であり、生成済みAOTを後から切り替える設定ではない。

## これまでとの違い

前回の試作は「String入力をこの条件なら受理」「この小さな手続きを展開」と、
既存の専用SSA計画の受理条件を広げるものだった。

今回のLLVM adapterはScratchのopcodeやベンチ名で分岐しない。
共通IRの演算・ブロック・Phi・解析結果だけを見る。
同じlowererでif/else、分岐後の変数転送、引数付き手続き、入れ子呼び出し、それらの組み合わせを扱う。
整数幅も文の並び方ではなく、SSA値全体の区間証明から選ぶ。

### ファイルの役割

| 層 | 実装 | 役割 |
|---|---|---|
| 共通IR | [numeric_region.hpp](<../src/ir/numeric_region.hpp>) | 既存SsaGraph/ControlFlow/Phiを再利用したbackend-neutral数値命令、検証、区間解析 |
| Scratch frontend | [numeric_region_lower.hpp](<../src/analysis/numeric_region_lower.hpp>) | 命令からSSAを構成、状態転送・Phi・引数束縛、未知効果の拒否 |
| LLVM adapter | [numeric_region_ssa.inc](<../src/jit/numeric_region_ssa.inc>) | 型特化、CFG/PHI生成、ガード、反復単位rollback、状態公開 |
| 接続 | [numeric_repeat_ssa.inc](<../src/jit/numeric_repeat_ssa.inc>) | フラグ付きの計画選択と既存64tickスケジューラへの接続 |

既存の共通ExecutableやLLVM/WASM共通ABIは変更していない。
数値区間IRはLLVM依存を持たないが、**今回実装した実行adapterはLLVMのみ**。
WASM/Binaryen adapterが完成したという意味ではない。
共有のコード生成本体は新しいfrontend headerのinclude 1行だけを追加し、他担当の変更は維持した。

## 対応する命令と性質

- 初期の実行hookは、既存コード生成が定数のi32反復数を作れるwarp repeatが対象。
  動的な反復数や他のループ構文の共通化は、この変更では行っていない。
- 数値set/change、変数コピー、四則演算、mod。
- 数値比較、and/or/not、if/if_else。合流点はSSA Phiとして構成する。
- 純粋warp手続き：最大8引数、最大4階層、非再帰。手続き内の分岐にも対応。
- 実引数は呼び出し側で評価してから、変更されない字句的束縛として保持する。
  手続き内で同じ変数を書き換えても、捕捉済み引数は変わらない。
- スカラーの入力・書き込み集合をslotで保持。読み取り専用のsummaryと出力の整合も検証する。
- 副作用・yield・リスト・cloud変数・未対応演算・入れ子ループは、この初期経路では拒否する。
- 最大2,048ノード/定義、128ブロック、32変数などのコンパイル量上限を持つ。

### 型タグと文字列

数値shadowを裸で変数へ代入したとき、Stringのタグや綴りが残る場合がある。
そのため「literalをdouble化して代入すればよい」とは扱わない。
String literalの正確なコピーや、空白/不正文字列を数値比較へ置き換えることは拒否する。

外部scalar入力には実行時のNumberガードを置く。
例外は、**IR上で算術入力としてしか使われず、書き換えられず、観測可能なコピー・Phi・比較へ流れない**入力。
これは入口でScratchの算術変換を使えるが、元のString自体は変更しない。
この判定は手続き展開後のSSA上で行うので、変数名や文の位置だけには依存しない。

### 整数特化

1. 各チャンク入口でNumber、整数、`0..2^21`、非負ゼロをガードする。
2. CFGの両分岐を保守的に合流し、出力を次反復の入力へ同時にフィードバックする。
3. 最大64反復にわたり、すべての整数中間値が `±2^48` 以内で正確に表現できることを確認する。
4. 全SSA値がsigned i32にも収まればi32、そうでなければi64を使う。

掛け算の負のゼロ、負の剰余、一般の除算等で整数証明ができなければ、f64経路を選ぶ。
初期版のmod整数証明は非負の被除数と正の整数literal除数に限定する。
動的除数はf64として実行できるが、まだ整数証明を一般化していない。
入力が整数入口ガードに合わなければ、通常JITへ戻る。整数/f64の両バージョンを毎回生成する設計ではない。

### f64とNaN時の反復単位rollback

入力Number NaNはガードで拒否し、f64演算の結果がNaNになったら、次の命令や比較へ進む前に復帰する。

- 現在の反復の書き込みはまだ公開していない。
- それ以前に完了した反復の状態だけを公開する。
- 反復カウンターとtick位相を、その完了分だけ進める。
- NaNを生成した反復は元のJIT本体に最初から実行させる。

これによりScratchのNaN→0算術変換や、NaN比較の文字列へのフォールバックをIEEE比較に誤置換しない。
負のゼロ・Infinityも保持する。fast-mathによる式の再結合は使用していない。

## 性能実験

同じ非負整数LCGを9構造で生成し、3設定を同一バイナリで比較した。
`x*16+x` と `x*17` 等の同値性はこの整数ベンチの条件に限る。
任意の浮動小数点入力についてその式変形を許したわけではない。

- 基底：v1.2.1 `d6654a6ba9d8de81fe6b4cb2f69ffdb56833b0bf` を隔離して展開。
- 前回のコピー既定ON・PHI配置修正・試作を含め、必要なheaderと最小hookだけを適用。
- Release、GCC 16.2.1、LLVM 23.1.4、i7-12700、Linux、SDL2 OFF。
- CPU12固定、12M反復、eager、2回ウォームアップ後3回測定、6ブロック。
- 9構造×3設定×6ブロック＝162プロセス、486測定時間。
- 順序をシャッフルし次ブロックは逆順。GPU/PAR/ParFor、厳密計数、prof、dangerは使わない。
- 実行ファイルSHA-256：`43fe3b41f09d445cfa21540a06edd4d954e41bdab5a7a8b0db9305cc12cde62c`。

### 結果

時間は各プロセス内中央値を取り、さらに6プロセスの中央値。
倍率は対応するブロックの時間比の中央値なので、表の中央値同士の割り算とは一致しない場合がある。

| 構造 | 従来 ms | 前回2試作を併用 ms | 共通CFG ms | 対応ある高速化 |
|---|---:|---:|---:|---:|
| 直接記述 | 33.0 | 33.0 | 33.0 | 1.00× |
| if/elseで包む | 142.5 | 146.5 | 32.5 | 4.47× |
| 分岐先で一時変数を設定して合流 | 163.0 | 167.5 | 35.0 | 4.91× |
| 引数付き手続き | 171.0 | 165.5 | 33.0 | 5.32× |
| 手続き内に分岐 | 178.5 | 178.5 | 33.0 | 5.24× |
| 入れ子の手続き呼び出し | 167.5 | 160.0 | 34.5 | 5.00× |
| 分岐の片側で手続きを呼ぶ | 236.5 | 226.0 | 33.0 | 7.33× |
| 変更前の変数を引数へ捕捉 | 118.5 | 115.0 | 35.0 | 3.40× |
| String係数＋分岐＋入れ子手続き | 258.0 | 260.0 | 48.0 | 5.17× |

最後のケースには動的な係数・追加状態があり、直接literalのループと生成命令まで同じではない。
それでも、新しい専用パターンを組み合わせごとに追加することなく、共通経路で高速化できた。

ホスト負荷の変動は残る。直接記述のプロセス中央値にも約31〜56msの外れがあり、
CPU固定はCPU隔離ではない。数%の差や完全な無回帰は断定しない。
対象構造の大きな改善は全6ブロックで再現し、例としてif/elseは3.76〜4.91×、
引数付き手続きは4.82〜5.52×、分岐＋手続きは6.22〜7.92×だった。
コンパイル費用・RSS・実作品のFPS/p99との損益評価は未実施。

[集計](<../scratch-bench/results/typed-numeric-region-v121/summary.json>)、
[生データ](<../scratch-bench/results/typed-numeric-region-v121/results.json>)、
[manifestとソース来歴](<../scratch-bench/results/typed-numeric-region-v121/manifest.json>)、
[タグからの再現用パッチ](<../scratch-bench/results/typed-numeric-region-v121/source.patch>)を保存した。

## 正しさと実際の中断の検証

| 検証 | 結果 |
|---|---|
| [共通CFG差分](<../tests/differential/jit_numeric_region.py>) | 991フィクスチャ、1,982比較がインタプリタと一致 |
| CFG検証・区間解析の[standalone smoke](<../tests/platform/numeric_region_smoke.cpp>) | C++20 / -Wall -Wextra -Werror -pedantic でPASS |
| [native checkpoint smoke](<../tests/platform/numeric_region_checkpoint_smoke.cpp>) | 2構造×ON/OFFで、それぞれ16回の実yieldと1,025反復を検証 |
| 既存execute | ON/OFF各116/116、assert 338/338 |
| 既存Numeric Repeat/Until | eager/hotspot、warp/nonwarpを含む16 runで一致 |
| AOT | 整数CFG5ケース＋NaN復帰/負のゼロ2ケースで生成・ロード・実行結果一致 |
| 本測定 | 全162プロセスの結果を独立Python参照値と照合 |

差分テストは型タグ、String literal、空白比較、負のゼロ、Infinity、Number NaN、
63/64/65/66/129反復、Phiでの変数発見、引数の評価順・字句的束縛を含む。
64反復後の65反復目にNaNを起こすケースで、完了済みprefixだけが保持されることも確認した。

checkpoint smokeは `warpTimeMs=-1` をテスト注入し、実際のnative Fiber yield後にホストから入力を変更する。
String/Number、空白・不正文字列、負のゼロ、整数入口範囲外、fractionへ切り替え、
各yieldで独立計算した状態と一致すること、元のString型と綴りを壊さないことを確認した。
単に「64回のループを実行した」だけの試験ではない。

[差分期待値](<../scratch-bench/results/typed-numeric-region-v121/parity.json>)、
[実中断ログ](<../scratch-bench/results/typed-numeric-region-v121/checkpoint.log>)、
[execute ONログ](<../scratch-bench/results/typed-numeric-region-v121/execute-region-1.log>)、
[AOT結果](<../scratch-bench/results/typed-numeric-region-v121/aot.json>)、
[f64 AOT結果](<../scratch-bench/results/typed-numeric-region-v121/aot-f64.json>)を保存。

## まだ実装していないこと

これは**純粋数値区間の共通化の第1段階**であり、実行系全体の汎用最適化が完成したわけではない。

- 未知の副作用・待機・リスト操作を含む場合、今はその外側ループの共通経路を拒否する。
  不明な処理だけ通常実行し、その前後の区間を最適化する「局所分割」は未実装。
- その局所分割には、効果要約・部分状態の公開・正確なPC・guard失敗後の継続を共通CallFrameへ接続する必要がある。
- 手続きは予算内でSSAへ展開する。展開せずに手続き要約だけを使って型を伝播する方式は未実装。
- 入れ子ループ全体の共通CFG化、Stringを書き込む型混合SSA、リストの要素型・長さ・alias解析は未実装。
- 既存共通IRのExecutable/host-effect ABIを統合し直したわけではなく、WASM/Binaryen実行adapterは未実装。
- portable CallFrame/OSR/実行中のsource編集との統合は未検証。checkpoint smokeはそれらを無効にして検証した。
- 値の変更は実yieldで検証したが、変数ID/nameの対応変更・削除後の再解決は未検証。slot共有はbindingが安定している前提。
- LLVM adapterは現在のfrontendが作るCFG専用。将来IR定数畳み込み等でNaN定数を導入する場合には追加ガードが必要。
  整数証明も、現在の64反復・入口範囲と対応した解析結果を使う契約である。
- 共有dirtyツリー全体の統合ビルドは行わず、隔離したタグ基底＋保存パッチで検証した。

したがって新経路は既定OFFとし、既存の最適化を置き換えず併存させる。

## 再実行

```sh
python3 tests/differential/jit_numeric_region.py \
  --binary build/typed-region-v121/native/xyo --out build/region-parity-repeat
python3 tests/bench/algos/bench_numeric_regions.py \
  --binary build/typed-region-v121/native/xyo --out build/region-bench-repeat \
  --count 12000000 --blocks 6 --samples 3 --cpu 12 --diagnostics
c++ -std=c++20 -Wall -Wextra -Werror -pedantic -Isrc \
  tests/platform/numeric_region_smoke.cpp -o build/numeric-region-smoke
build/numeric-region-smoke

# 実native中断試験：同じsnapshotのheaders/libxyoへリンクする
c++ -std=c++20 -O2 -DXYO_JIT=1 -DXYO_LLVM_GET_OR_INSERT=1 \
  -Ibuild/typed-region-v121/source/src \
  tests/platform/numeric_region_checkpoint_smoke.cpp \
  build/typed-region-v121/native/libxyo.a -lz -ldl -lLLVM-23 -pthread \
  -Wl,--export-dynamic -o build/numeric-region-checkpoint
build/numeric-region-checkpoint \
  scratch-bench/results/typed-numeric-region-v121/checkpoint-fixtures/helper-False.sb3 \
  scratch-bench/results/typed-numeric-region-v121/checkpoint-fixtures/helper-True.sb3
```

新規の出力ディレクトリを指定する。テストのCMake登録や `--opt-all` への追加はしていない。
