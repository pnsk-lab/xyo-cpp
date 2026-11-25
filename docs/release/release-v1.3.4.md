# v1.3.4 — 漸化式ループの行列累乗化と再帰手続きの自動 DP

v1.3.3 以降の 2 つの自動最適化を入れる。どちらも**意味論は変えない**
（条件を満たさなければ従来の逐次実行へフォールバックする）。

## matpow — 線形漸化式ループを O(log N) へ

warp 手続き内の `repeat` / `repeat until` / `while` の本体が「状態変数の
affine（一次）更新」だけで書けるなら、1 反復を拡大状態ベクトル `[v;1]` への
行列掛けとみなし、反復を **行列累乗** に置き換える（`src/jit/matpow.*`、
`xyo_matpow`）。

- `set a to b; set b to a+b`（fib）→ 3x3、`change x by 3` → `[[1,3],[0,1]]`。
  **負の係数**（`change x by -3`、引き算）も可 — 範囲証明を
  `W(2t)=max(W(t), |A|^t·W(t))` の厳密な上界ラダーにしたので、係数行列が
  非負である必要はない。
- 内側 `repeat` は (a) 定数回 → 内側行列の t 乗、(b) 回数が affine かつ
  本体が定数加算だけ → `A^t = I + t(A-I)` で affine のまま畳む。
- `floor`/`ceiling`/`round` は範囲証明が整数性を保証するので恒等として扱う。
- `repeat until` / `while` は条件が単調な affine 比較（`<`/`>`）なら
  停止反復数を**二分探索**して行列累乗を適用する。
- **f64 で逐次と bit 一致**: 状態変数が有限・整数・|v| ≤ 4.5e15・-0 でない
  ことを確認し、`stmtBound · max_e |A|^e·|x0| < 2^52` を証明できたときだけ
  適用する。JIT が数値リテラルの影を文字列で持つ性質に合わせて「1 反復の型
  写像」も trip 回合成し、文字列が残るなら逐次へ落とす。
- legacy codegen と MLIR（既定の Mega 経路、`xyo.matpow.try`）の両方。
  `XYO_MATPOW=0` で無効、`XYO_MATPOW_DEBUG=1` で受理/拒否理由を表示。

実測: `repeat(2e9){ change x by 3 }` 536 ms → **37 ms**、`repeat(2e8)`
152 ms → 38 ms、`repeat until i < 1 { change i by -1; change s by i }`
（i=1e6、逐次 1e6 反復）が O(log N) で一瞬で終わる。

## DP — 純粋な再帰手続きの自動メモ化

ロード直後に reporter 手続きを走査し、条件を満たすものをトップダウン DP
（メモ化）へ書き換える（`src/load/dp_transform.*`）。

- 条件: warp 定義・引数 1 個・本体が純粋（変数/リスト・sensing/motion/looks/
  sound/pen・乱数・broadcast・clone・wait/stop を使わず、数値演算と制御構文
  と自己再帰だけ）・値付き `return` がある。
- 入口に `i = item # of n in __xyo_dp_keys; if i > 0 then return vals[i]`、
  各 `return e` を `r = e; if len(keys) < 8192 { add n to keys; add r to vals };
  return r` に置換。隠しリスト/変数はスプライトローカルで、**ファイルは
  書き換えない**（`XYO_DP=0` で無効）。
- 文字列を作る op を禁止しているのは、キー照合が `item # of`（数値等価）で
  `"01"` と `1` を同一視するため。数値だけなら等価 ⇒ 同じ `toNumber` ⇒
  同じ結果なので安全。

実測: fib(30) 159 ms → **39 ms**、fib(32) 334 ms → **40 ms**、
fib(36) 2134 ms → **43 ms**（n を増やしてもほぼ一定）。

## 修正

- **MLIR ビルダの `terminated_` スコープ**（`src/mlir/scratch_build.cpp`）:
  分岐本体内の `return`/`stop` が「この鎖の後ろは実行されない」フラグを
  外側へ漏らしており、`if <cond> then return x` の**後ろに続く文が丸ごと
  消えていた**。`emitSubstack` で save/restore するよう修正。
- `xyo_matpow` の宣言を実 ABI（cond/cmp/diff 付き 14 引数）に一致させた。

## 検証

- `tests/unit/test_matpow.cpp`（115 checks）と `tests/unit/test_dp.cpp`
  （34 checks）: 検出の受理/拒否と実走の値一致。
- 全ユニット: 既知の pre-existing 失敗（`test_e2e.cpp` 4 件、
  scheduler の fanout 1 件）のみ。
- `tests/mlir/xcheck.py` 45/45、`tests/mega/run_mega_suite.py` の
  パリティ/fixture/paired は全 PASS。

## 既知の制限

- interpreter は再帰呼び出しが 2 か所ある reporter 手続き（`f(n-1)+f(n-2)`）を誤る
  （MLIR 経路は正しい）。DP 変換後も同じ経路で走るため、interpreter で
  fib 形を動かすと値が違う（本リリース前からの挙動）。
- matpow は warp 実行中のみ・AOT/`--block-count` では無効（par-for と同じ
  ゲート）。
