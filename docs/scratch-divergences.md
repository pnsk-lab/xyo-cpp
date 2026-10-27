# Scratch との意味論差分（再現と修正）

`refs/scratch-vm`（TurboWarp 2.1.46）を参照実装として、同じ `.sb3` を
scratch-vm と xyo で実行し、観測可能な差分を再現した。すべて**既製の
Scratch 3 ブロックだけ**で作った最小プロジェクトで、`tests/differential/`
に保存してある。

```sh
python3 tests/differential/run_repros.py
```

修正後は **19/19 が HotSpot・インタプリタの両方で scratch-vm と一致**する。
以下は再現手順、原因、修正内容の記録。

## 修正一覧

| # | 重要度 | ケース | 修正 |
|---|---|---|---|
| 1 | **Critical** | `delete (0/0) of L` / `insert (0/0) at 1 of L` で SIGSEGV | `toListIndexD` が NaN を `LIST_INVALID` に。`SList::insert0/erase0` に防御チェック |
| 2 | High | 2^53 超の数の `join`/表示 | `jsnum::toString` の整数 fast path を `\|v\| <= 2^53` に限定 |
| 3 | High | JIT の `mod`（`Infinity`/`1e21`/非正規化数） | JIT/`xyo_mod` を scratch-vm と同じ `r = n%m; if (r/m<0) r += m` に |
| 4 | High | JIT の `sin/cos(1e308)` | JIT の掛け順を `(pi*x)/180` に（Scratch と同じ overflow） |
| 5 | Medium | `round(-0.5)`・`-0` の符号 | `jsnum::jsRound`（Math.round 準拠）を追加し、全 `std::round` 呼出しを置換 |
| 6 | Medium | `switch costume to (0.5)` | `Target::setCostume` も `jsRound` に |
| 7 | Medium | 非 BMP 文字の大小無視比較 | `u16ToLower` をコードポイント単位にし、astral 小文字表を追加 |
| 8 | Low | JIT の `asin`（1 ULP） | JIT を `(v*180)/pi` の順に |
| 9 | Low | `ln/log` の最終桁（glibc vs V8） | fdlibm `e_log.c` を `jsnum::jsLog` として移植 |
| 10 | Low | `asin/acos/atan`/`e ^` の最終ビット | V8 14.6 の fdlibm 実装を `src/core/v8math.cpp` へ移植 |
| 11 | Medium | 語末の Greek sigma（`"ΟΣ" = "οσ"`） | `u16ToLower` に Unicode Final_Sigma 判定を追加 |
| 12 | Low | Unicode 17 の新規小文字（U+A7CE 等） | 生成テーブルへ 28 コードポイント追加 |
| 13 | Medium | 1 桁数値リストの `list contents`（`[1,2,3]`） | `rt::listContents` を scratch-vm の `typeof string` 判定に変更 |
| 14 | Low | `point towards` の向き（`atan2` の最終ビット） | V8 fdlibm の `atan2` を `v8math` へ移植 |
| 15 | Medium | `move () steps` の x/y（`sin`/`cos` の最終ビット） | V8 fdlibm の `sin`/`cos`（`rem_pio2` 込み）を `v8math` へ移植 |
| 16 | Low | `sensing_timer` のミリ秒量子化 | `currentMSecs` を整数 ms にし、ヘッドレスでも毎フレーム最低 1ms 進める |
| 17 | Medium | JIT の変数キャッシュが id だけでキーされていた | `(id,name,list)` のキーへ変更（`varSlot` のスロットキーも同様） |

AOT は JIT と同じ codegen を通るため #3・#4・#8 も修正済み。WASM
コンパイラ（`src/wasm/wasm_compile.cpp`）の `mod`/`round` も同じ式へ揃えた。

## 再現方法

各 repro は結果をステージのグローバルリスト `out` に積む。scratch-vm 側は
`String(value)` で全変数をダンプするので、`join` / `say` / リスト表示と
同じ表記で比較できる。JIT 系は `repeat 1` を含めて HotSpot の初回
コンパイルを誘発し、`xyo run --jit off` とも比較する。

