# OSR-016 — Scratch repeat 小数境界での3階層OSR復帰

2026-11-09

## 目的

OSR-013で通過した「中間 `times - 15.5`、最深部 `times + 0.5`」の
小数回数を、丸め境界の**直下**と**ちょうど境界**へ分解して検証する。
両カウンターはLLVMの実yield時に元のprivate loop counterから復帰し、
本体で上書きされた`times`の再評価を決して使用しない。

実行検証は `tests/differential/jit_nested_warp_fractional_boundary.py` と
既存OSR-013のfixture生成器・ヘッドレス実行ハーネスを再利用する。
新たに `tests/differential/osr016_fractional_boundary_tests.cmake` にCTest定義を分離し、
共通 `tests/CMakeLists.txt` にincludeを1個追加した。
OSR-014/015や本体ABIは変更していない。

## 4通りの境界

| case | middle TIMES | round | leaf TIMES | round | outer=100000での合計 |
|---|---|---:|---|---:|---:|
| both-below | 17 - 14.500000000001 | 2 | 17 + 0.499999999999 | 17 | 20,400,000 |
| middle-tie | 17 - 14.5 | 3 | 17 + 0.499999999999 | 17 | 30,600,000 |
| leaf-tie | 17 - 14.500000000001 | 2 | 17 + 0.5 | 18 | 21,600,000 |
| both-tie | 17 - 14.5 | 3 | 17 + 0.5 | 18 | 32,400,000 |

合計は root repeat(3) * outer repeat(100000) * middle * leaf * change(sum,2)。
これらの定数はdoubleで0.5境界の上下を確実に分離できる。

## 検証方式

- baseline: JITなしのScratch Interpreter。
- disabled: OSR専用フラグを切った場合、native promotionを拒否しInterpreter一致。
- positive: OSR promotionとnative Fiber yieldが起こり、編集なしでInterpreter最終状態・計数が一致。
- guarded: 本物のphase3 native yieldで`sum`への加算先を`branchCount`に編集。後続効果が新しい変数に反映され、最終合計・保存済み3カウンター・元caller状態が一致。
- `--late`: 同じ編集を4回目のyieldでも追加検証。
- `--without-block-count`: 計数OFFで同じ試験を実行。

## 実行

```bash
python3 tests/differential/jit_nested_warp_fractional_boundary.py \
  --harness board/osr-triple-yield-verified-smoke \
  --out board/.osr-fractional-boundary-counted-v2 \
  --all-cases

python3 tests/differential/jit_nested_warp_fractional_boundary.py \
  --harness board/osr-triple-yield-verified-smoke \
  --out board/.osr-fractional-boundary-fast-v2 \
  --all-cases --without-block-count
```

既存凍結OSR-013ハーネスのSHA256:
`b4456adacebc8fdf62f406345855aa6034e30dd446e5653a3944128fbbe50777`。

注意: `--outer-count 30000`では非同期JITのpromotionが十分に起こらず
`native LLVM never yielded` が発生した。そのため既存OSR-013と同じ
既定値 `100000` とする。テスト失敗時は「nativeに昇格しなかった」
のか「昇格後の復帰が不一致」かを区別する。

## 2026-11-09 検証結果

凍結済みOSR-013実行ファイル（SHA256を上記に記載）で完走。

| モード | both-below | middle-tie | leaf-tie | both-tie |
|---|---|---|---|---|
| 計数ON（baseline / disabled / positive / guarded） | PASS | PASS | PASS | PASS |
| 計数OFF（baseline / disabled / positive / guarded） | PASS | PASS | PASS | PASS |

`both-tie`では計数ON・OFFそれぞれで`guarded-late`も追加実行し、
**4回目の実native yield後のsource edit・三重カウンター復帰ともPASS**。
`guarded`と`guarded-late`は編集前後の二変数分割に対し
`sum + branchCount == expected_sum`、
それ以外の全状態およびON時の実行ブロック数がInterpreterと一致。

本テストは固定されたOSR-013実行ファイルでの結果。
OSR-014/015が並行更新する現在の共有ソースからの再リンク後回帰、
GUI parity、およびCI CTest登録は未実施。
独立したPythonハーネスの実行が完了したことのみ主張する。

Python構文検証と`git diff --check`もPASS。

実行状況は `board/tasks/OSR-016.md` と投稿を参照。

