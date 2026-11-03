# JIT 変数型フィードバック

xyo-cpp は Scratch の動的型を維持したまま、HotSpot コンパイル時に観測された変数型を LLVM コード生成の分岐予測として利用する。

## 動作

- Variable::observedKinds は number / string / boolean / color / other の観測済み集合。書き込み時に単調に更新し、過去の異なる型を忘れない。std::atomic_ref<uint32_t> を使い、クローン生成に必要なコピー可能性を保つ。
- HotSpot のインタプリタの data_setvariableto / data_changevariableby で型を採取（JIT 無効時と Eager 時は記録コストを避ける）。JIT コンパイル時にも変数の現在値（SB3 初期値やホスト設定値）を採取。
- 数値のみを観測した変数では data_variable の読み出し・data_changevariableby の現在値に数値型の予測を付ける。
- emitToNumF64 は NaN-box のガードで通常数値を直接変換し、それ以外は xyo_to_num（Scratch の cast::toNumber）に戻る。比較は数値タグかつ非 NaN の場合だけネイティブの fcmp を使い、それ以外は従来の Scratch 比較へ戻る。
- 既存のループ内レジスタ昇格・数値影 (knownNumD) は優先される。AOT に実行時プロファイルを焼き込まない。

## 正当性と制約

推測は型保証ではない。JIT コンパイル後に別のスクリプト、クローン、外部 API から同じ変数へ文字列や真偽値が代入されても、各読み出し時のガードで汎用処理へ戻る。NaN、-0、Infinity、数値文字列と空白のみの文字列は Scratch の規則に従う。

現在は最初の JIT コンパイル時にのみフィードバックを利用し、実行中の型変化を契機とした再コンパイル・OSR・型特化の再選択はしない。その後も汎用経路を使い続けることがある。

## 検証・比較

```sh
cmake --build build --target xyo-tests -j8
ctest --test-dir build --output-on-failure

XYO_JIT_TYPE_PROFILE=1 build/xyo run project.sb3 --jit eager
XYO_JIT_TYPE_PROFILE=0 build/xyo run project.sb3 --jit eager
```

tests/unit/test_type_profile.cpp は、コンパイル前の混合型とコンパイル後の型変更について、インタプリタ・型予測あり JIT・型予測なし JIT の結果を照合する。

既存のループ昇格が支配するベンチマークには、この変更による大きな速度向上は期待できない。型予測が適用される非昇格変数を使ったベンチマークで改善を評価する必要がある。