- ハーネス: [tests/differential/README.md](<../tests/differential/README.md>)
- repro 一式: [tests/differential/repro/](<../tests/differential/repro/>)
- 生成器: [gen_repros.py](<../tests/differential/gen_repros.py>)

## 1. NaN インデックスのリスト操作（Critical）

**再現**: `nan_index_delete.sb3`, `nan_index_insert.sb3`

```
delete (0 / 0) of [L]          // L = [a, b, c]
add (item 1 of L) to [out]
```

修正前は xyo が `SIGSEGV`（gdb では `SList::erase0` → `memmove`、
`rt::listDelete` 経由）。scratch-vm は no-op で `out = ["a"]`。

### 原因

scratch-vm の `Cast.toListIndex` は `Cast.toNumber` が NaN を 0 に潰すため
NaN を返さない。xyo の `toListIndexD` は NaN を保持し、`NaN < 1` も
`NaN > length` も false なので範囲チェックを通過していた。`listDelete` は
`i = 0` の後 `erase0(i - 1)` を呼び、`uint32_t(-1)` を `memmove` の長さに
使っていた。`dtInsertAtList` も同様。

### 修正

- [cast.cpp:194](<../src/core/cast.cpp>) `toListIndexD`: `std::floor` 後に
  `NaN` を `LIST_INVALID` として返す（scratch-vm と同じ no-op になる）。
- [runtime_helpers.cpp:286](<../src/rt/runtime_helpers.cpp>) `listDelete` /
  [core.cpp:592](<../src/blocks/core.cpp>) `dtInsertAtList` の NaN 特別扱いを
  削除し、`i < 1` を防御。
- [slist.hpp](<../src/core/slist.hpp>) `insert0` / `erase0` に
  `i > len` / `i >= len` のガードを追加（将来の呼出し追加でも破壊しない）。

## 2. 2^53 超の数の文字列化（High）

**再現**: `big_number_string.sb3`

| 値 | scratch-vm | 修正前の xyo |
|---|---|---|
| 2^57 | `144115188075855870` | `144115188075855872` |
| 2^56 | `72057594037927940` | `72057594037927936` |
| 2^55 | `36028797018963970` | `36028797018963968` |

ES の `String(x)` は「その double に round-trip する最短の十進表記」を返す。
2^57 の正確な整数値は `…872` だが、`…870` も同じ double に丸まるため V8 は
末尾 0 の短い方を選ぶ。

### 原因

[jsnum.cpp:31](<../src/core/jsnum.cpp>) の整数 fast path が、i64 に往復できる
整数を `std::to_chars` で**正確な十進整数**として出力していた。この高速路は
|v| <= 2^53（すべての整数が正確に表現でき、最短表現＝その整数）では正しいが、
2^53 超では最短 round-trip 表現と一致しない。

### 修正

fast path の条件を `|v| <= 2^53` に狭め、それ以上は既存の「精度 p=1..17 の
`%.*e` で round-trip する最小 p」経路に落とす。2^57 は
`1.4411518807585587e+17` として最短表現が見つかり `…870` になる。

## 3. JIT の `mod`（High, HotSpot のみ）

**再現**: `jit_mod_extremes.sb3`

| 式 | scratch-vm | 修正前 HotSpot | interp |
|---|---|---|---|
| `1 mod Infinity` | `1` | `NaN` | `1` |
| `0 mod Infinity` | `0` | `NaN` | `0` |
| `-1 mod -Infinity` | `-1` | `NaN` | `-1` |
| `1 mod 1e21` | `1` | `0` | `1` |
| `1e-7 mod 1` | `1e-7` | `1.0000000005838672e-7` | `1e-7` |
| `5e-324 mod -1` | `-1` | `0` | `-1` |

### 原因

scratch-vm は `r = n % m; if (r / m < 0) r += m`。修正前の JIT は
`((n % m) + m) % m` をインライン展開していた。`m = ±Infinity` では
`(n + Inf) % Inf` が `NaN`、`|m|` が大きいと `r + m` の丸めで `r` が消え、
`|n|` が小さいと `n + m` の桁落ちで最下位ビットが変わる。さらに
`r == 0` を `+0` に正規化して `-0` を失っていた。

### 修正

