# MLIR 最適化の現状（Round 235 時点）

## 検証済みの改善（controlled A/B＋IR＋チェックサム＋gates）

| 施策 | 効果 | 検証方法 | 状態 |
|------|------|----------|------|
| box-repeat | sort 1.8x、sieve 1.1x | GUARD on/off A/B (3x交互) | ✅ 完了・有効 |
| box-while | collatz 5.9x | GUARD on/off A/B | ✅ 完了・有効（既存実装の検証） |
| hoisting（純粋式＋item、選択的 bail、拡張 read、post-hoist CSE） | sort +15% | 3x交互A/B＋IR (11→7 items) | ✅ 完了・有効（sort型に特化） |
| ULE inrange | 簡素化（1命令化） | 等価性証明＋gates | ✅ 完了・無害 |
| intValued＋NaN skip | list_read ミニ +6〜26% | 複数回交互A/B | ✅ 完了・ミニのみ有効（suite では不発） |
| 手動 unroll | +20〜55% | controlled A/B（変動添字でも再現） | ✅ 方向付け確定・自動化は未着手 |

## 検証ゲート（全て緑・継続確認済み）
- xcheck 45/45（box 5種＋0/負/大 trip 含む、guard/int 発火も検証。chunked warp math の回帰を追加）
- differential 19/19（Mega/HotSpot/interpreter の3モード）
- unit 既存 7件のみ（pre-existing、他エージェント領域または既知 issue）
- 主要ミニ全てチェックサム一致（正しさ維持）

## 残件と優先順（証拠ベース）

### P0（正確性）
- **解決済み（Round 230）**
  - `expsqrt` 727708→718102: 64 反復チャンク展開が `numShadow_`（to_num の f64 影）を
    コピー間で共有していた。同じ基本ブロックへ 64 コピーを並べるためブロック判定の
    キャッシュが効いてしまい、2 コピー目以降が先頭反復のカウンタ値を掴んでいた。
    コピーごとに `numShadow_.clear()`（trig も同時に解決）。
  - `trig` 416980→421876: `atan(i/100)` の除算が整数昇格され `div.i`（切り捨て）で
    計算されていた。`operator_divide`/`xyo.div` を整数昇格の対象から除外
    （`blockIntExpr`・`evalIntValue`・`cleanProvable`・`intExprProvable`・in-place i64 化）。
  - スイート HotSpot の SIGSEGV: `hoistIfDuplicates` が重複 op の削除判定で
    入れ子 region 内の use を見ておらず、さらに use の差し替えも入れ子 region を
    見ていなかった。6018 op の sched スクリプトで未定義値参照→fallback 経由で
    部分 lowering モジュールが materialize されていた。入れ子 region を再帰走査して
    use を差し替え・未使用判定するよう修正。これでスイートが完走（チェックサム全一致）。

### P1（効果大・要大規模実装）
- **item fast path 抜本**（推定 2〜5x）: list_read/list_replace 0.2x の主因。item 読み＋k_acc boxing＋warp_yield が支配的。index  int 化では届かない（Amdahl 則で確認）。要 lowering 書き換え＋perf tools での検証。
- **自動アンロール**（推定 1.2〜1.5x）: 手動で +20〜55% を実証（変動添字でも有効、チェックサム一致）。実装に必要な要素を特定済み（§64.41/64.44）: loop body 複製基盤（新設、~100行）、trip 調整、PHI 連鎖、yield/inrange 複製、ヒューリスティクス、box 版との相互作用。単独ラウンド規模を超えるため別途計画が必要。

### P2（効果中・要慎重検証）
- **d-only 運搬**: per-iter box.f 除去で +5〜10% と推定。過去に hang の history あり。要慎重な検証（suspend/resume の正しさ）。
- **ネストループ対応**: sieve/matmul/primes の外側ループが未昇格（Round 233 で原因を `scanBlock` の region 打ち切りと特定、下節に設計）。legacy 比 sieve 0.43x の主因。
- **flow-sensitive intValued**: suite での NaN skip 発火に必要（現状はミニのみ）。到達定義解析 or fill パターン認識。設計済み（§64.45）。

### P3（調査・要 perf tools）
- **スイート文脈効果**: exact-match ミニ（同一コード＋データ、checksum 一致）で parity（0.96x）を確認。一方 suite では 0.22x。GC/モード/予算/fps/box/folding/コード差分/データ差分/ハーネス/単一カーネル文脈/thermal/コア割当では説明できず、I-cache 圧力・allocator・分岐予測・OS スケジューリングのいずれかと推定（要 perf tools、本環境では不可）。

## バッチ yield（Round 232、`XYO_MLIR_NOBATCH=1` で A/B）

- 昇格ループ（`xyo.warp_yield`）の per-iteration 検査を、64 反復ごとの
  バッチ境界 1 回にまとめた。本体は 1 コピーだけ lower し、内側に
  `batchPhi` のループを持つ（チャンク展開の 64 コピーはしない）。
- カウンタは「直前位置に step を足して 64 の倍数を跨いだら検査」で、
  step==1 のとき従来の `(n&63)==0` と厳密に同値。端数バッチでも
  跨いだときだけ検査する。
