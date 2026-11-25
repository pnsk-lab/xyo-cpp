# 共通 CFG 実行の状態（2026-11-08）

共通 CFG は `XYO_JIT_COMMON_CFG=1` で有効になる実験経路で、既定の
LLVM コード生成全体を置き換えてはいない。LLVM と WASM は同一の
Executable を消費する。Binaryen は生成 WASM を最適化する。

## 現在の実行契約

- 静的イベントハットの新規起動時に適用する。任意位置の OSR は行わない。
- 数値スカラーの読み書きは区間単位でステージし、成功時だけ公開する。
- repeat / while / repeat until / forever、ネスト、if / if-else を扱う。
  非 warp では各ループ末尾がスケジューラへの継続点になる。
- repeat 回数は開始時に一度だけ評価する。定数・動的とも丸め後100万回まで。
  スケジューラなしの試験経路は1024回までで、総実行量制限も残る。
  RepeatCount 命令自身に上限を保持し、verifier と両 backend が共有する。
- deopt は保存した親経路からループ・分岐フレームを復元する。
  外側ループの実行中の反復は二重に数えず、選択済み分岐を再判定しない。
- 同じ warp drive 内で成功区間の後に失敗しても、成功済みの更新を再実行しない。
- Blocks の revision が変わった場合は古いコードを走らせず deopt する。
  復帰先が削除された場合は停止する。直接フィールドを変更する埋込み側は
  既存の Blocks 契約どおり resetCache を呼ぶ必要がある。
- NaN は値の読み出し時には保持する。算術 consumer が必要な変換を行い、
  比較 consumer は NaN を検知して Scratch 比較を行うインタプリタへ戻る。

## 同期手続き

数値更新・条件分岐・同期呼出しで構成する非再帰手続きを、
warp / 非 warp ともインライン展開できる。引数上限は共通 CallFrame ABI と
同じ32個で、各呼出し前に左から評価して SSA へ束縛する。同じ手続きを複数回
呼び出せる。真偽値の実引数は別の i1 引数として条件式へ渡せる。

引数なし・非 warp の非再帰手続き内ループは共通 CFG で実行できる。
途中 deopt では呼出しフレームと空の引数環境も復元する。
同じループを複数の呼出し箇所へ展開して復帰先が曖昧になる場合は拒否する。

引数付きまたは warp 手続き内のループ、再帰、待機、副作用、GC 管理値を含む一般的な呼出しは、
この共通 CFG では未対応。真偽値引数を型付き Value として変数へ保存する場合も
フォールバックする。インタプリタや既存の個別 backend が実行できることと、
共通 CFG でコンパイルできることは区別する。

## 検証

- tests/platform/numeric_sequencer_smoke.cpp:
  実 LLVM runner、ループ・分岐の途中復帰、編集無効化、NaN、
  8か所での強制失敗による prefix / suffix の重複検出。
- tests/differential/common_cfg_backend_parity.py:
  インタプリタ・LLVM・builtin WASM・Binaryen の33ケース。
  compiled 完了と compiled 後 deopt を区別し、実 Binaryen callback を確認する。
- tests/differential/common_cfg_procedure_parity.py:
  同期呼出し、32引数境界、引数 snapshot、真偽値条件、手続き内ループの途中 deopt、拒否ケースの28ケース。

これらは全 Scratch opcode、レンダリング、音声、一般的な再帰・待機、
GC-rooted continuation の完全互換を証明するものではない。
次の統合課題は、手続き内の継続点に対応する引数・戻り先・GC ルートを、
スケジューラと両 backend が同じ契約で保存・復元することである。
