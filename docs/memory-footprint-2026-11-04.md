# xyo-cpp メモリ使用量の削減 (2026-11-04)

## 測定

Linux/x86_64の同一ビルド、同一SB3で、各実行を別プロセスとして起動し `resource.getrusage(RUSAGE_CHILDREN).ru_maxrss`（KiB）を記録。ピークRSSはOSが観測した最大常駐ページ量で、GCの論理割当量やGPU VRAMではない。CLIの`run`は描画なし、`play`はSDL offscreen rendererを初期化。パリティSB3の`run`は視覚依存テストが失敗するため、`run`はメモリ比較にのみ使用し、正当性は`play`で確認した。

| 入力 | ブロック数 | 描画なし・インタプリタ変更前 | 変更後 | 削減 |
|---|---:|---:|---:|---:|
| 60手続きの合成SB3 | 4,512 | 82,548 KiB | 75,688 KiB | 約8.3% |
| 乱数単独抽出SB3 | 8,625 | 158,976 KiB | 132,988 KiB | 約16.3% |
| 全パリティSB3 | 11,192 | 172,744 KiB | 142,364 KiB | 約17.6% |

読み込み最適化の前後でバイナリを保存して比較。 `src/load/sb3.cpp` は以前、全ターゲットのJSONを別の`JsonArray`にディープコピーしていた。通常のプロジェクト・単体スプライトとも元のJSONの各ターゲットをポインタで参照するよう変更した。また `BlockJson` から `Block`への変換で、input、field、mutation等をディープコピーせずムーブするよう変更した。パース済みJSONの寿命は全ターゲットロード完了まで維持されている。

### LLVMのメモリ

強制遅延JITはORCが未実体化の関数を保持するためRSSが増える。ホットスポットで遅延強制時、パリティSB3（描画なし）の初期試験では約333MiB。スクリプトごとの`ThreadSafeContext`を採用すると同条件のメモリが約288〜309MiBの範囲となった（測定順のノイズあり）。新既定はHotSpot auto/onで**スクリプトごとにLLVMContextを生成**し、ORCでそのスクリプトのIRと一緒に回収可能な寿命とする。環境変数`XYO_JIT_ISOLATED_CONTEXT=0`で従来の共有コンテキストへ戻せる。

LLVM一時SSA値の名前も、IR診断時以外は`LLVMContext::setDiscardValueNames(true)`で省く。大型パリティSB3では数MiBの削減。 `XYO_JIT_KEEP_VALUE_NAMES=1`または`XYO_JIT_DUMP`/`XYO_JIT_DUMP2`で診断用の名前を維持できる。グローバル関数シンボルは変わらない。

### 描画のオーバーヘッド

パリティプロジェクトでインタプリタの描画なしが約171MiB、SDL offscreen描画ありが約338MiB（プロジェクトロード・画像処理・SDL/GL初期化を含む差分）。描画ありのHotSpotでは、SDLのソフトウェアドライバを選ぶとピークRSSが383,924 KiB→344,940 KiBに減った（1プロセスずつ測定）。`SDL_RENDER_DRIVER=software`を指定すれば利用できるが、GPU/GLによるペン高速化を使いたい場合は性能のトレードオフになるため、**既定ドライバは変更しない**。

低メモリ設定の例:

```bash
# 既定のHotSpot。低メモリでのパリティを優先する場合
SDL_RENDER_DRIVER=software XYO_JIT_LAZY_MODE=off xyo play project.sb3

# ホットスポットの自動最適化を保つ通常の実行
xyo run project.sb3 --jit hotspot --jit-lazy auto
```

`XYO_JIT_LAZY_MODE=off`は初回コンパイル時間が増えるケースがある。静的な大規模手続きでは実行コストやコード量との兼ね合いで要比較。

## 安全性・今後

現行の他作業によるGC/文字列/JIT変更は保持し、それらをこのメモリ最適化の効果として計上しない。今回の変更はロードの所有権移動とLLVMContextの寿命・デバッグ情報のみ。大規模リスト、長い文字列、同時実行Threadが最大RSSを占める場合は別途ワークロード別の分析が必要。

## 最終回帰検証（2026-11-04）

- `cmake --build build --target xyo xyo-tests xyo-gpu-tests -j8` は成功。
- `ctest --test-dir build --output-on-failure -j4` は**12/12成功**。
- SDL offscreenの`play`で、HotSpot `--jit-lazy=auto,on,off`を各3回、合計9回実行し**全て2123/2123**。描画がない`run`での1899/2123は視覚依存テストの仕様上の差なので正当性判定には含めない。
- 変更前後の`xyo dump`を全パリティおよび8,625ブロックのSB3で比較し、テキストが**完全一致**。
- 同一パリティSB3を`play`したピークRSSの一例（HotSpot auto）は変更前381,992 KiB、変更後367,480 KiB（約3.8%削減）。描画ありインタプリタでは346,476 KiB→345,120 KiB。描画負荷のピークが支配的なため、ロード単体の30MiB削減がそのまま全体の30MiB削減になるとは限らない。
- 改善版に`SDL_RENDER_DRIVER=software`を指定した場合、HotSpot autoのピークRSSは336,656 KiB（既定の367,840 KiBより31,184 KiB少ない）。ソフトウェア描画を既定に変更するのはGPU高速化を壊すため行わない。

コードの一時実験用`build/measure_mem.py`と変更前バイナリ`build/xyo-membefore-20261104`は検証用で、リポジトリの追跡対象には加えていない。今回の変更は未コミット・未push。