- 安全条件: 本体内に **中断点が無い** こと。具体的には (1) 入れ子ループ
  なし、(2) 葉 op は演算・変数・比較・リスト読み/置換・warp_yield の
  許可リストのみ（`xyo.call` / `xyo.compat` / `xyo.loop_yield` / 待機系は
  除外）、(3) リージョン付きは `xyo.if` のみ、(4) warp_yield は 1 個で
  yield の先頭に 1:1 で並ぶ（bail-if 越しの場合は then 側で確認）。
  これは「バッチ内では他スクリプトへ実行が移らない」＝昇格値の
  spill/reload 契約が崩れない、という条件。
- 制御された A/B（同一バイナリ・3x 交互、チェックサム一致）:
  - `sorth`（suite と同一カーネル）: 259,627 → **335,473 ops/ms（+29.2%）**
  - `lrdh`（list_read）: 675,299 → **889,299 ops/ms（+31.7%）**
- スイート（hotspot、単発 runs=1）: `algo_sort` 268K→334〜355K
  （legacy 比 0.59x→0.74〜0.79x）、`list_read` 114K→121〜140K。
  単発のスイート値は ±10〜15% 揺れるので、効果の主張は上記の
  controlled A/B を根拠にする。

## 同一バイナリでのスイート A/B（2026-11-22、hotspot、runs=1）

| 部門 | MLIR | legacy | 比 |
|------|------|--------|----|
| overall | 319,875 | 154,022 | 2.08x |
| Loops & control | 599,564,635 | 163,290,154 | 3.67x |
| Variables | 123,707,475 | 1,440,890 | 85.9x |
| Math & comparison | 1,161,768 | 178,635 | 6.50x |
| Strings | 201,327 | 76,679 | 2.63x |
| Lists | 100,286 | 119,151 | 0.84x |
| Algorithms | 125,482 | 219,070 | 0.57x |
| Applications | 8,409 | 16,856 | 0.50x |
| Scheduler | 1,719 | 1,864 | 0.92x |

- 大幅な上振れは **定数ループのコンパイル時畳み込み**（`xyo.repeat` の trip と
  入力が全て定数で副作用なし → 最終値を literal 化）による。チェックサムは
  全 47 ベンチで一致するので意味論は保たれている（`XYO_MLIR_NOCONSTLOOP=1` で
  legacy 同等の ops に戻ることを確認済み）。ハーネスの ops は
  「カーネルが実行したはずの演算数」なので、畳み込めた分だけ throughput が
  上がる。残る弱点は Lists 0.84x・Algorithms 0.57x・Applications 0.50x。

## 中央値・勝数について
- r22-current: 中央値 0.89→1.00、勝数 18→21（参考値）。
- ただしスイート間比較は時間的交絡（±10% ノイズ、数時間離れた実行）を含むため、小効果の主張には使えない。信頼できるのは controlled mini A/B のみ。

## Algorithms の遅さの切り分け（Round 233）

### まず yield を疑うのをやめた（診断）

同一バイナリ・3x 交互・スイート全体（hotspot, runs=1）で legacy / MLIR /
MLIR+`XYO_MLIR_NOYIELD=1` / MLIR+`XYO_MLIR_NOBATCH=1` を測った（sieve の
チェックサムは全構成で 669 一致）:

| カーネル | legacy | MLIR | NOYIELD | NOBATCH |
|---|---|---|---|---|
| algo_sieve | 196,672 | 83,882 (0.43x) | 90,323 (0.46x) | 80,894 (0.41x) |
| algo_matmul | 363,084 | 192,559 (0.53x) | 201,206 (0.55x) | 160,226 (0.44x) |
| algo_primes | 55,386 | 27,140 (0.49x) | 33,002 (0.60x) | 26,499 (0.48x) |
| algo_sort | 444,123 | 324,110 (0.73x) | 384,450 (0.87x) | 234,133 (0.53x) |

- **sieve は yield が主因ではない**: 検査を丸ごと外しても +7.7% だけ。
  （sort は +18.6%、list_append は +15% と、バッチが効く形では yield が
  主因のまま。sieve は違う。）
- **原因は外側ループが整数昇格されていないこと**。`XYO_MLIR_DUMP` の IR で
  確認: `repeat 4999` の本体は `getvar k_i` → `xyo.list.item(ptr, value)` →
  `xyo.eq` → `getvar k_g` の汎用 Value 経路で、内側の `until` だけが
  `warp_yield kinds="iii"` の SSA 形式に昇格している。理由は
  `scanBlock`（`src/mlir/passes.cpp`）が `xyo.if` 以外のリージョン op を
  見つけると false を返すため（入れ子ループは本体走査の対象外）。
- その結果、1 反復あたり legacy が
  `cvttsd2si`→`sub`→`icmp ult`→item load→**bits 直接比較**なのに対し、
  MLIR は tag 検査＋範囲 fcmp＋`llvm.trunc.f64`＋`fptosi` で添字を検証し、
  **`xyo_eq` の呼び出し**（文字列比較込みの汎用等値）を通る。

### 入れた改善: 比較のインライン数値 fast path（`XYO_MLIR_NOINLINECMP=1` で A/B）

- `xyo.lt/gt/eq` の両辺が「素の double」（符号付き bits < kNan）なら、
  helper と同じ結果になるので `fcmp` に置き換える。NaN は kNan そのもの
  なので fast path に入らず helper に落ちる。`xyo_eq` は helper 先頭の
  「bits 一致 → 真」も足して厳密に同値にした。
