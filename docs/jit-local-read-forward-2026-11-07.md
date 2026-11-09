# JITの同一基本ブロック内・混合型変数読み出し転送（2026-11-07）

## 実装

`src/jit/codegen.cpp`に `XYO_JIT_LOCAL_READ_FORWARD=1` を追加した。既定値は **OFF**。

従来の `localVarFacts` は、昇格済み変数への代入結果を同じLLVM基本ブロック内の後続読み出しへ転送していた。この変更では、最初の `load i64` から得たboxed値を追加登録し、その後の同じ基本ブロック内の読み出しを同一SSA値に転送する。例として、`x + x` の両レポータで同一の読み出し結果を扱えるようになる。

適用範囲：

- ループ昇格済みで、動的型に対応する `lazyValid` シャドウを持つスカラー変数のみ
- 変数IDの解決が一意と確認されたループだけ
- 初回の読み出しと次の読み出しが**同じScratch命令内かつ同じLLVM `BasicBlock*`** にある場合だけ
- 同じ変数の代入・change-byで既存の書き込み転送が値を更新・無効化
- Scratchの命令境界で読み出し由来の事実を破棄。分岐では局所的な値情報をクリアし、yield後の `reloadPromoted()` でもクリア
- 手続き呼び出しや不明な副作用を含むループは、元の昇格解析が除外

`knownNumD` と `knownNumOk` が同じboxed SSA値から得られた数値・比較可否を保持するため、後続の数値利用は二重のScratch数値変換を生成せずに済む場合がある。ただし、**型の静的な決め打ちは行わず**、`toNumber()`・比較用の文字列判定とyield後の再読み出しを保持する。

`XYO_JIT_LOCAL_READ_STATS=1` はコンパイル時に `[jit-local-read] SCRIPT reuses=N` をstderr出力する。代入結果の転送は回数に含めず、初回の読み出しから得たSSA値の再利用だけを数える。

## 差分検証

`tests/differential/jit_local_read_forward.py` を作成し、CTestに `xyo-jit-local-read-forward` として登録。混合型文字列 `" 12.5 "`, `"NaN"`, `" "`, `"Infinity"`, `"-Infinity"`, `"-0"`, `"0x10"`, `"0"`, `"hello"`, `"1e3"` と数値の組み合わせをwarp・非warp両方で検証。Eager／HotSpot、ON/OFFをインタプリタと比較し、結果一致を確認。

IR出力（LLVM最適化前）での `load i64` の数：

| 構成 | OFF | ON |
| --- | ---: | ---: |
| warp | 159 | 149 |
| 非warp | 159 | 149 |

再利用回数はどちらも10。型の境界条件を変える最適化ではなく、同一boxed値の再読み出しを10個削除した。

## LLVM O3後の性能（各モード5回の中央値）

`tests/bench/phi_demand_metrics.py --local-read`で測定。コンパイル時間と実行ウォール時間は別に取得し、オブジェクトサイズはORC出力の`.text`を測る。

| SB3 | OFFウォール | ONウォール | OFF JIT | ON JIT | `.text` OFF/ON |
| --- | ---: | ---: | ---: | ---: | ---: |
| 専用warp | 59.33ms | 60.30ms | 44.64ms | 45.29ms | 13,125B / 13,125B |
| 専用非warp | 59.55ms | 58.12ms | 44.75ms | 44.36ms | 13,125B / 13,125B |
| quicksort_4000 | 158.13ms | 158.09ms | 141.79ms | 141.65ms | 28,355B / 28,355B |
| mergesort_4000 | 129.43ms | 129.46ms | 112.24ms | 112.31ms | 24,307B / 24,307B |
| micro_read_1000000 | 49.87ms | 49.75ms | 31.06ms | 31.06ms | 6,086B / 6,086B |

結果のチェック値は一致。LLVM最適化後にはコードサイズの減少を確認できず、速度の違いも測定ノイズ程度。**既定OFFのまま維持する**。

再現データ：`build/jit-local-read-metrics.json` （ビルド作業ディレクトリに保存、Git未追跡）。

## 次の候補

この狭い重複読み出しはLLVM O3でほぼ処理されている可能性が高い。より強い高速化を得るには、(1) コンパイル後のプロファイルから重複する `xyo_to_num` / `xyo_eq` などの実呼出し箇所を特定する、(2) ループSSAへの型影伝播の適用範囲を増やす、(3) 実SB3の支配的な遅延要因と合わせて設計する必要がある。

## O1・追加検証

LLVM O1についても、各モード5回ずつの中央値を測定した
(\`build/jit-local-read-O1.json\`)。

| SB3 | O1 OFFウォール | O1 ONウォール | O1 \`.text\` OFF/ON |
| --- | ---: | ---: | ---: |
| 専用warp | 71.47ms | 71.25ms | 17,874B / 17,874B |
| quicksort_4000 | 141.76ms | 143.40ms | 27,574B / 27,574B |
| micro_read_1000000 | 43.19ms | 43.51ms | 5,526B / 5,526B |

O1にしてもネイティブコードサイズの削減は確認できず、
起動・コンパイル込みの総時間差は測定ノイズ程度だった。
\`XYO_JIT_OPT_LEVEL=0\` は現行バックエンドでサポートされず、
**O3にフォールバックする**ため、測定スクリプトも1〜3のみ受け付ける。

最終ビルド後の関連CTestは7件中7件通過：
\`xyo-tests\`, \`xyo-jit-lazy-typed-ssa\`,
\`xyo-jit-type-flow-fuzz\`, \`xyo-jit-if-phi\`,
\`xyo-jit-local-read-forward\`, \`xyo-jit-osr\`,
\`xyo-jit-ssa-scheduler\`。最適化前IRの\`load i64\`数の
減少もwarp／非warp双方のテストでアサート済み。

**現時点でLLVM O1/O3が削除する冗長なloadを
JITフロントエンドで前倒し除去したにとどまり、
数値変換helper呼び出しの数の減少や
機械語実行速度の改善は未確認**。
