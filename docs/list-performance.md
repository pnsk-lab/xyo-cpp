# リスト操作の高速化

## 第3段階: 読み取り専用リストの数値変換メモ

### 実際に通る経路を確認

`micro_read` の `num(7)` は、xyoではrawな数値文字列としてリストに追加される。生のdoubleを想定した命令列だけを調べても実際のコストは説明できなかった。`XYO_JIT_DUMP_OBJECT=<prefix>` を追加して、AOTの近似ではなく実際にORC JITが生成したオブジェクトを `<prefix>-N.o` へ保存し、文字列のタグ分類・numState参照・numCache参照が反復ごとに実行されていることを確認した。この診断は既定off。番号はプロセス単位なので、別プロセスには別prefixを使う。

### 採用した変更

- **局所的な1-entry coercion memo**: 読み取り専用・leafな昇格ループのlist reporterから得たValueだけを対象にする。通常のdouble fast pathは維持し、文字列等のslow pathでraw bitsが登録済みkeyに一致すれば、保存済みdoubleを使う。
- **有限性を加算まで伝播**: 最初の変換結果がfiniteの場合だけ登録し、memo hitを証明できる経路の`change by item`ではNaN正規化を省く。current shadowはnon-NaNなので、finite deltaとの加算はoverflowして±InfになることはあってもNaNにはならない。missや±Infは従来の正規化を行い、NaN修復を低頻度分岐へ分離する。
- **ミスで更新し続けない**: 区間内では最初のkeyを保持する。異なる文字列を読むたびにkey/doubleを書き換える方式は回帰が大きかったため撤回した。非finiteの初回値では`+0 / 0`の組にして登録を抑止する。
- **文字列を先に分類**: plain-numberを除いた経路では、文字列をnumber/boolマッピングより先に処理する。
- **yield後にヘッダを再取得**: len/items影をSSA化可能なスロットに置き、yield確認ヘルパの後で現在のSListから読み直す。別スクリプトによるclearやrealloc後に古い配列を読む既存問題も修正した。
- **重複null検査の省略**: 昇格リストのnull経路はlen=0/items=nullを保証するため、数値索引はbounds checkだけで拒否できる。未昇格リストと特殊索引のhelper経路は維持する。

### 意味論とGC

rawなリスト要素や変数のValueを数値へ置き換える最適化ではない。文字列`"7"`と数値`7`の区別、比較、list contents、入力評価順、yield位相、warp予算は維持する。

本体にlist writeが一つでもあれば、別idのaliasも含めてmemoを無効にする。nested loopも無効にし、すべてのmemo siteが生成された後のループ末尾だけで同期する。key/doubleはloop preheaderで`undefined / 0`に初期化し、yield確認ヘルパから戻るたびに同じ組へresetする。SStrの内容はimmutableであり、readonly listがkey文字列をrootする。yield中に古いkeyが回収されても、再開後は旧ポインタを比較・逆参照せずresetするため、GCによるアドレス再利用を誤ってhitと扱わない。

`XYO_LIST_READ_MEMO=0`でcoercion memoとその有限性証明を無効にできる。リスト加算のNaN修復分岐、文字列先dispatch、ヘッダ再取得は維持する。

### 再計測

[専用ランナー](<../tests/bench/algos/bench_list_read.py>)は、同一／異なる数値と数値文字列を分けて計測する。各プロセス内でウォームアップ後3回計測し、プロセスを3回実行する。変更前・変更後・TurboWarpの順を毎回rotateし、全checksumをPythonで計算した期待値と照合する。

```sh
python3 tests/bench/algos/bench_list_read.py \
  --before /path/to/previous/xyo --xyo build/xyo --cpu 8 \
  --json bench-output/list-read.json

XYO_LIST_READ_MEMO=0 python3 tests/bench/algos/bench_list_read.py \
  --before /path/to/previous/xyo --xyo build/xyo --cpu 8 \
  --json bench-output/list-read-no-memo.json
```

CPU番号は環境に合わせる。今回の比較は同一のP-core（CPU 8）へ全プロセスを固定した。計測区間はプロジェクト内の演算時間で、ロード・コンパイル・データ構築は含まない。

