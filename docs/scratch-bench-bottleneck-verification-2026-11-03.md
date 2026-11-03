# ScratchBench残差の原因検証（2026-11-03）

## 結論

**JITからのC++ヘルパ呼び出しが多い**という仮説は、`algo_primes`、`app_raycast`、`str_join`、スケジューラの一部で支持された。ただしCPU仮想化の`VMEXIT`やV8式のdeoptimizationではない。LLVMで生成したnativeコードから同一プロセス内のC++関数へcallし、戻る通常の実行経路である。**call回数だけでなくcalleeの実際の仕事量が大きい。**

`math_arith`は反例。数値変換・等値判定ヘルパへの実行時呼び出しが計測上0回で、残差を「ヘルパ脱出が多い」だけでは説明できない。

## 測定環境と方法

- i7-12700 / Linux / Node v26.4.0 / TurboWarp 2.1.46 compiler / ScratchBench同梱の`performance/project`
- xyo-cpp: v0.5.0以降のローカル変更を含むRelease HotSpot JIT。ビルド`scratch-bench/.cache/runtimes/xyo-build/3e8f8d7d/xyo`（`XYO_DISABLE_RT_PROF=ON`）。
- `tests/bench/algos/scratchbench_probe.py`で既存のfractchソースを**複製**し、1カーネルだけ実行するSB3を生成（元のScratchBenchは変更していない）。
- 両VMで同じSB3を用い、計時はScratchプロジェクト内の`daysSince2000()`。ベンチのウォームアップ1回＋計測ラウンドを維持。
- 比較はそれぞれ3プロセス、実行順を反転させながら測定した中央値。TurboWarp側はNodeアダプタを直接実行（通常の`bun run bench`でTurboWarpのDONE捕捉が失敗することがあったため）。
- チェックサムを各条件で照合。真偽値置換は**各カーネルで当該変数が0/1に限られることに依存する**。一般的なScratchの等値比較と真偽変換は同値ではない。
- ヘルパ回数は別途`XYO_PROF=1`で採取。`xyo_truthy`、`xyo_to_num`は一時的に`XYO_RT_PROF`を入れて計数し、**計測後にソースを元へ戻した**。計時結果にはプロファイラ計測値を用いない。
- ヘルパ計数はプログラムの稼働時間で正規化していない。処理速度が違う条件同士の絶対回数をそのまま比較しない。

## 1. 等値判定が遅い：強い因果的証拠

### 素数判定

| `algo_primes` | xyo ops/ms | TurboWarp ops/ms | xyoの元比 |
|---|---:|---:|---:|
| 元の`k_n == 0` | 15,629 | 61,635 | 1.00 |
| `k_n == 0` → `!k_n` | **29,373** | 63,598 | **1.88倍** |

両者ともチェックサム`110`。ほかの対照実験（各3回中央値）では、`k_i % k_j == 0` → `k_i % k_j < 1`のみはxyoが15,684→15,882（約1.01倍）、`k_n == 0` → `k_n < 0.5`のみは15,684→17,907（約1.14倍）。剰余式よりループの終了条件側の等値判定が疑わしい。

- 元の診断実行: `xyo_eq` **20,130,552回**、`xyo_loop_yield` 369,692回。
- `!k_n`へ変更し一時カウンタを入れた別実行: `xyo_truthy` **27,004,024回**。その条件では`xyo_eq`の大量呼び出しは消滅。
- **修正点:** これは「native→C++呼び出しが約2000万回減った」証拠ではない。`xyo_eq`が`xyo_truthy`へ置き換わっており、**後者の方が軽いため速い**という説明が支持される。
- 実JITの最適化後IRには、元の`k_n == 0`で`@xyo_eq`へのslow path、書き換え後には`@xyo_truthy`へのslow pathが存在する。該当箇所の`numeric shadow`が安全に数値比較へ落ちず、汎用経路がホットになっている。

### レイキャスティング

| `app_raycast` | xyo ops/ms | TurboWarp ops/ms | xyoの元比 |
|---|---:|---:|---:|
| 元の`until rc_hit == 1` | 11,632 | 33,707 | 1.00 |
| `until rc_hit` | **21,612** | 32,488 | **1.86倍** |
| `if rc_rdx == 0`、`if rc_rdy == 0`だけを`!rc_rdx`、`!rc_rdy`へ | 11,599 | 32,654 | 1.00 |
| 上記全部 | 21,439 | 32,685 | 1.84 |

すべてチェックサム`24094`。主な重さは方向ベクトルと0の比較ではなく、**DDA探索の内側での`rc_hit == 1`** と特定できる。

