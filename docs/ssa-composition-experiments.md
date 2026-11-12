# 数値SSAの追加合成実験：不変文字列と純粋warp手続き

## 結論

2案を実装して、別々の環境変数で比較した。**今回追加した2案は既定OFF**。
前回ユーザー指示で既定ONにしたスカラーコピーは、そのまま既定ONを維持した。
新しい環境変数はコード生成時の選択に使う。生成済みAOTオブジェクトの経路を後から切り替える設定ではない。

| 案 | 対象ケース | 単独の対応ある高速化倍率 | 判断 |
|---|---|---:|---|
| 不変Stringの整数算術用変換 | 文字列係数の参照 | 約2.29× | 有望。実験フラグとして残す |
| SSA計画時の純粋warp手続き展開 | 本体を手続きへ切り出したループ | 約3.78× | 有望。ただし適用範囲は狭く、継続状態の統合検証は別途必要 |
| 2案併用 | 文字列係数を使う本体を手続き化 | 約3.44× | 単独では解消しない2つの適用境界を越えられた |

これは実作品全体の倍率や競合ランタイムに対する倍率ではない。
当該カーネルにおける同一バイナリのフラグOFF/ON比較である。

## 実装

変更は[数値repeat SSA](<../src/jit/numeric_repeat_ssa.inc>)に限定した。
共有作業中のJITコード生成本体やCallFrame ABIは変更していない。

### A. 不変Stringの整数算術用変換

```sh
XYO_JIT_NUMERIC_REPEAT_READONLY_STRING=1
```

従来、入口で全スカラーへplain Numberを要求していたため、数値として読める文字列係数でも
高速経路に入れなかった。今回の経路は次の条件に限定する。

- 既存の整数性・値域解析が対象を整数キャリアと分類している。
- ループ内で書き換えられない。
- **どの代入文でも、bare variableコピーのコピー元になっていない**。
- 元のValueはNumberまたはString（空文字列のsingletonも含む）。
- 既存の入口整数範囲、非負・有限・整数・負のゼロ除外を満たす。

元のStringはメモリ上で変更しない。通常のチャンク入口ごとに既存のScratch算術変換を使い、
整数演算用の数値を得る。値や型を中断境界を越えて固定しない。
負のゼロは元のStringポインタではなく、**変換後のdoubleのビット列**で除外する。

これは「数値文字列だけの判定」ではない。Scratchの算術では `"abc"`、空白、空文字列も0へ変換されるため、
この経路でも0として扱える。比較の意味まで数値へ変えるわけではなく、Stringのキャッシュ状態を強制変更しない。

コピー元を除外する理由：整数キャリアからValueを再構築すると数値型になる。
`set a to source` のようなコピーを許すと、元のStringの型タグ・綴りが失われ得る。
算術入力だけに限定することで、この観測可能な差を避ける。

浮動小数点専用SSAやNumeric Untilの適用範囲は、この実験で広げていない。

### B. SSA計画時の純粋warp手続き展開

```sh
XYO_JIT_NUMERIC_REPEAT_INLINE=1
```

LLVMの通常のインライン化より前、Scratchブロックから数値SSA計画を作る段階で、
次を満たす手続きの文列を計画へ取り込む。プロジェクトのブロックグラフ自体は書き換えない。

- 引数ID・名前・デフォルトが空で、呼び出し側の入力も空。
- 呼び出しはstatementで、prototypeがwarpかつproccodeと整合する。
- 本体は既存の数値文法で検証できるset/changeのみ。
- 手続き内の別手続き呼び出し、再帰、分岐、ループ、待機、リスト操作、sensing等は拒否。
- 展開後は最大20文、走査は最大64ノード。壊れたリンクや循環は保守的に拒否。
- Numeric Untilの計画では展開しない。

拡張するのは形の受理だけで、数値型・整数性・値域・warpのガードは維持する。
前提が成立しない実行は、元の手続き呼び出しを含む通常JIT本体へ戻る。
空のwarp手続きを取り除いて数値SSAの適用を復活させるケースも確認した。

