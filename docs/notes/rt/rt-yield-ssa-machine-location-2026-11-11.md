# RT-001：yield地点のboxed i64／pointer SSAを機械StackMapへ写す分離診断（2026-11-11）

## 結論

LLVM23で実際のScratch Fiberの`xyo_yield`／`xyo_yield_fast`直前に、従来の**呼び出し位置用StackMap**に加え、最適化後の生存LLVM SSA値を載せた独立StackMapを作る実験を行った。

2帽子・24,000回文字列連結の実プロジェクト`ssa_budget_cross_fiber_two_hats`では、4つのyield地点で合計**160個の生存i64 SSA**と**454個の生存ポインタSSA**が検出された（同じ値が複数のyield地点で数えられるため、プロジェクト内のユニーク値数ではない）。

`llvm-readobj --stackmap`から各IDの実機械location数を抽出し、コンパイラの期待数と一致することを確認した。独立のyield-edge ELF oracleで`xyo_yield`への再配置済みCALL命令と対応し、新旧StackMap間にCALLや分岐が割り込まないことを逆アセンブルで検証した。

**重要な不具合：生存i64 SSAと生存pointer SSAを1つのStackMapに同時に載せると、ScratchのUnicode文字列変数がポインタ風数値へ破損した。** ループカウンタ`y=48005`が一致しても`x`と`z`が破損していたため、「一部の変数が合っている」を合格条件にしてはいけない。整数のみ、ポインタのみの別々の実験では対象プロジェクトで結果一致したが、根本原因（レジスタ割付の干渉、LLVM backendの問題、スタック配置の変化、既存ランタイムの潜在不具合など）は未確定。

ポインタoperand数の上限を0、1、8、16、32、48、64、80、96、104、112、120件に変えると、一部の中間上限だけが破損した。**閾値に単調性がなく、単に80件以下なら安全とも言えない。**

このため**混合実験はユーザー指定を含め拒否**する。実験フラグを明示したとしても未検証の混合StackMapを生成せず、`[rt001-yield-ssa-refused] reason=unsafe_combined_ssa_probe ... gc_safe=0`のみ出力する。デフォルトでは新規の診断自体が完全に無効。GCゲートに一切の変更はない。

## 実装

- 新規`src/ir/rt_yield_ssa_machine_stackmap.hpp`：
  - `emitExperimentalYieldSsaSnapshots`は、最適化後の`xyo_script_*`内の直接yield呼び出しのみを対象とし、既存`yieldEdgeStackmapId`を持つ正規の直接呼び出しgeometry StackMapが隣接していることを検証。
  - `ir::analyzeLlvmCheckpointLiveness`と`rt::analyzeExtraLiveSsa`で保守的な生存i64／pointer SSAを列挙し、アドレスや値の重複operandを除去したうえで新しい`llvm.experimental.stackmap`へ追加。最大256 locationを超えたり、livenessが不正な場合には発行拒否する。
  - i1/i32や浮動小数点の値は未対応数として記録。生存値の種類をすべて網羅したという意味ではない。
  - SSA StackMap IDは`rt001.yield.live.ssa`ドメインで生成し、従来の直接yield Edge Mapと衝突させない。すでに同一IDのSSA StackMapが存在する場合は重複発行を拒否する。
  - `YieldSsaSelection::I64Only`／`PointersOnly`／`ContextOnly`で生成operandを分離。正規のGC root certificateとして使用しない。
- `src/jit/backend_llvm.cpp`：
  - `XYO_RT_YIELD_EDGE_STACKMAP_PROBE=1`かつ`XYO_RT_YIELD_LIVE_SSA_STACKMAP=1`時にのみ試験を行う。
  - `XYO_RT_YIELD_SSA_FILTER=i64`ならi64だけ、`pointer`ならポインタだけ。混合`all`およびフィルター未指定は**発行拒否**。診断用途の`context`は従来の最小引数確認のみ。受け付けるフラグのいずれでも`ownership_proven=0 machine_roots_proven=0 gc_safe=0`を常に出す。
  - `XYO_RT_YIELD_SSA_POINTER_LIMIT`を使い（分離実験用のみ）、ポインタlocation数のレジスタ割付依存を切り分けた。この上限は安全な件数を意味しない。
- 新規`tests/platform/rt_yield_ssa_machine_stackmap_smoke.cpp`：
  - LLVM O3の実IRで、yield後にも利用するi64 SSAとpointer計算を生成。既存geometry Mapなしの試行を拒否すること、正規geometry Mapの直前なら新SSA Mapを追加できること、LLVM verifierが通ること、二重挿入を拒否することを`-UNDEBUG`有効で検証。
- 新規`tests/differential/rt_yield_live_ssa_machine.py`：
  - 最初に混合指定を意図的に無指定で試み、**明示拒否され、Interpreterと全変数が一致し、SSA StackMapが出ないこと**を確認。
  - i64のみ／pointerのみ、Eager／Lazyの4構成を実際のxyo CLIで検証。各構成で4つの直接yieldのSSA Map IDを収集し、実ELFの各location数を比較。
  - 既存のnative CALL幾何学的oracleも実行し、2種類のStackMapが16バイト以内の同じCALLエッジに対応し、中間の逆アセンブル命令に分岐やCALLがないことを要求。
  - LLVM O2とO3の両方で確認。全変数がInterpreterと一致しなければ失敗。
- `tests/CMakeLists.txt`：`xyo-rt001-yield-ssa-stackmap-smoke`および`xyo-rt001-yield-ssa-machine-separated-smoke`を登録。

## 実測データ（各値は4地点での合計）

| JIT | SSA診断 | i64 SSA | pointer SSA | ELF location数照合 | Interpreter一致 |
| --- | --- | ---: | ---: | --- | --- |
| O3 Eager | i64のみ | 160 | 454 | 4/4 | PASS |
| O3 Lazy | i64のみ | 160 | 454 | 4/4 | PASS |
| O3 Eager | pointerのみ | 160 | 454 | 4/4 | PASS |
| O3 Lazy | pointerのみ | 160 | 454 | 4/4 | PASS |
| O2 Eager | i64のみ | 160 | 454 | 4/4 | PASS |
| O2 Lazy | i64のみ | 160 | 454 | 4/4 | PASS |
| O2 Eager | pointerのみ | 160 | 454 | 4/4 | PASS |
| O2 Lazy | pointerのみ | 160 | 454 | 4/4 | PASS |

`i64/pointer SSA`欄はいずれも**検出値の合計**を表し、各構成で実際にStackMapへ挿入したoperand数そのものではない。分離モードでは指定した種類のみを挿入する。整数/ポインタの合計が一定だからといって、GCルート集合が完全だと判断しない。

## 意味上の制限と残課題

1. **機械locationはその時点の値の位置**であって、GCが移動・解放する管理対象かどうか、所有権が正しいか、当該ルートが値として有効かを証明しない。
2. 実FiberはStackMapそのもので止まらず、`xyo_yield`のホスト実装内で`xyo_fiber_switch`を介して止まる。CFIを用いた中断時のレジスタ・stack homeの一対一復元はまだ全値について未証明。
3. `xyo_wait_yield`、イベント待機、非同期レポータなど、ヘルパ内部の中断にはこの直接yield診断を適用しない。
4. 混合StackMapによる文字列破損の根本原因は未確定。安全な上限を仮定したり、GC禁止を解除してはならない。
5. フィルタOFFの標準ビルド／ランタイムには影響しない。未コミット、他担当変更は保持。

全テストログは`build/rt001-yield-ssa/`以下。全リポジトリCTestは未実施。