### 最終計測（50M reads）

同一P-coreで、変更前・変更後・TurboWarpをrotateしながら各3プロセス×3回測定した。`--json`の出力（`results-list-read.json` と `results-list-read-no-memo.json`）は `.gitignore` 対象の計測結果で、手元に保存される。

| 要素の種類 | 第2段階後 | 第3段階後・既定 | TurboWarp compiled |
|---|---:|---:|---:|
| 同じ数値文字列 | 124 ms | **85 ms** | 94 ms |
| 同じ数値double | 119 ms | **86 ms** | 94 ms |
| 異なる数値double（1..10） | 124 ms | **89 ms** | 98 ms |
| 異なる数値文字列（1..10） | 134 ms | **160 ms** | 342 ms |

元のreadベンチは約31%短縮し、TurboWarpより約10%短い時間になった。数値doubleの2ケースもTurboWarpより約9%短い。計測した4ケースでは既定版がTurboWarpを上回ったが、異なる数値文字列では第2段階後の自分自身に対して約19%の回帰が残る。このケースは`XYO_LIST_READ_MEMO=0`で **134ms**（同じ計測で変更前134ms、TurboWarp329ms）となる。単一キーの変換メモは万能ではない。

ミス後にメモをretireし別の変換経路へ自動退避する案も試したが、数値文字列の変化ケースは152→146ms程度の小改善にとどまり、元のreadが84→92msへ悪化したため撤回した。既定経路を小さく保ち、トレードオフと無効化設定を明示する方を採用した。無効化設定はJITのコンパイル時、AOTの生成時に適用する。

ほかのアルゴリズムの交互測定でもquicksort 30→29ms、mergesort 46→43ms、bubblesort 90→83msを確認。sieve 16ms、fib 30ms、sha256 61msは実質不変。micro loopは35→37ms、appendは47→49msの小さな悪化があった。これらは同一CPU固定のため、並列appendの絶対時間を第2段階の非固定結果と直接比較しない。

### 検証

[専用回帰テスト](<../tests/unit/test_list_read_memo.cpp>)では、memo有効JIT／無効JIT／interpを比較し、数値文字列・混合型・非有限値・NaNからの復帰・ゼロ反復・再入・raw Value不変性を確認する。非warp yield中の別スクリプトによるreplace／resize／clear、およびGCを実行した後の再開も含む。GCの実行は検証するが、保守的ルートが残るため旧ポインタが必ず回収・再利用されたとは主張しない。

最終版は専用テスト **836 checks / 0 failures**、全ctest **6/6 PASS**、Scratch差分 **19/19一致**（HotSpot / interp両方）。既存10ベンチと型別read 4ケースの計14プロジェクトは、AOT生成・実ロード・実行後のchecksumもすべて期待値と一致した。

## 第2段階: 真偽値変換と条件付き更新の依存鎖

境界チェックだけでは TurboWarp との差を埋められなかったため、生成 IR と機械語を調べ直した。

### 採用した変更

- 比較や論理演算が生成した `select(condition, kTrue, kFalse)` は、真偽値を再変換せず元の `i1` を分岐へ渡す。
- 動的な `kFalse` も真偽値変換の inline 側で処理する。以前は `kTrue` だけが inline で、比較結果が false の反復に `xyo_truthy(kFalse)` が残っていた。
- 変数を昇格できるループ内の `control_if` は、then 側に無命令の `llvm.sideeffect` を置き、条件付き更新をループ運搬値に対する FP select/blend へ変換させない。予測可能な分岐では、`fadd → compare → blend → 次の fadd` の依存鎖を避けられる。`XYO_BRANCH_IF=0` でこの分岐保持だけを無効にして比較できる。

入力評価順、真偽値の定義、Value の表現、yield の頻度や warp 予算は変えていない。JIT / AOT 共通のコード生成で適用する。

### 同一設定での交互計測