## 実験条件と来歴

- 基底：v1.2.1、`d6654a6ba9d8de81fe6b4cb2f69ffdb56833b0bf`。
- タグを隔離領域へ展開し、数値SSAのincludeだけを差し替えてビルド。
- 前回のコピー既定ON化と、現在の作業ツリーに既存のPHI配置修正を含む。
- [タグからの全差分](<../scratch-bench/results/ssa-composition-v121/source.patch>)を保存。
- Release、GCC 16.2.1、LLVM 23.1.4、i7-12700、Linux、SDL2 OFF。
- CPU 12固定、eager JIT、2回ウォームアップ後3回測定。
- 12M反復、10ケース、4プロファイル、6ブロック。240プロセス、720個の測定時間。
- 順序をシャッフルし、次ブロックでは逆順で実行。
- GPU、並列スケジューラ、ParForは無効。厳密ブロック計数・XYO_PROFは使用しない。
- IR出力は測定と別プロセス。高速IRの存在だけから実行時ガード通過率は断定しない。

バイナリSHA-256：`9d4766556c07f6b60528ee94b8d9f5b2c28d9c18824fd61664fa50429f31e826`。
[manifest](<../scratch-bench/results/ssa-composition-v121/manifest.json>)にバイナリ・ドライバ・生成器のハッシュ、
CPU情報、環境、回数を記録した。共有作業ツリーの別変更はビルドに混ぜていない。

**今回はホスト負荷の変動が大きい。** 直接ループのプロセス中央値でも約31〜40msの変動があり、
別プロファイルにはさらに大きな外れ値がある。前回の31msと今回の値を直接比較しない。
CPU固定はCPU隔離ではない。数%の差や「回帰が一切ない」という判断は行わず、
対象ケースで全ブロックにわたり得られた大きな改善を主結果とする。信頼区間は未算出。

## 性能結果

時間は各プロセス内3回の中央値を取り、6プロセスの中央値を示す。
倍率はブロックごとの対応ある比率の中央値なので、表の時間同士の割り算と必ずしも一致しない。

| ケース | 2案OFF ms | Stringのみ ms | 展開のみ ms | 併用 ms |
|---|---:|---:|---:|---:|
| 直接記述 | 38.5 | 39.0 | 37.5 | 37.5 |
| 数値型の係数変数 | 53.5 | 55.5 | 53.0 | 51.5 |
| **Stringの係数変数** | **122.0** | **54.0** | 125.0 | 52.5 |
| **本体を手続き化** | **138.0** | 147.5 | **39.5** | 38.0 |
| 空手続きの呼び出しを追加 | 138.0 | 131.0 | 39.0 | 38.5 |
| **String係数＋手続き化** | **179.0** | 175.5 | 172.5 | **54.5** |
| リストから係数を取得 | 121.5 | 128.5 | 124.5 | 115.5 |
| 同じ本体のif/else | 134.0 | 135.0 | 137.0 | 132.5 |

対応ある倍率の範囲：

- String係数、Stringのみ：**2.14〜2.69×、中央値2.29×**。
- 手続き化、展開のみ：**3.26〜4.00×、中央値3.78×**。
- 空手続き、展開のみ：**3.21〜3.80×、中央値3.54×**。
- String係数＋手続き化、併用：**3.00〜3.57×、中央値3.44×**。

String係数＋手続き化では、一方だけを有効にしてもほぼ改善しない。
展開だけではString型の入口ガードで落ち、String変換だけでは手続きの形を受理できないため。
併用すると両方の制約を越えられる。倍率を掛け合わせた結果ではない。

リスト参照や冗長分岐は今回の対象外で、そこでの揺れを改善とは扱わない。
コンパイル費用の制御された対照実験、メモリ・コードサイズ評価は未実施。
今回の測定は定常カーネル性能であり、短時間実行の採算を証明していない。