- [codegen.cpp:892](<../src/jit/codegen.cpp>) `AOp::Mod`:
  `r0 = frem(n, m); r = (r0/m < 0) ? r0 + m : r0`。
- [rt.cpp:143](<../src/jit/rt.cpp>) `xyo_mod` を同じ式にし、`-0` を保持
  （`if (r == 0) r = 0` を削除）。
- インタプリタの [runtime_helpers.cpp:159](<../src/rt/runtime_helpers.cpp>)
  `rt::mod` は元から scratch-vm と同じ。

## 4. JIT の `sin`/`cos`（High, HotSpot のみ）

**再現**: `jit_trig_overflow.sb3`

| 式 | scratch-vm | 修正前 HotSpot | interp |
|---|---|---|---|
| `sin(1e308)` | `NaN` | `0.9709671675` | `NaN` |
| `cos(1e308)` | `NaN` | `-0.2392127917` | `NaN` |

scratch-vm は `(Math.PI * n) / 180` の順で計算し、`Math.PI * 1e308` が
`Infinity` に overflow して `sin(Infinity) = NaN` になる。修正前の JIT は
[codegen.cpp:981](<../src/jit/codegen.cpp>) で `x * (pi / 180)` と掛け順を
変えていたため有限値になっていた。

### 修正

JIT も `(pi * x) / 180` の順にした。tan はヘルパ `xyo_mathop` が元から
`(pi * angle) / 180` で、丸めだけ `jsRound` に揃えた。

## 5. `round()` の負の .5 と `-0`（Medium）

**再現**: `round_negative_half.sb3`, `jit_negzero.sb3`

| 式 | scratch-vm | 修正前 xyo |
|---|---|---|
| `round(-0.5)` | `0` | `-1` |
| `round(-1.5)` | `-1` | `-2` |
| `round(-2.5)` | `-2` | `-3` |
| `1 / round(-0.5)` | `-Infinity` | `-1` |
| `1 / round(-0)`, `1 / round(-0.4)` | `-Infinity` | HotSpot は `Infinity` |
| `1 / mod(-0, 1)` | `-Infinity` | HotSpot は `Infinity` |

`Math.round` は「床関数 + 0.5」寄り（負の .5 は 0 方向）で `-0` を返す。
修正前は `std::round`（0 から遠ざかる）を使い、JIT は `llvm.round(x) + 0.0`
で `-0` を `+0` に潰していた。

### 修正

[jsnum.cpp:234](<../src/core/jsnum.cpp>) に `jsnum::jsRound` を追加:

```cpp
if (!std::isfinite(x) || x == 0.0) return x;
if (x > 0.0 && x < 0.5) return 0.0;
if (x < 0.0 && x >= -0.5) return -0.0;
if (|x| >= 2^52) return x;       // 端数が表現できない
return std::floor(x + 0.5);
```

`operator_round`（interp / JIT の `xyo_round`）、`control_repeat`、
`looks_size`、`tanDeg`、`limitPrecision`、コスチューム、乱数方向などを
すべて `jsRound` に統一した。JIT は `call xyo_round` に変更。

## 6. `switch costume to (0.5)`（Medium）

**再現**: `costume_rounding.sb3`（3 コスチューム）

| 指定 | scratch-vm | 修正前 xyo |
|---|---|---|
| `-0.5` | #3 (`c3`) | #2 (`c2`) |
| `-1.5` | #2 (`c2`) | #1 (`c1`) |
| `-2.5` | #1 (`c1`) | #3 (`c3`) |
| `0.5` | #1 (`c1`) | #3 (`c3`) |
| `-3.5` | #3 (`c3`) | #2 (`c2`) |

`looks_switchcostumeto` は数値を 1 引いて `setCostume` に渡す。scratch-vm は
`Math.round(index)`、修正前の xyo は
[target.cpp:219](<../src/model/target.cpp>) で `std::round(index)` を使っていた。

### 修正

`Target::setCostume` を `jsnum::jsRound` に変更（#5 と同じ関数）。
`switch backdrop` も同じ経路なので同時に直る。

## 7. 非 BMP 文字の大小無視比較（Medium）

**再現**: `unicode_casefold.sb3`