- 効果（同一バイナリ・6x 交互・スイート、47 ベンチの中共値）:
  **中央値 +3.0% / 幾何平均 +4.5%**。Algorithms は幾何平均 **+10.8%**
  （`algo_fib` **+65.6%**、`algo_ack` **+19.5%**、`algo_sieve` +3.2%）。
  再帰系は 1 ノードごとの比較呼び出しが消えるため効果が大きい。
  退行は `sched_pingpong` -6.9% / `algo_primes` -5.3% / `algo_sort` -4.7%
  （昇格済みループでは分岐が増える分だけ不利）。
- ゲート: xcheck 45/45（eager/hotspot）、unit 全パス（mlir frontend 41 checks）。

### 残る本命: 入れ子ループを持つループの昇格

- 外側 `repeat` を昇格できれば、添字が i32 になり `xyo.list.item.i`
  （`(uint)(idx-1) < len` の 1 命令検査）と `xyo.list.replace.i` が使える。
  推定で legacy 同等まで詰まる（sieve/matmul/primes/life が該当）。
- ただし安全側の条件が要る: 昇格値は **入れ子ループが中断しうる間も
  メモリ上で正しくなければならない**。設計案は
  (1) 入れ子ループへ入る直前に外側の昇格変数を全部 spill、
  (2) 入れ子ループが運搬している変数は戻り値（block 引数）を新しい
  現在値として使う、(3) **運搬していない** 昇格変数は戻り時に
  型ガード付きで reload（他スクリプトの同時書き込みを潰さないため）、
  (4) それ以外の中断点（call/compat/待機）が入れ子内にあれば不適用。
  (3) が無いと「入れ子の中断中に他スクリプトが書いた値を、外側の
  SSA 値が巻き戻す」という意味論差が出る。

## 入れ子ループを持つループの昇格（Round 234、`XYO_MLIR_NONESTPROMOTE=1` で A/B）

Round 233 で「sieve の外側 `repeat` が昇格されない」原因は `scanBlock` が
`xyo.if` 以外のリージョン op を打ち切るため、と特定した。Round 234 で
実際に昇格させた。

### 方式: 昇格前の入れ子ループをブラックボックスとして包む

- 昇格順を **外側から** に変えた（新しい 2 段階ドライバ）。外側を先に昇格
  すると、内側はまだ昇格前＝実メモリを読み書きする形なので、外側は
  「入る前に全昇格値を実メモリへ spill → 抜けたら getvar + to_int/to_num で
  reload」で安全に包める。入れ子の中で中断が起きても、メモリの値は
  「入る直前の値」または「入れ子自身が書いた値」のどちらかになる。
- 対象は「入れ子の中に中断点（call/compat/待機/より深いループ）が無い」
  ループだけ（`plainNestedLoop`）。無いことを確認できないものは従来どおり
  昇格しない。guard/box の複製版も作らない（複製は入れ子も複製するため、
  spill/reload の対象 op をポインタで追えなくなる）。
- 入れ子が書く変数は整数化の対象から落とす（抜けた後の区間が不明）。
  読むだけの変数はそのまま昇格できるので、**添字とカウンタは i32 のまま**
  回り、`xyo.list.item.i`（`(uint)(idx-1) < len` の 1 命令検査）が効く。
- 内側ループの昇格は従来どおり（外側の昇格後も独立に走る）。外側の
  spill/reload は実メモリ経由なので、両者が干渉しない。
- 2 段階にしたのは、外側優先を全ループに適用すると無関係なベンチまで
 昇格順が変わって動くため。**入れ子を持つループだけ**先に処理し、
  残りは従来どおり内側から。

### 副産物として直した潜在バグ 2 件

1. `commitRepeat` が `xyo.to_num` に整数運搬の SSA（i32）をそのまま渡して
   いた。回数が整数昇格した変数の式になると不正 IR になり、ISel で
   `Cannot select: bitcast i32 -> f64` でプロセスが落ちる。`bitsOf` で
   box してから `to_num` するよう修正。
2. 昇格ループ内の入れ子ループ（`xyo.repeat` の回数など）は、Value を取る
   オペランドが in-place 整数化で i32 になっていることがある。入れ子を
   そのまま残す経路で box し直すよう修正（同上のクラッシュ）。

### 効果（同一バイナリ・3x 交互・スイート全体 hotspot）

| ベンチ | nest | nonest | legacy | nest/legacy | nest/nonest |
|---|---|---|---|---|---|
| algo_sieve | 100,960 | 88,159 | 190,895 | 0.53x | **1.15x** |
| math_mandel | 534,627 | 274,561 | 284,928 | 1.88x | **1.95x** |
| app_raycast | 29,725 | 22,803 | 27,931 | 1.06x | **1.30x** |
| algo_sort | 2,048,513 | 327,290 | 414,010 | 4.95x | **6.26x** |
| algo_primes | 27,346 | 28,374 | 50,727 | 0.54x | 0.96x |
| algo_matmul | 184,802 | 184,327 | 361,628 | 0.51x | 1.00x |

- 47 ベンチ: 中央値 **+0.8%**、幾何平均 **+7.0%**。最良は mandel +95%、
  sort +526%（定数ループ畳み込みが効く形になったため。ops の意味では
  数値が跳ねるがチェックサムは一致）、raycast +30%、sieve +15%。
  最悪は sched_fanwait -6.3%、primes -3.6%、ack -3.4%。
