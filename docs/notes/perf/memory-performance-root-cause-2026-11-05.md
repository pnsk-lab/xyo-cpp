# performance.sb3 異常メモリ増大: 直接の割当元を特定 (2026-11-05)

## 再現環境
`debug/projects/performance.sb3`, `build/xyo`, Linux/x86_64。完全実行は `--frames 1000000` で、RSS上限を超えたら測定器が SIGKILL する。対象は4 targets / 2310 blocks。作業ツリーに既存の未コミット変更が多数あるため、最適化自体の修正はまだ行っていない。

## 確定した事実

- Interpreter: exit 0、32.0秒、最大116060KiB
- HotSpot auto: 約14.9秒でRSS 1GiBを突破して強制停止。14.22〜14.87秒にRSS約197MiB→1024MiB。主な増加は `[heap]`
- `XYO_PAR=0`（スケジューラ並列無効）でも約14.5秒で512MiB突破
- `XYO_PARFOR=0`、`--list-limit 200000`、`--jit-lazy off` でも発生
- **`XYO_JIT_TRANSIENT_STRINGS=0` にするとexit 0、33.81秒、最大248552KiB (242.7MiB)**
- `XYO_JIT_TRANSIENT_MULTI=0` だけでは約14.7秒で512MiB突破
- **`XYO_JIT_TRANSIENT_BATCH=0` ではexit 0、34.04秒、最大374592KiB (365.8MiB)**

## mallocのスタック追跡

計測用に `build/memtrace.cpp` / `build/memtrace.so` を作成し、`LD_PRELOAD` で `malloc`、`calloc`、`realloc` の累積要求バイト数に応じてバックトレースを出した。`addr2line -a -i -f -C -e build/xyo 0x254bb2 0x24ed1c` より、急増区間の確保元は:

```
JIT-generated code
  -> xyo_transient_append_repeat_packed
      src/core/string_util.cpp:287
  -> AppendLog::append
      src/core/string_util.cpp:55
  -> std::make_unique<char16_t[]>(256) [512-byte chunks]
  -> operator new / malloc
```

全体の累積要求バイト数はRSSでも生存バイト数でもない。だが同じ時刻に実際のヒープRSSが激増しており、割当先を裏付ける。初期の累積配分はLLVMの最適化処理からも来るが、512MiB以降の呼び出し元は主に`AppendLog::append`。

## 機構と未解決点

- `src/jit/codegen.cpp` の `emitTransientRepeat` は自己連結ワープループを変換し、64回単位で `xyo_transient_append_repeat_packed` を呼ぶ。この実装ではUTF-16の各文字を固定チャンクに**即座に具象化**するため、長い文字列で実メモリが線形増加する。
- 一括追加を無効にすると正常完走するため、根本の誘発条件は単なるGCではなく**バッチ実行されたTransient AppendLog**。時間で処理量を決めるベンチの場合、速度上昇が生成データ量を増やす可能性もある。現状では論理反復数や出力文字列長の同値性は未確認で、バッチによる過剰反復と高速化による大量生成を切り分ける必要がある。
- **「不要オブジェクトが解放されない」という意味のメモリリークは未証明**。実際に長いAppendLogが参照されている場合は生存オブジェクトの膨張である。

## 低メモリ回避設定

```bash
XYO_JIT_TRANSIENT_STRINGS=0 build/xyo run debug/projects/performance.sb3 --jit hotspot
```

バッチのみ外す場合は `XYO_JIT_TRANSIENT_BATCH=0`。今回は前者がメモリ使用量最小。恒久対応は大規模AppendLogの表現変更（連続チャンクを惰性的な繰返し表現にする、または安全な上限から通常の文字列連結へ切替え）とバッチの反復数パリティ検証。生成文字列の意味を変える上限による切り捨ては不可。

## データ

- `bench-output/memory-performance-allocationtrace.log`
- `bench-output/memory-performance-allocationtrace.json`
- `bench-output/memory-performance-no-transient.json`
- `bench-output/memory-performance-no-transient-multi.json`
- `bench-output/memory-performance-no-transient-batch.json`