- 元の診断実行: `xyo_eq` **19,114,836回**。
- `until rc_hit`へ変更し一時カウンタを入れた別実行: `xyo_truthy` **24,239,685回**、`xyo_eq` 936,057回。やはりnative→C++の総呼び出し回数が減ったとは言えない。
- `rc_hit`はカーネル内で0または1しか代入されないため、この**特定の**書き換えでは同じ終了条件。一般のScratchコードへ無条件適用してはいけない。

## 2. str_join：外部ヘルパ＋GCスラブ確保が支配的

- 診断実行: `xyo_join` **16,720,010回**、GC collect 167回（計測用プロファイラ有効時）。
- GCプールのON/OFFを実行時変数`XYO_GC_POOL`で交互に3回ずつ比較（Release、診断無効、チェックサム40000一致）:
  - **ON**: 中央値 **67,131 ops/ms**
  - **OFF**: 中央値 **17,558 ops/ms**
  - 約 **3.82倍**。通常のslab poolが大量のcons確保に極めて重要だと直接確認できた。
- `XYO_GC_DEBUG=1`で別途確認: `XYO_GC_THRESHOLD=8`MiBは416 collections、128MiBは19 collections。**どちらも`flatten=1`**なので、毎回の文字列コピーが原因ではない。
- GC閾値の診断無効3回中央値: 8MiB 67,164 / 32MiB 63,427 / 128MiB 53,312 / 256MiB 60,412 ops/ms。単にGC回数を減らしても速くはならない。閾値比較は実行順固定のため参考値。
- `"ab"`を20,000回追加する代わりに`"abababab"`を5,000回追加すると、xyoは66,325→292,285（約4.41倍）、TurboWarpは186,209→753,162（約4.04倍）。同じ出力文字列に対し確保・連結回数を減らした効果であり、xyo独自の問題だけではない。
- 既存のサンプリング`docs/string-profiling.md`でも`Gc::sweepSmallSlabs`、`Gc::alloc`、`Gc::allocSmallTls`に大量のself samplesがある。改善対象はcallee内の確保・GC走査・sweepであり、単なるcall命令の除去とは区別すべき。

## 3. math_arith：ヘルパ脱出仮説は支持されない

- `xyo_to_num`に一時カウンタを入れて単独実行したところ**0回**（`[fn]`集計に存在せず）、`xyo_eq`も0回。`xyo_loop_yield`は約950,972回。
- 最適化後の実JIT IRには`urem`、`srem`の整数高速経路と`frem`のフォールバックがある。静的に`xyo_to_num`の呼び出し箇所は残るが、**この実験では動的に通過しなかった**。
- ベンチのxyo約93,195 ops/ms、TurboWarp約214,470 ops/ms（それぞれ2回中央値）。剰余を`x - floor(x/m)*m`にする派生版はxyo約71,169 ops/ms、TurboWarp約95,894 ops/msとなり、**剰余処理を置き換えれば速くなるという単純な仮説は否定**。式自体の計算内容が変わるので、剰余コストが0だという証明ではない。
- 次は`LLVM JIT object`の逆アセンブルで、`idiv/div`、浮動小数変換、タグ付け、分岐、レジスタスピルのホットパスを測るべき。

## 4. スケジューラ：yieldの大量発生を確認、寄与率は未測定

- `sched_waituntil`: `xyo_yield` 1,555,300回 / `xyo_wait_yield` 1,539,881回。
- `sched_threads`: `xyo_yield` 2,336,624回 / `xyo_eq` 2,313,607回 / `xyo_wait_yield` 22,912回。
- いずれも計測用プロファイラ有効の診断実行。スイートの非warp条件・実時間制約が混ざり、**総処理量やフレーム配分を揃えた因果実験にはまだなっていない**。
- 特にRTProfの`yield`のinclusive時間はファイバが中断されていた時間も含み得るので、**「yieldが50秒消費」とは解釈しない**。

## 次の実装優先順位

1. **JITの`operator_equals`**: 数値`0/1`の頻出比較を、値の型・範囲についての厳密な証明か高速ガード付きで最適化する。意味が変わる無条件の`==0 → !x`は禁止。比較ヘルパ自身の文字列・数値変換キャッシュも検証。
2. **`xyo_truthy`**: 今回の改善後も大量に呼ばれる。特化した真偽値判定をIRへ直接下ろせる条件を追加できれば、さらに改善余地がある。
3. **`str_join`**: slab allocator・sweep・GCルート処理のプロファイルを継続。スラブプールを切ると3.82倍遅くなるので、既存のプールを維持したまま高速化する。
4. **`math_arith`**: ヘルパではなく機械語を計測。型変換ではなくIR/レジスタ割当・剰余分岐に注目。
5. **scheduler**: ファイバ切替数、実行待ちスレッド数、1フレームあたりの仕事量、条件再評価の頻度を別々に計測。

## 再現

