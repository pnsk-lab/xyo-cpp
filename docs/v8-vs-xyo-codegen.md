# TurboWarp の出力を V8 バイトコード / 最適化コードまで降ろして xyo と比べる

[`algo-bench.md`](algo-bench.md) で「ループとリスト読み書きで TurboWarp が
1.6〜2.5 倍速い」と分かった。その理由を推測ではなく**実際の生成コード**で
確かめるため、TurboWarp が吐いた JavaScript を V8 の Ignition バイトコードと
TurboFan のマシンコードまで降ろし、xyo の LLVM IR / マシンコードと並べた。

対象は 3 つ。

| | 内容 | 得意な側 |
|---|---|---|
| `micro_loop` | `repeat n { change i by 1 }`（50M 回） | TurboWarp |
| `micro_read` | `sum += item i of L`（50M 回） | TurboWarp |
| `fib` | 素朴な再帰（430 万呼び出し） | xyo |

## 方法（再現手順は末尾）

TurboWarp は `JSGenerator.testingApparatus` というテスト用フックを持って
いる（`refs/scratch-vm/src/compiler/jsgen.js`）。これにコールバックを
差し込むと、**コンパイルされたスクリプト／手続きの JS ソースがそのまま
取れる**。`tests/bench/algos/dump_v8.js` がこれを行い、プロジェクトを
ヘッドレスで最後まで走らせる（＝JIT を温める）。

```sh
# 生成 JS を取り出す
node tests/bench/algos/dump_v8.js out/micro_loop_50000000.sb3 v8out
# Ignition バイトコード
node --print-bytecode --print-bytecode-filter=fun0_bench_ \
     tests/bench/algos/dump_v8.js out/micro_loop_50000000.sb3 v8out
# TurboFan の最適化マシンコード
node --print-opt-code --print-opt-code-filter=fun0_bench_ --code-comments \
     tests/bench/algos/dump_v8.js out/micro_loop_50000000.sb3 v8out
```

xyo 側は同じ `.sb3` を AOT コンパイルしてオブジェクトを出し、
`llvm-objdump` で逆アセンブルした（AOT と JIT は同じ `codegen.cpp` を
通るので、マシンコードは実質同一）。

```sh
./build/xyo aot out/micro_loop_50000000.sb3 -o app --obj v8out/micro_loop.o
llvm-objdump -d --no-show-raw-insn v8out/micro_loop.o
```

## 1. TurboWarp が出す JavaScript

`micro_loop` のホットな手続き（`bench`）はこうなる。

```js
(function factory1(thread) {
  const target = thread.target;
  const runtime = target.runtime;
  const b0 = target.variables["v_i"];          // 変数は Variable オブジェクト
  return function fun0_bench_ (p0) {
    b0.value = 0;
    for (var a0 = toNotNaN(+p0); a0 >= 0.5; a0--) {
      b0.value = (b0.value + 1);               // ← 毎回プロパティ読み書き
    }
    return "";
  };
})
```

`micro_read` のループはこう。

```js
for (var a0 = toNotNaN(+p0); a0 >= 0.5; a0--) {
  b0.value = (b0.value + 1);
  if ((b0.value > 1000)) { b0.value = 1; }
  b1.value = (toNotNaN(+b1.value) + toNotNaN(+(b2.value[b0.value - 1] ?? "")));
}
```

**TurboWarp は変数を JS のローカル変数へ昇格しない。** `b0.value` という
ヒープ上のプロパティのままである。リストも `b2.value` という生の JS 配列で、
`item i of L` は `b2.value[i - 1]` という**素の配列アクセス**になる
（境界外は `undefined` → `?? ""`）。速さは V8 のインラインキャッシュと
TurboFan が、この「同じ形のプロパティアクセス」を固定オフセットの
ロード／ストアへ畳み込むことから来ている。

## 2. Ignition バイトコード

`fun0_bench_` のバイトコードは 61 バイトしかない。ループ本体は 8 命令。

```
 35: 19 02             LdaImmutableCurrentContextSlot [2]   ; b0
 37: d1                Star1
 38: 33 f8 01 06       GetNamedProperty r1, [1:"value"], FBV[6]   ; b0.value
 42: 4d 01 05          AddSmi [1], FBV[5]                          ; + 1（Smi 即値）
 45: 39 f8 01 00       SetNamedProperty r1, [1:"value"], FBV[0]   ; b0.value = ...
 49: 0b f9             Ldar r0
 51: 5a 08             Dec FBV[8]                                  ; a0--
 54: 95 1b 00 09       JumpLoop [27], [0], FBV[9]
```

