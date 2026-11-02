# 文字列連結の実行経路プロファイル：xyo-cpp と TurboWarp

## 結論

今回の調査ではランタイムの最適化を追加していない。既存の文字列ベンチに対し、生成コード、動的カウンタ、CPUサンプルを別々に取得した。

**通常の連結の残差を「汎用ヘルパ境界だけ」で説明するのは不十分だった。** xyoの長時間joinでは、確保処理とGCのsweepに多くのnative self samplesが集まった。一方、型判定・関数呼び出し境界も実JITに残っている。次の実験は、この両方を分けて評価する必要がある。

TurboWarpの先頭参照と末尾参照には、どちらも`Runtime_StringCharCodeAt`への動的到達が確認できた。同じNodeリリースの実装はその中で`String::Flatten`を呼ぶ。したがって「末尾なら必ず右leafへ直接到達して終わる」というモデルは、この生成コードの説明として不十分。ただし**実際のflattenによるコピー回数は未計測**である。

集計・入力ハッシュ・バイナリハッシュは[保存したプロファイル結果](<../tests/bench/algos/results-string-profiles.json>)にある。従来の速度比較は[文字列性能の記録](<string-performance.md>)を参照。ここにある診断実行の時間で、その比較結果を置き換えない。

## 条件と証拠の範囲

- Node **v26.4.0** / V8 **14.6.202.34-node.21**、参照VMのpackage versionは**2.1.46**。compiler有効。
- xyoは既存のビルド済みバイナリを使用。SHA-256は集計に保存。hotspot JIT、danger off、GC有効。
- scale80の`join`・`join_observe`・`join_observe_last`は、両ランタイムで**同じSB3のSHA-256**を照合した。
- 長時間サンプリングには、同じ生成器で作った**scale8000のjoin**を両者で使用した。入力ハッシュも一致。1セットの文字列長は従来どおり40,000で、セット数だけを増やす。
- すべてchecksumを検証した。長時間joinは両者とも`320000000`。xyoでは最終本文も`"ab" × 20000`と一致。
- 診断は基本的に**プロセス全体**を対象とし、ロード、コンパイル、ウォームアップ、計測3回、最終検証を含む。タイムスタンプ・サンプルの種類が違うため、両者の割合を直接比較しない。
- 実装確認、静的call site、動的到達回数、CPU self samples、未検証の原因推測を区別する。

## 1. xyo：生成コードとランタイム境界

AOTによる代用ではなく、`XYO_JIT_DUMP2`で**LLVM O2後の実JIT IR**を、`XYO_JIT_DUMP_OBJECT`で**同じORCコンパイルのリンク前オブジェクト**を取得した。hotspot・danger offでも確認済み。リンク前オブジェクトのアドレスは最終実行アドレスではなく、外部関数はrelocationで確認した。

通常の長文連結には次の呼び出し境界が残る。

```text
JIT loop
  → xyo_join
      → rt::join
          → sstrCons
              → Gc::get
              → Gc::alloc
                  → Gc::allocSmallTls
```

確認できたこと：

- JITのループ内には`xyo_join`へのcallが残る。確保処理はIRへインライン展開されていない。
- `xyo_join`は単なるtrampolineではない。**プロファイル無効時も**`RTProf::get`、初期化guard、enabled検査が残る。有効時のtimer・name-map更新は別経路。
- `rt::join`には文字列タグ、空文字列、長さ合計、16コードユニット閾値の判定が残る。
- cons生成はSStrの各フィールドを初期化する。**SStr=56B、GC header=16B、要求72B、スラブセル80B**をヘッダのsizeofとnative命令の両方で確認した。
- 確保の通常経路は、TLS owner/世代検査、スラブ取得、bitmapの空き探索、bit設定、cursor/live更新、ヘッダ初期化、確保量のTLS加算を行う。**単一のbump-pointer更新ではない。** 通常経路にmutexはなく、スラブ補充経路にはある。
- JIT callerの非yield・継続経路に限る静的集計は、joinが11命令・1 call・2条件分岐、先頭参照付きが15命令・2 call・2条件分岐。**calleeを除く**ため、これを総命令数やサイクル数として扱わない。

詳細なIR・逆アセンブルと調査メモは[実JIT調査](<../build/string-profile-xyo-code/REPORT.md>)、[joinの実JIT逆アセンブル](<../build/string-profile-xyo-code/join-hotspot-jit-0.asm>)に保存している。

