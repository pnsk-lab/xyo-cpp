# Scratch の有名アルゴリズムで xyo と TurboWarp を比べる

Scratch 3 のブロックだけで古典アルゴリズムを実装した `.sb3` を作り、
**同じファイルを xyo と TurboWarp（`refs/scratch-vm` 2.1.46）で実行**して、
プロジェクト自身が計測した実行時間を比較した。この計測で見つかった弱点は
JIT 側で修正し、[最適化](#最適化計測後に直したもの)の節に前後の数値を残した。
生成器・ランナー・検証は `tests/bench/algos/` にあり、
`python3 tests/bench/algos/bench.py` で再現できる。

## 結論

| アルゴリズム | n | TurboWarp (compiled) | xyo JIT | xyo / TW | xyo interp | TW interp |
|---|---:|---:|---:|---:|---:|---:|
| `quicksort` | 200000 | 33.0 ms | 46.0 ms | **0.72x** | 3398 ms | 3411 ms |
| `mergesort` | 200000 | 41.0 ms | 85.0 ms | **0.48x** | 8362 ms | 8189 ms |
| `bubblesort` | 5000 | 58.0 ms | 123.0 ms | **0.47x** | 14231 ms | 13920 ms |
| `sieve` | 2000000 | 15.0 ms | 21.0 ms | **0.71x** | 2959 ms | 2957 ms |
| `fib` | 31 | 54.0 ms | 36.0 ms | **1.50x** | 4833 ms | 3747 ms |
| `sha256` | 65536 | 87.0 ms | 61.0 ms | **1.43x** | 16128 ms | 4554 ms |

（`xyo / TW` は TurboWarp / xyo。1 より大きいほど xyo が速い。
全 6 アルゴリズム × 5 モードで checksum が Python 参照実装と一致した。）

- **TurboWarp のコンパイラは「リスト読み書き」で強い。**
  quicksort / mergesort / bubblesort / sieve で xyo の 1.2〜2.2 倍速い。
- **xyo の JIT は「密な数値ループ + 手続き呼び出し + 算術 + append」で強い。**
  `change i by 1` だけのループは **xyo が 1.5 倍速い**（最適化前は 2.8 倍
  遅かった）。`fib`（再帰呼び出し）で 1.50 倍、`sha256` で 1.43 倍、
  `add` だけで 7.2 倍。
- **インタプリタ同士はほぼ互角**（ソート系は xyo が 0.8〜2% 速い）。
  例外は `fib`（TW が 1.28 倍速い）と `sha256`（TW が 3.5 倍速い）。
- warp 予算（xyo の 500 ms）はこのサイズでは発動しない。
  `--danger`（予算なし）との差は 0〜5% で、比較条件は実質同じ。

Scratch プログラムの実行速度は一様ではなく、**ループとリスト索引が主か、
呼び出しと算術が主かで勝者が入れ替わる**。xyo がすべての Scratch コードで
TurboWarp より速い、という結果にはならなかった。

## 環境

| | |
|---|---|
| CPU | 12th Gen Intel Core i7-12700 |
| OS | Linux (x86-64) |
| TurboWarp | `refs/scratch-vm` 2.1.46（TurboWarp フォーク）、`setCompilerOptions({enabled:true})`（TW の既定） |
| scratch-vm interp | 同フォーク、`enabled:false` |
| JS | Node v26.4.0 (V8) |
| xyo | `build/xyo`（RelWithDebInfo, LLVM 23.1.4, HotSpot JIT, 下記の最適化込み） |

TurboWarp は GUI/Electron ではなく、その VM 本体（`scratch-vm`）を Node で
ヘッドレスに回している。コンパイラのコードは TurboWarp デスクトップと同じで、
V8 の版がわずかに違うだけ。**プロジェクトのロード・コンパイル時間は
計測区間に含まれない**（後述）。

## 計測方法

各 `.sb3` は緑旗で次の順に動く（`tests/bench/algos/gen_algo_bench.py`）。

1. データセットを **warp 手続き**で構築する（計測外）。
2. アルゴリズムを 1 回ウォームアップ実行する。ここで xyo の JIT コンパイルと
   TurboWarp のコンパイルが終わる（計測外）。
3. `repeat 3` で
   「データ構築（計測外）→ `days since 2000` → アルゴリズム →
   `days since 2000`」を繰り返し、差分 × 86400000 を `times` リストへ入れる。
4. 結果を独立に検証する（整列済み判定 + 総和 + ローリングハッシュ、
   sieve は素数の個数と位置ハッシュ、 SHA-256 は 8 ワードのダイジェスト）。

`days since 2000` は scratch-vm では `Date.now()`、xyo では
`std::chrono` のミリ秒で、どちらも Scratch と同じ意味論。計測区間は
**アルゴリズム呼び出しだけ**で、ロード・コンパイル・データ生成は含まれない
（アルゴリズム実行中に走る GC は両 VM とも含まれる）。壁時計ではなく
プロジェクト内の値を使うのは、ロード時間（xyo と Node で全然違う）を
混ぜないため。

`bench.py` は各実行のチェックサムを Python の参照実装と照合し、一致しない
実行は `INVALID` として捨てる。今回の全 30 構成（6 アルゴリズム × 5 モード）で
一致した。SHA-256 のダイジェストは
`23c9946f842d90a17f182b2803ae8ba1784df35c6a13ebccf3d204fb963de248`
（`hashlib.sha256` と一致）。

サンプル数は TurboWarp compiled / xyo JIT / xyo danger が
3 プロセス × 3 回 = 9、インタプリタ系が 1 プロセス × 3 回 = 3。
1 プロセス内の中央値を取り、さらにプロセス間の中央値を取る。単純な
全サンプル中央値ではなくこの二段にしているのは、2M 要素リストを毎回
作り直す `sieve` で **V8 の GC が 3 回目の反復だけ 40 ms 前後まで
伸ばす**ため（TurboWarp 側は 21/21/39 のようなサンプル列になる）。
TurboWarp compiled の値は実行間で 10〜20% 動く（`bubblesort` は
55〜67 ms）。表は 1 回の通し実行の値で、傾向は再実行でも変わらない。

### アルゴリズムとサイズ

| 名前 | 実装 | 主な負荷 | n |
|---|---|---|---|
| `quicksort` | in-place Hoare、中央ピボット、再帰 | リスト読み書き・比較・再帰 | 200000 |
| `mergesort` | 再帰 + 補助リストへのマージ | リスト読み書き・再帰 | 200000 |
| `bubblesort` | 早期終了つき O(n²) | リスト読み書き・ループ | 5000 |
| `sieve` | エラトステネス（0/1 フラグ） | リスト読み書き・ループ | 2000000 |
| `fib` | 素朴な再帰（明示スタック） | 手続き呼び出し・再帰 | 31 |
| `sha256` | FIPS 180-4 圧縮関数 | 32bit 演算・テーブル参照・呼び出し | 65536 バイト |

Scratch にはローカル変数もビット演算も無い。再帰の途中状態はリストの
スタックへ退避し、SHA-256 の XOR は 64 KiB のバイト XOR 表を引く
（AND は `(x + y - (x xor y)) / 2`、ROTR/NOT は算術）。これは Scratch で
ハッシュを書くときの普通の方法で、どの VM でも同じブロック列が実行される。

## 対照実験: ループ・リスト・append

勝敗がアルゴリズム固有なのか、ブロック実行そのものの差なのかを見るため、
同じ形の極小プロジェクトも測った（`bench.py --micro`）。

| 対照 | 内容 | n | TurboWarp | xyo JIT | xyo / TW |
|---|---|---|---:|---:|---:|
| `micro_loop` | `repeat n { change i by 1 }` | 50000000 | 53.0 ms | 35.0 ms | **1.51x** |
| `micro_read` | `sum += item i of L`（i は 1..1000 を巡回） | 50000000 | 91.0 ms | 180.0 ms | **0.51x** |
| `micro_write` | `replace item i of L with k` | 50000000 | 114.0 ms | 135.0 ms | **0.84x** |
| `micro_append` | `add k to L`（空リストから n 個） | 20000000 | 180.0 ms | 25.0 ms | **7.2x** |

1 反復あたりに直すと、`micro_loop` は TurboWarp 1.1 ns・xyo 0.7 ns、
`micro_append` は TurboWarp 9.0 ns・xyo 1.3 ns。

TurboWarp のコンパイル結果は V8 の最適化 JIT に渡り、変数は Smi（整数）
として扱われ、リストは生の JS 配列になる。xyo も JIT 最適化でループ内変数を
**f64 のままレジスタに載せた**ので、純粋な数値ループでは逆に速くなった
（`micro_loop` 1.31 倍）。差が残るのはリストアクセスで、V8 は
「整数 1 回の境界検査 + 要素ロード」、xyo は「f64 の境界検査 2 回 +
変換 + 要素のタグ検査」を払う。

一方 **append は xyo の SList が圧倒的に安い**（8.2 倍）。TurboWarp の
コンパイル済み `add to list` は 1 回 8.2 ns かかる。このため「データ生成を
計測区間に含めるか」で `sieve` の勝敗が入れ替わる。本ベンチは他の
アルゴリズムと条件を揃えてデータ生成を計測外にしたので、`sieve` は
読み書き中心の比較になり TurboWarp が勝つ。フラグリストの構築も
計測区間に含めると append 8.2 倍がそのまま効いて xyo が勝つ（初版はそう
だった）。

ではなぜ `fib` / `sha256` で xyo が勝つのか。

- **`fib` はほぼ全部が再帰呼び出し**（fib(31) で約 430 万回）。xyo の JIT は
  手続き呼び出しをネイティブ呼び出しにするので、TW のコンパイル済み
  関数呼び出しより 1 回あたりが安いと考えられる。ループの少なさが効いて
  1.58 倍。
- **`sha256` は呼び出しと算術が主体**。1 ラウンドにヘルパ手続き呼び出しが
  十数回、その中身は 4 項の多項式（division/modulo/乗算）で、LLVM が
  レジスタに載せて最適化できる。リスト参照（XOR 表）は 1 演算あたり 4 回で、
  全体に占める割合は小さい。結果として 1.43 倍。

`quicksort` が `mergesort` より差が小さい（0.73x vs 0.47x）のも同じ理屈で、
quicksort は比較と再帰が多く、mergesort は要素ごとのリスト読み書きが
多いため。

## 最適化（計測後に直したもの）

上の計測で「ループとリスト読み書きが遅い」と分かったので、JIT の codegen
（`src/jit/codegen.cpp`）を次のように直した。

1. **タグ変換の fast path** — `toNum` と比較の入力が「素の IEEE double」
   なら、true/false/NaN のタグ変換カスケードを飛ばす。判定は符号付き
   `bits < kNan`（負の double も含む）。比較だけは **kNan を fast path から
   外す** — Scratch は NaN 同士の比較を文字列比較に落とすため。
2. **`change` の canonicalize 省略** — 加算が有限定数で、`toNum` が NaN を
   返さないので `numCanonical` の `fcmp`+`select` を省く。
3. **リストヘッダの hoist** — `Variable::list` のロードと、ループ内で
   resize しないリストの `len`/`items` を preheader で一度だけロードする
   （毎アクセスの 3 段依存ロードを 1 段に）。
4. **ループ内スカラー変数のレジスタ昇格** — ループ本体（と条件）の変数
   アクセスがすべて JIT の inline 経路で完結するループでは、値を
   alloca（mem2reg で SSA レジスタ）に載せる。手続き呼出し・broadcast・
   wait・stop・compat・sensing_of・append を含むループは対象外。yield の
   前後だけ実メモリへ spill/reload するので、他スレッドから見た値も
   64 反復以内で一致する。
5. **yield 判定の巻き上げ** — `isWarp` をループ入口で一度だけロードし、
   `yieldCheckN` もレジスタに載せてループ脱出時だけ ctx へ書き戻す。
6. **数値影（numeric shadow）** — 昇格する変数を `(bits, d = toNum(bits))`
   の対で持ち、**d を f64 のまま XMM レジスタに置く**。数値としての読み出しは
   `d` をそのまま返すので、タグ検査・NaN 正規化・GP↔XMM 移動がすべて消える
   （従来は読み出しごとに `cmpq`/`jge` + `vmovq` + `vcmpsd` が入り、
   ループキャリーが 6〜8 サイクルだった）。`bits` は常に正準な Value
   （文字列のまま代入された値もそのまま）なので、`join` など Value としての
   読み出し・yield 時の spill・ループ脱出時の書き戻しは従来どおり厳密。
   `change` は有限定数デルタなら `d + delta` をそのまま `bits` にも写す
   （NaN は生じない）。これで `micro_loop` が 143 → **39 ms** になった。
7. **分岐重み** — タグ検査・yield・リスト境界の分岐に `branch_weights` を
   付け、LLVM に高速側を fall-through させる（遅い側を関数末尾へ）。
8. **リスト索引の素通し** — 索引が数値影や数値リテラルなら、`emitListIdxAsF64`
   のタグ検査と分岐を丸ごと省く。
9. **比較も数値影を素通し** — `emitCmpNumF64` が数値影の f64 をそのまま
   使う。`repeat until i > j` のような昇格変数同士の比較からタグ検査が消える。
10. **yield 判定を 1 分岐に** — ループ入口で `mask = isWarp ? 63 : 0` を
    作る。`(++counter & mask) == 0` が「yield する」判定になり、非 warp は
    mask=0 で毎反復 true（従来と同じ）。ループ不変な isWarp の分岐が
    毎反復から消え、条件分岐が 2 → 1 になる。
11. **数値リテラルとの比較の畳み込み（相手が数値のときだけ）** —
    `i > 1000` のリテラル側は文字列 "1000" なので、素朴には毎回 numState
    のロードと分岐が要る。相手オペランドが数値影・既知数値・数値定数の
    いずれかなら、cast::compare は必ず数値比較になるので、リテラルを
    コンパイル時に数値へ畳める（空白のみの文字列は `cast::isWhiteSpace`
    で除外）。`micro_read` の `if i > 1000` から ~8 命令が消えた。
11. **`change` の正規化省略**（有限定数デルタ）は 2 の一般化。

**不採用（実測付き）**:

- 数値リテラルとの比較の**無条件**畳み込みは `literal context` の
  219,850 checks のうち 5,843 件を落とした（リテラル側の `numState` 経路が
  空白文字列を state 3 として正しく扱うため）。「相手が数値のときだけ」に
  限定した 11 の形なら全テストが通る。
- **リスト読み書きの専用高速経路（4 案、すべて不採用）**:
  (a) 昇格リスト用に null 検査を省きヘッダを preheader で固定する案 — read 183 → **238 ms**
  （ヘッダを GP レジスタに常駐させたことでレジスタ圧が上がり、影の XMM が
  スタックへ spill した）。(b) 索引の bits で整数境界検査 — 数値影の `bits` は
  `set i to 1` のように**文字列**のことがあるため不正（quicksort が無限再帰 →
  スタック溢れで SEGV）。(c) `fptosi(d)-1` の整数境界検査 — 範囲外の `fptosi` は
  LLVM IR で poison になり、それを使う比較は UB（同じく SEGV）。(d) yield 呼出しを
  `preserve_all` 規約にして影の XMM を保つ案 — 効果なし（spill の原因は呼出しでは
  なかった）。
- **ループ本体の実測内訳（診断）**: yield 検査を丸ごと外すと read 180 → **159 ms**、
  write 137 → **113 ms**、`change i by 1` ループ 35 → **22 ms**（TurboWarp 53 ms の
  2.4 倍）。つまり yield は 1 反復あたり 1〜2 サイクル。残る差の主因は、ループ
  運搬値（`sum` の影）が **16 バイトの `vmovapd` でスタックへ store され、スカラ
  `vaddsd` で読み戻される**ことによる store-forwarding の遅延で、IR 上は SSA の
  phi なのにレジスタ割当がメモリに落としている（IR にメモリ操作は無い）。これを
  レジスタに載せられれば read はもう一段速くなる見込み。
- `toNum` の fast path を `d == d`（ord）で判定する版: read が 180 → 192 ms
  に悪化（FP 比較は分岐解決が 2〜3 サイクル遅く、ループのキャリー依存に
  乗る）。
- fast path を `bits < kNan`（kNan を除き NaN 正規化を省く）版: read が
  180 → 200 ms に悪化。`bits < kFalse` は kTrue/kFalse と同じ 0x7FF8 系列で
  LLVM が movabs + leaq を共有できるため、命令数が減っても遅くなる。

同じベンチマークでの前後（xyo JIT、ms）:

| アルゴリズム | 最適化前 | 後 | 改善 |
|---|---:|---:|---:|
| `quicksort` | 60 | 46 | −23% |
| `mergesort` | 114 | 85 | −25% |
| `bubblesort` | 183 | 123 | −33% |
| `sieve` | 37 | 21 | −43% |
| `fib` | 43 | 36 | −16% |
| `sha256` | 64 | 61 | −5% |
| `micro_loop` | 161 | 35 | **−78%** |
| `micro_read` | 285 | 180 | −37% |
| `micro_write` | 247 | 135 | −45% |

検証: `ctest` 6/6（`literal context` 219,850 checks、`e2e` 99 checks、
parfor/pen/minify/deps 等を含む）、scratch-vm 差分 repro 19/19（HotSpot と
インタプリタの両方）、本ベンチの全 checksum 一致。

**まだ残っている差**はソート系の 1.4〜2.2 倍で、いまはほぼ
**リストアクセス**に集約されている。内訳は (a) 索引の境界検査が f64 比較
2 回（V8 は整数比較 1 回）、(b) リスト要素の読み出しにタグ検査が入ること、
(c) データ依存の if/else 連鎖の分岐予測。純粋な数値ループは逆転したので、
次の一手はリスト側 — 要素の型を投機するか、索引を整数のまま持ち回るか。

### なぜ純粋な数値ループが逆転したのか

最適化前の `micro_loop` は 1 反復 16 命令・条件分岐 5・無条件ジャンプ 2 で、
ループキャリーが `vmovq → vaddsd → vmovq` の 6〜8 サイクルだった。
数値影にした後は 1 反復 8 命令・分岐 3・キャリーは `vaddsd` の 4 サイクル
だけで、TurboWarp（V8 の Smi 演算 + 4 倍アンロール）より速い。

## インタプリタ同士

xyo interp と scratch-vm interp は、ソート系ではほぼ互角（xyo が 0.8〜2% 速い。
quicksort 3189 vs 3259 ms、mergesort 7728 vs 7794 ms）。
`fib` は TW が 1.28 倍速く、`sha256` は TW が 3.5 倍速い。

SHA-256 は 1 ラウンドに数万回の `item of list`（XOR 表）を踏む。xyo の
インタプリタは 1 回ごとに Value のタグ検査とインデックス変換をするため、
リスト参照の多いコードで差が開く。JIT ではこれが逆転する（xyo が 1.42 倍
速い）のが面白いところで、**xyo の JIT は自分のインタプリタ比で 260 倍**、
TurboWarp のコンパイラは 53 倍になる。逆にソート系では TurboWarp の
コンパイラの倍率が大きい（quicksort で 102 倍 vs xyo 61 倍）。

## 再現

```sh
python3 tests/bench/algos/bench.py                 # 全モード（約 7 分）
python3 tests/bench/algos/bench.py --skip-interp   # JIT と TW だけ（約 30 秒）
python3 tests/bench/algos/bench.py --micro         # 対照実験つき
python3 tests/bench/algos/bench.py --quick         # 小さいサイズで動作確認
```

生のサンプルは `tests/bench/algos/results.json`（`--micro` は
`results-micro.json`）に、アルゴリズムごとの期待チェックサムは
`tests/bench/algos/out/manifest.json` に入る。`.sb3` 自体も
`tests/bench/algos/gen_algo_bench.py` で生成できる。

## 限界

- 6 アルゴリズム・1 つの CPU・1 つの Node 版での結果。特に
  「TurboWarp が 1.6〜2.5 倍速い」は**ループとリスト読み書き**に限られ、
  呼び出し・算術・append 中心のコードでは xyo が勝つ。
- 計測は `days since 2000` の ms 精度。TurboWarp 側は 1 ms 量子化なので、
  数十 ms の測定では ±1〜2% の不確かさがある（サンプル中央値で緩和）。
  TurboWarp compiled の実行間変動は ±10〜20% 程度。
- データは固定シードの LCG（0〜65535）で、ソートの最悪ケース
  （整列済み入力など）は測っていない。
- シングルスレッドの比較。xyo の並列スケジューラは 1 スクリプトでは
  何も並列化しない。