コミット・プッシュはしない。


## 最新の共有ソースからの一貫した再ビルド（OSR-016 継続）

2026-11-09現在の共有ソースを、既存ビルドと独立した
`board/.osr016-current-headless`（Release, LLVM 23, SDL2 OFF）
でビルドし、`tools/consistent-build.ts`のソース前後SHA256照合を通過。

- 新規一貫バイナリ: `board/osr016-current-triple-smoke`
- ソースSHA256: `c61963b51e43e4dac9b763b743acbdb82e0e588f1127961f2197403603449287`
- バイナリSHA256: `707baaa7e5c629db14e1be92b36365fada38b7b745de475f7931cf5cc31e769d`
- ログ: `board/osr016-current-build.log`
- 他作業者の未コミット変更を戻さず、Rendererビルド時の
  `dlss_opaque_coverage.hpp`はCMakeのCXX_FLAGSから強制include。
  ソース本体を変更していない。

この新しいバイナリでも、2x2小数境界4ケースが
計数ON/OFF **それぞれ4/4件PASS**。
どのケースもbaseline/disabled/positive/guardedの比較が成功し、
本物のphase3 native yield・source edit・三重カウンター復元・
変数分割と全状態・ブロック数のInterpreter parityを確認。

- `board/osr016-current-counted.log`: 4/4 PASS
- `board/osr016-current-fast.log`: 4/4 PASS
- `board/osr016-current-late-counted.log`: both-tieの4回目yield後編集 PASS
- `board/osr016-current-late-fast.log`: 同上、ブロック計数OFF PASS

元から登録されている OSR-013 のCTestもこの新しい独立ビルドで実行し、
**4/4 PASS (100.57秒)**。

```bash
ctest --test-dir board/.osr016-current-headless \
  -R '^xyo-nested-warp-triple-yield-(deopt|fractional)(-fast)?$' \
  --output-on-failure --parallel 2
```

- `board/osr016-current-osr013-ctest.log`: 4/4 PASS
- OSR-016専用CTestのソース登録は、他担当が同時編集する
  `tests/CMakeLists.txt`との競合を避けて未実施。
- GUI Parityは別工程。LLVM 23のheadless回帰成功を
  GUI 2123/2123の確認と混同しない。


## 2026-11-09 OSR-016 正式CTest登録・実GUIパリティ検証

新規 `tests/differential/osr016_fractional_boundary_tests.cmake` を
既存OSR-013のCTest群からincludeし、4件を自動登録。
`ctest -N -R '^xyo-nested-warp-fractional-boundary'` で4件を検出した。
従来のCTest定義は書き換えていない。

```bash
ctest --test-dir board/.osr016-current-headless \
  -R '^xyo-nested-warp-fractional-boundary' \
  --output-on-failure --parallel 2
```

| Test | Result |
|---|---:|
| xyo-nested-warp-fractional-boundary | PASS / 170.22s |
| xyo-nested-warp-fractional-boundary-fast | PASS / 158.79s |
| xyo-nested-warp-fractional-boundary-late | PASS / 65.43s |
| xyo-nested-warp-fractional-boundary-late-fast | PASS / 66.36s |

**4/4 PASS, elapsed wall-clock 236.58s**（2並列）。
ログ `board/osr016-official-ctest.log`。
共有ソース変更が最初の再ビルド中に検出されたため、
一貫したバイナリ確定まで2試行。確定したheadlessバイナリは
`board/osr016-ctest-coherent-smoke`（SHA256
`39a807b4738de47c09950f3ba9c7e029db96b530d0ad2343fdb70ecb4b57bd66`）。
本体ソース digest
`c85996e7b0900ee18f45d88ee3cfa6f8faf72a011650129c974a72a1cb6c3437`。

### 実SDL GUI Scratch Parity

独立 `board/.osr016-gui` をLLVM23, SDL2 ON, Releaseで一貫ビルド。
CMakeの `-include src/render/dlss_opaque_coverage.hpp` は同じ局所回避策。
`board/osr016-gui-coherent-xyo` バイナリSHA256
`96077672acff2c76523b716ff320187ba5df2c2247561b8ff2a0ea55f0ceed8f`。

`ctest --test-dir board/.osr016-gui -R '^xyo-gui-scratch-parity$' -V`:
**1/1 PASS (22.48s)**。Scratch fixtureの実際のオラクル判定で
Interpreter, HotSpot, Eagerすべて **2123/2123**。
ログ `board/osr016-gui-parity-ctest.log`。