## 2. xyo：確保・GC・flattenの動的カウンタ

scale80の1プロセスは、ウォームアップ1セットと計測3回×80セットを実行し、最後に検証する。以下は**そのプロセス全体**のカウンタで、1回の計測区間だけではない。

| ワークロード | cons生成 | cons本文の実体化 | GC回数 | GCログの累計ms |
|---|---:|---:|---:|---:|
| join | 4,818,314 | 2 | 48 | 34.2 |
| join_observe | 480,314 | 14,944 | 4 | 4.1 |
| join_observe_last | 480,314 | 2 | 4 | 3.3 |

`XYO_STR_DEBUG`のflattenカウンタは`consBody`が未実体化本文の生成を始める箇所で増える。この単一スクリプトの実行では競合はないが、一般に「全文字列API呼出し数」ではない。

- joinと末尾参照の**全体で2回**は、少なくとも「毎連結で全文実体化」のモデルを否定する。
- 先頭参照は依然として実体化を繰り返す。最大32段の探索後にルートを実体化する現在の方針に整合する。
- `XYO_PROF`で確認したhelper回数は、joinの`xyo_join`が4,820,001回、先頭参照の`xyo_letterof`が482,000回、末尾参照の同helperが480,313回。末尾の短いflat文字列ではJIT内fast pathが使われるため、ソース上の参照数とhelper回数は一致しない。

### 計時プロファイラの摂動

| ワークロード | 通常実行の中央値ms | カウンタ/GCログ有効 | RTProfも有効 |
|---|---:|---:|---:|
| join | 29 | 33 | 88 |
| join_observe | 8 | 9 | 19 |
| join_observe_last | 5 | 5 | 14 |

これは各1プロセスの診断対照であり、新しい反復ベンチ表ではない。RTProfはtimer、atomic、関数名mapのmutexを追加する。**この実行のinclusive時間を通常版の時間から引いたり、GCとの和を取ったりしてはいけない。** 既存の「GC閾値を変えても速くならなかった」という実験だけで、sweepの仕事量が小さいとも結論できない。

生ログと設定は[カウンタ集計](<../build/string-profile-xyo/summary.json>)にある。

## 3. xyo：長時間joinのnative self sampling

perf/valgrindは未導入だったため、診断専用の[小さなSIGPROFサンプラ](<../tests/bench/algos/sample_native.cpp>)を別の共有ライブラリとして使用した。**xyoのソースやビルドフラグは変更していない。** 実スタックからPCのみを取り、call stackは取得しない。

Linux x86_64限定。ITIMER_PROFの要求間隔は1ms。handlerは固定容量のlock-free atomic histogramに記録し、割当・I/O・mutexを使わない。終了後にmapsとELFのPT_LOAD、`nm`のサイズ付きsymbolから[オフライン解決](<../tests/bench/algos/profile_native_strings.py>)した。無害なsleep/CPU loopで事前検証してからxyoを実行した。

**main-thread self samples 7,995、他スレッドへの配信0、histogram overflow 0。** 次の割合は全main samplesに対するもので、inclusive時間でも改善可能率でもない。

| native関数 | self samples | 割合 |
|---|---:|---:|
| `Gc::sweepSmallSlabs` | 1,702 | **21.29%** |
| `Gc::alloc` | 1,521 | **19.02%** |
| `Gc::allocSmallTls` | 1,094 | **13.68%** |
| `rt::join` | 611 | 7.64% |
| `sstrCons` | 474 | 5.93% |
| `xyo_join` | 394 | 4.93% |
| `Gc::validHeader` | 329 | 4.12% |
| `RTProf::get` | 120 | 1.50% |

**確保の2関数で約32.7%、sweepだけで約21.3%。** 呼び出し境界の調査を進める根拠はあるが、GC回収側も無視できない。`rt::join`や`sstrCons`のself samplesは型判定・初期化なども含み、純粋なcall命令のコストではない。

制約：

- サンプルはプロセス全体。長時間化でロード等の比率を下げたが、計測区間だけにゲートしてはいない。
- process CPU timerの配信・main filtering・周期性による偏り、signal自体の摂動があり得る。
- 終了時mapsにない302サンプルは未解決。**JIT由来と決めつけていない。** ほかにもlibcなどのsymbol範囲外がある。
- libcのread/lseekにもサンプルがあるが、stackがないため呼出し元は未確定。GCの`pthread_getattr_np`との関係などは今後の検証対象。
- `LD_PRELOAD`は**最終のxyo実行ファイルにだけ設定**する。taskset等のwrapperへ先に設定すると、execでhandlerがリセットされ、timerだけ残って異常終了する。初回のこの失敗を切り分け、以下の正しい順序で正常終了を確認した。

