# xyo-cpp メモリマッピング調査 (2026-11-05)

## 実装

`tools/profile_memory.py` を追加。Linux `/proc/<pid>/smaps` から RSS/PSS/Private_Dirty を約25ms間隔で収集し、最大サンプルRSS時のマッピング内訳をJSON出力する。通常のxyo実行には計測処理を追加しない。フレーム上限到達によるxyoの終了コード2は期待終了として区別する。

## Scratch Bench 実測

対象: `scratch-bench/parity/Scratch Parity Test.sb3`、`build/xyo`、描画なし。

| 実行 | フレーム上限 | sampled peak RSS | heap RSS | anonymous RSS | LLVM-file RSS |
|---|---:|---:|---:|---:|---:|
| Interpreter | 20 | 129612 KiB | 55668 KiB | 27100 KiB | 30584 KiB |
| HotSpot auto | 20 | 175652 KiB | 97052 KiB | 3468 KiB | 56976 KiB |
| Interpreter | 1 | 140296 KiB | 66284 KiB | 27100 KiB | 30584 KiB |
| HotSpot auto | 1 | 172648 KiB | 96140 KiB | 2260 KiB | 56976 KiB |

1フレームでもHotSpotはInterpreterよりRSSを多く消費した。ただし、測定順、OSページキャッシュ、アロケータの挙動、短時間のピークのサンプル漏れで揺れる。LLVM-fileにはLLVM共有ライブラリの実ページが含まれ、実行時JITが使用した容量そのものではない。GC・Scratchモデル・LLVM管理メモリはheap/anonymous内で区別できない。 `smaps` 集計はGPU VRAMを含まない。

## 使い方

```bash
python3 tools/profile_memory.py 'scratch-bench/parity/Scratch Parity Test.sb3' \
  --jit hotspot --frames 20 --output bench-output/memory-hotspot.json
python3 tools/profile_memory.py 'scratch-bench/parity/Scratch Parity Test.sb3' \
  --jit off --frames 20 --output bench-output/memory-interpreter.json
```

## 次の計測項目

- GC の live bytes, reserved slab bytes, list capacity bytes
- Runtime の owned/reusable Threadと各frame/vectorの容量
- JIT のキャッシュエントリ数、IR/ORCモジュール数、生成コードサイズ
- ロード後のBlocks/Target/Variableのヒープの推定占有量
- 描画バックエンドのCPU側surface/texture/asset容量

上記は今の `smaps` だけでは厳密に分解できないので、別途C++側のオプトイン計器が必要。
