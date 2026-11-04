# v1.0.0 Linux Release の AOT SIGILL 修正（2026-11-04）

## 障害

- Release Actions の Linux / JIT + SDL2 + librsvg ジョブで通常のビルドと `xyo run` は成功。
- `bash tools/ci/smoke.sh run build/xyo` の AOT 実行だけが SIGILL（終了コード132）で失敗。`--aot` で同じオブジェクトをロードしても SIGILL。
- Windows版・Linux完全静的版のビルドは成功。GitHub Releaseの公開処理はスキップ。
- 発生したワークフロー: https://github.com/pnsk-lab/xyo-cpp/actions/runs/41760841099

## 原因と修正

`src/aot/aot.cpp` で LLVM のターゲットCPU名に `llvm::sys::getHostCPUName()` を指定していた。これはランナーの仮想CPU情報と実際に利用可能な命令セットが一致しない環境や、コンパイル機と実行機が異なる環境で、生成したAOTに利用不可能な拡張命令を含める可能性がある。

既定のAOT生成を `generic` CPU（そのアーキテクチャのベースライン）に変更し、意図的なホスト固有最適化のみ `XYO_AOT_TARGET_CPU=native` を指定する方式へ変更した。

LLVM 18上で `llvm.roundeven` をベースライン向けにloweringすると、外部シンボル `roundeven` が現れる。Windowsなどでのシンボル解決を確実にするため、AOTは従来の登録済み `xyo_round` ランタイムヘルパを用い、JITは従来の高速なLLVM `roundeven` 直接生成を維持した。

## 再現と検証

- Ubuntu 24.04 / LLVM 18.1.3 / Release / SDL2 + librsvg / 静的LLVMの環境をPodmanコンテナで再現し、ビルド・通常のAOTスモークが成功。
- 旧 `XYO_AOT_TARGET_CPU=native` で作った `play_demo.sb3` AOTオブジェクト（4952バイト）と、新しい既定設定で作ったもの（4968バイト）を比較。
- QEMU `qemu-x86_64 -cpu qemu64`（基本x86-64 CPUモデル）で、**旧ネイティブCPU向けAOTはSIGILL、新しい汎用CPU向けAOTは成功**。
- LLVM 18で `round_mod.sb3` のAOTを確認。新AOTの未解決シンボルは `xyo_round`（従来の登録済みランタイム呼び出し）になり、`roundeven` libcallが除去された。
- ローカル `cmake --build ...` と `ctest` は **11/11成功**。
- ScratchBenchパリティのHotSpot・Eager・Interpreterは全2123/2123。AOTは既知の乱数試験 `Pick Random Rules 2` が一部の実行で失敗し、追加切り分けが必要。AOTの生成と起動は成功。
- ReleaseワークフローにQEMU `qemu64` でAOTオブジェクトを実行する専用ゲートを追加。今後のネイティブCPU依存への回帰を防ぐ。

## 補足

QEMUでの再現は、今回のLinuxランナーでの命令違反と整合する移植性問題を実証する。一方、元のランナーのコアダンプ／命令アドレス自体は保存されていないため、元のSIGILLの正確な命令は未特定。