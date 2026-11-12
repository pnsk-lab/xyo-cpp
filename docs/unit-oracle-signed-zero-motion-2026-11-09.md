# QA-001: xyo-tests の誤った参照値修正（2026-11-09）

## 症状

LLVM 23 / Release / SDL2 ON の共有ビルドで OSR専用CTest 37/37件はPASSしたが、
`xyo-tests` は 11件の失敗を記録した。GUI Scratch ParityはPASS。

- `numeric motion`: 11,549 checks中5件失敗。
- `nan codegen`: 113,368 checks中6件失敗。
- 変更前ログ: `board/osr016-ci-gui-core-smoke.log`。

## Numeric Motion（誤った固定境界）

元テストはBounce後の中心座標を `|x| <= 220 && |y| <= 170` と固定していた。
現在のScratch互換 `bounceOffEdge()` は回転角・サイズ・可視性に応じて
描画可能画素中心の外接領域を最大半画素強だけ内側に補正するため、
Spriteの中心座標が固定境界をわずかに超えても正常にフェンスされる。

他担当のテスト本体を直接計測変更せず、専用コピー `board/.qa001/motion_debug.cpp` を
別バイナリ `board/.qa001/motion_debug` としてコンパイルして実測した。
5件の失敗はすべて `tests/unit/test_motion_numeric.cpp` の最終境界判定のみだった。

| 初期位置 | 方向 | bounce後の中心位置 |
|---|---:|---|
| (230,0) | 37° | (220.70022526659966,0) |
| (230,0) | 90° | (220.5,0) |
| (240,180) | 45° | (220.70710678118655,170.70710678118655) |
| (-240,-180) | -135° | (-220.70710678118655,-170.70710678118655) |
| (0,175) | 0° | (0,170.5) |

Interpreterの `bounceOffEdge` とJITの `xyo_mot_edgebounce` の位置・方向は全例一致。
最終境界だけをレンダラ仕様の `pixelInset = abs(size/200) *
(|cos(angle)|+|sin(angle)|)`（不可視なら0）に対応させた。
許容数値誤差は1e-9。

## NaN Codegen（sin(-0)の参照モデル）

全6件の失敗は `output=1041`（式347、`sin(x)`）で、
負のゼロを含む入力番号1,19に集中した。
実測値 `+0` (bits `0000000000000000`) に対して、
参照モデルの期待値は `-0` (bits `8000000000000000`) だった。
Interpreter、JIT Eager、AOTのいずれも同じ結果。

Scratchの `toFixed(10)` が `sin(-0)` を正のゼロにするため、
既存 `src/blocks/core.cpp` `src/jit/rt.cpp` `src/jit/codegen.cpp` は
明示的に `+0` を返す。独立したテストの参照モデルだけが
直接 `std::sin(-0)` を使用していたため、ゼロ入力を `+0` に補正した。

## 変更範囲と再検証

修正は次の2つの既存テストの期待値のみ。

- `tests/unit/test_motion_numeric.cpp`
- `tests/unit/test_nan_codegen.cpp`

製品ランタイム、JIT、描画処理は変更なし。

`cmake --build board/.osr016-ci-gui-sim --parallel 3 --target xyo-tests` はPASS。
再実行した `ctest -R '^(xyo-tests|xyo-gui-scratch-parity)$' -j2` は**2/2 PASS**。

- `xyo-tests`: **46.29秒 PASS**。`numeric motion` **11,549/11,549**、`nan codegen` **113,368/113,368**。
- `xyo-gui-scratch-parity`: **22.91秒 PASS**。
- ログ: `board/qa001-rebuild.log`、`board/qa001-core-ctest.log`、CTest LastTest.log。

OSR専用CI 37/37件PASS（`board/osr016-ci-all37.log`）はこの変更に先立つ結果。
修正後にOSR抜粋2件も再リンクして再実行し、**2/2 PASS（72.93秒、2並列）**を確認した。

- `xyo-nested-warp-fractional-boundary-late-fast`: PASS（46.39秒）
- `xyo-nested-warp-topology-failclosed-fast`: PASS（72.93秒）
- ログ: `board/qa001-osr-rebuild.log`、`board/qa001-osr-smoke.log`。
GitHub CI実行、commit、pushは未実施。

