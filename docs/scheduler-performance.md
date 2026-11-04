# scratch-bench: scheduler の最適化記録

## 第1段階: membership 検索と fiber 切替

### 計測結果

Intel Core i7-12700 の P-core（論理 CPU 8）に固定。同じ作業ツリーの
変更直前・変更後を Release / HotSpot / 30 fps で各7回交互に実行した中央値。
既存の未コミット変更も両バイナリに含め、今回の scheduler 変更だけを比較した。

| 項目 | 変更前 ops/ms | 変更後 ops/ms | 比率 |
|---|---:|---:|---:|
| sched_looptick | 11,509.486 | 11,496.760 | 1.00× |
| sched_pingpong | 509.506 | 517.613 | 1.02× |
| sched_restart | 1,712.283 | 1,721.460 | 1.01× |
| sched_clonechurn | 584.938 | 580.597 | 0.99× |
| **sched_threads** | **11,609.722** | **30,506.267** | **2.63×** |
| sched_fanout | 881.363 | 888.900 | 1.01× |
| sched_fanwait | 678.824 | 700.865 | 1.03× |
| **sched_waituntil** | **95.803** | **170.042** | **1.77×** |

**8項目の等重み幾何平均は1.2203倍（+22.0%）**。
主な改善は101本の同時実行と待機ポーリングで、他の項目はほぼ同等だった。
これは scheduler の集計であり、全カテゴリの overall score ではない。
生データとハッシュは `build/scheduler-pinned/results.json` に保存した。

元の全体 sb3 を変更せずに実行した比較でも、時間に依存しない39項目の
checksum がすべて一致した（`build/scheduler-full/results.json`）。
この1回の比較でも threads は2.66倍、waituntil は1.87倍で、
上記の scheduler だけを呼び出す測定と同じ改善傾向だった。

## 変更内容

### 実行中リストの検索を通常経路で O(1) にする

従来の `Runtime::isActiveThread` は `threads` を先頭から検索していた。
JIT は fiber を再開するたびにこの確認を行い、逐次経路ではその後にも
同じ確認を行う。N 本の短いスクリプトでは、1 パスの検索量が O(N²) になる。

`Thread::runtimeIndex` に位置を保存し、`threads[index] == thread` を確認して
一致すれば検索を省く。push・restart・完了スレッドの圧縮・killed の除去で
位置を更新する。不一致なら従来の検索に戻るため、公開されている `threads`
配列の並べ替えや削除にも対応する。キャッシュは正しさの前提ではない。

単なる配列内の存在確認は `Runtime::containsThread` として分離した。
JIT/interpreter の broadcast-and-wait と backdrop-and-wait にも使用する。
**DONE でも配列から除去されるまでは存在する**という従来の判定を保ち、
`isActiveThread` の status/stack 判定で代用しない。

### broadcast 受信スクリプトにも直接 fiber 切替を使う

既存の共通 writer バッチは、全スクリプトが同じ変数・リストに依存する場合に
順序を保って直列実行し、条件が合えば fiber から次の fiber へ直接切り替える。
従来は broadcast receiver を一律に除外していた。

新しい条件はハットの種類ではなく、**現在の配列内に同一 Thread が存在し、
実行可能な状態か**で判定する。このバッチに入るのは workerSafe のスクリプト
だけで、spawn/restart/stop、互換実行、描画・共有 GPU 呼び出しは対象外。
逐次スクリプトの直前にはバッチを実行し終えるので、受信者の再起動は
直接切替の途中に入らない。

各 JitCtx の target・warp clock/deadline を通常経路と同様に更新し、
切替後も完了・停止・membership を確認する。yield の回数、実行順、
フレーム時間予算は変えていない。ASan の通常 resume へのフォールバックと
profiler 有効時の chain 無効化も維持する。

## 再現手順

変更前後で同じ構成の Release バイナリを作り、変更前のバイナリを保存する。
計測ハーネスはカーネル本体を変えず、起動チェーンから scheduler 以外の
suite 呼び出しだけを除いた一時 sb3 を作る。