- **チェックサムは nest と legacy で全 47 ベンチ一致**（nonest と違うのは
  時間予算型ベンチで、速くなった分チャンク数が変わるため）。
- Algorithms の legacy 比（幾何平均）: nonest 0.68x → nest 0.94x。
  ただし sort の +526% は畳み込み由来なので、実力の改善は
  sieve +15% / mandel +95% / raycast +30% と読むのが正しい。
- ミニ（スイートと同じ sieve カーネルを切り出したもの）では
  149〜152K ops/ms で legacy 161〜163K の **0.93x**（変更前は 0.66x）。
  スイート内では 0.53x のままで、この乖離は既知の「スイート文脈効果」。

### ゲート
- xcheck 45/45（eager / hotspot）、unit 全パス、differential 19/19（3 モード）。
- ミニのチェックサム: sorth 150504、sieve 669、trig 421876、exp 一致。

## if/else の select 化（Round 235、`XYO_MLIR_NOIFSELECT=1` で A/B）

Scratch の if/else は「値を作るだけ」の形が多い。分岐の予測が外れると
1 回 15〜20 サイクル失うので、両側を計算して `select` にした方が速い。
legacy codegen は collatz の偶奇分岐を実際に select 化していた
（IR で `fmul 0.5` と `fmul 3` → `select`）。MLIR 側は `xyo.if` を
そのまま分岐＋phi に lower していたため、collatz で 8 倍の差が出ていた。

- 対象は「両分岐が安い純粋式だけ + yield」の `xyo.if`（`selectableIf`）。
  許可リストは算術・変換・比較・select・round/mathop の `.f` 系。
  除外: region 付き op、リスト読み書き、文字列生成、呼び出しの重い
  reporter、**整数除算/剰余**（0 除算は未定義なので投機実行できない）。
- 効果（同一バイナリ・3x 交互・スイート）: `math_collatz` **+159%**
  （98,736 → 255,875 ops/ms、legacy 比 0.11x → 0.34x）、`math_trig` +15%、
  `algo_sieve` +8%、他はほぼ中立（47 ベンチの幾何平均 +1.8%）。
  ミニでも collatz 99K → 254K（+156%）。
- ゲート: xcheck 45/45（eager/hotspot）、unit 全パス、differential 19/19、
  スイートのチェックサムは時間予算型を除き legacy と一致（collatz 3142 も一致）。

### 切り分けに使った道具

スイートの任意のベンチを 1 本だけ切り出す `build-mlir/tmp/trim_bench.py`
（`trim_bench.py <suite.sb3> <bench-id> <out.sb3>`）。bench_end の id 入力で
三つ組を特定し、そのコードをそのまま残すので、数秒で回せる再現になる。
ミニとスイートで legacy 側の数値が大きく違うベンチ（list_read など）が
あり、その差は「スイート文脈効果」として未解決のまま。

## Round 236: 試して採らなかったもの（記録）

if/else の select 化（前節）で collatz は片付いた。残りの大きい差に対して、
次の 3 つを実装して測り、**いずれも採用しなかった**（中立〜微減）。

1. **整数 mod の fast path**（`xyo.mod.f` で除数が整数定数なら
   `fptosi.sat` + `srem` + 床補正）。list_read の `i*7919 % 100000` を
   狙ったが、この形は**すでに整数昇格で `xyo.mod.i {subk=-2}` に
   なっていた**ため効果なし（3 回交互で有意差なし）。実装は撤回。
2. **abs/floor/ceil/sqrt の intrinsic 化**（`xyo_math_*` は std:: の
   薄いラッパなので IEEE 同一）。`math_expsqrt` は 53.8K → 47.3K と
   **微減**したため撤回（呼び出しの方が命令配置に有利だった）。
3. **`letter of` + 1 文字比較の融合**。融合自体は `xyo.cmp.guarded`
   経路でも発火していることを確認したが、`xyo.letterof` は**別 op として
   先に lower される**ため消えず、`to_num` も残る（str_letter で
   1 反復あたり letterof 呼び出し + to_num 呼び出しが残る）。
   本気で削るには **pass 側で融合**して `letter_eq` に置き換え、
   死んだ letterof/to_num を DCE させる必要がある（未着手）。

### 残りの差の内訳（ミニで再現、legacy 比）

| ベンチ | MLIR | legacy | 比 | 主因（推定） |
|---|---|---|---|---|
| app_brainfuck | 28.1K | 107.3K | 0.26x | 1 ステップあたり letterof 1 回 + letter_eq 6 回 + リスト操作。legacy は typed SSA で変換を挟まない |
| str_letter | 64.0K | 143.0K | 0.45x | letterof/to_num が融合後も残る（上記 3） |
| app_ast | 14.8K | 32.3K | 0.46x | 文字列 reporter の呼び出し |
| list_replace | 248.6K | 415.7K | 0.60x | `list.replace.i` の bounds/extra チェックと書き込み |
| algo_matmul | 198.9K | 332.6K | 0.60x | 内側 repeat の昇格とリスト書き |
| list_read | 100.5K | 138.9K | 0.72x | `to_int(list.item.i)` の毎反復変換 |
| math_expsqrt | 53.8K | 105.0K | 0.51x | exp/ln/pow10 がソフトウェア実装（legacy も同じだが変換が少ない） |

