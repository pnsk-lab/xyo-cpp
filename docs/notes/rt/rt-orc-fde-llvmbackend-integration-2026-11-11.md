# RT-001：ORC JITLink FDE登録・解除をLLVMBackendへ実験的に統合（2026-11-11）

## 結論

実際のLLVM23 ORCの`LinkGraphLinkingLayer::Plugin`を利用し、**JITLinkの生成完了・リソース削除・所有権移譲に同期したFDE登録寿命管理**を、本番`LLVMBackend`に明示的な二重opt-inで追加した。

**通常ビルドは既定OFF**。実験時にはCMakeオプションと実行時フラグの両方が必要：

```sh
cmake -S . -B /home/kicky/.rt001-orc-fde-cmake -G Ninja \
  -DXYO_JIT=ON -DXYO_JIT_BACKEND=llvm \
  -DXYO_RT001_ORC_FDE_PLUGIN=ON
cmake --build /home/kicky/.rt001-orc-fde-cmake --target xyo-cli -j 8

XYO_RT_ORC_FDE_REGISTRATION=1 \
  /home/kicky/.rt001-orc-fde-cmake/xyo <command>
```

`XYO_RT001_ORC_FDE_PLUGIN`がOFFなら、libunwindへの依存とORCプラグイン本体はそもそも組み込まれない。ONでも`XYO_RT_ORC_FDE_REGISTRATION`未設定なら登録動作なし。

**GC安全性は一切変更しない**：native Fiberの回収禁止、`FiberPool::unsafeCount()`のゲート、SSR/SSAのcaptured Value ownership、StackMapは本変更で変更なし。全ログは`gc_safe=0`を明示する。

## 実装

1. 新規`src/jit/rt_orc_fde_lifecycle.hpp`（Linux x86-64実験用）：
   - `OrcJitFdeLifecyclePlugin`は`LinkGraphLinkingLayer::Plugin`を継承。
   - `modifyPassConfig`にPostFixup passを追加。JITLinkが解決した後の`xyo_script_*` callable symbolsの**実アドレスとJITLinkが報告する正確なsymbol size**を収集。可能性のある任意の関数範囲を固定値で推定しない。
   - `notifyEmitted`時にGNU `_Unwind_Find_FDE`を実JIT PCで呼び、対象関数の所有者一致を確認。FDE→関数先頭の相対オフセットが符号付き32bitに収まるものだけ、`libunwind`の`UNW_INFO_FORMAT_REMOTE_TABLE`へ登録。`unw_get_proc_info_by_ip`から**正確な先頭・末尾（symbol size）**が復元されることを検証。1つでも失敗すればそのmaterializationの一時登録を自動解除し、`[rt001-orc-fde] ... invalid_extent=1/fde_missing=1`の診断のみ表示（JITの通常実行は継続）。
   - `notifyRemovingResources`は該当ORC ResourceKeyの登録をすべて解除し`unw_flush_cache`した後に返す。ORCコード/FDEがunmapされる前に借用FDEメタデータを破棄する。
   - `notifyTransferringResources`で登録所有権をResourceKeyとともに移す。複数の並列materializationが同じresource keyを使う可能性に備え、pendingはMaterializationResponsibilityの個体IDで管理し、`std::mutex`で保護。
   - `notifyFailed`では未完了pendingデータを破棄する。現時点のプラグインはxyo_script_*に絞っている。その他のJIT/AOT関数が対象外であり、全関数CFIを完全登録したという証明ではない。
2. `src/jit/backend_llvm.cpp`：実験マクロのあるビルドで、EagerおよびORC Lazyの`LLJIT`作成時にプラグインを追加。実行時フラグOFFなら登録なし。デフォルトの変換経路と既存GCの状態遷移を変更しない。
3. `CMakeLists.txt`：既定OFFの`XYO_RT001_ORC_FDE_PLUGIN`を追加。Linux x86-64 LLVMかつlibunwind+libunwind-x86_64のみ受理。opt-inに限ってxyoにinclude、compile definition、libunwindのリンク依存を追加。
4. 新規`tests/platform/rt_orc_fde_link_plugin_smoke.cpp`：本物のORC JITLink plugin callbackを使い、emit→register、ResourceTracker remove→unregister、transfer、再materializeを検証。直前までの独立所有権テストよりさらに実際のJITLink通知へ近い。
5. 新規`tests/differential/rt_orc_fde_runtime_plugin.py`：**実際のxyo CLI**を実Scratch 2-Fiberプロジェクトで使用。Interpreterと最終状態を比較、EagerとLazyそれぞれでJITFDE 4個の登録・解除、各エンジンへのplugin install、GC許可フラグなし、フラグOFFのJITにプラグインの副作用なし、StackMap同時ONでもGC認可しないことをチェック。
6. `tests/CMakeLists.txt`：新規Ctestsを追加。旧standalone ResourceTracker3種類も同じCMake buildで維持。