```sh
python3 tests/bench/algos/bench_scheduler.py \
  --before build/scheduler-baseline-xyo --after build/scheduler-round2-baseline-xyo \
  --project performance.sb3 --runs 7 --cpu 8 --out build/scheduler-pinned
```

- 変更前→変更後、変更後→変更前を交互に実行する。
- 両方とも HotSpot、30 fps。danger は使わず、並列実行の設定も変更しない。
- `--jit eager` / `--jit off` で実行モードを選べる。
- `--full` は元の sb3 をそのまま実行し、scheduler 以外の checksum も比較する。
- 結果には生の stdout/stderr、各項目の中央値、sb3 とバイナリの SHA-256 を残す。
- scheduler の checksum は時間依存なので一致を要求しない。
- scheduler 全8項目の比率は等重みの幾何平均で集計する。

## 回帰テスト

`tests/unit/test_scheduler.cpp` に以下を追加した。

- RUNNING / DONE / empty stack と、配列内の存在との区別。
- 公開配列の並べ替え・削除・再挿入後の位置キャッシュの検証。
- 実行中スクリプトの restart による新旧 identity の区別。
- stopAll の旧配列 detach と、新しい配列への spawn。
- 異なる寿命の broadcast receiver が共有リストへ追記する順序。
- 中断中 receiver の再起動後に旧 fiber が再実行されないこと。

既存の self-broadcast、共有変数の順序、GC、並列計算と描画の境界テストも
そのまま実行する。

実行結果:

- ctest: **12/12 通過**。
- `XYO_PAR=0`、`XYO_FIBER_CHAIN=0` の各設定でもユニットテスト通過。
- execute フィクスチャ: **JIT 116/116（assert 338）、interpreter 116/116（assert 339）**。
- 元の全体ベンチ: scheduler 以外の **39/39 checksum 一致**。

## 第2段階: スレッド生成・回収と wait-until

第1段階の改善済みバイナリを保存し、追加の変更だけを比較した。

### 元の全体ベンチでの計測結果

元の sb3 を一切変更せず、全カテゴリを通常の順序で実行した。
CPU 8固定、Release / HotSpot / 30 fps、変更前後を各3回交互に測定した中央値。
**scheduler 8項目の等重み幾何平均は、前回の改善済み版からさらに+31.1%**。

| 項目 | 前回版 ops/ms | 今回版 ops/ms | 追加の改善倍率 |
|---|---:|---:|---:|
| sched_looptick | 11,137.747 | 11,391.245 | 1.02× |
| sched_pingpong | 550.694 | 627.072 | 1.14× |
| sched_restart | 1,676.663 | 1,700.161 | 1.01× |
| sched_clonechurn | 640.541 | 828.856 | 1.29× |
| sched_threads | 29,976.456 | 29,462.600 | 0.98× |
| **sched_fanout** | **941.261** | **1,651.928** | **1.76×** |
| **sched_fanwait** | **912.656** | **1,727.422** | **1.89×** |
| **sched_waituntil** | **178.623** | **311.546** | **1.74×** |

時間に依存しない39項目の checksum は、全6実行で一致した。
測定結果: [scheduler-round2-full-paired/results.json](../build/scheduler-round2-full-paired/results.json)。

```sh
python3 tests/bench/algos/bench_scheduler.py \
  --before build/scheduler-round2-baseline-xyo --after build/fast-release/xyo \
  --project performance.sb3 --runs 3 --full --cpu 8 --out build/scheduler-round2-full-paired
```

### scheduler 単独の計測結果

第1段階と同じ条件（CPU 8固定、Release / HotSpot / 30 fps）で各7回、
実行順を交互にした中央値。起動時の他カテゴリ呼び出しだけを省き、
scheduler のカーネル本体は変更していない。

