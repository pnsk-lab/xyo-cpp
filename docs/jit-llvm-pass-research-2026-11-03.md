# LLVM JITパス費用の検証（2026-11-03）

診断用に`XYO_JIT_PASS_TIMING=1`と`XYO_JIT_OPT_LEVEL=1|2|3`を追加。既定はO3で変更なし。

最大のパリティ用モジュールは165関数、約15万LLVM IR命令。`InstCombinePass`が1320回・344.5ms、`IPSCCPPass`が88.4ms、`EarlyCSEPass`が68.8ms、`GVNPass`が63.4ms。

O1/O2/O3とも2123/2123。O1はLLVM最適化の費用が減るが、47ワークロードの実行性能比O1/O3は幾何平均0.783倍のため既定変更しない。

O3の計測回ではLLVM最適化約1067ms、ORC機械語生成・シンボル解決約1497ms。多数の関数に対する最適化の繰り返しが重い。

次は手続きごとのモジュール分離・遅延コンパイルを検討する。GCルート・Fiber再開・再帰フレーム・AOT互換性に注意。
