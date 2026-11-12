# xyo-cpp 実行モデル解説（Manim）

本編: **xyo_execution_model.mp4**  
動画ソース: **xyo_execution_model.py**  
ポスター: **xyo_execution_model_poster.png**

約58秒の、日本語テロップとアニメーション図解による解説動画。音声なし。
Manim Community 0.22.1、1280×720、30fps、H.264で作成。

## 内容

| 時間の目安 | テーマ |
|---|---|
| 0:00–0:05 | xyo-cppの紹介 |
| 0:05–0:12 | SB3 → Source IR → Runtime |
| 0:12–0:19 | Interpreter / Eager / HotSpot |
| 0:19–0:26 | LLVM IR → O3 → ORC JIT |
| 0:26–0:33 | Sequencer / Fiber / yield |
| 0:33–0:40 | 型ガードとScratch互換の処理へのフォールバック |
| 0:40–0:46 | 非同期コンパイルと限定的OSR（試験機能） |
| 0:46–0:53 | LLVM JIT・WASM・AOTの違い |
| 0:53–0:58 | まとめ |

## 再レンダリング

xyo-cppのルートで、Manim CommunityとFFmpeg、Cairo/Pango、日本語フォント Noto Sans CJK JP が利用できる環境から実行:

    python -m manim -qm docs/videos/xyo_execution_model.py XyoExecutionModel \
      -o xyo_execution_model.mp4 --media_dir docs/videos/media

Manimの出力動画:

    docs/videos/media/videos/xyo_execution_model/720p30/xyo_execution_model.mp4

閲覧・共有用には上記を docs/videos/xyo_execution_model.mp4 にコピーする。

## 根拠となる実装資料

- docs/design.md — Runtime、Sequencer、Fiber、yield
- RUNNING.md — JITモード、遅延JIT、AOT
- docs/jit-backend-architecture.md — LLVMとバックエンド分離
- docs/scratch-unified-source-ir.md — Source IRの対応範囲
- docs/jit-interpreter-during-compile-2026-11-06.md — 非同期JIT
- docs/jit-osr-2026-11-06.md — opt-inでの限定的OSR
- docs/wasm-hotspot.md / docs/jit-backend-compatibility.md — WASM HotSpotの対応範囲

動画は理解のために図を単純化している。共通実行可能IRへの全面移行、WASMのLLVM完全互換、一般化されたOSRを完成済みとは説明しない。OSRと非同期コンパイルは試験用オプションとして明示している。
