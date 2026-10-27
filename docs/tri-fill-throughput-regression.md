# Tri fill engineの高stamps時のThroughput低下

## 再現と原因

対象は[The fastest tri fill engine.sb3](<../The fastest tri fill engine.sb3>)。既定の`@stamps=10000`では、プロジェクトのミリ秒単位の計時により約1,000万tpsが表示される場合がある。この値だけでは、高stamps時の速度を確認できない。

初期`@stamps`だけを変更したアーカイブを作り、現行のheadless `run --frames 120`で確認した。10万stampsではGPUが120回実行され、最終表示は10,000,023tpsだった。100万stampsではGPU実行成功が0回、CPUフォールバックが120回で、最終表示は2,331,003tpsだった。数学boxing最適化前の保存バイナリでも、100万stampsで2,227,172tps・GPU実行成功0回となった。したがって、この再現では直近の数学boxing変更が原因ではない。

TFGのGPUプログラムは1反復あたり32ストローク分の記録領域を予約する。100万stampsでは`1,000,000 × 32 × 48 = 1,536,000,000 bytes`となり、Vulkan executorの512MiB総容量ガードに達する。大きいループを分割せず全体をCPUへ戻すため、Throughputが急に落ちていた。

Vulkanはヘッドレス用で描画面を持たず、描画レコードの内容を呼び出し側へ返すこともない。それでも従来はレコードの領域を確保し、シェーダから書き込んでいた。[再現ログ](<../build/tri-fill-regression/current-1000000.stderr>)と[変数出力](<../build/tri-fill-regression/current-1000000.stdout>)を保存した。

## 修正

[Vulkan executor](<../src/gpu/gpu_vulkan.cpp>)の既定経路で、未使用の描画レコードの確保と書き込みを省いた。[共有VMシェーダ](<../src/gpu/shaders/gpu_vm.comp>)から、Vulkanヘッドレス用のSPIR-Vを追加生成する。GL描画側は従来と同じレコード出力を使う。

省くのはレコードのpayloadだけである。座標の有限性検査、ストローク件数、レコード上限・命令数のガード、ペン属性・位置・変数のwriteback、lane別RNG系列は同じ経路を通る。レコードbinding自体は48バイトの有効なダミーバッファとして残す。今回、512MiBの上限を引き上げたり、GPUメモリを無制限に使うようにはしていない。lane出力など他のバッファの容量・SSBO・workgroup制限は維持する。

`XYO_GPU_VK_RECORDS=1`で、同一バイナリ内の従来のレコード生成・確保へ戻せる。パイプラインの選択はプロセス起動時に固定する。この切替はVulkanだけに適用し、GL描画の設定は変更しない。

併せて、共有シェーダが読む終端ペン状態の7フィールドをVulkanのパラメータ領域にも確保・初期化した。従来は固定20＋writebackスロット分しか渡しておらず、後続laneの初期状態を渡す領域が不足していた。GLと同じ`penDownAll`、終端属性mask、5属性値を渡す。これはレコード省略とは別のABI整合性修正で、両A/B経路に適用する。

## 検証方法

[実Vulkan回帰テスト](<../tests/gpu/test_vulkan_headless.cpp>)を追加した。`XYO_TEST_VULKAN=1`で明示的に有効にした場合、GPU利用不可をskipではなく失敗とする。

- 65laneの部分workgroupで、件数・位置・ペン属性・flags・writeback値・shadeを独立した期待値と比較する。
- lane 0と後続laneで異なる初期ペン状態を与え、終端状態の7フィールドが正しく渡ることを検査する。
- ストローク上限超過と命令ガード超過で失敗し、Runtimeの変数・位置・ペン状態・移動フック・RNGを変更しないことを検査する。
- 100万lane×32の理論上のレコード容量でも既定経路が成功し、全laneの状態と300万ストロークという件数が一致することを検査する。従来経路は容量ガードで失敗することも確認する。

実行結果は既定経路70チェック、従来レコード経路67チェックで、どちらも失敗0だった。

[交互A/Bスクリプト](<../tests/bench/tri_fill_regression.py>)は実行ファイルを固定してSHA-256を照合する。原本SB3は変更せず、初期stampsを100万にし、指定数の完全なバッチを実行して停止する計測用アーカイブを作る。CPUの500ms予算で1バッチがフレームを跨いでも、途中で打ち切らない。各バッチのThroughput更新後に完了カウンタを増やし、両群の総stamps数を照合する。元のThroughput計算式やテスト本体は変えない。

診断実行では従来経路の容量ガードと、修正版のGPU実行成功を確認する。時間計測中は診断・プロファイルを外し、同じseed・同じバッチ数で交互に測定する。プロジェクトのtps表示は最後のバッチの値であり、全プロセス時間にはロード・JITなども含まれるため、両者を分けて記録する。

