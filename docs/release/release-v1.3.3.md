# v1.3.3 — scratch-bench リグレッション修正

v1.3.2 で混入した `xyo.parfor.try` の dialect 宣言漏れを修正する
hotfix リリース。性能系の新規機能は無い。

## 修正

- **`xyo.parfor.try` を `dialect::isRegionOp`/`regionCount`（1 region）へ
  登録**（`src/mlir/ir.cpp`）。
  v1.3.2 の par-for 実装は `xyo.parfor.try` を並列本体 region 付きで emit
  するが、dialect の region 数テーブルが未更新のままだった。内部 verifier が
  `region count mismatch` で全ての par-for 候補スクリプトを拒否し、
  `tryEmitScript` が false を返して **script 全体が legacy codegen へ
  サイレントにフォールバック**していた。scratch-bench の
  `performance.sb3` では green-flag driver script が全カーネル proc を
  含むため、1 件の失敗で全カーネルが legacy に落ち、
  `math_arith` 194M→0.26M ops/ms（約 745 分の 1）等の全面的な性能低下に
  なっていた（v1.3.1 比で scratch-bench の中央値がほぼ半減した原因）。

## 検証

- `XYO_JIT_MODE=hotspot` で `performance.sb3` を実行し MLIR fallback
  0 件を確認（修正前は `~Mv: verifier failed (region count mismatch for
  xyo.parfor.try)`）。
- 代表カーネル（`XYO_FPS=30`, `SDL_VIDEODRIVER=dummy`）:
  `math_arith` ≈194M、`loops_repeat` ≈777M、`loops_nested` ≈730M、
  `vars_setchange` ≈382M、`loops_ifelse` ≈294M、`vars_many` ≈75M
  ops/ms。いずれも v1.3.1 水準またはそれ以上に復帰。

## 備考

- v1.3.2 のタグ/リリース成果物はこの regression を含むため、
  scratch-bench 系の計測には v1.3.3 以降を使うこと。
