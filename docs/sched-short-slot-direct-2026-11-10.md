# SCHED-003: 短い受信者の変数 slot 初期化

## 目的

前段の未実行 broadcast の省略とは別に、毎回実行される fanout/fanwait の短命受信者を軽くする。

## 原因

生成コードは `(id, exact UTF-16 name, list-kind)` ごとに初期化を1回だけ生成し、その後は `varSlotData` を直接読む。しかし従来の `xyo_varslot_init` は `xyo_jitVar` を経由して、slot配列とは別の `Thread::jitVarCache` にハッシュノードを確保する。短い受信者ではそのキャッシュを一度も再使用せず、完了処理で解放していた。

さらに `lookupOrCreateVariable/List` を直接呼ぶと、IDだけで見つかる場合にも UTF-16 の名前を一時 `std::u16string` に変換する。

## 実装

[src/jit/rt.cpp](../src/jit/rt.cpp) の `xyo_varslot_init` に `XYO_JIT_SHORT_SLOT_DIRECT` 切替を追加。

- native entry があり、`shortSync && !mayYield` のときだけ対象。
- `XYO_RT_GUARD_NATIVE_VAR_SLOTS` の per-use 世代検証経路は従来どおり。
- helperキャッシュが空のとき、現在の Target に対して既存 ID を直接解決し slot へ格納する。クローンごと・実行ごとに解決し直す。
- ID が無い場合は、従来と同じ name fallback／作成と exact pointer-key のキャッシュ登録を行う。以降はキャッシュが非空になるため、他の slot も従来経路へ戻る。
- 既存キャッシュを直接lookupで上書きしない。存在しない同一IDを異なる名前で使う場合、後の作成で先行 name binding が変わらないようにする。
- scalar/list のID衝突で型をフィルタしないという既存意味論も維持する。
- Variable*／コードの新たな永続キャッシュ、ABI変更、GC root の削除、完了後の束縛再利用は導入しない。

## プロファイル（採否用のスループット値ではない）

計測専用 Release ビルド（`XYO_DISABLE_RT_PROF=OFF`）、HotSpot、30fps、CPU8に固定。最初の prototype では var ヘルパの平均コストが fanout **125→55ns/call**、fanwait **120→41ns/call** に減った。これは計測オーバーヘッドを含む単発の内訳であり、ScratchBench の速度改善倍率ではない。

[fanout OFF](../build-sched-light/fan-profile/sched_fanout-profile-0.err) / [fanout ON](../build-sched-light/fan-profile/sched_fanout-profile-1.err) / [fanwait OFF](../build-sched-light/fan-profile/sched_fanwait-profile-0.err) / [fanwait ON](../build-sched-light/fan-profile/sched_fanwait-profile-1.err)。この計測後に、欠落IDのaliasを保持する安全側の条件へ絞っている。

## テストと性能評価

### 通常 Release の分離測定

RT profiler無効、HotSpot、30fps、CPU8固定、同一バイナリで各16回交互に測定した。元のカーネル本体は変更せず、起動チェーンから他の scheduler 呼出しを除いただけの入力を使用。

| 項目 | OFF中央値 ops/ms | ON中央値 ops/ms | ペア速度比の幾何平均 | ON勝利数 |
|---|---:|---:|---:|---:|
| sched_fanout | 1,768.63 | **1,996.06** | **1.12397倍** | **16/16** |
| sched_fanwait | 1,764.47 | **2,060.15** | **1.16163倍** | **16/16** |

[16ペアの生データ](../build-sched-light/slot-direct-pilot/results.json)。実行ファイル SHA256 `712dc683dc69e9bef122d23fb45e841ab06fe61bf93f54fcf6e0d8223b2f8763`。この結果から狭い直接ID束縛を**既定ON**に変更した。`XYO_JIT_SHORT_SLOT_DIRECT=0` で従来経路へ戻せる。

### 既定ON版での再確認

既定値を変更した最終バイナリ（SHA256 `ec7251046f97b5f16007101c90fdedc618dd712a15b1416eef35dc2f68441410`）でも同じ条件で各8ペアを追加測定した。

| 項目 | OFF中央値 ops/ms | ON中央値 ops/ms | ペア速度比の幾何平均 | ON勝利数 |
|---|---:|---:|---:|---:|
| sched_fanout | 1,835.30 | **2,066.92** | **1.13868倍** | **8/8** |
| sched_fanwait | 1,812.73 | **2,124.73** | **1.18205倍** | **8/8** |

