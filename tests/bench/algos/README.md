# アルゴリズムベンチマーク（xyo vs TurboWarp）

Scratch 3 のブロックだけで古典アルゴリズムを実装した `.sb3` を生成し、
同じファイルを xyo と `refs/scratch-vm`（TurboWarp 2.1.46）で実行して
**プロジェクト内で計測した実行時間**を比較する。結果と考察は
[`docs/algo-bench.md`](../../../docs/algo-bench.md)。

```sh
# 生成のみ（tests/bench/algos/out/*.sb3 + manifest.json）
python3 tests/bench/algos/gen_algo_bench.py

# 全モードで計測（TurboWarp コンパイラ / scratch-vm interp / xyo JIT / xyo interp）
python3 tests/bench/algos/bench.py

# 小さなサイズで動作確認
python3 tests/bench/algos/bench.py --quick --skip-interp

# ループ / リストアクセスの対照実験つき
python3 tests/bench/algos/bench.py --micro
```

## リスト読み出しを型別に比較する

[専用ランナー](<bench_list_read.py>)は、同じ／異なる数値doubleと数値文字列の4ケースを分ける。変更前・変更後・TurboWarpの実行順をrotateし、全checksumを独立した期待値と照合する。`--cpu`はLinuxでの同一コア固定、`--n`は1000の正の倍数、`--reps`はプロセス反復数（各プロセス内でも3回計測）。

```sh
python3 tests/bench/algos/bench_list_read.py \
  --before /path/to/old/xyo --xyo build/xyo --cpu 8 \
  --json bench-output/list-read.json

# 同じ値を再利用しないプロジェクトの比較
XYO_LIST_READ_MEMO=0 python3 tests/bench/algos/bench_list_read.py \
  --before /path/to/old/xyo --xyo build/xyo --cpu 8 \
  --json bench-output/list-read-no-memo.json
```

生成物は`out/list-read/`。計測結果のJSONは`.gitignore`対象で、`--json`に指定した場所へ保存される。考察・適用条件・回帰も含む記録は[リスト性能メモ](<../../../docs/list-performance.md>)を参照。

## ファイル

| ファイル | 役割 |
|---|---|
| `gen_algo_bench.py` | ブロックグラフ DSL + 各アルゴリズムの生成器 + Python 参照実装（checksum） |
| `run_tw.js` | TurboWarp / scratch-vm をヘッドレスで 1 プロジェクト実行し `times`/`check` を JSON で出す |
| `bench.py` | 生成 → 全モード実行 → 検証 → 表と `results.json` |
| `out/` | 生成物（`.gitignore` 済み。`manifest.json` に期待 checksum） |

## 計測方法

各プロジェクトは緑旗で:

1. データセットを **warp 手続き**で構築（計測外）。
2. アルゴリズムを 1 回ウォームアップ実行（JIT / TurboWarp のコンパイル費用を
   計測外へ追い出す）。
3. `repeat 3` で「データ構築（計測外）→ `days since 2000` で開始時刻 →
   アルゴリズム → 終了時刻」を繰り返し、`times` リストに ms を記録。
4. 結果を独立に検証（整列済み判定 + 総和 + ローリングハッシュ、 sieve は
   素数の個数と位置ハッシュ、 SHA-256 はダイジェスト）。

sieve のフラグリスト（2M 要素）も他のベンチのデータセットと同じく
**計測外**で構築する。リスト構築を含めると append の速度差（xyo が
8.6 倍速い）が混ざり、アルゴリズム本体の比較にならないため。

`days since 2000` は scratch-vm では `Date.now()`、xyo では
`std::chrono` の ms で、どちらも同じ意味論（ms 精度）。計測区間の外側の
コスト（ロード・コンパイル・データ生成）は含まれない。

`bench.py` は全モードの checksum が `gen_algo_bench.py` の Python 参照実装と
一致した場合だけ数値を採用し、一致しなければ `INVALID` と表示する。

## アルゴリズム

| 名前 | 実装 | 主な負荷 |
|---|---|---|
| `quicksort` | in-place Hoare partition、中央ピボット、再帰 | リスト読み書き・比較・再帰 |
| `mergesort` | 再帰 + 補助リストへのマージ | リスト読み書き・再帰 |
| `bubblesort` | 早期終了つき O(n²) | リスト読み書き・ループ |
| `sieve` | エラトステネス（0/1 フラグのリスト） | リスト書き込み・ループ |
| `fib` | 素朴な再帰 Fibonacci（明示スタック） | 手続き呼び出し・再帰 |
| `sha256` | FIPS 180-4 圧縮関数 | 32bit 演算・テーブル参照・ループ |

Scratch にはローカル変数もビット演算も無いため、再帰の途中状態はリストの
スタックへ退避し、SHA-256 の XOR は 64 KiB のバイト XOR 表（`x ^ y`）を
引く（AND は `(x + y - (x xor y)) / 2`、ROTR/NOT は算術）。これは Scratch で
ハッシュを書くときの標準的な方法。

## サイズ

| アルゴリズム | n | 意味 |
|---|---|---|
| `quicksort` / `mergesort` | 200000 | 要素数 |
| `bubblesort` | 5000 | 要素数 |
| `sieve` | 2000000 | 上限 |
| `fib` | 31 | fib(31) = 1346269 |
| `sha256` | 65536 | メッセージ長（バイト） |
| `micro_loop` / `micro_read` / `micro_write` | 50000000 | 反復回数（`--micro` のみ） |
| `micro_append` | 20000000 | append 回数（`--micro` のみ） |

## 生成コードの比較（V8 バイトコード / TurboFan / LLVM）

`dump_v8.js` は TurboWarp のコンパイラが実際に吐いた JS を取り出す
（`JSGenerator.testingApparatus` フックを使う）。V8 のフラグと組み合わせると、
Ignition バイトコードと TurboFan の最適化マシンコードが得られる。

```sh
node dump_v8.js out/micro_loop_50000000.sb3 v8out   # v8out/js_*.js に生成 JS
node --print-bytecode --print-bytecode-filter=fun0_bench_ \
     dump_v8.js out/micro_loop_50000000.sb3 v8out   # バイトコード
node --print-opt-code --print-opt-code-filter=fun0_bench_ --code-comments \
     dump_v8.js out/micro_loop_50000000.sb3 v8out   # TurboFan のマシンコード
```

xyo 側は AOT オブジェクトを逆アセンブルすると同じ粒度で比べられる。

```sh
./build/xyo aot out/micro_loop_50000000.sb3 -o app --obj v8out/micro_loop.o
llvm-objdump -d --no-show-raw-insn v8out/micro_loop.o
```

結果と考察は [`docs/v8-vs-xyo-codegen.md`](../../docs/v8-vs-xyo-codegen.md)。