いずれも「legacy の typed SSA（数値と分かっている値の変換を省く）」と
「インライン文字列 fast path」の差で、1 ベンチあたり +20〜40% が見込める
が、共通基盤（値の型伝播）を入れる規模の作業になる。

## Round 237: CSE の鍵バグ（誤コード）を修正

**症状**: 入れ子ループ昇格を入れた `math_mandel` のチェックサムが legacy と
一致しない（1957 のはずが 732 / 556）。`algo_sort` も suite で 174055 に
なっていた（mini では一致するので気付きにくかった）。

**原因**: `cseKey`（`src/mlir/passes.cpp`）が属性を
`key += std::to_string(a.i); key += ':'; key += a.s;` としか連結しておらず、
**double 属性（`xyo.const.f64` の `d`）が鍵に入っていなかった**。
そのため同じブロック内の別値の `const.f64` が 1 個に畳まれ、
mandel の `(k_i*3-24)/12` が `…/0` に化けていた（IR で確認）。
`Attr::Kind` ごとに `d`（ビット列）も鍵に含めるよう修正。

- これで入れ子ループ昇格の結果も legacy と一致するようになったため、
  `XYO_MLIR_NONESTPROMOTE=1` の opt-in から**既定 on に戻した**。
- 副作用: `algo_sort` の「5x」は壊れた CSE が偶然作っていた畳み込みで、
  修正後は 0.75x。**Algorithms の実力値は 0.77x**（Round 232 の 0.57x から
  改善、内訳: fib 1.20x / ack 0.90x / sort 0.75x / primes 0.57x /
  sieve 0.52x / matmul 0.52x）。
- ゲート: xcheck 45/45（eager/hotspot）、unit 全パス、differential 19/19、
  スイート 47 ベンチの**データ系チェックサムは全て legacy と一致**
  （sched_* のみ時間予算依存でチャンク数が変わるため不一致）。
- スイート 1 回（hotspot）: overall 2.11x legacy / Loops 1.53x /
  Vars 13.1x / Math 210x / Strings 188x / Lists 0.58x / Algorithms 0.77x /
  Apps 0.43x / Scheduler 0.99x。

## Round 238: 「MLIR を既定にする」の評価（結論: 保留）

`isEnabled()` を既定 on にしてみて、実際のゲートを回した。切替自体は
1 行（`if (!e) return true;`）で、`XYO_MLIR=0` で戻せる形にしてあるが、
**既定化は保留**した。理由は下の 1 件の不一致。

### 良かった点（既定 on で確認）

| ゲート | 結果 |
|---|---|
| parity（Scratch Parity Test、レンダラ経路 `play`） | **2656/2656**（通る回では 100%） |
| differential（Mega / HotSpot / interpreter の 3 経路） | 19/19 一致 |
| mega suite（5 経路パリティ + paired bench） | **PASSED** |
| xcheck | 45/45（eager / hotspot） |
| unit | 全パス |
| performance suite | overall **2.11x** legacy（Lists 0.58x / Apps 0.43x が弱点） |

### 実プロジェクト（projects suite、6 本を実時間再生）

| プロジェクト | legacy | MLIR | 差 |
|---|---|---|---|
| Bloons TD | 10,393 ms | 10,702 ms | +3% |
| Doom v0.2.4 | 10,855 ms | 10,909 ms | +0.5% |
| Navier-Stokes fluid | 174,902 ticks | **231,656 ticks** | **+32%** |
| Linux 6.1.14-rv32ima | 64 ticks | 63 ticks | ±0% |
| SGBE Gameboy | 8,137 ms | 8,353 ms | +2.7% |
| （1312523445） | 334 ticks | 190 ticks | **-43%** |
| 合計 | 29,385 ms | 29,964 ms | +2% |

計算主体（流体シム）は大きく勝ち、レンダラ/リスト主体はわずかに負ける。
合計ではほぼ互角なので、既定化の是非は「弱点を許容できるか」の判断になる。

### 保留の理由: parity の 1 件が MLIR 経路で高頻度に落ちる

`Sound: Clone Volume` の Test 2（clone が volume を -10 して 90 になる）が
`got ''` で落ちる。直近の測定では **MLIR: 3/3 失敗**（それ以前は 2/5）、
**legacy: 0/6 失敗**。`volume` reporter 自体は単体プロジェクトで
legacy/MLIR とも同じ値（45）を返すので、clone + broadcast + wait_until の
順序に依存した差と見られる。原因特定まで既定化しない。

### 既定化するときにやること

1. 上記の clone/sound の差を直す（parity を MLIR で 10 回連続 2656/2656）。
2. Lists 0.58x / Apps 0.43x の底上げ（legacy の typed SSA とインライン
   文字列 fast path の移植）。もしくは「リスト主体のスクリプトだけ
   従来経路」という選択的既定にする。
3. 切替は `src/mlir/frontend.cpp` の `isEnabled()` の 1 行
   （`if (!e) return true;`）。`XYO_MLIR=0` が非常口。

## Round 239: MLIR を既定にした（Eager を除く）

### 有効化の条件（`src/mlir/frontend.cpp` の `isEnabled()`）