| 項目 | 前回版 ops/ms | 今回版 ops/ms | 追加の改善倍率 |
|---|---:|---:|---:|
| sched_looptick | 11,270.551 | 11,438.859 | 1.01× |
| sched_pingpong | 503.070 | 630.856 | 1.25× |
| sched_restart | 1,687.633 | 1,677.843 | 0.99× |
| sched_clonechurn | 576.413 | 832.426 | 1.44× |
| sched_threads | 29,860.920 | 30,159.550 | 1.01× |
| **sched_fanout** | **860.510** | **1,617.990** | **1.88×** |
| **sched_fanwait** | **659.975** | **1,701.279** | **2.58×** |
| **sched_waituntil** | **166.854** | **326.407** | **1.96×** |

scheduler 8項目の等重み幾何平均は、**前回の改善済み版からさらに+43.0%**。
第1段階で改善した `sched_threads` も維持している。
全体実行と単独実行では前段の処理やメモリアロケータの状態が異なるため、
倍率は一致しない。通常の全体実行に対する効果は上の **+31.1%** を参照。
測定結果: [scheduler-round2-pinned/results.json](../build/scheduler-round2-pinned/results.json)。

```sh
python3 tests/bench/algos/bench_scheduler.py \
  --before build/scheduler-round2-baseline-xyo --after build/fast-release/xyo \
  --project performance.sb3 --runs 7 --cpu 8 --out build/scheduler-round2-pinned
```

### 変更内容

1. **空のスレッドでも発生するメモリ確保を削減**。
   `stackFrames` と `trashFrames` は末尾操作と添字アクセスしか使わないため、
   `deque<unique_ptr<StackFrame>>` を `vector<unique_ptr<StackFrame>>` に変更。
   空 deque の内部管理領域・初期ブロックの確保がなくなる。
   StackFrame 本体は引き続き個別所有するので、vector の再確保や
   stack→trash の移動でも、実行中のフレーム・ExecContext への参照は変わらない。
2. **スレッドIDの不要なコピーを削減**。
   `getId()` は既にキャッシュしている文字列の const 参照を返す。
   threadMap への登録・除去・再構築で一時文字列を確保しなくなる。
3. **完了スレッドの所有領域を圧縮**。
   従来は回収後も null 要素を残し、次の回収しきい値や GC の走査範囲が
   累計生成数に比例して増えていた。未参照の完了スレッドと空要素を除き、
   生存数から次のしきい値を決める。join、待機キュー、旧実行配列、
   実行中スレッドなどが保持する参照は、推移的な参照も含めて保護する。
   移動するのは所有ポインタだけで、Thread の住所・identity は変えない。
   GC の所有領域走査にも null チェックを追加した。
4. **wait-until の過剰な逐次指定を解除**。
   `control_wait_until` は `control_wait` と異なり、共有 waitQueue や
   redrawRequested を変更しない。JIT は条件評価と自分の fiber の yield だけを行う。
   条件式の依存関係は引き続き解析し、IO の更新・未知の拡張などが含まれれば
   従来どおり逐次実行する。副作用のない条件ならバッチ化・直接fiber切替が可能になる。
   待機を省略したり、条件を評価する回数を減らしたりはしていない。

### 追加テスト

- 2,048個の完了スレッドを回収し、各種参照で保護された12個だけが残ること。
- join の推移的参照、JIT/compat/trash の参照、待機キュー、旧実行配列を保護すること。
- 保護参照を外すと対象も回収され、GC が空要素を安全に扱うこと。
- 512階層のフレーム成長と stack→trash 移動でも参照先が変わらないこと。
- 複数の wait-until 受信者と gate 更新スクリプトが、共有リストへ元の順序で追記すること。
- wait-until の条件と後続処理の依存関係を保持し、副作用のある条件や timed wait は逐次に残ること。

### 第2段階の検証結果

- ctest: **12/12 通過**。
- `XYO_PAR=0`、`XYO_FIBER_CHAIN=0` の各設定でもユニットテスト通過。
- execute フィクスチャ: **JIT 116/116（assert 338）、interpreter 116/116（assert 339）**。
- 全体ベンチ各3回: scheduler 以外の **39/39 checksum 一致**。
- `git diff --check` 通過。