[全集計](<../scratch-bench/results/ssa-composition-v121/summary.json>)、
[生データ](<../scratch-bench/results/ssa-composition-v121/results.json>)、
[生成入力](<../scratch-bench/results/ssa-composition-v121/projects>)を保存。

## 正しさ・適用条件の検証

| 検証 | 結果 |
|---|---|
| [String専用・併用差分](<../tests/differential/jit_numeric_repeat_readonly_string.py>) | 383フィクスチャ、**766比較**すべて一致 |
| [手続き専用差分](<../tests/differential/jit_numeric_repeat_inline.py>) | 495フィクスチャ、**991比較**すべて一致 |
| 構造ベンチの小規模参照値照合 | 400/400 |
| 本測定 | 全240プロセスの最終値がPython参照値と一致 |
| 既存スカラーコピー差分 | 357フィクスチャ、1,071比較が一致 |
| 既存Numeric Repeat / Until差分 | warp/nonwarp、eager/hotspot等16構成が一致 |
| executeフィクスチャ | 既定・String・展開・併用それぞれ116/116、assert 338/338 |
| AOT | String係数・手続き化・併用の3ケースで生成・実行・参照値照合に成功 |
| [ハーネス単体テスト](<../tests/bench/algos/test_structure_cliffs.py>) | 5/5 |

String差分は数値・不正文字列・空文字列・Unicode空白・範囲境界・負のゼロ・Infinity等を含む。
`listcontents` で型タグと元の綴り、逆数で負のゼロを観測し、コピー前後・source自体の書き込みも検証した。
併用時には、手続き内のコピーや書き込みを不変と誤認しないことも確認した。

手続き差分はwarp/nonwarp、空手続き、複数呼び出し、コピー、0/1/63/64/65/129反復を含む。
20/21文、64/65走査ノードの境界と、引数・非warp・リスト書き込み・分岐・wait・入れ子呼び出しの拒否を確認した。
IR検証では、期待した設定に限り数値SSAが生成されることを確認している。

[検証記録](<../scratch-bench/results/ssa-composition-v121/readonly-parity.json>)、
[手続き検証](<../scratch-bench/results/ssa-composition-v121/inline-parity.json>)、
[AOT結果](<../scratch-bench/results/ssa-composition-v121/aot-results.json>)、
[併用executeログ](<../scratch-bench/results/ssa-composition-v121/execute-both.log>)。

## 未検証・採用判断

有望な2案として残すが、新しいフラグは既定OFFを維持する。

- 実作品のFPS、p99、起動時間、メモリ消費、コンパイル増加との損益分岐は未評価。
- warp時間予算を強制失効させ、外部スクリプトがsourceを書き換える境界の専用試験は未実施。
- 実行中のhelper編集・呼び先変更と、共通CallFrame/OSR/deoptのソース依存証明との統合は未検証。
- フレーム・呼び出しトレースの診断上の同一性は保証していない。
- 検証対象は隔離したリリース基底＋パッチ。現在の共有dirtyツリー全体の再ビルドは行っていない。
- テストは独立コマンドで実行。共有編集中のCMakeテスト定義や `--opt-all` の登録は変更していない。

## 再実行

```sh
python3 tests/differential/jit_numeric_repeat_readonly_string.py \
  --binary build/ssa-composition-v121/native/xyo --out build/string-parity-repeat
python3 tests/differential/jit_numeric_repeat_inline.py \
  --binary build/ssa-composition-v121/native/xyo --out build/helper-parity-repeat
python3 tests/bench/algos/bench_structure_cliffs.py \
  --binary build/ssa-composition-v121/native/xyo --out build/composition-bench-repeat \
  --count 12000000 --blocks 6 --samples 3 --cpu 12 \
  --profiles default,readonly_string,inline_helpers,composition_all --diagnostics
```

出力先には新規ディレクトリを指定する。保存済み測定結果を上書きしない。
