# START-001: Eager JIT巨大スクリプトの起動高速化（2026-11-09）

## 原因と修正

ScratchBench parity.sb3（2656件）の巨大なトップレベルスクリプト
`~1rf` は241関数・439605 IR命令を含む。従来のEager JITでは
最初のプロジェクト出力より前にLLVM最適化に約5秒、
ORCオブジェクト生成・解決に約6秒かかっていた。

`src/jit/backend_llvm.cpp` の既存のORC遅延マテリアライズを再利用し、
EagerのGUI起動に限り、**64関数以上かつ10万IR命令以上**の単位で
エントリだけを先にJITし、未使用手続きを初回呼び出し時にコンパイルする。
小さなEagerスクリプトとHotSpotの動作は従来どおり。
ヘッドレスEagerは総処理時間を優先して従来の事前コンパイルを維持する。

元の動作へは `XYO_JIT_FAST_START=0` または
`XYO_JIT_LAZY_MODE=off` で戻せる。
`XYO_JIT_LAZY_MODE=on` の明示指定は引き続き使える。

## 起動時間

同じSDL/LLVM Releaseバイナリを使い、
最初のScratchプロジェクト出力までを交互に3回ずつ計測。
システム負荷やキャッシュにより値は変動する。

| 回 | 自動遅延 | `XYO_JIT_FAST_START=0` |
|---|---:|---:|
| 1 | 2619.3 ms | 11775.9 ms |
| 2 | 3962.1 ms | 12120.4 ms |
| 3 | 2537.9 ms | 13380.9 ms |
| **中央値** | **2619.3 ms** | **12120.4 ms** |

起動時間 **78.4%減、4.63倍高速化**。プロファイルでは巨大単位が
`tier=lazy`、小規模単位が `tier=eager` であることも確認。
O1/O2に最適化レベルを下げる手法は改善しなかった。

## 互換性とトレードオフ

- 実装変更後、Eager・HotSpot・Interpreterすべて
  **Scratch期待値2656/2656 PASS**。
- `tests/differential/gui_eager_startup_tier.py` PASS。
  起動時間の数値でCIを落とさず、最適化方式を実際のGUIで検査する。
  CIのScratchBenchジョブにも登録済み。
- 遅延処理は実行中に移る。別の1回ずつの完走計測では
  最適化ONが起動5.55秒＋実行29.03秒＝**34.58秒**、
  OFFが起動11.11秒＋実行17.81秒＝**28.91秒**で、
  **総実行時間は悪化**した。GUIの初期応答性を優先する選択であり、
  一括処理の速度改善を意味しない。ヘッドレスは自動対象外。
- 未コミット・未push。CIの変更は未実行。

## 再現

```bash
BIN=/path/to/xyo
SB3=scratch-bench/.cache/build/parity.sb3
python3 tests/differential/gui_eager_startup_tier.py --native "$BIN" --project "$SB3"
python3 tests/differential/gui_scratch_parity_regression.py \
  --native "$BIN" --project "$SB3" --minimum 2656 --modes eager hotspot off
```

共有作業ツリーには他のエージェントの未完了なnumeric_region実験があり、
直接の全体ビルドは一時的に失敗した。このテストではキャッシュ内の
ソース複製のみで未使用の実験用includeを除去し、数値repeatの
追跡済み実装を使った独立Releaseビルドで検証した。
共有ツリーの他の機能や差分は上書きしていない。