| 比較 | scratch-vm | 修正前 xyo |
|---|---|---|
| `"𐐀" = "𐐨"`（Deseret U+10400 / U+10428） | `1` | `0` |
| `"Ä" = "ä"` | `1` | `1` |
| `"K"(U+212A) = "k"` | `1` | `1` |

`Cast.compare` は両辺が数値でないとき `toLowerCase()` で比較する。JS は
サロゲートペアも畳み込むが、修正前の
[lowerUnit / lowerCp](<../src/core/string_util.cpp>) は BMP のみ扱い、
`cp > 0xFFFF` をそのまま返していた。

### 修正

`u16ToLower` をコードポイント単位にし、非 BMP の小文字表
（Deseret / Osage / Adlam / Warang Citi / Medefaidrin / Vithkuqi など
11 レンジ、Unicode 16.0 で V8 と一致を確認）を
[lower_table.inc](<../src/core/lower_table.inc>) に追加した。孤立サロゲートは
JS と同じくそのまま残す。ASCII 高速路は従来どおり。

## 8. JIT の逆三角関数（Low, HotSpot のみ）

**再現**: `jit_invtrig_ulp.sb3`

| 式 | scratch-vm | 修正前 HotSpot | interp |
|---|---|---|---|
| `asin(1e-7)` | `0.000005729577951308242` | `0.0000057295779513082416` | `…242` |

scratch-vm / xyo インタプリタは `(asin(n) * 180) / pi`、修正前の JIT は
`asin(n) * (180 / pi)` と定数を先に畳んでいた。

### 修正

JIT も `(v * 180) / pi` の順にした（asin/acos/atan）。

## 9. `ln` / `log` の最終桁（Low）

**再現**: `libm_ln_log.sb3`

| 式 | scratch-vm (V8) | 修正前 xyo (glibc) |
|---|---|---|
| `ln(3)` | `1.0986122886681096` | `1.0986122886681098` |
| `log(3)` | `0.4771212547196623` | `0.47712125471966244` |

V8 は fdlibm 系の実装を同梱してプラットフォーム間で結果を固定しているが、
xyo は `std::log`（glibc）を使っていた。

### 修正

fdlibm `e_log.c` を [jsnum.cpp:248](<../src/core/jsnum.cpp>) の
`jsnum::jsLog` として移植した。ランダムな有限 double 約 20 万件で V8 の
`Math.log` とビット一致することを確認済み。interp / JIT / AOT の
`ln`・`log` を `jsLog` に統一した（JIT は `xyo_log` ヘルパ）。

## 10. `asin`/`acos`/`atan`/`e ^` の最終ビット（Low）

**再現**: `math_fdlibm.sb3`

| 式 | scratch-vm (V8) | 修正前 xyo (glibc) |
|---|---|---|
| `asin(0.7137281021284307)` | `45.53905926050618` | `45.539059260506185` |
| `acos(-0.5467314076748808)` | `123.143061563109` | `123.14306156310899` |
| `atan(0.04135948024180158)` | `2.3683738242135073` | `2.3683738242135064` |
| `e ^ 0.33089555746090094` | `1.392214378309982` | `1.3922143783099818` |

Scratch (V8) の `Math.asin`/`acos`/`atan`/`exp` は fdlibm を同梱している。
xyo は glibc の `std::asin` 等を使っており、約 2〜8% の入力で最終ビットが
違っていた。`log`（#9）と同じ問題。

### 修正

V8 14.6 の `src/base/ieee754.cc` から `asin`/`acos`/`atan`/`exp` を
[src/core/v8math.cpp](<../src/core/v8math.cpp>) へ移植した（fdlibm の
著作権表示を保持）。interp・JIT・AOT は `v8math::*` / `xyo_asin` 等の
共通ヘルパを使う。`pow` は V8 が glibc の `math::pow` を使うため対象外
（`10 ^` は元から一致）。

## 11. 語末の Greek sigma（Medium）

**再現**: `final_sigma.sb3`

| 比較 | scratch-vm | 修正前 xyo |
|---|---|---|
| `"ΟΣ" = "ος"` | `true` | `true`（偶然一致） |
| `"ΟΣ" = "οσ"` | `false` | `true` |
| `"ΣΣ" = "σς"` | `true` | `false` |
| `"ΑΣ" = "ας"` | `true` | `false` |

