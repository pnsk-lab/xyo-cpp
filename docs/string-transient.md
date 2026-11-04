# Scratch向けTransient String最適化（2026-11-04）

## 対象と実装

JITのwarp手続きで、変数への自己追記が外部から観測されない区間を静的に検出し、
毎回GC管理の不変`SStr`を確保する代わりに可変の`AppendLog`へ追記する。

```text
repeat N:
    change i by 1
    set s to join(s, join("prefix", i))
    set t to join(t, "(定数)")

repeat until i > N:
    set s to join(s, "abc")
    change i by 1
```

実装済みの適用範囲：

- ループは`repeat`、`repeat until`、`while`、`for each`（warpのみ）。
- **最大16個の命令**を持つループ本体に、同じ文字列変数への自己追記と、別変数への純粋な代入・加算を混在可能。
- `join(s, join("a", join(i,"b")))`のような**入れ子の結合**を最大16要素まで分解し、中間の連結文字列を確保しない。
- 数値や文字列の変数参照、手続き引数、加減乗除・剰余・丸め、単純比較・真偽演算・長さ・文字取得など、静的に副作用がないと確認した右辺を処理。
- 同じ`repeat`で**2～4個の独立した文字列変数**をそれぞれ定数追記する場合、別々の追記ログを使用。
- 4 UTF-16単位以下のリテラルは64bit整数として埋込み、4単位を超えるリテラルも一括追記へ拡張。
- 定数のみの連結ループや、ループ内で値が変わらない1個の純粋な右辺式は、**次の64反復のスケジューラ検査位置まで**まとめて追記する。yield後には必ず式を再評価。

証明できない式、外部可視の副作用（手続き呼出し、リスト変更など）、未公開文字列の途中参照、書換え対象を変える処理、非warpループ、AOTは従来の意味論を保つ経路へ戻す。

`xyo_transient_append_value`は数値・文字列をScratchの変換規則で追記する。整数の`[-999999999, 999999999]`に収まる値は`std::string`や`SStr`を作らず、スタック上のUTF-16へ直接変換する。それ以外の数値は従来の`jsnum::toString`へ戻る。

短い固定リテラル（4 UTF-16単位以下）は`xyo_transient_append_packed`の64bit整数引数に格納できる。さらに**最大64反復を1回のランタイムヘルパ呼出しにまとめる**`xyo_transient_append_repeat_packed`を追加した。JITは現在のyield計数器の位相に合わせて次の64反復境界までをまとめる。実際に必要な反復回数が不足する場合や数値の大きさが危険な場合は逐次反復に戻す。warpの時間予算チェックを無効化していない。

## 不変性・yield・GC

`AppendLog`は256 UTF-16単位のチャンクを持つ。公開済みのプレフィックスは書き換えず、スナップショット`(log, length)`から古い文字列を引き続き参照できる。ループ中の未公開部分には繰り返し`SStr`を確保しない。

yield前に現在の長さのスナップショットをScratch変数へ公開する。yield後は**公開中のスナップショットに保持されたGCルート経由でのみ**ログを再取得する。同じログの末尾ならそのまま再開し、ほかのスクリプトが変数を上書きした場合などは、その**現在値を起点に新しいログを構築**する。中断したJITフレームに残る古い生ポインタを逆参照しない。

GCは`GcType::AppendLog`を追跡し、`SStr::left`経由でマークする。チャンクの外部メモリはGCの生存容量に含める。チャンク一覧への読み書きはミューテックスで同期する。ループ終了時は4096 UTF-16単位以下なら平坦化して文字参照高速経路に戻す。

## 切り替えフラグ

| 環境変数 | 動作 |
| --- | --- |
| `XYO_JIT_TRANSIENT_STRINGS=0` | Transient String全体を無効化 |
| `XYO_JIT_TRANSIENT_PACKED=0` | 固定短文字列の64bit埋込みを無効化 |
| `XYO_JIT_TRANSIENT_BATCH=0` | 64反復までの一括追記を無効化 |
| `XYO_JIT_TRANSIENT_MULTI=0` | 2～4変数同時最適化のみ無効化 |
| `XYO_STRING_APPEND_LOG=1` | **別実験**：通常の文字列連結もAppendLog化（性能悪化のため既定OFF） |

上のJIT最適化は静的証明に成功した箇所だけ既定ON。

## 実測

### 固定仕事量（Scratchブロックによるベンチ）

`tests/bench/algos/bench_strings.py`、CPU12に固定、scale 80、各方式9回。下表はスクリプト内の時計による中央値（ms、小さいほど高速）。文字列の内容とチェックサムを検証済み。

| 処理 | Transient逐次追記 | Transient一括追記 | TurboWarp |
| --- | ---: | ---: | ---: |
| `join` | 24 | **5** | 11 |
| `join_number`（定数加算） | 25 | **5** | 12 |
| `letter` | 14 | 14 | 4 |
| `join_observe` | 10 | 10 | 24 |
| `join_observe_last` | 4 | 5 | 26 |
| `short_dynamic` | 39 | 38 | 16 |
| `cast` | 26 | 26 | 22 |
| `contains` | 707 | 707 | 1201 |

