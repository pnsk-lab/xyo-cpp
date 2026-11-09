# AOT Build / AOT Run の進捗表示

`xyo aot` と、AOT オブジェクトをロードして再生する `xyo run --aot`、および単体実行ファイルでは、標準エラー出力に進捗が表示される。

GUI ランチャーの **AOT 書き出し** と **AOT 実行** でも、従来の固定メッセージに代えて各段階が表示される。スクリプト生成、スロット束縛、スクリプト登録など実際の処理件数が分かる工程のみ、段階ごとの進捗バーとパーセンテージを表示する。LLVM O2 最適化とオブジェクト生成には総作業量が分からないため、偽の進捗率や残り時間は表示しない。GUI は処理開始からの経過秒数を表示する。進捗の更新はメインスレッド上のチェックポイントで行うため、単一スクリプト生成や最適化の途中に進捗率は増加しない。

## 実行例

```sh
./build/xyo aot project.sb3 --obj program.o
./build/xyo aot project.sb3 -o project-native
./build/xyo run project.sb3 --aot program.o --frames 100
./project-native run --frames 100

# 進捗メッセージの抑制
./build/xyo aot project.sb3 --obj program.o --no-progress
XYO_AOT_PROGRESS=0 ./build/xyo run project.sb3 --aot program.o
```

`[aot-build]` はスクリプトごとの LLVM IR 生成 → O2 最適化 → オブジェクト生成 → ファイル書き出しを報告する。`[aot-run]` はオブジェクト読込 → スロット束縛 → スクリプト登録 → ロード完了を報告する。

AOT 実行中のヘッドレスモードでは、およそ1秒ごとに実行フレーム数、経過時間、稼働スレッド数を報告する。`--frames N` 指定時に表示するパーセンテージは **フレーム上限に対する割合** であり、プロジェクトの完了率ではない。GUI 再生時も約1秒ごとにフレーム数を標準エラー出力へ報告する。すぐに終了した処理は最後の結果のみ表示する。

進捗は標準エラー出力に書くため、標準出力（リストの出力、最終結果など）を破壊しない。`--no-progress` / `XYO_AOT_PROGRESS=0` は追加の進捗出力を抑制し、従来の結果・エラー表示はそのまま残す。

## API

`aot::compileProject` と `JitEngine::loadAot` の省略可能な `aot::ProgressCallback` により、CLI・GUI共通の型付き `aot::Progress` イベントを取得できる。`completed/total` はその段階で確定した作業件数であり、`total=0` は進捗率不明を示す。従来の引数のみを使う呼び出しは変更する必要がない。