注目すべきは、(1) 変数アクセスが**名前付きプロパティ**（`FBV` = feedback
vector）であること、(2) 加算が `AddSmi` という**整数即値専用の命令**で
あること、(3) ループ条件の `a0 >= 0.5` が毎回 `TestGreaterThanOrEqual`
で double 比較になること。型の情報はここではなく feedback vector に
溜まり、TurboFan がそれを使って投機する。

バイトコードから分かること:

- **8 命令/反復**で、xyo のマシンコード（16 命令 + 分岐）より短い。しかしこれは
  「実行されるコード」ではない。Ignition のままなら xyo の JIT より
  桁違いに遅く、本ベンチはウォームアップで必ず TurboFan に到達させる。
  つまり**比較しているのは V8 の最適化コードであって、バイトコードでは
  ない**。
- `GetNamedProperty` / `SetNamedProperty` に feedback slot が付いており、
  ここに「毎回同じ Variable オブジェクト・同じ `value` オフセット」という
  観測が溜まる。TurboFan はこれを使って **map check をループ外へ hoist**
  する（§3）。xyo のタグ検査に相当するものを、V8 は「投機 + 脱最適化」で
  消している。
- `AddSmi` は「Smi + 即値」専用の命令で、V8 が**最初から整数専用の
  演算を持っている**ことを示す。xyo の Value に相当するものが無く、
  すべて f64 として扱う設計上の差がここに現れている。

## 3. TurboFan のマシンコード（micro_loop）

最適化後のループ本体（`--print-opt-code`、4 倍アンロールされた 1 回分）。

```
 151: movq rdx,[r8+0x37]        ; b0.value をロード
 155: sarq rdx,32               ; タグ付き Smi の下位 32bit が値
 159: movl rdx,rdx
 15b: addl rdx,0x1              ; ★ 整数の add（1 サイクル）
 15e: jo   <deopt overflow>     ; 溢れたら脱最適化
 164: shlq rdx,32               ; 再タグ付け
 168: movq [r8+0x37],rdx        ; b0.value へストア
 16c: movq r9,rcx
 16f: subl r9,0x1               ; a0--
 173: jo   <deopt overflow>
 179: vcvtlsi2sd xmm0,xmm15,r9  ; ループ条件だけは double で比較
 17e: vucomisd xmm0,xmm1
 182: jc   <exit>
```

重要な性質:

1. **値は Smi（31bit 整数）としてメモリに置かれたまま**で、加算は
   `addl`。オーバーフローは `jo` で脱最適化する。**f64 への変換は無い。**
2. 変数オブジェクトの型チェック（map check）は**ループの外へ
   巻き上げ済み**で、ループ内には無い（リストの配列 map check は
   `micro_read` のようにループ内に残ることもある）。残るチェックは
   `jo`（オーバーフロー）だけ。
3. すべてのチェックは **out-of-line の deopt スタブ**への `jcc` で、
   ループ本体は分岐が 2 個（`jc` と `jo`）しかない。
4. **4 倍アンロール**され、割り込みチェック（`cmpb [r13-0x4f]`）は
   アンロール 1 グループに 1 回だけ。

## 4. xyo のマシンコード（micro_loop）

同じループのホットパス（`llvm-objdump`、AOT オブジェクト）。

```
 114: vaddsd (%rip),%xmm2,%xmm2   ; repeat カウンタ - 1
 11c: vucomisd (%rip),%xmm2       ; >= 0.5 ?
 124: jb   <exit>
 12a: cmpq %r15,%rdi              ; ★ i のタグ検査（毎回）
 12d: jge  <非数値パス>
 12f: vmovq %rdi,%xmm0            ; ★ GP → XMM
 134: vcmpordsd %xmm0,%xmm0,%xmm1 ; NaN 検査
 139: vandpd %xmm0,%xmm1,%xmm0    ; NaN なら 0
 13d: jmp  1e0                    ; ★ 無条件ジャンプ（ブロック合流）
 1e0: vaddsd %xmm3,%xmm0,%xmm0    ; i + 1
 1e4: testl %r12d,%r12d           ; ★ isWarp 検査（毎回）
 1e7: je   <yield パス>
 1ed: incq %r13
 1f0: testb $0x3f,%r13b           ; ★ yield カウンタ（毎回）
 1f4: je   <yield パス>
 1fa: vmovq %xmm0,%rdi            ; ★ XMM → GP
 1ff: jmp  114                    ; ループバック
```

