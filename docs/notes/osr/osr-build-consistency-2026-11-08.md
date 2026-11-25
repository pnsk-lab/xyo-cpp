# OSRの実行検証とJITビルドの一貫性（2026-11-08）

## 隔離ビルドとヒープ破損の切り分け

共有ワークツリーの `build/xyo` は、複数エージェントが
`CompiledScript`、`Thread`、`JitCtx`、LLVMコード生成を同時に更新する間にビルドされていた。
旧共有バイナリでは `XYO_JIT_OSR=0` にしてもLinux RISC-Vを動かすだけで
`free(): invalid size`、`corrupted double-linked list`、SIGSEGVが発生した。
古いcoreの破棄スタックは `CompiledScript::~CompiledScript` の
`unordered_map<std::string, ScriptFn> procedures` で停止していた。
これは**破損を発見した位置であって、破損させた命令の位置とは限らない**。

そこで実行ファイルを独立したNinja Release buildで全再コンパイルし、
`board/osr-isolated-frozen-xyo` にコピーした（SHA256：
`a3c5535cf92cb8ff3efea25b557f39e213a745d09650ade7288e36167efd124f`）。
このバイナリで同じLinux RISC-VをOSR無効・HotSpot asyncで実行：

- 15フレーム：正常終了
- 8フレーム×5回：全5回正常終了
- GUI Scratch Parity：off/hotspot/eagerすべて2,123/2,123

この結果は「共有ビルドの新旧オブジェクトが異なる構造体レイアウトで混在した」という仮説と整合するが、
**根本原因の証明にはなっていない**。同じソースでヒープ破損が再現したらASan等で再調査する。

## 限定ネストwarp OSR

別担当が実装中の `XYO_JIT_OSR_NESTED=1` （引数なしwarp手続きの単一`repeat`のみ）を
上記隔離バイナリで実行した。

`tests/differential/jit_nested_warp_osr.py` により、Interpreter、
HotSpot（途中移行ON/OFF）、Eagerで、手続きprefix・ループ・suffixの最終値が一致。
HotSpot ONでは実際に `[jit-osr-nested] promoted` が出た。
一般の引数付き手続きへの途中移行ではなく、専用コンパイル済みloop entryに限定される。

`tests/differential/jit_nested_warp_args_refusal.py` ではUTF-16文字列と数値の引数を持つwarp手続きを実行。
ネストOSRをONにしても危険な途中移行は発生せず、Interpreter/HotSpot/Eagerの実行結果が一致した。
この安全なfallbackを、今後の引数付きOSR実装でも回帰条件にする。

## タイミング依存テスト

既存の `tests/differential/jit_osr.py` のトップレベルOSRでは
`long`、`fractional`、`list`、`nested` は隔離バイナリで結果一致。
`until` は `[jit-async] ... finished 86.529ms` のログが出た一方、ネイティブへ途中移行する前に
インタプリタが終了したため「OSR never triggered」という検証条件に失敗した。
**これはScratch状態の不一致ではなく、途中移行が起こるというタイミング条件の不成立**。
パリティとOSR移行のテストは分離することが望ましい。

## ビルド一貫性の仕組み

`tools/consistent-build.ts` は共有ソースがビルド中に変更されたかを
`src/`、CMake設定のSHA256で確認し、変更を検知したら再実行する。
ビルド後、ソースが安定していた場合だけバイナリをコピーし、
ソースとバイナリのSHA256をmanifestに記録する。
例：

```bash
bun tools/consistent-build.ts \
  --build board/.osr-isolation \
  --target xyo-cli \
  --output board/xyo-coherent \
  --jobs 3 \
  --attempts 4
```

`--build` 先は独立した事前設定済みCMake buildである必要がある。
共有作業中のビルドは、全 translation unit が同一 `Thread`/
`CompiledScript` ヘッダ構造を使ったかをソース更新検出と依存再構築で
確かめてから利用する。**SHA256はコンパイラの意味論まで検証するものではない**。

## 未解決

引数付き／複数制御分岐／任意副作用を含む深いwarp手続きのLLVM途中移行には
SourceFrameSymbolsで認証した継続PC、SSA一時値、GCルート、effectのcommit位置、
実行中Fiberとの所有権移動が必要。現在の `InterpreterOsrSnapshot` は
構造を検証して保持するのみで、LLVMエントリへ無理に制御を移さない。

## 追加の検証（最終）

`bun tools/consistent-build.ts` の実行結果は、ビルド中ソース変更を3回検知して再実行、4回目で正常終了。
生成物 `board/osr-coherent-verified-xyo` と
`board/osr-coherent-verified-xyo.manifest.json` に記録されたソースとバイナリのダイジェスト：

- source SHA-256: `e02bf81b0c7203cb1d90f189566e98f96c951bd1137c8b7489f0b77259c79b11`
- binary SHA-256: `4a69a6be5ad55bef8effbd74dfa55391e903b62dffd80f037546dbba1f727631`

この検証済みの最新バイナリでも、限定ネストwarp OSRテストは
3/3 PASS（HotSpot opt-inでpromotion実発生）、引数付きwarpの
安全なOSR拒否と Interpreter への残留は3/3 PASS。
GUI Scratch Parityはoff/hotspot/eagerそれぞれ2,123/2,123。
Linux RISC-V HotSpot async、OSR無効の8フレーム実行は正常終了。
引数付き拒否テストは `xyo-nested-warp-args-osr-refusal` としてCTestに登録し、
独立CMake設定の `ctest -N` で1件表示されることを確認した。
