# RT-001：ORC ResourceTracker と libunwind FDE の寿命連動（2026-11-10）

## 結論

LLVM 23 ORC `LLJIT` の **実際の `ResourceTracker`** と `libunwind` の動的JIT FDE登録の寿命を連動させる独立C++診断を実装・検証した。

- 2つのJIT LLVM IRモジュールを別々のResourceTrackerでmaterializeし、LLVM JITLink側のGNU `_Unwind_Find_FDE` が返した **実際の再配置済みFDE** をlibunwindの `_U_dyn_register` へ公開する。
- 2つの生存FDEを独立して保持し、片方のResourceTrackerを解放した場合、必ず**先に**libunwindの登録を解除する。別のTracker/FDEは有効なまま保持される。
- Tracker解放後、libunwindだけでなくGNU `_Unwind_Find_FDE` も当該FDEを返さないことを確認。別TrackerのGNU登録は残る。
- 以前解放したものと**同名**の関数を再コンパイルし、新しいJITモジュールのFDEが正しく登録され、解放時に再度消えることを確認。
- `ResourceTracker::transferTo` によるORCコード所有権移譲に合わせて登録オブジェクトの寿命も新所有者に移動。元Trackerを解放してもFDEは残り、移譲先Trackerの解放で消える。
- 4回の追加の生成→解放サイクルで、JIT実行可能メモリのアドレス再利用（ABAが起こりうるケース）でも古いFDEを登録し続けないことを確認。
- `Default`、`Less`、`None` の3種のLLVMネイティブcodegen設定で、独立プロセス各10回、**30/30 PASS**。また実際の分離CMakeビルドでコンパイル・CTestの3設定、**3/3 PASS**。

## 実装

新規：
`tests/platform/rt_orc_fde_resource_tracker_smoke.cpp`

実験用 `TrackedModule` は `llvm::orc::ResourceTrackerSP` と `std::vector<std::unique_ptr<FdeLease>>` の所有権をまとめ、`remove()` 実行時にまず全`FdeLease`をdestructし、`_U_dyn_cancel`→`unw_flush_cache`を確実に実行してから`ResourceTracker::remove()`を呼ぶ。`transferTo`では実ORCのResourceTracker移譲を実行し、FDE所有権も同時に移す。未解放のTrackedModuleデストラクタはabort（エラーを握りつぶしたままコード寿命を終わらせない）。

実験用`FdeLease`は、`_Unwind_Find_FDE`で得た実FDEへの符号付き32bit相対距離を検証し、libunwind `UNW_INFO_FORMAT_REMOTE_TABLE`に登録する。C++所有オブジェクトのアドレスは安定させているため、libunwindに渡した`unw_dyn_info_t`とエントリ表は、必ず登録解除時まで生存する。登録解除した後の古い実行アドレスは解決・実行しない。

この単体テストは**意図的に10バイト前後の小さなleaf関数**を生成し、実験登録の探索区間を最大256バイトに制限して、別のコード割当領域と重なることを防ぐ。これは本番JITで任意の手続きに適用できるFDE区間の取得ロジックではない。

追加CTest：
- `xyo-rt001-orc-fde-resource-tracker-smoke`
- `xyo-rt001-orc-fde-resource-tracker-less-smoke`
- `xyo-rt001-orc-fde-resource-tracker-none-smoke`

登録先`tests/CMakeLists.txt`。Linux x86-64、LLVM backend、libunwind/libunwind-x86_64等の依存が揃う時のみ有効。スタンドアロンテスト実行ファイルであり、`xyo`本体にリンクを追加しない。

## 実行ログと検証

隔離CMake設定ディレクトリ：
`/home/kicky/.rt001-orc-fde-cmake`

このディレクトリでCMake configure・新CTest対象のビルド・実際のCTest実行に成功。対象3件がすべてPASS、全所要0.05秒。

ログ：
- `build/rt001-orc-resource-tracker/cmake-config.log`
- `build/rt001-orc-resource-tracker/cmake-build.log`
- `build/rt001-orc-resource-tracker/ctest.log`
- 実行ファイル：`/home/kicky/.rt001-orc-fde-cmake/tests/xyo-rt001-orc-fde-resource-tracker-smoke`

以前のRT-001全ネイティブ差分回帰955/955 PASSは`build/rt001-machine-stackmap-full.log`の結果であり、今回本体ソースを変更していないため再実行していない。共有作業ツリー全体のclean buildや全CTestも未実施。コミット・pushなし。RT-001は`in_progress`。

## GCに対する制限

今回の独立テストが証明しているのは **ORCの実コードresource lifetimeとlibunwindに公開するFDE登録の寿命を、明示的な所有権ラッパーによって対応付けられること**。本番`xyo-cpp`のLLVMBackend内にこのラッパーを自動で取り込んだわけではない。

本番への組み込み時は、JITリンク／アンリンクをORC ResourceTracker単位で検知して、実際の関数PC範囲をGDB/CFI登録と一致させ、コードアンロード前にFDEを解放するフックが必要。Lazy JIT・並列JIT・リソース移譲・複数FDEを扱う寿命契約も必要。

さらに**実際のFiber保存PCはホスト`xyo_fiber_switch`の復帰先**で、実JITフレームのyield戻り先オフセット1869、既存StackMapの1861とは異なる。完全な機械レベルsafepoint対応およびboxed Scratch Valueを含む全GCルートの保証がないため、引き続き`FiberPool::unsafeCount()`を使ったネイティブFiberのGC回収禁止を解除しない。