xyo はループ内変数を**レジスタへ昇格してはいる**（メモリ読み書きは無い）。
しかし昇格した値は **i64（ボックス化された Value）のまま**なので、

- 読み出しのたびにタグ検査（`cmpq`/`jge`）と NaN 正規化（2 命令）が入り、
- 演算のたびに **GP ↔ XMM の移動が 2 回**入る（`vmovq` ⇄ `vmovq`）。

`i` のループキャリー依存は「`vmovq` → `vaddsd` → `vmovq`」で 6〜8 サイクルに
なる。V8 は同じ場所が 1 サイクルの `addl`（＋ストア転送）で済む。

## 5. 数字で比べる

`micro_loop`（50M 回、1 反復あたり）:

| | TurboWarp (V8) | xyo |
|---|---:|---:|
| 命令数 | 約 13〜14 | 16 |
| 条件分岐 | 2（+ 割り込み 0.25） | 5 |
| 無条件ジャンプ | 0（ループバックのみ） | 2（レイアウト） |
| 実際に成立する分岐 | 1〜2 | 2 |
| GP↔FP 移動 | 0 | 2 |
| 演算の表現 | Smi `addl`（1c） | f64 `vaddsd`（4c） |
| アンロール | 4 倍 | なし |
| 実測 | 1.06 ns/回 | 2.98 ns/回 |

`micro_read`（50M 回）。1 反復のうち**リスト読み出し区間だけ**を
両者の逆アセンブルから数えると:

| | TurboWarp (V8) | xyo |
|---|---:|---:|
| 命令数 | 7 | 13 |
| 条件分岐 | 2（どちらも deopt） | 4 |
| インデックス変換 | `sarq`/`subl`（整数） | タグ検査 + `vmovq` |
| 境界検査 | `cmpl r15,r11` + `jnc`（整数 1 回） | `vucomisd 1.0` と `vucomisd len+1` の 2 回 |
| 要素ロード | `movq [rbx+r15*8+0xf]` | `vcvttsd2si` → `movq -0x8(%rcx,%rax,8)` |
| 実測（1 反復全体） | 1.80 ns | 4.36 ns |

V8 側（`optcode_read.txt`, 0x28a〜0x2a8）:

```
 28a: movq r15,[r8+0x37]        ; i
 28e: sarq r15,32
 292: movl r15,r15
 295: subl r15,0x1              ; i - 1
 299: jo   <deopt>
 29f: cmpl r15,r11              ; ★ 境界検査（整数 1 回）
 2a2: jnc  <deopt>
 2a8: movq r15,[rbx+r15*8+0xf]  ; ★ 要素ロード（そのまま）
```

xyo 側（`micro_read.dis`, 0x528〜0x56d）:

```
 528: cmpq $0x0,0x20(%rsp)      ; リストポインタの null 検査
 52e: je   <slow>
 534: cmpq %r14,%rdx            ; ★ インデックスのタグ検査
 537: jge  <非数値パス>
 539: vmovq %rdx,%xmm0
 53e: vucomisd 1.0,%xmm0        ; ★ 下限検査（f64）
 54d: jb   <empty>
 553: vmovsd 0x38(%rsp),%xmm1   ; len+1（preheader で hoist 済み）
 559: vucomisd %xmm0,%xmm1
 55d: jbe  <empty>              ; ★ 上限検査（f64）
 563: vcvttsd2si %xmm0,%rax
 568: movq 0x30(%rsp),%rcx      ; items（hoist 済み）
 56d: movq -0x8(%rcx,%rax,8),%rax
```

どちらも「境界検査 + 1 ロード」だが、xyo は境界を **f64 で 2 回**比較し、
その前に**インデックスのタグ検査**が入る。あわせて命令数は約 2 倍、
分岐も 2 倍になる。

（ns/回は `algo-bench` の中央値 ÷ 反復数。xyo は JIT 最適化後の値。）