[解決済みプロファイル](<../build/string-profile-native/join-scale8000.resolved.json>)、[生サンプル](<../build/string-profile-native/join-scale8000.samples>)、[検証結果](<../build/string-profile-native/verification.json>)。

## 4. TurboWarp / V8：生成JSと実際のfallback

取得した生成JSの手続き名は`fun0_bench_`。それをフィルタに指定して最適化コードを採取した。

```js
// join
b1.value = b1.value + "ab";
// join_observe: 連結直後
b0.value = b1.value[1 - 1] || "";
// join_observe_last: 連結直後
b0.value = b1.value[b1.value.length - 1] || "";
```

確認事項：

- 3ケースすべての最適化コードに`StringAdd_CheckNone` builtinへのcallがある。**各callで必ずconsを確保する**とまでは断定しない。
- 文字参照にはinlineの1-byte/2-byte load経路とruntime fallbackがある。
- fallbackのnativeアドレスを、実際に使用したNode実行ファイルのsymbolで`Runtime_StringCharCodeAt`と同定した。
- [一致するNode v26.4.0の公式ソース](https://raw.githubusercontent.com/nodejs/node/v26.4.0/deps/v8/src/runtime/runtime-strings.cc)では、この関数は`subject = String::Flatten(isolate, subject)`の後に文字を読む。これは**fallbackの実装確認**であり、静的call siteだけなら実到達の証明ではない。
- 最適化ログにはMaglev/TurboFanの昇格とOSR関連のdeoptがある。OSRを抜けるログを、すべて定常実行の型不一致と解釈しない。

[生成JS](<../build/string-profile-v8/join_observe/trace/generated_1.js>)、[V8最適化コード](<../build/string-profile-v8/join_observe/opt.log>)、[詳細報告](<../build/string-profile-v8/REPORT.md>)。

### 動的到達をGDBで確認

scale1の小さい別入力を生成し、`Runtime_StringCharCodeAt`でsilent breakpointを数えた。ロード完了後・greenFlag直前にSIGUSR2を送り、GDB側でカウンタをリセットした。

| 入力 | リセット前の到達数 | リセット後の到達数 | checksum |
|---|---:|---:|---:|
| join対照 | 1,682 | 50 | 40,000 |
| join_observe | 1,682 | **8,025** | 4,000 |
| join_observe_last | 1,682 | **8,025** | 4,000 |

観測ケースには8,000回の明示的な文字参照がある。リセット後もコンパイル、warmup、計測3回、verify、終了を含むので、余分な25回をベンチへ帰属していない。対照を差し引いても7,975で、正確なベンチ単独カウンタではない。

それでも、**先頭・末尾ともにruntime fallbackが動的に使われている**こと、同じ設定で到達数が等しいことは確認できた。先頭/末尾の速度が近かったことに対し、単なるropeの左右探索だけでなく、このfallbackは根拠のある説明候補になる。

**到達数＝実コピー数ではない。** `String::Flatten`が既にflatな文字列へ何もしない場合などをまだ区別していない。GDB下の時間は性能比較に使わない。[範囲と再現手順](<../build/string-profile-v8/gdb/RESET-RESULTS.md>)。

## 5. V8のCPU・GCとオブジェクトサイズ

### scale80のtrace：プロセス全体

| 入力 | GCイベント数 | 種類 | GCログの先頭pause値の合計ms |
|---|---:|---|---:|
| join | 26 | Scavenge 25 + Incremental Mark-Compact 1 | 9.62 |
| join_observe | 126 | Scavenge 125 + Incremental Mark-Compact 1 | 13.12 |
| join_observe_last | 126 | Scavenge 125 + Incremental Mark-Compact 1 | 13.42 |

起動とロードを含み、ログの`X / Y ms`のXの合計である。concurrent/incrementalの追加仕事を含む完全なGC CPU時間ではない。xyoのGCログ時間とは定義も実装も違う。

scale80のJS CPU profileは144〜229サンプルと少なかったため、scale8000の**同一join SB3**でも取得した。全1,778サンプルのうち、`fun0_bench_`が925、`(garbage collector)`が448、`gen0`が280。GC traceは660 Scavenge + 1 major、先頭pause値の合計118.0ms。

V8のJSプロファイラはnative builtinの仕事をcaller側へ帰属することもあり、これをxyoのnative PC別self割合と直接比較しない。ネイティブ名のStringAdd/Flattenがprofileにないことを、呼出し0回の証拠にもしない。長時間版もchecksumは一致。[V8長時間集計](<../build/string-profile-v8-long/summary.json>)。

### 独立対照によるサイズ確認

同Nodeはpointer compression無効。独立JSで保持したconsをheap snapshotへ出すと、`concatenated string` 1,991ノードすべてが**self_size 32B**だった。[集計](<../build/string-profile-v8/heap-layout-summary.json>)。

xyoのpayload 56B＋header 16B（要求72B、cell80B）に比べ、V8側には小さい表現を使える余地がある。ただし**V8 snapshotのobject self_sizeとxyoのslab占有量は異なる指標**であり、32対80をそのまま速度差や総メモリ差に換算しない。この独立対照はTurboWarp本体の割当数も測っていない。

## 6. 次に分離して試すもの

| 優先 | 実験 | 今回の根拠 | 未確定な点 |
|---|---|---|---|
| 1 | cons確保・回収を軽くする案を個別A/B | native samplesがalloc/TLS/sweepへ集中 | セル小型化・bitmap更新・sweep方式のどれが最も効くか |
| 2 | 文字列専用join経路とdisabled profiler境界の削減 | JITとwrapperにcall・guard・タグ判定が残る | 安全に何個の判定・呼出しを除去できるか、SB3全体の効果 |
| 3 | GC内のstack範囲取得等を分離計測 | libc read/lseekサンプルとcollect内の範囲取得 | そのlibcサンプルの正確な呼出し元 |
| 4 | V8のmaterialize回数・コピー量の直接計測 | char runtimeに動的到達、公式実装にFlatten | Flat判定no-opと実コピーの内訳 |

今回はdouble cacheやDragonbox、containsの段階別寄与を測っていない。今回の3ケースから、それらの優先度や効果を断定しない。

## 再現

既存scale80入力に対するxyoカウンタとV8の生成コード・trace・CPU profile：

```sh
python3 tests/bench/algos/profile_strings.py \
  --out build/profile-xyo-new --cpu 12 --scale 80
node tests/bench/algos/profile_tw_strings.js build/profile-v8-new
```

両スクリプトは新規の出力先を指定する。xyo側は入力のproject.jsonを生成器の期待内容と照合し、V8側は全診断実行のchecksumと入力ハッシュを保存する。

長時間SB3の生成とnative sampling（Linux x86_64、workspace root）：

```sh
python3 - <<'PY'
import sys
sys.path.insert(0, 'tests/bench/algos')
from bench_strings import generate
from gen_algo_bench import write_sb3
project, expected = generate('join', 8000)
write_sb3('build/profile-long-new/join.sb3', project)
print(expected)
PY
c++ -O2 -std=c++20 -shared -fPIC tests/bench/algos/sample_native.cpp \
  -o build/profile-long-new/sampler.so
# Preload is deliberately applied AFTER taskset.
taskset -c 12 env -i PATH="$PATH" HOME="$HOME" \
  LD_PRELOAD="$PWD/build/profile-long-new/sampler.so" \
  XYO_NATIVE_SAMPLE_OUT="$PWD/build/profile-long-new/join" XYO_DUMPVARS=json \
  build/xyo run build/profile-long-new/join.sb3 --jit hotspot \
  > build/profile-long-new/join.stdout 2> build/profile-long-new/join.stderr
python3 tests/bench/algos/profile_native_strings.py build/profile-long-new/join
# Separate process, same project; CPU profiling is not a new speed benchmark.
taskset -c 12 node --cpu-prof --cpu-prof-dir=build/profile-long-new \
  --cpu-prof-name=v8.cpuprofile --trace-gc tests/bench/algos/run_tw.js \
  build/profile-long-new/join.sb3 compiled > build/profile-long-new/v8.log
```

サンプラはSIGPROF/ITIMER_PROFが既に使われている場合には無効化する。execするアプリケーション、他のサンプラとの併用、非Linux/non-x86_64は対象外。生のIR・asm・cpuprofile・GDBログ・heap snapshotはサイズが大きいためbuild配下に保持し、主要な数値・入力同一性・条件は上記の追跡可能なJSONへ保存した。