JS の `toLowerCase` は Unicode の Final_Sigma 規則を適用する。語末の
`Σ` は `ς` (U+03C2)、それ以外は `σ` (U+03C3) になる。xyo は 1 文字ずつ
`σ` に畳んでいたため、語末の sigma を含む比較・contains・ブロードキャスト
名の照合がずれた。

### 修正

[u16ToLower](<../src/core/string_util.cpp>) をコードポイント列に直し、
`Cased` / `Case_Ignorable` のテーブル（Node 26 の `\p{Cased}` /
`\p{Case_Ignorable}` から生成、[case_props.inc](<../src/core/case_props.inc>)）
を使って Final_Sigma を判定する。`"ΟΣ"` → `"ος"`、`"ΣΣ"` → `"σς"`、
結合文字を挟む `"ΌΣ"` / `"ΟΣ́"` も JS と一致する。

## 12. Unicode 17 の新規小文字（Low）

**再現**: `unicode17_casefold.sb3`

| 比較 | scratch-vm (Node 26 / Unicode 17) | 修正前 xyo (Unicode 16 表) |
|---|---|---|
| U+A7CE = U+A7CF | `true` | `false` |
| U+16EA0 = U+16EBB | `true` | `false` |

`lower_table.inc` は Python 3 の Unicode 16 データから生成されていた。
Node 26 は Unicode 17 で、U+A7CE/A7D2/A7D4 と U+16EA0..16EB8 の
大文字小文字ペアが追加されている。

### 修正

[lower_table.inc](<../src/core/lower_table.inc>) に 28 コードポイントを追加。
全 1,112,064 コードポイントについて Node 26 の `toLowerCase` と一致する
ことを確認した（xyo の出力は UTF-16、比較時にコードポイントへ正規化）。

## 13. 1 桁数値リストの list contents（Medium）

**再現**: `list_contents.sb3`

| リスト | 公式 Scratch (scratch-vm) | TurboWarp コンパイラ | 修正前 xyo |
|---|---|---|---|
| `[1,2,3]` | `1 2 3` | `123` | `123` |
| `["a",1]` | `a 1` | `a1` | `a1` |
| `["a","b"]` | `ab` | `ab` | `ab` |
| `["a","bc"]` | `a bc` | `a bc` | `a bc` |

`data_listcontents` は「全要素が**文字列**で長さ 1」のときだけ区切りなしで
連結する。数値の `1` は `typeof === 'number'` なので区切りが入る。
TurboWarp のコンパイラは `(item + '').length === 1` と判定するため
`[1,2,3]` を `123` にする（TW の interp と JIT でも結果が違う箇所）。
xyo は TW コンパイラ側に合わせていた。

### 修正

[runtime_helpers.cpp:346](<../src/rt/runtime_helpers.cpp>) の `rt::listContents`
を scratch-vm と同じ `Value` が文字列かつ長さ 1、という判定に変更した。
数値・真偽値は必ず区切り対象になる。

差分ハーネスの参照側も、公式 Scratch と同じ scratch-vm インタプリタで
動かすよう `vm.setCompilerOptions({enabled: false})` を設定した
（TurboWarp のコンパイラは既定で有効）。これで `list_contents.sb3` を含む
15 件が scratch-vm と一致する。

## 14. point towards の atan2（Low）

**再現**: `point_towards_ulp.sb3`

| ターゲット位置 | scratch-vm | 修正前 xyo |
|---|---|---|
| `(-45.079…, 145.565…)` | `-17.206855667874862` | `-17.206855667874834` |
| `(-17.989…, 116.550…)` | `-8.774209501515571` | `-8.774209501515557` |
| `(-57.472…, 130.781…)` | `-23.72316714955369` | `-23.72316714955366` |

`motion_pointtowards` は `90 - atan2(dy, dx) * 180 / pi` で向きを求める。
xyo は glibc の `std::atan2` を使っており、80 個のランダム位置のうち
3 個で V8 の fdlibm `atan2` と最終ビットが違った。

### 修正