## 6. 考察 — 差はどこから来るか

### (1) 型特化があるか（Smi vs 常に f64）

V8 は feedback vector から「この値は Smi」と学習し、**整数のまま**加算する。
xyo の Value は NaN-box された 64bit で、数値は常に f64（または文字列
ポインタ）である。整数専用の表現が無いので、`i + 1` は必ず `vaddsd` に
なる。これは「1 サイクル vs 4 サイクル」の差であり、ループキャリー
依存がそのままレイテンシになる。

### (2) 昇格した値が i64 のままで、GP↔FP 移動が毎回入る（xyo 側の最大の無駄）

xyo はループ内変数をレジスタに載せたが、**載せた値は「ボックス化された
i64」**である。タグ検査は整数レジスタでやりたい、加算は XMM でやりたい、
という相反する要求のため、毎回 `vmovq` で行き来している。V8 はそもそも
メモリ上の Smi を直接 `addl` するので、この移動が存在しない。

**ループ入口で「この変数は数値である」と分かれば、phi を f64 のまま
XMM に置ける**（下の改善案 1）。

### (3) チェックの表現が違う（out-of-line deopt vs inline 合流）

V8 の投機チェックは「成立しなければ関数ごと脱最適化」なので、**本体から
遠く離れたスタブへの分岐**でよい。本体は一直線に並び、分岐は予測で
ほぼ無料になる。

xyo のチェックは「成立しなければ別のブロックで処理を続ける」ので、
**本体の中でブロックが合流する**。そのため高速パスが
「条件分岐 → 数命令 → 無条件ジャンプ → 合流先」という形になり、
無条件ジャンプが 2 個/反復入る。これはフロントエンドのスループットを
直接落とす。

改善は「遅い側のブロックに `cold` を付けて分岐重みを極端に寄せる」
だけで、LLVM のブロック配置が高速パスを fall-through にしてくれる。

### (4) yield 検査を毎反復やっている

xyo は warp 中でも「`isWarp` か」「カウンタが 64 の倍数か」を毎回見る。
V8 は（そもそも yield しないコードなので）割り込みチェックだけを
アンロール 1 グループに 1 回しかしない。`isWarp` はループ不変なので
ループの外へ出せる。実測では yield 検査全体を消すと 149→142ms（約 5%）
だったので、ここは 5% 程度の改善が見込める。

### (5) リスト境界検査が浮動小数点

Scratch のリスト索引は小数を切り捨てる（`item 2.5 of L` は 2）ため、
xyo は「`1 <= d < len+1`」を f64 で検査してから `cvttsd2si` している。
V8 の JS 配列は整数索引しかないので、`cmpl` 1 回で済む。

しかし**切り捨てを前提にすれば整数比較で等価**である:
`(uint64)(trunc(d) - 1) < len` は `1 <= d < len+1` と完全に一致する
（NaN/±Inf は飽和変換で範囲外になる）。`llvm.fptosi.sat` を使えば
未定義動作も避けられる。

### (6) アンロールしていない

V8 は 4 倍アンロールして、ループ条件・割り込みチェック・バックエッジを
償却している。xyo のループは本体に分岐が多く、LLVM の既定ヒューリスティック
ではアンロールされない。

### 逆に、なぜ xyo が fib / sha256 / append で勝つのか

`fib` の TurboWarp 生成コードはこうなっている。

```js
if (compareLessThan(p0, 2)) { b0.value = p0; }
else {
  thread.procedures["Wfib %s"]((toNotNaN(+p0) - 1));   // ★ 文字列キーで手続き表を引く
  b1.value.push(b0.value);
  b1._monitorUpToDate = false;                          // ★ push のたびにプロパティ書き込み
  thread.procedures["Wfib %s"]((toNotNaN(+p0) - 2));
  ...
}
```

- 手続き呼び出しが **`thread.procedures["Wfib %s"]` という文字列キーの
  プロパティ参照**（IC でキャッシュされるが、呼び出しごとにロード）＋
  JS 関数呼び出し。xyo はネイティブ呼び出しで引数をレジスタ渡しする。
- `add to list` のたびに `_monitorUpToDate = false` を書く（モニタ更新の
  ため）。xyo の `SList::push` にはこれが無い（append 7.6 倍の一因）。
- 数値変換 `toNotNaN(+x)` が呼び出し境界ごとに入る。