前回完了時の実行ファイルを保存し、今回版と交互に実行した。各プロセスはウォームアップ後3回計測し、その中央値を採用。プロセスを3回起動し、3個の中央値の中央値を表に示す。各組の間に TurboWarp compiled も実行した。全モードの checksum を Python 参照実装と照合し、全て一致した。JIT は既定の HotSpot、warp 予算は通常設定であり、danger は使用していない。

| 処理 | 今回変更前 | 今回変更後 | TurboWarp compiled |
|---|---:|---:|---:|
| read（50M） | 178 ms | **124 ms** | 93 ms |
| replace（50M） | 128 ms | **72 ms** | 115 ms |
| append（20M） | 25 ms | 24 ms | 198 ms |
| quicksort（200000） | 47 ms | **29 ms** | 33 ms |
| mergesort（200000） | 83 ms | **45 ms** | 42 ms |
| bubblesort（5000） | 115 ms | **89 ms** | 58 ms |
| sieve（2000000） | 21 ms | **16 ms** | 16 ms |
| fib（31） | 36 ms | **30 ms** | 56 ms |
| sha256（65536） | 61 ms | 60 ms | 91 ms |
| change loop（50M） | 35 ms | 35 ms | 53 ms |

read は約30%、replace は約44%、quicksort は約38%、mergesort は約46%の時間短縮。replace は TurboWarp の約1.6倍、quicksort は約1.14倍の速度になった。一方、read は依然として TurboWarp の約1.33倍の時間を要し、bubblesort も未達である。すべてのリスト処理で逆転したわけではない。

各プロセスの中央値は[交互計測結果](<../tests/bench/algos/results-list-round2.json>)に保存した。再計測には既存のベンチランナーを使い、`--xyo` に変更前／変更後の実行ファイルを指定する。最終 IR で micro_read の `xyo_truthy` 呼び出しが消えていることも確認した。

### 不採用の実験

数値影の遅延ボックス化、yield を外側へ出す最大64反復の strip mining、有限 delta の型ガード、NaN 修復の低頻度分岐化、NaN を除外する整数タグ判定も試した。単独または組合せで改善が小さいか他の操作を悪化させたため撤回した。特に有限 delta ガードは read を約123msから184msへ悪化させた。

従来の「主因は yield による spill」という説明だけでは不十分だった。strip mining 自体は生成されていたが、内側に false の汎用真偽値変換が残っていた。さらに、その呼び出しを除いた後も条件付き FP blend の依存鎖が支配的になった。

### 回帰テスト

[数値昇格と真偽値のテスト](<../tests/unit/test_numeric_promotion.cpp>)で、raw 文字列／bool／NaN／Infinity の保持、動的 delta の加算、条件付き更新、ゼロ反復、非warp yield 中の別スクリプトの書き換え、16種類の値に対する16種類の真偽分岐を interp / eager JIT と独立した期待値で検証する。数値 NaN の符号・payload は Scratch から観測できないため同一視するが、文字列 `"NaN"` と数値 NaN は区別し、−0を含む他の数値はビット単位で比較する。

最終版の検証結果:

- 数値昇格・真偽値テスト **1294 checks / 0 failures**。
- 全 ctest **6/6 PASS**。
- Scratch 差分テスト **19/19一致**（HotSpot / interp 両方）。
- 上表の10ベンチについて、AOTオブジェクト生成・実ロード・実行を確認し、全 checksum が参照実装と一致。

## 第1段階: 境界チェック

### 変更

[コード生成](<../src/jit/codegen.cpp>)の `item of list` と `replace item of list` で、x86-64 の数値索引を `CVTTSD2SI64` → wrapping subtraction → unsigned の長さ比較で処理する。従来の浮動小数点による下限・上限比較を整数比較1回に置き換える。

通常の LLVM `fptosi` を無条件に使う方法とは異なり、x86 intrinsic は NaN・無限大・範囲外を `INT64_MIN` にする。1を引くと `INT64_MAX` となり、最大でも `UINT32_MAX` のリスト長との unsigned 比較で必ず拒否される。負値・ゼロ・1未満も拒否され、有効な正の小数は従来どおり切り捨てられる。通常の masked FP 例外環境を前提とする。

