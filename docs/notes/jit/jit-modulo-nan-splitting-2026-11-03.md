# JIT剰余のNaN判定分離とスラブ割当試験（2026-11-03）

## 概要

ScratchBenchの `math_arith` は正の整数の固定除数による剰余で、成功する整数高速経路であっても、すべての分岐を合流させた後に `fcmp uno` とNaN正準化を実行していた。`src/jit/codegen.cpp` の `emitArithNumeric(AOp::Mod)` を変更し、**整数経路ではNaNフラグを定数false、浮動小数点フォールバック経路では実際のNaNチェック**としてSSAに合流させる方式を試した。

実装は固定除数と可変除数の両方に対応。整数経路の戻り値が正確にNaN以外であることだけを用いるため、整数ではない値、負のゼロ、±Infinity、NaN、ゼロ除数などは従来の浮動小数点経路・正準化へ落ちる。非NaNなら通常のIEEE doubleの生ビット、NaNならScratchの正準 `kNan`、次の `toNumber` では0という不変条件を維持する。

無効化用のコンパイル時スイッチ: `XYO_JIT_MOD_NAN_SPLIT=0`。未指定では有効。ランタイムの反復ホットパスに環境変数の条件分岐は追加しない。以前の `XYO_JIT_SAFE_ARITH_CANON` も維持。

### JIT実オブジェクト

同じ `math_arith.sb3` のORC JIT実オブジェクトから、`xyo_script_0_p2` の静的逆アセンブルを比較した。比較は同一Release実行ファイル内のON/OFF（`XYO_JIT_DUMP_OBJECT` と `XYO_JIT_DUMP2`）。

| 指標 | OFF | ON |
|---|---:|---:|
| IR行数 | 857 | 865 |
| x86-64機械語命令数 | **869** | **833** |
| `movsd` 命令 | 28 | 28 |
| `callq` 命令 | 24 | 24 |

IR行数はPHIの追加で少し増えるが、機械語には整数fast pathに不要なNaN判定と関連処理が減る。これは静的な計数であり動的命令数ではない。生ファイルは `build/mod-nan-asm-{on,off}.ll` と `build/mod-nan-asm-{on,off}.asm`。

### 比較試験

i7-12700上の同一Releaseバイナリ（`build/probe-mod-nan-xyo`）、CPU 12固定、ScratchBenchの原文を複製した単独SB3、各8ペア。ベンチ内ウォームアップ・計測を維持し、各ペアでON/OFFの順を交替、ペアごとのops/ms比の中央値。起動・コンパイル時間は対象外。全ケースのchecksumが一致。

| ケース | ON/OFF速度比 |
|---|---:|
| `math_arith` | **1.171倍** |
| `list_read` | **1.170倍** |
| `math_expsqrt` | 1.026倍 |
| `algo_primes` | 1.010倍 |
| `app_brainfuck` | 1.007倍 |
| `math_round` | 1.000倍 |
| `math_nbody` | 1.002倍 |
| `str_join` | 1.006倍 |
| `app_raycast` | 0.981倍 |
| `algo_sort` | 0.977倍 |

`app_raycast` の一部ペアは実行環境負荷で大きく乱れている。小差は断定しない。ソース変更はJITの型表現であり、ScratchBenchのブロック書換えを伴わない。

生データ `build/mod-nan-paired.json`、ログ `build/mod-nan-paired.log`。

### 検証

- `tests/unit/test_type_profile.cpp` のScratch入力に `x % x`（可変除数）・`x % 0`（ゼロ除数）を追加。0.5・負の小数・32bit境界・最大有限値・非正規化最小値・負のゼロ・±Infinity・文字列などをインタプリタ/Eager JITで照合。
- `cmake --build build --target xyo xyo-tests` 成功、`ctest --test-dir build --output-on-failure -j4` は **7/7成功**（`build/mod-nan-unit-ctest.log`）。
- `XYO_BIN=.../build/probe-mod-nan-xyo bun run bench --runtime xyo-cpp --suite parity --no-variants --offline` によるHotSpot JITの **2123/2123成功**（`build/mod-nan-parity.log`）。
- ScratchBenchの元のperformance.sb3はON/OFFとも **47項目完走・DONE・終了コード0**。時間依存schedulerを除く**39項目のchecksumがすべて一致**（`build/mod-nan-full-{on,off}.out`）。

### 不採用：GCスラブ空きビットのスレッドごとキャッシュ

別の試作として、`Gc::allocSmallTls` で64セル分の空きビットをTLSへキャッシュし、各割当でのスラブbitmap走査を減らす方式を試した。既存の `poolGen_` によるキャッシュ無効化と世代整合性を考慮し、`XYO_GC_FREEMASK_CACHE=0` の同一バイナリでOFFとの交互比較を行った。

`str_join` は **ON/OFF 0.982倍**（8ペア中央値）、`str_letter` 0.998倍、`str_contains` 1.000倍、`str_cast` 1.000倍、`list_read` 0.998倍。追加のTLSアクセスと分岐のコストが、もともと安いbitmap走査を上回った可能性がある。こちらは**完全に撤回**し、`src/core/gc.cpp` と `src/core/gc.hpp` の追加試験コードはソースから除去した。

GC測定: `build/pool-mask-paired.json` と `build/pool-mask-paired.log`。

## 状態

採用したのは `src/jit/codegen.cpp` の剰余NaN分離とパリティ試験の拡張のみ。前ターンまでの他の未コミット変更を保持し、コミット・pushは実施していない。

次の調査対象: 新しい `algo_sort`・`app_raycast` での小さな回帰が静的コード配置・レジスタ圧迫によるものか、実行時分岐分布によるものかを確認する。整数専用ループを一般に生成する値域解析は引き続き未実装。