V8 14.6 の `atan2` を [v8math.cpp](<../src/core/v8math.cpp>) へ追加し、
`std::atan2` を使っていた 4 箇所（`motion_pointtowards` の interp/JIT）を
`v8math::atan2` に置き換えた。ホスト libcall の `atan2` も同じ実装へ
マップしている。80 個のランダム位置で scratch-vm と完全一致する。

## 15. move steps の sin/cos（Medium）

**再現**: `move_steps_ulp.sb3`

`motion_movesteps` は `radians = pi * (90 - direction) / 180` として
`x += steps * cos(radians)`, `y += steps * sin(radians)` を計算する。
xyo は glibc の `std::cos`/`std::sin` を使っており、128 個の方向・歩数の
うち 10 個で x または y の最終ビットが V8 と違った。

| direction | steps | 軸 | scratch-vm | 修正前 xyo |
|---|---|---|---|---|
| `42` | `10` | y | `7.431448254773942` | `7.431448254773941` |
| `677.56447505852` | `1e-7` | y | `7.380371128908798e-8` | `7.380371128908799e-8` |
| `-79.68115708811217` | `1e7` | y | `1791257.7655303064` | `1791257.7655303061` |
| `230` | `1e-7` | x | `-7.66044443118978e-8` | `-7.660444431189779e-8` |
| `153` | `1` | x | `0.45399049973954686` | `0.4539904997395468` |

### 修正

V8 14.6 の fdlibm から `sin`/`cos` と、その依存である
`__kernel_sin`/`__kernel_cos`/`__ieee754_rem_pio2`/`__kernel_rem_pio2`
を [v8math.cpp](<../src/core/v8math.cpp>) へ移植した。`scalbn`/`floor` は
`std::scalbn`/`std::floor`（どちらもこの用途ではビット一致）を使う。
`motion_movesteps`、`if on edge bounce`、`operator_mathop` の `sin`/`cos`、
JIT の libcall マッピングをすべて `v8math` に統一した。

検証: 128 個の方向・歩数で scratch-vm と完全一致。sin/cos 単体でも
413 個の値（1e300 などの巨大引数、非正規化数、ランダム 400 個を含む）で
Node の `Math.sin`/`Math.cos` と **826/826 ビット一致**。

## 16. sensing_timer のミリ秒量子化（Low）

**再現**: `timer_granularity.sb3`

Scratch の `sensing_timer` は `runtime.currentMSecs`（`Date.now()` の
整数ミリ秒）を起点にした `timeElapsed() / 1000` を返す。つまり
**timer の値は常に 1ms の倍数**である。xyo は `std::chrono` のサブ ms 精度を
そのまま使っていたため、`timer == round(timer * 1000) / 1000` が false に
なった。

| 検査 | scratch-vm | 修正前 xyo |
|---|---|---|
| `timer == round(timer*1000)/1000` | `true` | `false` |

### 修正

`Runtime::updateCurrentMSecs` を整数 ms（`std::floor`）にし、ヘッドレスの
`step` が実時間より速く回っても毎フレーム最低 1ms 進むようにした。
scratch-vm の 30fps では `Date.now()` が毎フレーム進むため、この下限は
「timer がフレームごとに進む」という観測可能な性質を保つ。実行フィクスチャ
`hat-thread-execution.sb2`（`when timer > 0` が 10 回発火することを検査）も
116/116 のまま通る。

## 17. JIT 変数キャッシュのキー不足（Medium）

**再現**: `var_id_name_mismatch.sb3`

SB3 の変数フィールドは `id` と `name` を持つ。scratch-vm の
`lookupOrCreateVariable(id, name)` は **id → name** の順で解決するため、
id が存在せず name だけ一致する場合は name 側の変数が使われる。
xyo の JIT は `Thread::jitVarCache` と `varSlot` を **id だけ**で
キーしていたため、同じ id で異なる name を持つフィールドが混在すると
最初に解決した `Variable*` を別名のフィールドにも使い回していた。

| フィールド (`name`,`id`) | scratch-vm / interp | 修正前 HotSpot |
|---|---|---|
| `("stage","l")` → `L` を読む | `20` | `1` |

（`l` は存在しない id なので name `stage` で解決される。次の
`("local","g")` は id `g` が存在しないため name `local` で解決される、という
id→name の順序が JIT ではキャッシュに潰されていた。）

