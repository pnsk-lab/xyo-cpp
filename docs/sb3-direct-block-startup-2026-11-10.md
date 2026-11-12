# START-006: SB3を中間BlockJsonなしで直接構築する

2026-11-10。xyo-cppの起動ボトルネックのうち、
ScratchBench Parity（2656件・206グループ）では
SB3のブロック展開・登録が大きな割合を占めていた。
今回、**中間の`BlockJson`ハッシュ表・順序ID配列の生成と、
`BlockJson → Block`の二段階変換を省略**する経路を実装した。

## 実装

`src/load/sb3.cpp`の通常のSB3プロジェクト読み込みで、
JSONから最終的な実行用`Block`を直接生成し、
`Blocks::createBlock`へ追加する。

- プリミティブ（数値・文字列・変数・リストなど）のシャドウブロックも
  必要な位置で直接生成し、旧方式と同じ作成順・`seq`を保持。
- `procedures_prototype`、argument reporter、
  `control_stop`の互換性修正は登録完了後に実施。
- 元の`BlockJson`処理はバックパックやブロック断片の読み込み用として残す。
- 切り分け用に`XYO_SB3_DIRECT_BUILD=0`で旧経路へ戻せる。
  **通常のプロジェクト読み込みは直接構築が既定。**
- JIT/AOTのコード生成とネイティブAOTオブジェクト形式には変更なし。

## 起動時間

AOTを事前生成した`parity.sb3`をGUI
（SDL offscreen / dummy audio）で起動し、
Scratchプロジェクトの最初の標準出力までを測定。
プロセス起動からの壁時計時間。
CPU 10固定、両方式を交互に16組ずつ測定した。

### 独立した検証用Releaseビルド

| 確保器 | 旧・起動中央値 | 直接構築・起動中央値 | 改善勝数 |
|---|---:|---:|---:|
| glibc | 408.6 ms | **348.0 ms** | 15/16 |
| jemalloc（LD_PRELOAD） | 360.2 ms | **317.9 ms** | 13/16 |

ブロック構築部分の中央値はglibcで109.9→71.9ms、
jemallocで94.8→62.1ms。
入力順IDの`std::string`コピーと再検索だけを減らした先行実験は、
起動全体で安定した改善が出なかったため採用していない。

### 現行共有コードのReleaseビルド

システムのjemallocをリンクした**同一バイナリ**で、
同じプロジェクトと同じAOTオブジェクトを使ってON/OFF比較。

| JSONパーサー | 旧・起動中央値 | 直接構築・起動中央値 | 改善勝数 |
|---|---:|---:|---:|
| 従来JSON | 227.8 ms | **210.6 ms** | 16/16 |
| 任意のyyjson | 224.8 ms | **208.4 ms** | 16/16 |

交互A/B、各16組。ペア比率の幾何平均による高速化は
従来JSONで1.0795倍、yyjsonで1.0861倍。
ホストの負荷・キャッシュにより絶対値は変動する。
AOT生成コストは事前に支払うため計測に含まれない。

### 完走時間とメモリ

現行共有Release（jemalloc、yyjson）で全2656件を実行した各1回の値：

| 指標 | 旧方式 | 直接構築 |
|---|---:|---:|
| 完走までの壁時計時間 | 17.15秒 | 17.11秒 |
| 最大RSS | 426.97MB | **412.59MB** |

起動後の性能に悪化は見られず、最大RSSも約14.38MB減少した。
ただし完走時間・最大RSSは各1回の測定であり、
差の有意性を主張しない。

### 追加の小規模実験

直接構築のプリミティブ値について、条件式による`JsonValue`コピーを
参照へ置き換える案も隔離ビルドで計測した。
CPU10固定・jemalloc・交互16組では、
コピー版の起動中央値209.2ms、参照版210.5ms、
参照版が勝ったのは7/16組。
**起動全体で改善が再現しないため共有コードへは取り込まなかった。**

## 互換性と検証

`tests/platform/sb3_direct_load_parity.cpp`を新設し、
旧ローダーと直接構築ローダーによるグラフが一致することを検査。
ブロックの作成順`seq`を使って生成IDを正規化し、
入力参照・next/parent・フィールド値・ミューテーション属性・
トップレベル・shadow・座標・コメント・スクリプト順を比較する。

次の5プロジェクト、**合計100,408ブロックで完全一致**。

- basic.sb3：7ブロック
- calculator_tests.sb3：12,279ブロック
- calculator_graph_tests.sb3：12,855ブロック
- scratch-parity-test.sb3：21,818ブロック
- ScratchBench全量parity.sb3：53,449ブロック

標準CTestにはリポジトリ内の先頭4プロジェクトを組み込み、
ScratchBench全量もローカルの回帰検証で手動指定してPASS。

実際のGUI Scratchオラクルも、旧方式・直接方式のAOT双方、
および直接方式のInterpreter・HotSpot・Eagerで
**各2656/2656件PASS**。これは共有コードのReleaseビルドでも検証済み。

## 再現

```bash
# 既定の直接構築
./build/xyo play project.sb3 --aot project.o

# 従来方式に戻して差分を確認
XYO_SB3_DIRECT_BUILD=0 ./build/xyo play project.sb3 --aot project.o

# バンドルされた4プロジェクトの構造回帰チェック
cmake --build build --target xyo-sb3-direct-load-parity
ctest --test-dir build -R xyo-sb3-direct-load-parity --output-on-failure
```

同時作業者の変更は保持。今回の変更は未コミット・未pushで、
GitHub Actionsの新しいCTestはまだ実行していない。