さらにOSR-013相当の次の実行時opt-inフラグをすべて有効にして、
`tests/differential/gui_scratch_parity_regression.py` を再実行した。

```text
XYO_JIT_OSR=1
XYO_JIT_OSR_NESTED=1
XYO_JIT_OSR_NESTED_DEEP=1
XYO_JIT_OSR_NESTED_PARENT_REPEAT=1
XYO_JIT_OSR_NESTED_ARG_READS=1
XYO_JIT_OSR_NESTED_DIRECT_ARGS=1
XYO_JIT_OSR_NESTED_HOIST_ARGS=1
XYO_JIT_OSR_NESTED_SOURCE_PROOF=1
XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT=1
XYO_JIT_OSR_NESTED_INNER_YIELD_DEOPT=1
XYO_JIT_OSR_NESTED_INNER_DYNAMIC=1
XYO_JIT_OSR_NESTED_TRIPLE_YIELD=1
```

こちらも Interpreter / HotSpot / Eagerすべて
**2123/2123 PASS**。
ログ `board/osr016-gui-parity-osr-optin.log`。

重要：GUI全体のScratchオラクル判定と、実際にnative phase3
yieldからInterpreterへ復帰する専用ヘッドレス試験は別。
GUIフィクスチャ内のすべてのスクリプトがOSRを行ったと主張しない。
実ネイティブ復帰は上記の専用4 CTestsで確認している。

CIでのGitHub Actions実行とGit上のcommit/pushは未実施。
製品ランタイムのソースに変更なし。


## GitHub Actions CI coverage / dedicated OSR job (2026-11-09)