### 修正

`JitVarCacheKey{id,name,list}`（ポインタ同一性）を追加し、
`varSlotIdx` のキーにも変数名の UTF-16 バイト列を含めた。
これで JIT も呼び出しごとに Scratch と同じ id→name 解決を行う。
再現 fixture は scratch / HotSpot / interp の 3 者で一致する。

## 回帰テスト

| テスト | 内容 | 結果 |
|---|---|---|
| [tests/differential/run_repros.py](<../tests/differential/run_repros.py>) | 19 repro を scratch-vm / HotSpot / interp で比較 | 19/19 一致 |
| [test_scratch_compat.cpp](<../tests/unit/test_scratch_compat.cpp>) | `jsnum` 文字列化、`jsRound`、`jsLog`、`v8math`（atan2/sin/cos 含む）、Final_Sigma、Unicode 17、list contents、NaN リスト | 90 checks / 0 fail |
| [test_nan_codegen.cpp](<../tests/unit/test_nan_codegen.cpp>) | interp / JIT / AOT の `mod`・`round`・`sin/cos`・`asin`・`e^` を含むビット一致 | 113,368 checks / 0 fail |
| `xyo test refs/scratch-vm/test/fixtures/execute` | execute フィクスチャ | 116/116 files, 336/336 asserts |
| `ctest --test-dir build` | unit / render / gpu / UI | 6/6 passed |

## 一致を確認した範囲

修正の前後で、以下のアドホック差分（`out` 全要素比較）も確認した:

- 68 値（数値・数値文字列・空白・`Infinity`/`NaN` 文字列・Unicode・
  サロゲート・全角/半角）の全ペア × `+ - * / mod = < > join contains`
- リストの `item` / `item # of` / `insert` / `delete` / `replace` を
  `1, 2, 0, -1, 1.5, 2.9, Infinity, -Infinity, last, all, any, "", 1e21`
  などで実行
- ブロードキャストの順序、`broadcast and wait`、`wait 0`
- クローンの生成順・ローカル変数コピー・`delete this clone`・400 個生成
- `motion` / `looks` の数値状態（set x/y/direction/size/ghost, change, turn）
- `sensing_of` の実属性（x/y position, direction, size, costume #/name,
  backdrop #/name, volume）と変数/リスト参照
- クローンのローカルリストが複製され、元と独立していること
- `wait until <true>` が yield せず同 tick で進むこと
- 613 個の数値文字列（16 進/2 進/8 進、指数、Unicode 空白、`Infinity`、
  巨大桁など）の `Number()` 変換
- `list contents` の文字列/数値/混在/空リスト
- 80 個のランダムな位置への `point towards` の向き
- 128 個の方向・歩数での `move () steps` の x/y
- sin/cos 単体 413 値の Node とのビット比較
- `sensing_timer` のミリ秒量子化
- 変数フィールドの id/name 不一致（JIT の解決順）

## 制約

- 参照は vendored scratch-vm 2.1.46（TurboWarp）。公式 Scratch 3 と
  TurboWarp で挙動が違う箇所は TurboWarp を基準にした。
- renderer は `FakeRenderer`。`touching` / `distance to` / ペン描画など
  描画依存の意味論は対象外。
- 音声・キーボード・マウス・タイマー・乱数は非決定的なので使っていない。
- `jsLog` / `v8math` は V8（Node 26 / V8 14.6）で検証した。将来 V8 の
  実装が変わった場合は追従が必要。`v8math` は 16,000 個のランダム入力で
  NaN のペイロード以外は完全一致、`toLowerCase` は全 1,112,064 コード
  ポイントで一致する。
- 差分ハーネスは公式 Scratch と同じ scratch-vm インタプリタを参照する
  （TurboWarp コンパイラは `enabled: false`）。TW コンパイラ固有の
  最適化（`list contents` の 1 桁数値など）とは意図的に一致させない。
- `--danger`、GPU 並列ループ、par-for は対象外。GPU シェーダの `mod` は
  f32 の floored remainder のままで、Scratch の `Infinity` 剰余とは一致
  しない（GPU は元々 f32 近似で、今回の修正対象外）。