## 現在の全体テスト結果

### 実際のcurrent-source CMakeビルド

`/home/kicky/.rt001-orc-fde-cmake`を新規隔離configureし、現在の共有作業ツリーソースから`xyo-cli`全体をコンパイル・リンク。**成功**。これは古いlibxyo.aに一部の新コンパイラだけを差し替える方式ではなく、当該ディレクトリでxyo-cliと関連libxyoを全ソースからビルドした。

- `XYO_RT_ORC_FDE_REGISTRATION=1`、StackMap診断OFFのネイティブ復帰・失敗クローズ差分回帰：**955/955 PASS**（`build/rt001-orc-fde-clean-full.log`）。
- 実ScratchのEager JIT：`xyo_script_*` 4件FDE登録、4件解除、Interpreter最終値`y=48005`、PASS。
- 実ScratchのLazy JIT：4件FDE登録、4件解除、`y=48005`、PASS。
- Eager・Lazyとも、StackMap診断`XYO_RT_MACHINE_STACKMAP_PROBE=1`を同時に設定した時にも、FDE 4件の登録・解除／Interpreter一致／`gc_safe=0`：PASS。
- ビルドされたプラグインの実行時フラグを0にしたJITには登録ログもなく、Interpreterと一致：PASS。
- 関連CTest **6/6 PASS**（`build/rt001-orc-resource-tracker/ctest-plugin-six.log`）。本番LLVMBackend Eager+Lazy（StackMap OFF/ON）、standalone ResourceTracker default/less/none、JITLink実コールバックの6件。
- 先行するstandalone ResourceTrackerは30独立プロセスで全件PASS。実FDEを保持したままORCコードを解放しない順序が検証済み。

### 混在バイナリでのみ再現した障害

以前の検証済みアーカイブの隔離コピーに、新しいLLVMBackendと`rt.cpp`だけを再コンパイルして差し替えた混在CLI `/home/kicky/.rt001-orc-fde-runtime-isolated/xyo` では、`XYO_RT_MACHINE_STACKMAP_PROBE=1`を有効化すると`free(): invalid pointer`が再現した。FDE登録OFFでも再現するため本FDEプラグイン独自の原因とは考えにくい。

**全ソースを再コンパイルした上記CMakeビルドでは、同じScratch fixtureでStackMap+FDEの同時有効化がPASS。** 古い内部ABI／混在ビルドの不整合が疑われるが、その原因をメモリ検査で確定したわけではない。混在CLIはこの組み合わせの検証には使用しない。

## 安全性の留保

- ここで登録するのは**JIT命令のDWARF FDE（スタック巻き戻し情報）**であり、LLVMのGC statepoint/stackmapと完全に対応するroot提供機構ではない。
- 既存の実Fiberでは、unwindしたJIT内yield復帰アドレスと直前のStackMap命令の位置が異なる。**機械語の正確なsafepoint停止PCやGC root集合の完全性は依然として未証明**。
- プラグインはまだ実験用xyo_script_*のFDEに限定し、AOT／別名関数／GNU libgcc以外のアンワインダ／Windows・macOS／リソースの想定外同時破棄を保証するものではない。
- たとえFDEが登録済みでも、生存中Native Fiberに対するGCは引き続き延期する。
- 今回の分離CMake環境でxyo-cli全体と関連CTestを検証したが、リポジトリの全CTestは実行していない。コミット・pushなし。他の担当者の未コミット変更は維持。

## 次に検討する項目

1. 最適化後の実StackMap PCと、`xyo_yield`実際の中断地点を1対1にするSafePointプロトコル（たとえば呼び出し直前の明示safe-liveness statepoint）。
2. native `i64` boxed Value、GC-managed文字列／リスト参照の全ルートを機械レジスタ・スタック上に列挙して、収集の適格性を**fail-closed**判定する。
3. 生成・アンロードが並列に進む場合のORC resource keyの生存期間とJITコードの停止・非実行保証を整理した追加テスト。
4. すべての証明が揃うまで、`FiberPool::unsafeCount()`によるGC禁止は維持する。