- **既定 on**。`XYO_MLIR=0` で従来 codegen に戻せる。
- **Eager ティアと Mega プリセットでは既定 off**（`--jit=hotspot` か
  `--no-mega` で有効）。Eager + MLIR は broadcast-and-wait で
  ハングする（`general_bcastwait` で再現）ため。
- `XYO_MLIR=1` を明示した場合はこのガードを無視する（ベンチ・検証用）。
- `xyo test` はブロック数カウントのため従来どおり対象外。

### 既定化にあたって直した 2 件

1. **サウンド音量の compat 経路**（parity `Sound: Clone Volume` の失敗）。
   sound の音量系は compat に回ると、オーディオ待ち（PROMISE_WAIT）後の
   戻り値が空文字になり、clone 内の `volume()` が '' になっていた。
   `sound_setvolumeto` / `sound_changevolumeby` / `sound_volume` を
   native op（`xyo.snd.setvolume` 等）にして compat を通さない。
2. **手続き内の stop all / stop this script / delete this clone**。
   MLIR 経路は呼び出し元までの unwind を実装していないため
   `general_stop_all_in_proc` で SIGSEGV していた。これらを含む
   スクリプトは `fail()` させて従来経路へフォールバックする。

### 検証（すべて既定 on の状態で）

| ゲート | 結果 |
|---|---|
| parity（2656 テスト、play/レンダラ経路） | **2656/2656 を 3 連続** |
| mega suite（5 経路 + paired bench） | **PASSED** |
| differential（Mega / HotSpot / interp） | 19/19 |
| xcheck | 45/45（eager / hotspot） |
| unit | 全パス |

### 性能（hotspot、チェックサム全一致）

| 部門 | MLIR | legacy | 比 |
|---|---|---|---|
| overall | 5.33e9 | 3.11e9 | **1.72x** |
| Loops & control | | | 1.24x |
| Variables | | | 10.4x |
| Math & comparison | | | 169x |
| Strings | | | 159x |
| Lists | | | 0.46x |
| Algorithms | | | 0.63x |
| Applications | | | 0.33x |
| Scheduler | | | 0.84x |

（Round 238 の 2.11x は CSE バグが偶然作っていた畳み込みを含む値。
修正後は 1.72x が正しい。）

### 実プロジェクト（projects suite、play 経路）

legacy 29,385 ms → MLIR 29,964 ms（+2%）〜 31,906 ms（+9%）。
計算主体（流体シム）は ticks +32%、レンダラ主体は数 % 遅い。
**レンダラ接続時の既定を従来に戻したい場合は
`play`/`gui` だけ `isEnabled()` で renderer を見て false にする**のが次の調整点。

## Round 240: MLIR-005（再帰コルーチン化 + fallback ゼロ）後の再測定

- **hotspot スイート交互 A/B（同一バイナリ、runs=1×3 交互、中央値）**:
  全 47 ベンチ幾何平均 **1.78x**（R239 の 1.72x と同水準 = 誤差内）。
  fib 0.50x / ack 0.50x / sort 0.34x / read 0.16x / collatz 0.08x が従来どおり
  の弱点で、今回の仕事による変動ではない。チェックサム差は
  `sched_*`（時間依存）のみ。
- **なぜ動かないか**: ScratchBench の headless variant は
  `XYO_JIT_MODE=hotspot` を明示するため Mega プリセットが外れる
  （`Mega requires the Eager LLVM tier`）。今回の再帰コルーチン化と
  0-fallback 化は Mega/eager 経路の機構であり、hotspot の数値には
  影響しない — 正確性と `xyo run` 既定経路の拡張が目的の変更。
- **Mega 直接測定（`XYO_JIT_MEGA=1`、eager、同一 sb3）**: MLIR vs legacy =
  **2.98x 幾何平均**（hotspot の 1.78x を上回る）。定数ループ畳み込み系が
  Mega 融合でさらに伸びる（loops_empty 242x、str_join 408x）。
  fib/ack は warp 再帰（`procSafeRecursive` でフレーム自体を emit しない
  plain 呼出し＋sync batch 参加）なので 0.5x 据え置き — コルーチン化は
  非 warp 再帰向けの機構。
- **微修正**: `xyo.call` の `rec` attr 付与は `coroMode_`（コルーチン候補）
  のときだけに限定。コルーチンにならないスクリプトでは push_ex/pop
  フレームは純粋なオーバーヘッドなので（hotspot では `coroCandidate` が
  `generalEnabled`/`enabled` に連動するため実質 no-op だが、非候補の
  Fiber 経路でもフレームを emit しない形に揃えた）。

## Round 241: ループ運搬の NaN 正規化とリスト読み（numFacts 解析）

### 実装したもの

1. **`numFacts` 解析（f64 の non-NaN / finite を最大不動点で求める）**
   `lower_llvm.cpp`。`const.f64` / `to_num` / `add.f,sub.f` / `mul.f` /
   `div.f,mod.f` / `mathop.f` / `select` / `if` / `warp_yield` /
   ループ運搬 block argument を評価し、bit0=non-NaN・bit1=finite を返す。
   ループ運搬は「そのブロックの f64 arg を楽観 3 で種付けし、incoming を
   再評価して下げるだけ」の 6 反復不動点（単調なので必ず収束）。
   非 NaN が証明できた値は `f64Operand` / `numberShadow` の
   `select(uno(d),0,d)` を省く。
   **注意**: 不動点が既に確定済みの arg（`markArgNonNaN` で保証した値）を
   上書きしないこと。姉妹 arg の求解が確定値を 0 に落とし、heal が復活
   するバグを実測で踏んだ（下記の「効かなかった」の一因）。