非x86-64は従来の ordered FP guard を維持する。文字列の特殊索引を処理する既存の helper 経路、入力評価順、monitor dirty 更新、yield の頻度は変更しない。JIT と AOT で同じ lowering を使用する。

AOT の検証中に、リストヘッダ昇格の未使用 `len64` PHI が別の predecessor に属すべき変換命令を参照している既存問題も見つかった。未使用の補助値を削除した。

## 計測

[既存ベンチランナー](<../tests/bench/algos/bench.py>)を使用。ウォームアップ後のプロジェクト内時間（ms）を測り、各プロセス3サンプルの中央値を取り、3プロセスの中央値で比較した。checksum は独立した Python の期待値と照合した。

x86-64 / Intel Core i7-12700 / LLVM 23。既定設定での変更前と最終版の計測:

| 処理 | 変更前 | 最終版 | 最終版と同時に測った TurboWarp |
|---|---:|---:|---:|
| read（50M） | 181 ms | 176 ms | 92 ms |
| replace（50M） | 140.5 ms | 125 ms | 114 ms |
| change loop（50M） | 35 ms | 35 ms | 53 ms |

変更前は2プロセス、最終版は3プロセス。replace は約11%短縮、read は約3%短縮だが、後者は小差である。最終版では intrinsic が読まない上位レーンを未定義値にし、不要なゼロ化も避けた。

以下は、その最終調整前に行った交互実行での観測値:

| 処理 | 変更前側 | 整数境界チェック | TurboWarp compiled |
|---|---:|---:|---:|
| read（50M） | 185 ms | 181 ms | 92 ms |
| replace（50M） | 137 ms | 127 ms | 115 ms |
| bubblesort（5000） | 125 ms | 118 ms | 58 ms |
| mergesort（200000） | 88 ms | 84 ms | 42 ms |
| quicksort（200000） | 46 ms | 46 ms | 33 ms |
| sieve（2000000） | 22 ms | 22 ms | 20 ms |
| change loop（50M） | 35 ms | 35 ms | 54 ms |

比較用バイナリは、先に試した SLP 無効化実験時のもの。SLP 無効化単独では micro read は181→187 ms、replace は140.5→136 msで改善が安定せず、採用していない。上表は厳密に境界チェックだけの A/B ではないため、小差を断定的な改善率として扱わない。別途、既定設定の変更前計測では read 181 ms / replace 140.5 ms、整数境界チェック版の独立計測では read 174 ms / replace 124 msだった。主に replace に改善があり、read の大幅な高速化や TurboWarp との逆転は未達。

再計測:

```sh
python3 tests/bench/algos/bench.py --micro \
  --algo micro_loop,micro_read,micro_write,micro_append \
  --modes xyo-jit,tw-compiled --reps 3 --json bench-output/list.json
```

## 検証

- [境界テスト](<../tests/unit/test_list_codegen_bounds.cpp>): NaN、±Inf、正負の巨大値、2^63 / 2^64 と周辺値、ゼロ、小数、文字列索引、読み出し・置換の結果を interp/JIT で検証。
- [AOT テスト](<../tests/unit/test_aot.cpp>): リストを読む昇格可能ループの IR 検証と JIT/AOT の状態一致を確認。
- 最終ビルドで ctest **6/6 PASS**。AOTリスト回帰ケースを含む。
- [Scratch 差分テスト](<../tests/differential/run_repros.py>)は **19/19** が HotSpot / interp とも参照実装に一致（最終版で再実行済み）。
- リスト読み出しベンチの AOT オブジェクト生成とロード実行を確認し、checksum は `350000000` で一致。

## 残る課題

第3段階で計測したread 4ケースではTurboWarpを上回った。一方、異なる数値文字列では自分の前版からの回帰が残り、メモ無効化が有効な場合がある。値の分布を踏まえた自動選択は今後の課題。またbubblesortは依然TurboWarpが速く、すべてのプロジェクトで逆転したわけではない。全体SLP無効化やyield除去だけで解決するという根拠はなく、生成機械語・同一条件の交互計測・互換性テストで採否を決める。
