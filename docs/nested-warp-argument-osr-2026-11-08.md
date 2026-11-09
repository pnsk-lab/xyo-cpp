# 引数付き warp 手続きの限定 LLVM OSR（2026-11-08）

## 実装

ネストした `control_repeat` だけを LLVM の専用ネイティブエントリで走らせ、
周辺の `procedures_call` と元のインタプリタフレームは保持する。
**手続き呼出し元を最初から再実行しない**。
ネイティブループの終了後は残りのインタプリタ手続き本体に戻る。

今回の変更は二段階。

1. `XYO_JIT_OSR_NESTED=1`：引数が存在しても、コンパイル対象のループ
   (`TIMES` を含む) が引数レポーターを使わない場合は移行できる。
   `InterpreterOsrSnapshot::capture` で GC ルート済み引数・スタック位置・
   ソース版数・呼出し元との lexical path・repeat 残り回数を検証し、
   `procedures_prototype` の引数名と件数に完全一致する場合のみ許可する。
   引数は最大16個、名前の重複や欠落は許可しない。

2. `XYO_JIT_OSR_NESTED_ARG_READS=1`：引数レポーターがループ内にある場合も
   **明示的な追加 opt-in** で試せる。静的な検証対象は
   `argument_reporter_string_number` / `argument_reporter_boolean` で、
   名前が呼び出し先の宣言済み引数に一致するものに限る。通常のLLVMコード生成は
   未解決の引数レポーターを `xyo_rt_reporter` 経由でインタプリタ評価し、
   継続中の元 `Thread` の引数マップから取得する。
   さらに並行開発で追加された `XYO_JIT_OSR_NESTED_DIRECT_ARGS=1` は
   `xyo_rt_nested_osr_arg_get` を使って、2段の元インタプリタフレームから
   引数を直接参照する。継続状態が想定外の場合は元のレポーター評価へ退避。
   直接経路の実装元は `src/jit/codegen.cpp` と `src/jit/rt.cpp`。
   この実験設定に限り、純粋だがGCオブジェクトを生成する `operator_join`
   も許可する。ソースグラフの静的走査は最大256ノード。
   **動的な手続き呼出し、待機、ブロードキャスト、拡張効果、未知 reporter は拒否**。

どちらも `XYO_JIT_OSR=1` かつ HotSpot/LLVM JIT の opt-in 条件が必要。
既定では両方 OFF。 `XYO_JIT_OSR_TIMING=1` で promotion/restore を確認できる。
診断時には `XYO_JIT_OSR_NESTED_TRACE=1` がスナップショット/引数検証の結果を出す。

## 確認済み

- 引数 `text=「引数のGCルート 🍀」` と `delta=2` を含む warp 手続き。
  2,000万回の repeat に移行し、残り17,640,202回からネイティブ再開。
  prefix=2、sum=20,000,000、suffix=2、messageとdoneがInterpreterと一致。
  **6/6** のモード比較・フォールバックテスト合格。
- 引数 `delta=2` の reporterを repeat の加算に直接使う経路は追加フラグのみ有効。
  2,000万回ループで残り17,881,697回からネイティブ移行し、
  sum=40,000,000、GC文字列message、prefix/suffixが元のInterpreterと完全一致。
  フラグがOFFなら引数依存ループは移行を拒否した。
- GCストレスでは引数からの `operator_join` による文字列生成を
  500万回行い、`XYO_GC_THRESHOLD=1` で **368回 GC回収**。
  ネイティブ移行発生後も、引数と結果がInterpreterに一致し、
  手続き復帰後の日本語文字列が破損しなかった。
  レポーター経由と引数直接読み出しの両方で368回のGCを確認。
- `TIMES` 自体が引数レポーターの `repeat` を2000万回実行し、
  オプトインで元インタプリタの残り回数だけをネイティブへ移すことを
  検証。結果・suffixはInterpreterと一致。
- `safeNestedValue` は、実入力が欠落したときに評価されるshadowを
  検査から漏らさず、有効な入力（本体またはshadow）を再帰検証するよう強化した。
- GUI Scratch Parity Test の成功判定は xyo バックエンド同士の一致と混同せず、
  Scratch プロジェクトの出力oracleを検証する。
- 独立 Ninja + `tools/consistent-build.ts` で同一ソースに対応した
  バイナリだけを固定して検証。共有ワークツリーの旧ビルドは使用しない。

## 回帰テスト

- `tests/differential/jit_nested_warp_args_preserved.py`
- `tests/differential/jit_nested_warp_argreads.py`
- `tests/differential/jit_nested_warp_argroots_gc.py`
- `tests/differential/jit_nested_warp_argtimes.py`
- 従来の `tests/differential/jit_nested_warp_args_refusal.py`
- `tests/differential/jit_nested_warp_osr.py`
- GUIオラクル `tests/differential/gui_scratch_parity_regression.py`

CTestでは `xyo-nested-warp-args-preserved-osr`、
`xyo-nested-warp-argument-reads`、`xyo-nested-warp-argument-gc`、
`xyo-nested-warp-argument-times` を登録。

実験設定例：

```bash
XYO_JIT_MODE=hotspot \
XYO_JIT_ASYNC=1 \
XYO_JIT_OSR=1 \
XYO_JIT_OSR_NESTED=1 \
XYO_JIT_OSR_NESTED_ARG_READS=1 \
XYO_JIT_OSR_NESTED_DIRECT_ARGS=1 \
XYO_JIT_OSR_TIMING=1 \
  ./build/xyo play project.sb3
```

## 制約

- ネイティブ移行可能なのは1段の手続き呼出しと単一 `repeat` の
  **ソース構造を認証できた**場合だけ。深い再帰や複数の同時フレームの
  Source-PC/Return-PC再構築ではない。
- 引数レポーターの再利用は既存インタプリタトランポリンを介するため、
  native用SSA値として毎回ループ本体に格納するより負荷が高い可能性がある。
  大きな性能向上を保証しない。
- 効果のある命令を含むループは依然として選択対象外。
  追加の動的効果や例外的なロールバックは未実装。
- コード変更を伴う途中移行はここでは扱わず、IR-001 / RT-001 の
  元ソース版数・effect commit位置の検証が引き続き必要。
- 実行中の既存 Interpreter StackFrame は GC Root Provider から参照され、
  `Native` へ渡した引数ビット列を生存中に複製/変更しない。

## 引数読み出し性能の比較（参考）

同一20,000,000回の引数依存 `change variable by`、同一Releaseバイナリ、
各2回の中央値。JIT最適化・初期化を含む壁時計時間：

| 実行経路 | 2回中央値 |
|---|---:|
| Interpreter | 5.085秒 |
| LLVM OSR + `xyo_rt_reporter` | 2.879秒 |
| LLVM OSR + `xyo_rt_nested_osr_arg_get` | **0.812秒** |

このプロジェクトでは直接読み出しがInterpreter比約6.3倍、
旧トランポリン比約3.5倍。100%共通の数値のトランザクション計測ではなく、
**このソースに限った壁時計・段階移行込みの比較**。他作品のスコアを保証しない。

証拠：`board/osr-direct-argreads-speed.log`、
`board/osr-direct-argreads-test.log`、
`board/osr-direct-gc-test.log`。