GitHub の最後に確認した成功 CI は
[Actions run 42569913996](https://github.com/pnsk-lab/xyo-cpp/actions/runs/42569913996)
（commit `ce238c7e24b2d512b3a6827f0267ff1caf67810e`、2026-11-09 UTC）。
このリモートCIはコミット済みの変更しか含まない。
今回のOSR-016専用CTest・CI設定は未コミットであり、
**この成功CIをOSR-016のGitHub実行済みと扱わない。**

同runのLinux CTestは112件で1,888.26秒（31.47分）かかった。
そのうち既存 `xyo-nested-warp-*` 15件で921.16秒を占める。
共有ツリーには147〜149件のCTestが登録されており、
ネストwarp 37件を従来Linux 45分ジョブに含めると
タイムアウトしやすい。

`.github/workflows/ci.yml` の変更:

- 従来のLinuxジョブのCTestを
  `ctest --test-dir build --output-on-failure -E '^xyo-nested-warp-'`
  とし、OSRだけを別ジョブへ分離
- 新規 `linux-nested-osr` ジョブは Ubuntu 24.04 / LLVM 23 /
  SDL2 ON / Release / 90分制限
- SDL2 ON は重要。SDL2 OFFでは旧来のCLIベースOSR11件が
  登録されず26件しか実行できないため、すべて37件を
  対象にする
- 新規ジョブは `xyo-cli` と9種類の
  `xyo-nested-warp-...-smoke` だけをビルドし、
  `ctest -R '^xyo-nested-warp-'` を実行
- 実行前に `ctest -N` で最低37件とOSR-016固有4件の
  正確な登録を検査。OSRが無言で未登録になるのを防ぐ
- 失敗時には `build-osr/Testing/Temporary/LastTest.log`
  を独立artifactとして保存

ローカルでCIに相当する設定を検証した:
`board/.osr016-ci-gui-sim`、SDL2 ON、LLVM 23、
選択的ビルド10ターゲットが **PASS**。
標準Linux用とOSR専用の分割は
同じCTest登録全149件のうち112件+37件で
重複・漏れなし。YAMLパーサと`git diff --check`もPASS。
登録数の検査スクリプトも成功。
ログ: `board/osr016-ci-gui-sim-selective-build.log`,
`board/osr016-ci-guard.log`。

代表OSR回帰4件は実ビルドで **4/4 PASS（108.46秒、2並列）**:
`fractional-boundary-late-fast`,
`topology-failclosed-fast`,
`args-osr-refusal`,
`source-proof`。
ログ: `board/osr016-ci-gui-sim-selected-ctest.log`。
この時点では37件の全件実行は未実施だったが、以下の追加検証で完了した。

### CI全ビルドの独立したブロッカー

`board/.osr016-ci-sim` の新規CMake全ビルドは
`tests/xyo-tests` のリンクで失敗。
`tests/unit/jit_cfg_hint_*_smoke.cpp`、
`jit_guard_fallback_*_test.cpp` など複数の独立した
`main()` と`test_cast.cpp`の`main()`が同じターゲットに
混入し、`xyo_jit_guard_slow`も二重定義されていた。
`board/osr016-ci-sim-build.log` にエラーを保存し、
board/generalに報告した。
これはOSR-016のテスト失敗とは別の構成問題であり、
現時点の共有ツリーをそのままGitHubへ反映すれば
通常Linuxジョブのビルドを妨げる可能性がある。
他担当のテスト・ソースやビルド設定には無断修正を加えていない。

**未実施:** 現地GitHub Actions上でのOSR含むrun、commit/push。


## CI相当のOSR全37件一括実行（追補）

以前に選択ビルドを完了した`board/.osr016-ci-gui-sim`を使い、
LLVM23 / Release / SDL2 ON で37件を同時実行した。

```bash
ctest --test-dir board/.osr016-ci-gui-sim \
  -R '^xyo-nested-warp-' --output-on-failure \
  --parallel 4 --timeout 600
```

**37/37 PASS（失敗0）、実時間623.45秒。**
ログは `board/osr016-ci-all37.log`。
OSR-013の3階層復帰、OSR-014の4階層動的repeat、
OSR-015のfail-closed、OSR-016の小数境界と遅延ソース編集、
旧来の引数/GC/分岐/親repeat関連がすべてPASS。
これはGitHub Actionsではなくローカルでの全件結果である。
正式CIは既定の逐次実行であり、この実測623.45秒は
`--parallel 4` を指定したときだけの実測値。

対象バイナリSHA256:

- `board/.osr016-ci-gui-sim/xyo`: `bf280be80f9719d47b0b621ac546ad6c854699be36e17fd85227ef94295657a6`
- `board/.osr016-ci-gui-sim/tests/xyo-nested-warp-triple-yield-deopt-smoke`: `6d4162521c5edfbeebc4ddec354a21c7fc1f28627e403144ad25c69bfe073f8e`

また、先の`xyo-tests`の多重mainリンク障害は、並行する他担当の
`tests/CMakeLists.txt`修正（`unit/*.cpp`から`unit/test_*.cpp`へ対象限定）で
解消され、当該ターゲットの再ビルドでexit 0を確認した。
全体掲示板にも修正確認を報告済み。


### 追加の全ターゲット・非OSRスモーク検査

共有ツリーの`tests/CMakeLists.txt`におけるunitソースの絞り込み修正後、
`board/.osr016-ci-gui-sim`で
`cmake --build board/.osr016-ci-gui-sim --parallel 4`
を実行した結果、全ターゲットのリンクが成功した
（`board/osr016-ci-gui-sim-full-build.log`）。

ただし、別途実行した通常の単体テストにはOSR以外の失敗が残る。

- GUI Scratch oracle: PASS（27.85秒）
- `xyo-tests`: FAIL（61.59秒）
  - `numeric motion`: 11,549 checksのうち5 failures
  - `nan codegen`: 113,368 checksのうち6 failures

`nan codegen`の6件はすべて結果index=1041（式index=347、
`sin(x)`）で、入力index=1/19の負のゼロについて
実際は`+0`、参照モデルは`-0`と期待している。
Interpreter/Eager/AOTのいずれも同様に不一致であり、
ランタイム内のsin(±0)明示的`+0`処理と参照モデル側の
符号保存の不整合が疑われる。根拠:
`tests/unit/test_nan_codegen.cpp`、
`src/jit/rt.cpp`の`xyo_mathop`、
`src/jit/codegen.cpp`の`emitMathop`。

`numeric motion`の5件は未切り分け。
この問題はOSR-016の37/37合格とは独立で、
他担当のMATH-006とgeneral掲示板へ報告済み。
専有外のテスト・数値実装コードは改変していない。

ログ:
`board/osr016-ci-gui-core-smoke.log`、
`board/.osr016-ci-gui-sim/Testing/Temporary/LastTest.log`。

**GitHub Actions上のOSR-016実行は依然未検証。**