```sh
# xyoの最適化済みReleaseバイナリを指定
XYO_PROBE_BIN="$PWD/scratch-bench/.cache/runtimes/xyo-build/3e8f8d7d/xyo" \
  python3 tests/bench/algos/scratchbench_probe.py make --cases primes,primes_truthy,raycast,raycast_truthy

XYO_PROBE_BIN="$PWD/scratch-bench/.cache/runtimes/xyo-build/3e8f8d7d/xyo" \
  python3 tests/bench/algos/scratchbench_probe.py run \
    --cases primes,primes_truthy,raycast,raycast_truthy --mode both --reps 3
```

検証ログ: `build/eq-probe.log`、`build/truthy-probe.log`、`build/raycast-probe.log`、`build/probe-prof-summary.log`、`build/probe-truthy-count-*.err`、`build/probe-num-count-*.err`、`build/join-gc-pool-ab.log`、`build/gc-join-20261103.log`。派生SB3は`build/scratchbench-cause-probes/projects/`。

**本節までの検証段階では本体ランタイムの高速化を新規採用していなかった。後続の実装は下記を参照。** 診断のための`rt.cpp`への一時カウンタは撤回済み。前ターンからの未コミット変更は維持した。


## 後続実装：数値文字列比較キャッシュのJIT利用

ボトルネックの確認を受け、`src/jit/codegen.cpp`の`numericCmpOk`を変更した。

1. 既存の`literalCmpNums`に登録されている変更不可の文字列リテラル（例:`"0"`,`"1"`）について、`cast::compare`で数値比較に使えることをコンパイル時に判断済みなら`ok=true`としてよい。これまでの判定はraw IEEE double以外をすべて拒否していた。
2. ループ昇格開始とyieldからの再読込では、`emitToNumF64`で数値キャッシュを用意した直後に、文字列`numState`が1または4なら比較用数値として扱う。未計算/NaN/空白等の非数値は従来どおり一般比較へ。これは`cast::compare`の数値比較条件に対応する。
3. 既知の`knownNumOk`を再利用し、通常の`storeNumericPair`では文字列状態を毎回チェックしない。元のScratchソースの等値比較を変更しない。

### 同一リリースビルドの交互A/B（各4回中央値、診断無効）

| 項目 | リテラル既知判定のみ（v1） | 昇格開始・再読込での数値文字列チェック追加（v2） |
|---|---:|---:|
| `algo_primes` | 33,142 ops/ms | **41,894 ops/ms** |
| `app_raycast` | 23,842 ops/ms | **25,558 ops/ms** |
| `math_arith` | 87,454 ops/ms | 88,075 ops/ms |
| `str_join` | 56,374 ops/ms | 56,909 ops/ms |

v1対v2では`algo_primes`で約1.26倍、`app_raycast`で約1.07倍。`math_arith`・`str_join`の小さな差はノイズの範囲。いずれも元のSB3から作った単一カーネルで、入力・チェックサム一致。`build/numeric-cached-ab.log` / `build/numeric-cached-ab.json` に全サンプル。

最終版と最適化前の厳密な交互A/B比較は末尾に記載した。TurboWarpには依然追いついていない。

`math_arith`の未解決差は引き続き整数剰余・算術命令と値の変換であり、本修正の対象外である。


### 最終版と最適化前Releaseの交互A/B（各4回中央値）

同一のRelease構成（`XYO_DISABLE_RT_PROF=ON`）のバイナリ`build/probe-release-baseline-xyo`と修正版で交互に測定。全カーネルで元のScratchBenchソースを維持し、チェックサム一致。

| 項目 | 最適化前 ops/ms | 最終版 ops/ms | 速度比 |
|---|---:|---:|---:|
| `algo_primes` | 14,909 | **41,910** | **2.81倍** |
| `app_raycast` | 11,158 | **25,542** | **2.29倍** |
| `math_arith` | 87,728 | 88,079 | 1.004倍（ノイズ域） |
| `str_join` | 56,755 | 55,489 | 0.978倍（ノイズ域） |

ログ`build/numeric-final-baseline-ab.log`、全行`build/numeric-final-baseline-ab.json`。

TurboWarpの以前の直接実行（1～3回）では`algo_primes`は約60,000～65,000 ops/ms、`app_raycast`は約33,000～34,000 ops/ms。**現時点では依然としてTurboWarpに負ける**（それぞれ概ね0.65～0.70倍、0.75～0.77倍）。

最終コードのCTestは6/6成功。`scratch-bench`のパリティは2122/2123（従来から残る`Motion: If On Edge Bounce`の15/16だけ失敗、最適化前と同じ）。ログ`build/numeric-cached-final-ctest.log`、`build/numeric-cached-final-parity.log`。実際の性能スイート全47項目は`build/numeric-literal-full-performance.log`を参照（数値リテラル高速化第1段のビルドでの計測）。

変更はローカルに残してあり、コミット・pushは実施していない。
