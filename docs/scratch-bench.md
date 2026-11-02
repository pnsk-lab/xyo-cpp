# scratch-bench で xyo を測る（`scratch-bench/`）

[gradylink/scratch-bench](https://github.com/gradylink/scratch-bench) のコピーを
`scratch-bench/` に置き、そこへ **xyo-cpp ランタイムを追加した**（`xyo-cpp` という
id で、他の VM と同じ `bun run bench --runtime xyo-cpp` で回る）。フォーク側の
説明は `scratch-bench/XYO.md`。

## ベンチの仕組みと、xyo 側に足したもの

ベンチ本体は sb3 で、各カーネルを **プロジェクト内の `days since 2000`** で
100ms × 6 ラウンド計測し、結果をステージの `output` リストへ 1 行ずつ
`append` する。ランタイム側の仕事はその行を stdout へ出すことだけ
（node アダプタはリストを 20ms ごとにポーリングして `[PROJECT]` を付ける）。

xyo には同じことを CLI でやらせた:

- `xyo run <sb3> --print-list NAME` — ステージのリスト `NAME` の未出力要素を
  **1 step ごとに 1 行ずつ** stdout へ出す。他の出力（ロード情報・終了行）は
  stderr へ退避するので、stdout はプロジェクトの出力だけになる。
  追記専用のログリストを想定（clear で縮んだ分は読み飛ばす）。
- `xyo run <sb3> --fps N` — 1 ステップを実時間 1/N 秒に合わせる。`play` の
  `--fps` と同じ意味。既定 0 = 最速（従来動作）。
- `xyo play <sb3> --print-list NAME` + `XYO_EXIT_ON_DONE=1` — 描画が要る
  パリティスイート用。SDL の `offscreen` ビデオドライバで X 無しでも同じ CPU
  レンダラが動く。アダプタは `--list-limit 200000`（Scratch のリスト上限）も渡す。

`--fps 30` が要る理由: scratch-vm / TurboWarp は 30fps で step し、1 フレームの
中では作業予算の範囲で何度もパスを回す。`sched_*` は「100ms あたりのループ
通過回数」を数えるので、最速で回る `xyo run` のままだと 1ms に数千 tick 進んで
しまい比較にならない。カーネル自身が計時するベンチ（warp 手続き内）は
ペーシングの影響を受けない。

アダプタ（`scratch-bench/src/runtimes/xyo.ts`）は xyo をソースからビルドして
（cmake + ninja、Release、JIT on）`.cache/runtimes/xyo-build/<hash>/xyo` に置き、
`XYO_JIT_MODE` を切り替えて **hotspot / interpreter / eager / AOT** を 1 つの
ランタイムの variant として測る（AOT は計測前に `xyo aot --obj` で
オブジェクトを作るので、コンパイル時間は run に入らない）。

```sh
cd scratch-bench && bun install
bun run bench --runtime xyo-cpp --runs 3                            # 4 variants
bun run bench --runtime xyo-cpp,turbowarp,scratch --runs 3          # 並べて比較
bun run bench --runtime xyo-cpp --no-variants                       # hotspot のみ
```

## 結果（i7-12700 / 20 threads / 63GB / bun 1.4.4 / LLVM 20 / 各 2 run の中央値）

score はサイトの重み付き幾何平均（高いほど速い）。

| runtime | overall | loops | vars | math | strings | lists | algorithms | apps | scheduler |
|---|---|---|---|---|---|---|---|---|---|
| TurboWarp (compiler) | 40,265 | 1,161,816 | 169,528 | 90,307 | 25,276 | 42,318 | 154,428 | 8,351 | 1,089 |
| **xyo-cpp (JIT)** | **26,213** | 819,060 | 126,867 | 55,123 | 8,341 | 30,004 | 131,911 | 5,629 | 737 |
| Scratch | 653 | 3,709 | 628 | 924 | 1,484 | 1,053 | 834 | 67 | 547 |

最初の測定（JIT の比較バグ修正直後）は overall 14,835 / strings 715 /
scheduler 93 / run 43.7 秒 / ピーク RSS 1,736MB。同じマシンで交互に測った
TurboWarp 比だと overall 0.57 → **0.65 倍**、strings 0.09 → **0.33 倍**
（このマシンはノイズが大きく、TW 自身も 5% 動く）。

TurboWarp の 0.36 倍、Scratch の 22.6 倍。内訳は一様ではない:

- **xyo が勝つ**: `algo_fib` 2.3×（JIT のネイティブ再帰 + 手続きフレーム省略）、
  `list_insdel` 1.9×、`algo_ack` 1.6×、`loops_repeat` 1.5×、`vars_setchange` 1.4×、
  `sched_fanwait` 1.2×、`sched_fanout` 1.2×、`sched_looptick` 1.2×、`math_trig` 1.2×。
- **xyo が負ける**: `str_join` 0.044×（不変文字列の連結 — rope で 26 倍改善済み、
  残る差は確保コスト）、`str_cast` 0.13×、
  `sched_threads` 0.24×、`math_arith` 0.24×（V8 の Smi 特化 + 4 倍アンロールに
  対して xyo はタグ付き i64 のまま）、`loops_empty` 0.35×、`sched_waituntil` 0.28×。

起動とメモリ: xyo-cpp 1,523ms / ピーク RSS 1,620MB、TurboWarp 290ms / 362MB、
Scratch 631ms / 442MB。起動時間は hotspot JIT が緑旗スクリプトを初回実行前に
コンパイルする分。RSS は 32 秒間の最大値（`str_join` が 1 ラウンドで 40KB の
文字列を 20,000 回作り、スケジューラ系は broadcast ごとに約 100 本の
クローンスレッドを作る）。

## 分かったこと（この 2 回の計測で直したもの）

1. **JIT の誤コンパイルを checksum が捕まえた**（修正済み）。
   ループ内変数の昇格は `(bits, d=toNum(bits))` の対を持ち、比較は `d` を
   そのまま使っていた。これが正しいのは値が数値のときだけ — 文字列を持つ
   昇格変数では `d = NaN` になり、`k_s == "Hello World"` が **false**、
   空白のみの文字列（`toNum(" ") == 0`）は数値比較になっていた。
   `math_strcompare` の checksum が 8000（正解 12000）、`app_brainfuck` が
   31000（正解 931125）で発覚。修正は変数ごとに「比較に d を使ってよいか」
   フラグ（`knownNumOk`）を書き込み側で更新する形にしたので、比較のたびに
   タグ検査を出す必要がない。修正後は決定論的な全ベンチで checksum が
   インタプリタ / TurboWarp / Scratch と一致（`sched_*` は時間依存なので
   一致しないのが正常）。
2. **`broadcast and wait` が 1 フレーム待っていた**（`sequencer.cpp`）。
   インタプリタは `stepThreadBound` を抜けるときに YIELD を RUNNING に戻す
   ので「このパスで動けるスレッド」に数えられるが、**JIT のファイバ yield は
   YIELD を立てたまま**なので、スレッドが 1〜2 本だと `numActiveThreads` が 0
   になりフレームが終わっていた。`fiber->resume()` の後に正規化しただけで
   `sched_pingpong` 0.03 → 382 ops/ms（**12,000 倍**）、`sched_fanwait` 3 → 984
   （TurboWarp の 1.2 倍）、scheduler カテゴリ 93 → 708。
3. **GC が毎回 8MiB の未使用スタックを走査していた**（`gc.cpp`）。
   保守的ルート走査は現 rsp を pthread スタック範囲へクランプするが、
   **ファイバ上の rsp はその範囲の外**なので、クランプするとメインスタックの
   未使用域まで毎回走査していた（collect 1 回 2.9ms vs インタプリタ 0.16ms、
   `str_join` は 1 ラウンドに約 400 回 collect）。スケジューラの退避 rsp から
   走査するようにし（ファイバ自身は `markRoots` が見る）、collect の下限を
   2MiB → 8MiB にして `str_join` の 20,000 連結が 1,256ms → 46ms。
   **さらにスケジューラが速くなって次の問題が出た**: `ownedThreads` は
   `startedThreads` の生ポインタ安定性のため何も解放しておらず、32 秒の実行で
   **1,006,615 個の Thread**（RSS 3.0GB）。完了スレッドはフレームを即座に返し、
   broadcast-and-wait が握っていないものは解放する（`JitCtx::joinRefs` +
   `Runtime::reclaimThreads`）ようにして 1.6GB に戻した。
4. **数値 → 文字列が最大 17 回の `snprintf("%.*e")` + `strtod` 往復**で最短表現を
   探していた（`x/4 ++ ""` が 1.25µs、V8 は約 50ns）。`std::to_chars` は Ryu で
   同じ桁を返す（Node の `String(x)` と 12 万件でビット一致を確認）ので、ES の
   整形はその桁から始めるようにした。
5. **不変文字列なので `++` を安くするには rope しかない — 実装した**。
   `str_join` は 209 → **12,514 ops/ms（60 倍）**。残った壁は文字列ではなく
   **確保**: `Gc::alloc` が毎回 2 つの `std::unordered_set` に挿入していた
   （要素ごとにノード malloc）のを、フラットなオープンアドレッシング
   `PtrSet`（挿入/参照のみ、sweep 後に作り直し）に置き換え、ページ集合は
   同じページの重複挿入を省いた → cons ノード 1 個 200ns → 約 80ns。
   `join(x, "")` はコピーせず `x` を返す（`x/4 ++ ""` の確保が半分）、
   `contains` は小文字化コピー 2 本をやめて ASCII をその場で畳む
   （419 → 1,089 ops/ms = TW の 0.81 倍）。strings カテゴリは 4.4k → 8.3k。

   `join` はコピーせず **cons ノード**（left/right + 合計 len）を返し、本文は
   最初の `view()`/`data()` で 1 回だけ materialize する（`SStr::consBody`、
   反復 DFS なので 20 万回連結しても再帰しない。CAS で publish するので
   並列ワーカーの競合も安全）。GC は子をマークし（これも反復）、sweep で
   body を free する。**`str_join` 209 → 5,401 ops/ms（26 倍）**。
   残る差は**確保コスト**: 20,000 回の連結が 4ms = 1 ノード約 200ns で、
   12 文字の flat 連結も同じ 200ns なので `Gc::alloc`（再帰 mutex + malloc +
   hash set 2 回）が支配している（V8 の bump 確保は約 6ns）。ここから更に
   10 倍にするにはアロケータ側（bump/arena）の仕事になる。
   **注意**: 文字列を受けるループ変数には f64 の数値影を付けないようにした
   （代入のたびに `toNum` = flatten が走り、rope の意味が無くなるため）。
6. **パリティスイートも回るようにした**（`xyo play --print-list` +
   SDL `offscreen` ドライバ + `--list-limit 200000`）:
   **2061/2123（97.1%）**。失敗は Loop Time 4/5、Warp And Recursion 7/9、
   Custom Blocks: Recursion 8/10、Fencing 18/20、If On Edge Bounce 15/16、
   Pen: Color Params 6/8、Touching Sprite 15/20、Color Touching Color 4/13、
   Looks: Layers 19/33 など — どれも「何が違うか」がそのまま出る。
