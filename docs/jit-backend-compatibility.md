# JIT バックエンド互換性

## 判定基準

ここで実行互換は、同じ Scratch プロジェクトの結果、スケジューラ順序、ブロック副作用、型変換が一致することを指す。これは、全 opcode を WASM で JIT コンパイルできることとは区別する。WASM 側が対応しない命令は、共通 Scratch IR から生成されたコンパイル済み区間の間でも正確なインタプリタ実行へ切り替える（hybrid regions）。スクリプト全体が未対応である必要はない。

| 対象 | LLVM ネイティブ JIT | WASM HotSpot |
|---|---|---|
| HotSpot / Eager | 対応 | 対応 |
| 数値変数、算術、比較、if/if-else | 対応 | WASM へ変換 |
| operator_mathop（14演算） | 対応 | 4種はWASM命令、10種は既存数値ヘルパを共有 |
| 剰余・NaN 型変換 | ランタイム準拠 | ホスト fmod ヘルパ・NaN 正規化 |
| トップレベル repeat / while / until / forever（最大32個） | 対応 | 順次 WASM 再開 |
| 入れ子ループ、トップレベル33個以上 | 対応または互換ヘルパ | warp 手続き内の repeat は動的回数を含め1セグメント1024回まで試行。それ以外の入れ子ループは Interpreter |
| 静的ハット（グリーンフラグ・受信イベントなど） | 対応 | WASM 化 |
| edge-activated ハット | ランタイム判定 | Interpreter |
| 文字列・リスト・一般レポータ | 対応または互換ヘルパ | Valueタグ付き代入・join・letter_of・contains・listContents・文字列長・数値/文字列比較・混合型リストの検索・読取・追加・挿入・置換・削除をWASM化。random添字や未対応効果はInterpreter区間 |
| 手続き・再帰・引数・warp | 対応または互換ヘルパ | 型付きValue引数を持つ非再帰warp手続きと、yieldを含まない非warp手続きの一部をインライン展開。再帰・yieldなどはInterpreter |
| motion / pen / sound / looks / sensing | 対応または互換ヘルパ | sensing_dayssince2000 に限定対応。その他は Interpreter |
| wait / その他の yield | 対応 | Interpreter 区間で実行し、yield 後のトップレベル継続点から WASM 再開 |
| OSR（インタプリタ途中昇格） | 一部ループに対応 | 未対応 |
| 非同期コンパイル | 対応構成あり | 未対応 |
| ネイティブ AOT | 対応 | 未対応 |
| LLVM なしのビルド | 不可 | 対応（Emscripten） |
| ネイティブ Linux/Windows での WASM 実行 | 非該当 | 未対応（JS WebAssembly ホストが必要） |

## 実装済みの互換性保護

1. 未対応の opcode や不正なブロックグラフを WASM に誤変換しない。
2. 変数変更は一時領域で行い、WASM セグメントが正常終了してから反映する。
3. 型ガード失敗・WASM trap では書き戻さずInterpreterへ移行する。NaN/文字列比較そのものはホスト意味論で実行できる。
4. 順次トップレベルループの現在の添字・継続状態を Thread が保持し、途中の bailout 時に対象ループの StackFrame とカウンタを復元する。
5. round(NaN) は Scratch 同様に数値変換で 0 にしてから丸める。
6. 浮動小数点の剰余は既存ランタイムと同じ fmod を env.h 経由で呼ぶ。独自の近似剰余式は使用しない。
7. standalone WASM の剰余で精度を保証できない入力は bailout する。
8. 三角関数・対数・指数関数は LLVM JIT の既存数値実装を共有し、ホストヘルパのない standalone WASM では bailout する。

## 再現可能な検証

Emscripten の WASM HotSpot、ネイティブ LLVM JIT、Interpreter に同一 SB3 を実行させ、最終的な変数値を比較する。

~~~sh
python3 tests/differential/wasm_hotspot_integration.py \
  build-wasm-hotspot/xyo.js --out build-wasm-hotspot \
  --llvm build/xyo

python3 tests/differential/wasm_compat_fuzz.py \
  build-wasm-hotspot/xyo.js --llvm build/xyo \
  --out build-wasm-hotspot --cases 128
~~~

乱数生成試験の seed=42 で NaN 数値変換漏れ、seed=52 で剰余演算精度の不一致、複数ループ対応後の seed=6 でホスト剰余の一時領域が Scratch 変数に重なる不具合を検出・修正した。現在の試験では 512 シードで3方式の最終結果が一致した。

## 完全互換までの残作業

既存 LLVM codegen は Scratch ブロックから LLVM IR を直接構築する。WASM は数値・分岐・トップレベルに並ぶ最大32個のループに加えて、限定的な文字列・リスト・warp 手続きの対応を進めている。全機能を Binaryen へ移植するには以下が必要。

- GCルートと長寿命continuationを含む任意の副作用付きreporterをサポートする完全な共通Value ABI。文字列・混合型リストの基本操作は対応したが、レンダリングなどの複雑な処理は未対応。
- 任意深さの yield / procedure / warp / clone / stop を扱う共通継続状態。
- Interpreter と WASM の OSR、非同期コンパイル、guard / deopt 契約。
- LLVM と WASM 共通の型プロファイル、最適化、広範な ScratchBench 差分テスト。

現状は「未対応機能を安全に Interpreter 区間へ切り替え、後続の対応区間で JIT を再開する」段階である。「LLVM JIT と全く同じ機能を Binaryen でコンパイルする完全互換」には至っていない。


### ネストした手続き・反復の実験機能

`XYO_NESTED_WASM=1` では、インタプリタの手続き・反復スタックを
保持したまま、対応する数値命令区間だけをWASMに委譲できる。
`XYO_NESTED_WASM_WARP_ONLY=1` は副作用のない数値warp反復に限定し、
ネストした repeat / repeat until / while / for each、条件分岐と
引数なし・非再帰warp補助手続き、末尾の自己再帰も共通の最大1,024反復予算で
単一WASM呼び出しにまとめる。末尾再帰は初期値が数値定数、再帰引数が数値演算、
最大4引数、最後のIF内から自己呼び出しする場合に限定する。非warp反復は各ティックのyieldを保持する。
数値if/if-elseの本体は軽量WASMで処理可能だが、非末尾再帰も、引数なしwarp手続きで再帰呼び出し箇所が1つ、数値処理のみの
「前処理・条件付き自己呼び出し・後処理」形に限り、戻り先を圧縮継続深度として
保持する。複数の戻り先、引数を持つ非末尾再帰、非warp再帰・任意の副作用や
待機はまだフォールバックする。文字列・リストを含む実験モードは `XYO_NESTED_WASM_ALL=1` を追加する。
総合速度の改善が未確立のため、いずれも既定では無効。

詳細: [ネストしたWASM継続実行](nested-wasm-continuation-2026-11-07.md)