生データ：`build/append-log/batched9.json`（ローカル、Git追跡対象外）。

動的RHSの一括処理を追加した後の結果（`tests/bench/algos/bench_transient_strings.py`、CPU12、scale80、7反復、msの中央値）：

| RHS | Rope（無効） | 新方式（有効） | 一括処理無効 |
| --- | ---: | ---: | ---: |
| `v+1` | 14 | **4** | 13 |
| `v` | 16 | **4** | 13 |
| `v/2` | 45 | **5** | 34 |
| 固定短文字列 | 13 | **2** | 12 |
| 固定長文字列 | 13 | **7** | 14 |

すべての測定でScratch可視の出力を検証。ソースデータは`build/append-log/extended-dynamic7.json`（ローカル、Git追跡対象外）。結果は適用対象のベンチ限定であり、任意のScratchプロジェクトの速度比を意味しない。

### ScratchBench `str_join`（実際のベンチカーネル）

`tests/bench/algos/scratchbench_probe.py`の方式で`str_join`だけを単独SB3に隔離した。ベンチカーネルの処理内容は変更していない。CPU12固定、3構成をランダム順で7組実行し、すべてチェックサム`40000`で一致。

| 構成 | ops/ms の中央値 |
| --- | ---: |
| 従来Rope | 約43,217 |
| Transient逐次追記 | 約64,006 |
| **Transient一括追記** | **約292,550** |

対応するペアごとの速度比の中央値は、Ropeに対して**6.769倍**、逐次追記に対して**4.578倍**。ベンチ単独の結果であり、ほかのScratch処理の同じ改善率を意味しない。

生データ：`build/append-log/scratchbatch7.json`（ローカル、Git追跡対象外）。

## 回帰試験・互換性

- `cmake --build build --target xyo xyo-tests`成功。
- `ctest --test-dir build --output-on-failure`：**11/11成功**。
- `XYO_JIT_TRANSIENT_STRINGS=0`で`xyo-tests`再実行：成功。
- 実際の2本のgreen-flagスクリプトを同一変数上で動かし、warp時間予算0msで確実にyieldさせる。片方がスナップショットを公開した後にもう片方が変数を上書きし、GCを挟んで正しいプレフィックスから再開するテストを`tests/unit/test_danger.cpp`に追加。
- 反復回数0、0.49、0.5、1.5、63.49、63.5、64.49、64.5、65.5、127.49、127.5、128.5で一括化の境界を確認。
- `tests/unit/test_sstr_cons.cpp`でUnicodeサロゲート、旧スナップショット不変性、数値の通常経路・直接書式化、GC後のログ再利用を確認。
- `tests/CMakeLists.txt`に`xyo-jit-transient-strings`を登録し、18種類の実SB3を最適化有効・無効で比較する回帰テストを追加（長い文字列、絵文字、入れ子連結、複数変数、while/until/for each、観測・副作用による非適用）。
- 以前の単一変数実装では、ScratchBenchパリティSB3をSDLオフスクリーン描画・リスト制限200000で実行し、Interpreter／HotSpot JIT／Eager JIT／AOTが**各2123/2123成功**。ただし今回の拡張後は、ScratchBenchの更新によりキャッシュ済みSB3がなくなり、オフラインの再生成が`fractch`不足で失敗したため**最新版での全件再実行は未完了**。代わりに現行ビルドでCTest全件、実SB3による正負の経路検証を実施した。`xyo run`の描画なしパリティ実行は、描画を必要とするテストを失敗させるため比較対象にしない。

例：

```bash
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
XYO_EXIT_ON_DONE=1 XYO_JIT_MODE=eager \
./build/xyo play scratch-bench/.cache/build/parity.sb3 \
  --print-list output --list-limit 200000
```

追加の保護試験：`tests/bench/algos/bench_transient_extended.py`は、生成した実SB3を使って一括文字列・Unicode・複数命令・複数変数・入れ子の`join`・repeat until/while/for eachを検証する。意図的に最適化対象外としたリスト書込み・途中観測・不正な更新を拒否し、実際に生成されたJITが最適化経路を通っていることも検査する。

`tests/unit/test_danger.cpp`では、本物の2スクリプトを交互に実行して、追記先の上書き、右辺の数字や文字列の上書き、2つの追記ログの片方だけの上書きとGCを試験する。**warp時間予算はテストで0msに設定**して実際のyieldを強制し、グループ化によってスケジューラの観測位置が失われないことを確認する。

未実装：一般の逃避解析、非warpループでのバッファ化、リスト操作を含む混合ループ、分岐が複雑な任意の式。証明できない場合は常に従来のScratch実行経路を使用する。