2. **書き込み側 NaN 正規化（`healCarryOnWrite`）**
   読み側 `select(uno)` は `vcmpordsd(4cy)+blend(1)+vaddsd(4)` がループ
   キャリーの依存連鎖になる。バックエッジで `vucomisd; jp` の稀分岐に
   出し、正規化値は 0.0（影が NaN なら bits は box.f の正準 NaN で
   `to_num(正準 NaN)=0`）に置き換える。legacy と同じ形。
   `kinds="b"`（bits が box.f 由来で常に正準）の f64 側だけに適用。

3. **`indexF` の定数畳み込み**
   リスト索引が `xyo.const` のとき bitcast ではなく `to_num` で
   コンパイル時評価する。文字列定数（"1"）の bits はタグ付きポインタで、
   `bitcast→cvttsd2si` が INT64_MAX に fold され `icmp ult` が常に偽 →
   fast path ごと消えて 2 命令しかない slow ループになっていた。

4. **`xyo.list.item.num` のブロック順**（fast → fastNum → join → slow）

5. **`XYO_MLIR_NOFUSENUM`**（`fuseNumericListToNum` を止める A/B ゲート）

### 効いたもの（同一 sb3・`--fps 0`・プロジェクト内 `times` 計測）

| ミニ | legacy | MLIR 前 | MLIR 後 |
|------|--------|---------|---------|
| `micro_loop`（`change i by 1` 50M） | 29 | **111** | **32（= 1.1x、ほぼ parity）** |
| `abl_acc_const`（`sum += 7` 50M） | 29 | 32 | 32 |
| `abl_read_const`（`sum += item 1 of L`、NOFUSENUM） | 90 | 282 | 121 |

`change i by 1` の 3.4 倍差は「ループ運搬の非 NaN 証明」が無く、
毎反復 `vcmpord+blend` がキャリーに乗っていたことが原因だった。修正後は
legacy と同等（32 vs 29 ms）。

### 効かなかったもの（すべて実測で棄却）

- **リストヘッダの巻き上げ**（`XYO_MLIR_LISTHDR=1`）: 依存ロード 4 本が
  消えて内側ループが 5 命令になっても **-0〜2%** のみ。リスト読みの
  コストはロード連鎖ではない。
- **読み側 heal の削除**: 0%。
- **読み側 heal を書き込み側へ移動**: 定数索引で ±0%、変数索引で悪化。
- **バッチ yield**（`XYO_MLIR_NOBATCH=1`）: スイートで -2〜8%。
- **変数昇格の無効化**（`XYO_MLIR_NOPROMOTE=1`）: スイートで -20〜36%。
- **`list.item.num` への fusion**（`to_num(list.item)` → `.num`）:
  定数索引のミニでは 2.3 倍**速い**が、変数索引のミニとスイート
  （`list_read` 138K→128K）では**遅い**。既定は fusion 有効のまま。

### 決定的な観測: スイート差はコード生成の差ではない

- スイートは **1 スクリプト 5,649 op / 49 ループ / promoted=174** を
  **1 関数**として emit（`xyo-jit: compiled ~Mv 211.6ms`、legacy は 2668.7ms）。
  fallback は 0 件（`XYO_MLIR_TRACE=1` で確認）。
- 生成コードは MLIR の方が**小さい**（`.text` 242,666 B vs legacy 541,726 B）
  → I-cache 圧力説は棄却。
- 上記の全トグル（heal / ロード巻き上げ / バッチ / 昇格 / fusion）が
  スイートの app_*/algo_*/list_* を **±数% しか動かさない**一方、
  同じコードの隔離ミニでは 2〜3 倍動く。
- したがって Round 240 の P3（「ミニ parity・スイート 0.22x、原因は
  文脈」）は維持され、**P1「item fast path 抜本」をコード生成側の
  書き換えで解決する見込みは薄い**。ただし切り分けは進んだ:
  ループ本体の命令列・ロード連鎖・yield・昇格・コードサイズの
  いずれも主因ではない。
- 残る最有力は **ネストループの昇格漏れ**（P2、`scanBlock` の region
  打ち切り）。スイートで `int=10` / `loops=49` しか整数昇格が発火して
  おらず、app_*/algo_primes/sieve はすべて二重ループである点と整合する。

### スイート実測（Round 241 前 → 後、同一バイナリ差し替え前後での比較）

app_life +0.5% / app_brainfuck +3.3% / app_raycast +3.2% / app_ast +4.1% /
algo_sort +4.1% / algo_matmul +5.0% / algo_primes +4.4% / algo_ack +3.5% /
list_read +7.1% / list_insdel +15.9% / str_cast +5.0% / math_expsqrt -2.7%。
回帰は 1 件のみで、他は微増。

### 次の一手（Round 241 で特定した最有力リード）

スイート IR の yield 形を legacy と数えると差がはっきり出る
（`performance.sb3`、post-opt IR の出現数）:

| | `xyo_yield_fast` | `xyo_warp_yield_due` | `xyo_loop_yield` |
|---|---|---|---|
| legacy | 202 | **214** | **0** |
| MLIR | 244 | **58** | **357** |

- legacy は warp ループを**全数**「`warp_due` で問い合わせ → 真のときだけ
  spill+yield」で lowering する。
- MLIR は `emitBatchedRepeat` が適用できたループ（58 箇所）だけがその形で、
  残り 357 箇所は `xyo_loop_yield` を直接呼ぶ形（`emitWarpLoopYield`）に
  落ちている。`emitBatchedRepeat` の安全条件（本体内に入れ子ループ・
  手続き呼出し・待機系を許さない）が、まさに app_*/algo_primes/sieve の
  二重ループ＋手続き呼出しに当たる。
- したがって次ラウンドの本命は「**バッチが使えない warp ループでも、
  spill を稀分岐側へ出す**」— `emitWarpLoopYield` を
  「`xyo_warp_yield_due` で問い合わせ → 真のときだけ spill + `xyo_yield`
  + reload」に置き換える（legacy と同形）。現在の形は 64 反復ごとに
  `xyo_loop_yield` を呼び、その呼出しが yield し得るため運搬値を
  呼出し境界で spill せざるを得ない。
  正しさの条件（yield 後に他スレッドが同じ変数を書く可能性）を
  `recomputeHoisted` と同じ枠組みで満たす必要があり、
  `d`/`b` 運搬の reload 契約とセットで設計する。

## Round 242: warp yield 検査を sampled clock + 稀分岐ハンドオフへ

### 変更（`lower_llvm.cpp`）

`emitWarpDueCheck(handBB, done, warpKnown)` を新設し、warp 系 yield
3 箇所を legacy の `emitWarpDeadlineCheck` と同形にした:

- `emitWarpLoopYield`（未昇格 warp ループ）: これまで検査のたびに
  `xyo_loop_yield`（内部で時計を読む不透明呼出し）と
  `recomputeHoisted` を**無条件**で実行していた。今は
  「`isWarp==0` → handoff / `sampledWarpClockUs` があれば
  アトミック 2 ロード + 期限比較 / なければ `xyo_warp_yield_due`」の
  形で、**実際に譲るときだけ** `xyo_yield` + `recomputeHoisted` を走る。
- `emitLoopYield` の非コルーチン warp 枝: 同上。
- `xyo.warp_yield` の due ブロック: 直接 `xyo_warp_yield_due` を呼んで
  いたのを `emitWarpDueCheck` へ差し替え（sampled clock の fast path
  を使える + `isWarp==0` のとき必須 yield を飛ばす隙間も塞ぐ）。
  `kind=='d'` の box.f 化を due 側から handoff 側へ移した。

### 落とし穴（実測で発見）

`emitWarpDueCheck` が done へ分岐する直前ブロックは `mergeBB` で、
呼出し元の `dueBB` ではない。**done の PHI incoming は mergeBB を
指す必要がある** — 最初 dueBB のままにしていて IR verify に落ち、
MLIR frontend がまるごと legacy へ fallback していた
（`test_mlir` の "warp procedure compiled through MLIR" が検出した）。
修正後 `XYO_JIT_MEGA=1` / 非 Mega とも **0 FAIL**。

### 効果

- スイート hotspot（MLIR / legacy）: **algo_sort 0.75→0.83**、
  algo_sieve 0.61→0.67、algo_matmul 0.55→0.58、app_projection
  0.78→0.86、list_replace 0.35→0.37、list_read 0.24→0.25、
  math_round 0.43→0.46、loops_empty 0.93→0.94。回帰なし
  （app_raycast 0.23→0.21、app_brainfuck 0.23→0.22、app_ast
  0.33→0.32、algo_ack 0.52→0.51 は ±1〜2% 揺動圏内）。
- IR 出現数: `xyo_loop_yield` 357→**91**（handoff のみ）、
  `xyo_warp_yield_due` 58→477（フォールバックブロック分）、
  atomic ロード 836 箇所 = sampled clock fast path が発火している。

### 残る差（確定した次の一手）

- **「ミニは速い・スイートは遅い」は崩れた**: Round 241 までは隔離
  ミニで parity、スイートのみ 0.22x だったが、今回の
  `abl_read_varidx`（変数索引 list read、非昇格）は **MLIR 200ms /
  legacy 65ms** で隔離でも 3 倍差。残差は**コード生成差**で確定。
- legacy の内側ループは (1) リスト `items`/`len` をレジスタに保持し
  反復をまたぐ、（2) 変数スロットを 1 回解決して使い回す、
  (3) 文字列要素の `numState/numCache` fast path を inline で持つ
  （タグ==str のとき `bzhi` + `movl 0x10` + `vmovsd 0x8`）、
  (4) spill は実 yield 時のみ。MLIR は反復ごとに
  `varSlotData → List → len/items → item` の 3 段ポインタチェイスを
  行う。**P1「item fast path 抜本」はこの方向（ヘッダの
  ループ不変ホイスト + 非昇格変数のスロットキャッシュ + 文字列
  キャッシュ fast path）に限定できる**。`XYO_MLIR_LISTHDR=1`
  スケッチでは -2〜+2% しか出なかったので、ヘッダだけでなく
  変数スロット側が支配的と思われる。