## 最終交互A/B

RTX 3070、headless `run`、固定seed=1、100万stampsを6バッチ、各試行の総量は600万stamps。実行ファイルと入力アーカイブを固定し、従来の記録あり／修正後のヘッドレスを交互に各5回実行した。診断ログは別の実行だけで有効にし、計時中は無効にした。全試行で6バッチ完了を照合し、初期化・JITを含むプロセス全体の時間も記録した。

| 各5回の中央値 | 従来の記録あり・容量制限でCPUへ | 修正後のヘッドレスGPU |
| --- | ---: | ---: |
| 最終バッチのプロジェクト表示 | 2,352,942 tps | 15,624,982 tps |
| 表示tpsの範囲 | 2,298,850〜2,364,066 | 13,333,350〜16,393,434 |
| 全プロセスwall time | 2,863.572 ms | 785.153 ms |
| 全プロセスwall timeの範囲 | 2,850.913〜2,913.796 ms | 763.116〜802.140 ms |

表示tpsの中央値は約6.64倍、全プロセス時間は約3.65倍速く、5組全てで修正版のプロセス時間が短かった。両者は計測区間が異なるため、同じ比率にはならない。既定の1万stampsで1ms付近の値を比較したものではない。

[全試行](<../build/tri-fill-final-ab/results.json>)、[集計](<../build/tri-fill-final-ab/summary.json>)、[実行ファイル・原本・計測用アーカイブのハッシュ](<../build/tri-fill-final-ab/manifest.json>)を保存した。診断実行も、修正版は全6バッチで`executed 1000000 lanes`、従来経路は容量制限によるフォールバックとなった。

元の実行構造で初期stampsだけを100万にしたアーカイブも、修正後に`--frames 120`で確認した。120回すべてのGPU実行が成功し、CPUフォールバック0、最終表示は16,129,020tpsだった。この確認は各1回の診断であり、交互A/Bの表には混ぜていない。[変数出力](<../build/tri-fill-regression/fixed-original-1000000.stdout>)と[GPUログ](<../build/tri-fill-regression/fixed-original-1000000.stderr>)を保存した。

## AOTと回帰検証

修正後の実行ファイルからAOTアプリを書き出し、同じ100万stamps×6バッチをheadlessで実行した。修正版は全6バッチでGPU実行に成功し、最終表示は16,393,434tpsだった。`XYO_GPU_VK_RECORDS=1`では全6バッチがCPUへ戻り、2,267,573tpsとなった。これは診断付きの各1回なので、性能中央値には使っていない。[AOT実行ログ](<../build/tri-fill-regression/aot-headless.stderr>)を保存した。

回帰検証は次のとおり。

- [最終CTest](<../build/tri-fill-regression/ctest-final.log>)は6/6成功。
- 実Vulkanは既定70チェック／従来記録経路67チェック、失敗0。
- [実OpenGL並列テスト](<../build/tri-fill-regression/gl-live.log>)は377チェック、失敗0。
- インタプリタのunit suiteも成功。
- executeフィクスチャはJIT・インタプリタとも116/116成功（assert 336／339、失敗0）。

既存のGPU VMはf32とlane別RNGを使う。本修正はその実行規則を変えていないが、CPUへ戻っていた大きい負荷がGPUで実行されるようになるため、ランダムを含むTFGの最終状態が従来のCPU経路とビット一致するとは主張しない。独立したGPU状態の期待値・ガード・writebackのテストで、レコード省略以外の動作を確認した。

## 使用と再現

通常のheadless実行では追加設定は不要。修正を含む[build/xyo](<../build/xyo>)を使う。既存のAOTアプリは古いランタイムを含むため、修正後のバイナリから書き出し直す必要がある。

```sh
cmake --build build -j 2
XYO_TEST_VULKAN=1 build/tests/xyo-gpu-tests --vulkan-headless
XYO_TEST_VULKAN=1 XYO_GPU_VK_RECORDS=1 \
  build/tests/xyo-gpu-tests --vulkan-headless
ctest --test-dir build --output-on-failure
python3 tests/bench/tri_fill_regression.py \
  --frames 6 --trials 5 --output build/tri-fill-recheck
```

`XYO_GPU_VK_RECORDS`は変数が存在すると従来経路になる診断用スイッチで、通常は未設定にする。Vulkanが使えない環境や、他の出力バッファ・workgroup制限に達するさらに大きな負荷では、引き続きCPUへ戻る。この文書の修正はrun／AOTのヘッドレスVulkan経路を対象とする。その後のウィンドウ付きGL描画の分割実行と実測値は[play経由の記録](<tri-fill-play-chunking.md>)を参照。ヘッドレスのtpsをplayの性能値として扱わない。
