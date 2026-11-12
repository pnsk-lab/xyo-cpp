# START-007: AOT起動の小規模最適化再検証とCPU固定A/Bベンチマーク

2026-11-10。START-006の直接ブロック構築を既定にした後、
残る起動コストを6方向で検証した。

## 結論

今回は既定化できるほど再現性の高い追加の起動短縮は得られなかった。
性能低下があり得る実験的C++変更はすべて**分離したソース/ビルドに限定**し、
共有作業ツリーのC++実装には反映していない。

一方で今後の性能比較を標準化するため、
`tools/bench-startup-aot-ab.ts` を新設した。

## 実測結果

対象：事前生成済みAOTオブジェクトを使うScratchBench Parity
（2656件・206グループ、ネイティブAOT 479 scripts precompiled）、
Linux、CPU10固定、SDL offscreen / dummy audio、初回のScratch出力まで。
それぞれ**同一バイナリを使った20組の交互A/B**。
別グループの実験はホスト負荷が異なり、絶対時間の横断比較はしない。

| 候補 | 条件 | 従来の起動中央値 | 候補の起動中央値 | 候補勝数 |
|---|---|---:|---:|---:|
| キャッシュ無効化を読み込み末尾まで保留 | glibc | 249.6ms | 245.2ms | 12/20 |
| 同上 | jemalloc | 228.5ms | 223.8ms | 13/20 |
| `unordered_map::try_emplace` による一度の探索 | jemalloc | 211.5ms | 219.4ms | 7/20 |
| 同上＋キャッシュ無効化保留 | jemalloc | 213.1ms | 212.8ms | 12/20 |
| 直接ブロックのハッシュ表を事前確保 | glibc | 253.9ms | 256.5ms | 9/20 |
| 同上 | jemalloc | 227.6ms | 222.5ms | 13/20 |
| プリミティブIDを固定長16進変換に変更 | glibc | 281.3ms | 280.1ms | 9/20 |
| 同上 | jemalloc | 232.3ms | 232.6ms | 15/20 |
| 複数入力ブロックの入力配列を事前確保 | glibc | 311.3ms | 323.3ms | 5/20 |
| 同上 | jemalloc | 294.8ms | 298.0ms | 10/20 |
| JSON入力キーの重複検索を省略 | glibc | 241.9ms | 241.6ms | 12/20 |
| 同上 | jemalloc | 217.5ms | 218.2ms | 12/20 |

タイミングに大きなばらつきがあり、総合優位のない候補を
追加することは不適切。すでに検証済みの
`XYO_SB3_DIRECT_BUILD`の既定化はそのまま維持。

## 再現性のあるベンチマークツール

新しい`tools/bench-startup-aot-ab.ts`は次を実施する。

- `taskset -c CPU`で各起動プロセスのCPU affinityを固定
- 奇数/偶数ペアで実行順を反転（A→B、B→A）
- `performance.now()`によりプロセス生成開始から初回標準出力までを計測
- `aot: N scripts precompiled`を必須とし、
  `aot: load failed`や無言のJITフォールバックを拒否
- 期待出力の接頭辞を検証（`--prefix`）
- 1組ごとの経過時間、各モードの中央値、
  **ペア速度差の中央値・ペア速度比の幾何平均・勝数**を表示
- タイムアウト時や初回出力後の子プロセスを
  プロセスグループごと停止（計測用サンドボックス向け）

コマンド例：

```bash
bun tools/bench-startup-aot-ab.ts \
  --bin ./build/xyo \
  --project /path/to/parity.sb3 \
  --aot /path/to/parity.o \
  --env XYO_SB3_DIRECT_BUILD --a 0 --b 1 \
  --cpu 10 --pairs 20 --prefix 'Scratch Parity Test:'
```

`--`以降でxyo playの追加引数を渡せる
（AOTオブジェクトの指定上書きは安全のため拒否）。
AOT生成時間・キャッシュ生成時間は計測に含めない。
Scratchプロジェクト側が標準出力を行わない場合は使用できない。

実機で8組のテスト計測を実行し、
旧ブロック構築232.2ms→直接構築215.8ms（直接構築が7/8組で高速）、
幾何平均速度比1.0616倍と確認した。

## 検証

`bun test tools/bench-startup-aot-ab.test.ts`：**7/7 PASS**。
引数検証、中央値・幾何平均の集計、
ネイティブAOTロード成功、暗黙のJITフォールバック拒否、
不正な初回出力、出力しないプロセスのタイムアウトを確認。
CIの`scratch-bench`ジョブにも単体テストを追加した。
GitHub Actionsの実行は未確認。

新しい共有C++最適化なし。変更ファイル：
`tools/bench-startup-aot-ab.ts`、
`tools/bench-startup-aot-ab.test.ts`、
`.github/workflows/ci.yml`の追加テストステップ、
本レポート。
他エージェントの未コミット変更には触れていない。
未コミット・未push。