[最終8ペアの生データ](../build-sched-light/slot-direct-final-targeted/results.json)。最初の16ペアとは別実行で、倍率を掛け合わせない。両実行を通じて各項目24/24ペアでONが速かった。

### 回帰テスト

同一の最終ビルドからscheduler単体をリンクして確認:

- 通常の既定値、直接束縛OFF: 各 **361 checks / 0 failures**。
- `XYO_RT_GUARD_NATIVE_VAR_SLOTS=1`、さらに `FAST=1` の各モード: 各 **343 checks / 0 failures**。直接束縛のキャッシュ省略はこのモードでは使わない。unguarded cache alias 専用テストと並列ディスパッチ専用assertionのスキップは想定どおり。
- `XYO_PAR=0 XYO_FIBER_CHAIN=0`: **353 checks / 0 failures**。並列専用assertionのみスキップ。

- 完全な `xyo-tests`: **PASS 1/1、44.77秒**。[ログ](../build-sched-slot-direct-final-ctest.log)。全登録CTestを実行したという意味ではない。
- 元の全47項目のベンチをON/OFF各3回実行し、非scheduler **39/39 checksum が全6試行で一致**した。[全体ベンチの生データ](../build-sched-light/slot-direct-full/results.json)。
- `git diff --check`、測定Pythonの構文チェック: PASS。
- GUI parity / ASan / 独立したJIT無効ビルドは今回再実行していない。

### 元の全体ベンチの scheduler 結果

各3回の比較で fanout は **1,670.02→1,924.03 ops/ms**、fanwait は **1,683.67→1,923.52 ops/ms**。両項目とも3/3ペアでONが速かった。pingpong もペア幾何平均1.0783倍（3/3勝）だったが、主な効果の根拠は上記の分離16＋8ペアとする。全47項目のoverall score向上や、scheduler全項目の改善を主張しない。

全体3ペアで `sched_restart` は1.882k→1.802kと低下したため、追加の分離比較で確認した。CPU8の16ペアは中央値 **1,820.49→1,818.64** とほぼ同じだが、ON側に極端に遅い試行が3個ありペア幾何平均0.9061倍だった。[CPU8の生データ](../build-sched-light/slot-direct-restart-control/results.json)。別のP-core CPU14固定の16ペアでは **1,876.48→1,869.03**、ペア幾何平均 **0.99716倍**、7/16勝で、同程度の低下は再現しなかった。[CPU14の生データ](../build-sched-light/slot-direct-restart-cpu14/results.json)。restartに対する速度改善は無く、環境依存の外れ値があることは残して記録する。全項目無劣化の保証ではない。

### 再現用ハーネス

[bench_scheduler_flags.py](../tests/bench/algos/bench_scheduler_flags.py) に `--case` と `--full` を追加した。`--case` はsuiteの呼出しだけを選別し、`--full` は元の全47項目を実行して非scheduler39項目のchecksumを全ON/OFF試行で比較する。共有キャッシュが他の作業で再生成されても入力が変わらないように、元のsb3自体も出力先へ固定する。各試行のstdout/stderr、バイナリと入力のSHA256、実行順、ペア速度比を保存する。

```sh
python3 tests/bench/algos/bench_scheduler_flags.py \
  --binary build-sched-light/xyo-slot-direct-final \
  --project scratch-bench/.cache/build/performance.sb3 \
  --out build-sched-light/slot-direct-repeat \
  --flag XYO_JIT_SHORT_SLOT_DIRECT \
  --case sched_fanout --case sched_fanwait --runs 16 --cpu 8

python3 tests/bench/algos/bench_scheduler_flags.py \
  --binary build-sched-light/xyo-slot-direct-final \
  --project scratch-bench/.cache/build/performance.sb3 \
  --out build-sched-light/slot-direct-full-repeat \
  --flag XYO_JIT_SHORT_SLOT_DIRECT --full --runs 3 --cpu 8
```

追加の回帰テストは [test_scheduler.cpp](../tests/unit/test_scheduler.cpp)。ID/name alias の決定的な初期化順、既存Stage ID、scalar/list衝突、クローンごとの独立した scalar/list、実行間の変数削除と再作成、local-name／Stage-ID の優先順、GC後の再束縛を対象にする。