## CIの通常・ネイティブ復帰テストを分離して再実行（追補）

共有ツリーの再ビルドと通常CTestを実施。初回の通常115件は
**113/115 PASS**（4並列365.67秒）で2件が失敗した。

1. `xyo-native-simple-deopt-parity` は `TIMEOUT 120` 秒に到達。
   `tests/differential/native_simple_resume.py` を単独で完走させ、
   **915 native resume/fail-closed parity runs 全PASS**。
   120秒では試験規模に対して不足していた。
2. `xyo-ir001-postopt-write-audit-parity` は監査ログの解析に失敗。
   共有ソースの `proven_local_calls` の追加と古いバイナリが不整合だった。
   `bun tools/consistent-build.ts` で再リンクした後、IR-001 O2/O3双方が
   **1/1 PASS（0.21秒）**。ログ `board/qa001-ir001-audit-coherent.log`。

整合ビルドのsource digest:
`60c650f515a1a8860ab1ba5c9220e8f73e3c32460ea42377c42860192d285298`。
CLI SHA256:
`a2e4826d06998cb8cfa0cb3ba70ca2544e0149e20a13e964b714502988582d3d`。

### CTestとGitHub Actionsの変更

- `tests/CMakeLists.txt`: `xyo-native-simple-deopt-parity` のTIMEOUTを120から600秒へ。
- 公式CTestは **1/1 PASS、147.52秒**。915回の実yield/deoptとfail-closedを実施。
  ログ: `board/qa001-native-simple-ctest600.log`。
- `.github/workflows/ci.yml`: 非warp native resumeをOSR専用ジョブへ移動。
  CIのCTestフィルター: `^(xyo-nested-warp-|xyo-native-simple-deopt-parity$)`。
- 専用90分ジョブに **38件**、通常Linux45分ジョブに **115件**
  （生成済みScratchBench全量Parityを含むローカル登録値）。
- `ctest -N`、YAML解析、専用38件と必要な個別テスト名のガードはPASS。
  ログ: `board/qa001-ci-partition-guard.log`。

### 修正後の通常Linuxスイート

実行: `ctest --test-dir board/.osr016-ci-gui-sim -E '^(xyo-nested-warp-|xyo-native-simple-deopt-parity$)' --output-on-failure --parallel 4`。

**115/115 PASS、失敗0件、302.18秒（4並列）**。
`xyo-tests`、通常GUI Scratch Parity、ScratchBench全量GUI Parity、
IR-001監査、JIT/スケジューラ/描画/音楽関連の登録済みテストが全件PASS。
ログ: `board/qa001-ci-regular-final.log`。

※GitHub Actions自体は未実行であり、ローカル4並列の所要時間を
GitHubランナーの逐次実行時間とは同一視しない。
共用ツリー全体のcommit/pushは実施していない。

### 専用CI 38件の最終一括実行

現在の共有ソースを再リンクした後、専用CTestを次の条件で再実行した。

`ctest --test-dir board/.osr016-ci-gui-sim -R '^(xyo-nested-warp-|xyo-native-simple-deopt-parity$)' --output-on-failure --parallel 4`

**38/38 PASS、0 failures、666.01秒（4並列）**。

Native simple resume/fail-closed 915-run parityは、同時に他のネストwarpテストが動いている中でも
**158.01秒でPASS**。OSR-016の小数境界4件、3/4階層yield、動的repeat、
ソーストポロジのfail-closed、引数・GC・スタック復帰もすべてPASS。

ログ: `board/qa001-ci-dedicated-final-build.log`、`board/qa001-ci-dedicated-final.log`。
最新のCI YAMLでは専用CTestに`--parallel 2`を指定。これは検証済みの4並列より
低い同時実行数で、GitHubランナーのCPU/メモリ負荷を制限する構成。

**ローカルCI相当の検証合計：通常115/115＋専用38/38＝153/153 PASS。**
GitHub Actionsでこの差分を動かした結果ではない。
