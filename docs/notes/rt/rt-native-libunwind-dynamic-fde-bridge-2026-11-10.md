# RT-001：GNU登録JIT FDEをlibunwindへ一時公開する実験（2026-11-10）

## 結論

実LLVM23 ORC JITで2本のScratch Fiberを同時に停止し、GNU unwinderとlibunwindが参照しているJITのDWARF FDEを比較した。GNU `_Unwind_Find_FDE`は両方のJIT手続きに対して**本物のFDEと正しい所有関数**を返すが、既定の`libunwind`は1バイトの合成手続き範囲を返していた。libunwindの通常のキャッシュを消去しても変化しなかった。

今回、GNU側から取得した**すでに再配置・登録済みの本物のFDE**を`_U_dyn_register`に一時登録する、実験専用のbridgeを接続・検証した。登録後に`unw_flush_cache`し、**保存済みFiberRegsから新しいunw_cursor_tを作成**すると、両方のFiberで本物のJIT FDEを取得できた。登録解除後はそのFDEがlibunwindから見えなくなることを確認し、**ネイティブFiberを再開して最終的なScratchの状態まで一致**させた。

GCは引き続き禁止：本実験はFDEの可視性を示すものであり、LLVM StackMap位置でFiberが停止したこと、boxed Valueやレジスタ中のGCオブジェクトが網羅されたことは証明しない。

## 実装・テスト

- `tests/platform/rt_native_fiber_cfi_unwind_smoke.cpp`の独立したテストモード：
  - `XYO_RT001_GNU_FDE_PROBE=1`：`libgcc_s.so.1`から`_Unwind_Find_FDE`を取得し、保存中Fiberの正確なJIT復帰先PCから本物のFDEを特定する。所有関数`bases.func`と実JIT手続きの開始アドレスが一致しなければ拒否。
  - `XYO_RT001_LIBUNWIND_DYNAMIC_BRIDGE=1`：`_Unwind_Find_FDE`が返したFDEの位置がJIT手続き先頭から符号付き32bit相対距離に収まる場合に限り、libunwindの`UNW_INFO_FORMAT_REMOTE_TABLE`に実FDEへの相対参照を一時登録する。64KB等の固定範囲から作り物のCFIを合成するのではなく、**実際のJIT FDEのバイト列そのもの**を参照する。
  - 登録前に`unw_get_proc_info_by_ip`で正規JIT FDEが取得できないことを確認。
  - 登録後、`unw_get_proc_info_by_ip`と、元の保存レジスタから作り直した`unw_cursor_t`の両方で、正規FDEの所有関数・長さを確認。
  - `_U_dyn_cancel`で登録を解除し、キャッシュを消去した後に同じPCで正規FDEを再度取得できないことを確認。登録テーブルはスタック上の一時データであり、**寿命が尽きる前に必ず解除**する。
  - `XYO_RT001_RESUME_AFTER_CANCEL=1`：解除後に2本のLLVM Fiberを続行して終了させ、Scratch最終値`y=48005`を直接検証する。FDEが不要になった後でも元のネイティブコードの継続実行を妨げないことを確認。
- `tests/differential/rt_native_fiber_cfi_unwind.py`：
  - `--bridge`で上記モードを独立したプロセスに設定。
  - 2本分のGNU FDE所有関数、登録前の未取得、登録後の直接取得、**freshカーソルでの実FDE確認**、登録解除後に消失、継続実行後の`y=48005`をすべて検査。
  - `llvm-objdump --reloc`から元の`xyo_yield`の戻り先1869と古いキャプチャ呼出しの戻り先1849を区別。`llvm-readobj --stackmap`で1861を別のPCと判定。
  - `llvm-dwarfdump --eh-frame`で実ORC ELF FDEのサイズが正確に一致することを比較：2オブジェクトとも長さ3531バイト（FDE範囲[400,3931)、[368,3899)）。
  - すべてのログで`native_safepoint_verified=0`、`machine_roots_proven=0`、`gc_safe=0`を要求。
- `tests/CMakeLists.txt`：
  - 従来の`xyo-rt001-native-fiber-cfi-unwind-smoke`に加えて、`xyo-rt001-native-fiber-cfi-dynamic-bridge-smoke`を追加。Linux x86-64、libunwindヘッダ／ライブラリ、LLVM readobj/objdump/dwarfdumpとPythonが存在する場合のみ登録。**本番のxyo実行ファイルにはlibunwindを追加リンクしない**。

## 実験で観測した値

| 項目 | Fiber 1 | Fiber 2 |
|---|---:|---:|
| GNU `_Unwind_Find_FDE` | 発見 | 発見 |
| GNU FDE所有関数 | 一致 | 一致 |
| 登録前libunwind正規FDE | なし | なし |
| 動的登録後libunwind正規FDE | あり | あり |
| 保存レジスタから再作成したカーソルで正規FDE | あり | あり |
| 動的FDE長 | 3531バイト | 3531バイト |
| ORC ELF FDE長 | 3531バイト | 3531バイト |
| 解除後libunwind正規FDE | なし | なし |
| 解除後のネイティブ復帰・終了 | PASS | PASS |
| 最終Scratch変数 `y` | 48005 | 48005 |
| StackMap位置での停止・全GCルートの証明 | **未証明** | **未証明** |

LLVM O3・O2の両方で2/2のFiber実験がPASS。bridge OFFの対照テストもPASS。

## コードと環境

- 隔離LLVM23実行ファイル（本体コード変更なし）：`/home/kicky/.rt001-machine-stackmap-isolated/xyo`（SHA256 `a81115b29071eaba00836a09239a0ad1e98a04ca768fc0946ae535031bdade18`）。
- 独立C++テスト：`/home/kicky/.rt001-machine-stackmap-isolated/rt_native_fiber_cfi_unwind_smoke`。
- 実FDEの動的登録検証コマンド：
  ```sh
  python3 tests/differential/rt_native_fiber_cfi_unwind.py \
    --bridge \
    --smoke /home/kicky/.rt001-machine-stackmap-isolated/rt_native_fiber_cfi_unwind_smoke \
    --out build/rt001-fde-bridge-completion-o3
  XYO_JIT_OPT_LEVEL=2 python3 tests/differential/rt_native_fiber_cfi_unwind.py \
    --bridge \
    --smoke /home/kicky/.rt001-machine-stackmap-isolated/rt_native_fiber_cfi_unwind_smoke \
    --out build/rt001-fde-bridge-completion-o2
  ```
- 既存ネイティブ復帰・GC延期回帰は、同じ隔離xyoで以前`955/955`PASS済み（`build/rt001-machine-stackmap-full.log`）。今回変更したのは新規テストとCTest設定・文書のみで、本体に対する全回帰は再実行していない。
- CMake再生成・全CTestの再実行は未実施。コミット・pushは行っていない。他担当の変更は保持。

## 今後の課題

このFDEブリッジは**テストハーネス内で一時的に使用しただけ**。本番で持続的に使用するには、ORCがJITリンクしたコードのロード／アンロードとFDE登録・解除の寿命管理、複数オブジェクトの一意性、並行コンパイル時の相互排他、32bit相対オフセットの届かない場合の拒否を厳格に設計する必要がある。

また、正規FDEが見えるようになっても、Fiberが停止しているPCは`xyo_yield`からの復帰先1869であり、StackMapは別PCの1861。GC rootを適用するには機械レベルの停止点の同期、正確なCFA/stack-slot基準、GC-managedオブジェクト全体を含むrootの完備性の証明が不可欠。**現在の`FiberPool::unsafeCount()`による生存中FiberのGC禁止は変更しない。**