つまり **「呼び出し・リスト追加」は xyo の方が構造的に軽く、
「密な数値ループ」は V8 の型特化が上**、という綺麗な分担になっている。

## 7. 実施した改善と結果

この分析のあと、優先度順に実際に手を入れた。

1. **数値影（numeric shadow）** — 昇格する変数を `(bits, d = toNum(bits))` の
   対で持ち、**d を f64 のまま XMM に置く**。数値としての読み出しは `d` を
   そのまま返すので、`cmpq`/`jge` のタグ検査・NaN 正規化・`vmovq` の
   GP↔XMM 移動がすべて消える。`bits` は常に正準な Value なので、`join` の
   ような Value 読み出し・yield の spill・ループ脱出の書き戻しは厳密なまま。
2. **分岐重み** — タグ検査・yield・リスト境界の分岐に `branch_weights` を
   付けて、LLVM に高速側を fall-through させた。
3. **リスト索引の素通し** — 索引が数値影／数値リテラルなら、索引変換の
   タグ検査と分岐を丸ごと省く。
4. **リスト境界の整数化は不採用** — `fptosi.sat` は x86 で 6 命令
   （`cvttsd2si` + 2×`vucomisd` + 2×`cmov` + `movabs`）に展開され、
   f64 比較 2 回より高くついた（実測で read が 8% 悪化）。等価性は確認済み
   なので、AVX-512 の `vcvttsd2usi` が使える環境向けの選択肢としては残る。
5. **アンロールは不採用** — このループは本体に yield 検査があり、
   アンロールしても検査は償却されず、コードが増えるだけだった。

結果（同じベンチマーク、xyo JIT）:

| | 改善前 | 改善後 | TurboWarp |
|---|---:|---:|---:|
| `micro_loop`（1 反復） | 2.98 ns | **0.78 ns** | 1.02 ns |
| `micro_read`（1 反復） | 4.36 ns | **4.06 ns** | 1.76 ns |
| `micro_write`（1 反復） | 3.50 ns | **3.10 ns** | 2.18 ns |
| quicksort | 52 ms | **44 ms** | 32 ms |
| mergesort | 97 ms | **83 ms** | 39 ms |
| bubblesort | 153 ms | **119 ms** | 53 ms |
| sieve | 26 ms | **21 ms** | 14 ms |

`micro_loop` は TurboWarp を追い越した。数値影の後のループは 8 命令・
分岐 3・キャリー `vaddsd` 4 サイクルで、V8 の「Smi の `addl` + 4 倍
アンロール」より短い。

残っているのはリストアクセス（`micro_read` は 2.3 倍）。V8 が
「整数比較 1 回 + 要素ロード」なのに対し、xyo は f64 の境界検査 2 回 +
要素のタグ検査を払う。ここが次の一手になる。

## 8. 限界

- マシンコードの比較は 1 CPU（i7-12700）× 1 Node（V8 13.x）×
  LLVM 23 でのもの。V8 の最適化はバージョンで変わる。
- 動的命令数はハードウェアカウンタで測っていない（`perf` が無い環境）。
  表の「命令数」は逆アセンブル上の静的カウント（ホットパスのみ）で、
  ns/回はベンチマークの実測値。
- V8 の脱最適化が起きる入力（Smi 範囲外の値など）では、この最適化
  コードは使われず Ignition / 別の最適化コードに落ちる。本ベンチの
  入力は Smi に収まるので、V8 にとって最も有利なケースである。

## 9. 再現

```sh
python3 tests/bench/algos/gen_algo_bench.py out --only micro_loop --sizes micro_loop=50000000
node tests/bench/algos/dump_v8.js out/micro_loop_50000000.sb3 v8out
node --print-bytecode --print-bytecode-filter=fun0_bench_ \
     tests/bench/algos/dump_v8.js out/micro_loop_50000000.sb3 v8out
node --print-opt-code --print-opt-code-filter=fun0_bench_ --code-comments \
     tests/bench/algos/dump_v8.js out/micro_loop_50000000.sb3 v8out
./build/xyo aot out/micro_loop_50000000.sb3 -o app --obj v8out/micro_loop.o
llvm-objdump -d --no-show-raw-insn v8out/micro_loop.o | less
```

`v8out/` は生成物なので `.gitignore` 済み。
