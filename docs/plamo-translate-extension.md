# Scratch翻訳拡張 — ローカルPLaMo 3 Translate

xyo-cppの翻訳拡張は、**ユーザーが自分でダウンロードしたPLaMo 3 Translate**を使います。アプリケーション、ビルド成果物、リポジトリ、SB3にはモデルの重みを同梱しません。初回起動時の無断ダウンロードも行いません。

## 対応ブロック

- `translate_getTranslate`: 「(WORDS) を (LANGUAGE) に翻訳する」。ローカルサーバーのOpenAI互換Chat Completions APIで翻訳し、非同期に結果を報告します。
- `translate_getViewerLanguage`: 閲覧者の言語。現時点では実行プロセスの`LC_ALL`/`LANG`から推定（英語、日本語、スペイン語、フランス語、ドイツ語）。
- 翻訳先には日本語・英語などの言語名、`ja`/`en`などの2文字コードを指定できます。コードはPLaMoの対応言語に限って使用してください。

未設定時や、ローカルサーバーへの接続が失敗した場合は**原文をそのまま返します**。数字のみの入力は翻訳要求を送らずそのまま返します。成功した直近の翻訳結果は再利用します。同時翻訳要求数は最大4件、1入力は最大16KiBです。

## ユーザーによるセットアップ

PLaMo 3 Translateの配布条件を確認してから、ユーザー自身がモデルを取得してください。

- [PLaMo 3 Translate 2B GGUF / Hugging Face](https://huggingface.co/pfnet/plamo-3-translate-2b-GGUF)
- [PLaMo Community License（日本語）](https://plamo.preferredai.jp/info/plamo-community-license-ja)

例（推奨: GGUF Q4_K_M、約1.81GB）:

```bash
python -m pip install -U huggingface_hub
hf download pfnet/plamo-3-translate-2b-GGUF \
  plamo-3-translate-2b-Q4_K_M.gguf --local-dir ./models/plamo3
```

llama.cppの`llama-server`を別プロセスで起動:

```bash
llama-server -m ./models/plamo3/plamo-3-translate-2b-Q4_K_M.gguf \
  -c 16384 --port 8000 --host 127.0.0.1 \
  --alias plamo-3-translate --jinja --no-prefill-assistant
```

xyo-cppの起動時に明示的に有効化:

```bash
XYO_PLAMO_TRANSLATE=1 ./build/xyo play project.sb3
```

ポートを変える場合は`XYO_PLAMO_PORT=8001`等を設定します。HTTP通信先は**127.0.0.1のみ**で、プロキシは使用しません。別マシンのPLaMoサーバーに自動接続したり、翻訳する文字列を外部クラウドへ送る処理はありません。

## ライセンス

モデル重みは**PLaMo Community License**であり、MITなどの一般的なオープンソースライセンスではありません。個人・非商用利用は無償で、商用利用には事業規模に応じた登録や契約条件があります。また成果物の公表・モデルの再配布などにも条件があります。詳細は配布元のライセンス本文を確認してください。xyo-cppはモデルの取得やライセンス同意を代行しません。

HTTPクライアントに任意依存のlibcurlを使います。ビルドで`-DXYO_TRANSLATE_CURL=OFF`とすると通信機能を切り離し、翻訳ブロックは原文を返します。モデルはいつでもユーザー自身が管理・削除できます。

## テスト

`tests/differential/translate_plamo_local.py`は、モデルをダウンロードせず**127.0.0.1に模擬OpenAI APIサーバー**を起動します。Interpreter/eager/hotspot経路について、無効時の通信ゼロ、非同期の翻訳結果、キャッシュ、数字のみの入力、言語指定を確認します。実際のPLaMoの翻訳品質・実推論速度はモデル未ダウンロードのため未検証です。テスト環境によってループバックソケットの許可が必要です。


## 実モデルによる統合確認（2026-11-09）

ローカルで実際に起動されたPLaMo 3 Translate 2B Q4_K_M
(`llama-server --host 127.0.0.1 --port 8000 --alias plamo-3-translate`)で検証済み。
`GET /v1/models`でモデルIDを確認し、OpenAI互換
`POST /v1/chat/completions`に以下を送った：

- System: `[plamo-translate]\ntarget_lang=ja`
- User: `Hello from Scratch`
- 応答: `スクラッチからこんにちは`

**修正点：** C++の`JsonValue`初期化で
`{"role", "user"}`などの文字列リテラルが
boolとして変換され、実サーバーからHTTP 500
(`type must be string, but is boolean`)が発生した。
`std::string(...)`を明示することでHTTP 200へ修正。
モックテストでも`model`と両`role`が文字列であることを検査する。

ヘッドレス`xyo run`では、上限フレームまで高速実行すると
実時間が進まず、推論完了前にフレーム上限へ到達する。
実サーバーでの検証には`--fps 60 --frames 300`を付けた。
GUIは元々実時間進行。改善課題としてヘッドレスの
非同期通信待ち対応を残す。

**実モデル統合テスト：** `tests/differential/plamo_live_integration.py`
（モデルのダウンロードやサーバー起動は行わない）を追加。
モデルID確認、OFF時原文・ON時翻訳、同文再利用、
数字のみ入力、終了状態を`--jit off/eager/hotspot`の
6条件で **6/6 PASS**。ビルドは`XYO_JIT=OFF`のため
`--jit`の指定はJITコード生成の検証にはならない。
実LLVM JIT有効ビルドは別作業中のビルドエラー解消後に要検証。
実行例：

```bash
python3 tests/differential/plamo_live_integration.py \
  board/.translate-implementation/cmake/xyo
```

同ビルドのモックCTest `xyo-plamo-translate-local`も
**1/1 PASS（0.65秒）**。
デバッグ環境変数`XYO_PLAMO_TRACE=1`はHTTPコードと応答サイズのみ表示し、
翻訳文・入力文はログに出さない。


## 2026-11-09：実時間待ちのヘッドレス互換・JITの安全なフォールバック

ヘッドレス`xyo run`は、従来ネットワーク応答を待つ間にも
高速で`--frames`の上限を消費し、
本物のPLaMoが正常に応答しているのに`frame limit reached`となる。
`src/shell/main.cpp::runLoaded()`で、
全生存スクリプトが`STATUS_PROMISE_WAIT`で、
かつ翻訳要求が実行中の場合に限り、
`TranslationService::waitForPending(5ms)`の小刻みな待機と
main-thread側の`Runtime::step()`を行い、
このI/O専用待機期間をフレーム上限へ算入しない。
通常の実行可能なスクリプトや、`--fps`指定時には
従来どおりの実行を維持する。
HTTP接続・合計タイムアウトは既存の700ms/15sのまま。

LLVM JITは、非同期の`translate_getTranslate`を
ネイティブ式内の途中停止としてまだ表現できない。
`src/jit/codegen.cpp::jitEmitScript()`の共有IR入力を
調べ、当該レポータを含むスクリプト（到達可能な
手続き内のブロックを含む）はコンパイルを拒否し、
**Interpreterへ安全にフォールバック**。
翻訳のない他スクリプトはJIT対象のまま。
`XYO_JIT_VERBOSE=1`で
`xyo-jit: async translate reporter -> interpreter fallback`
を実際に確認した。
これは翻訳レポータをLLVMネイティブで実行したという主張ではない。

**検証：** `XYO_JIT=ON`の専用構成
`board/.music-implementation/cmake/xyo`がビルドPASS。
既存のPLaMo 3 Translateモデル（再ダウンロードなし）で、
`--fps`なし、`--frames 30`だけを指定し、
off/eager/hotspot × PLaMo無効/有効の6ケースが **6/6 PASS**。
有効時は「Hello from Scratch」→
「スクラッチからこんにちは」、二度目の翻訳も同値、
数字入力は「12345」のまま、doneは「1」。
`tests/differential/plamo_live_integration.py`から再実行可能。

モックAPIテストでは、各レスポンスに80msの遅延を加え、
30フレーム上限での完了、ポート異常値での原文フォールバック、
HTTP 200なのに不正JSONを返す場合の原文フォールバックを
off/eager/hotspot全モードで検証。
`xyo-plamo-translate-local` **1/1 PASS（1.44秒）**。
JSON parserは不正値をnullで返す実装であり、
xyo-cppの`-fno-exceptions`構成と両立する。
追加のMATH-019 i32選択と音楽のCTestも **3/3 PASS（9.77秒）**。

まだ残る課題は、ネイティブJITの非同期レポータ継続ABI、
外部サーバーの長時間停止時のキャンセル最適化、
スクリプト停止・クローン削除・再起動を交えた並行負荷テスト。
この変更はモデルの同梱・自動ダウンロードを行わず、
通常のモデルライセンスと配布方式を維持する。


## TRANSLATE-001：停止・再起動・クローン停止の通信キャンセル検証

`TranslationService::Pending`に共有`std::atomic<bool>`のキャンセルフラグを追加。
HTTPワーカーはScratchの`Thread*`/`Runtime*`に一切アクセスせず、
libcurlの`CURLOPT_XFERINFOFUNCTION`でキャンセルを確認する。
`Runtime::stopThread()`、`restartThread()`、`stopAll()`、
`~Runtime()`からキャンセル通知を発行する。
クローン削除時は既存の`stopForTarget()`から`stopThread()`経由で通知。
`TranslationService::poll()`はキャンセル済みの完了結果を破棄し、
古いScratchスクリプトに解決値を戻さず、成功キャッシュも更新しない。
キャンセル済みfutureは完了するまで保持し、実行中のScratchスレッドで
破棄・待機しない。HTTP接続/応答制限（700ms / 15秒）は従来通り。

同時実行上限は`pending_.size()`ではなく、
キャンセルされていない要求の数を使う。
キャンセル済みfutureは非ブロッキングで完了を確認できた時点で回収。
これにより4件キャンセル直後の新規翻訳を妨げない。
`hasPending()`と`waitForPending()`もキャンセル済み要求を除外。

### 検証

- `tests/platform/plamo_translation_lifecycle.cpp`: stopThread、restartThread、
  stopForTarget、stopAll、キャンセル済み4件の直後の新規要求の
  5ケースを検証。
- `tests/differential/plamo_lifecycle_delayed.py`: 127.0.0.1の模擬HTTP
  サーバーが120ms遅延応答。**実際に4件のHTTP要求が到達**した後、
  古い結果を別スレッドへ反映しないことを確認。
- 正式CTest `xyo-plamo-translate-local`、
  `xyo-plamo-translation-lifecycle`、
  `xyo-plamo-lifecycle-delayed`: **3/3 PASS（3.83秒）**。
- ユーザー起動中のPLaMo 3 Translateモデルを使った実翻訳は
  `--jit off/eager/hotspot` × 翻訳OFF/ONの **6/6 PASS**。
  翻訳含有スクリプトはLLVM JIT有効ビルドでも意図的に
  Interpreterへフォールバックする。
- MATH-019のi32選択と音楽関連CTestは **3/3 PASS（10.33秒）**。

なおHTTPを中断できるまでの時間はOS・libcurlの進行コールバック頻度に
左右される。強制停止の上限時間や大規模同時クローンの負荷テストは
今後も必要。モデルはダウンロード・同梱していない。
他担当の変更は保持し、未commit/未push。


## 2026-11-09：大量クローン・無応答サーバーの負荷安全性

追加で発見した問題と対応：

1. キャンセル済みfutureをアクティブ4件の上限から除外すると、
   HTTPワーカー総数が増え続ける可能性があった。
   **アクティブ4件に加え、キャンセル待ちを含む未完了ワーカー総数16件**
   の独立した上限を実装。超過した新規要求は入力原文を返す。
   応答済みキャンセルfutureのみ非ブロッキングで回収する。
2. Runtime::processWaitQueue() のnext-tick解決で
   停止・再起動・切り離されたスレッドを再開する危険を軽減。
   !isKilled、PROMISE_WAIT状態、containsThreadを検証する。
3. サーバーが3秒応答しないと従来のcurl_easy_performは
   中断通知をすぐ処理できず、クライアント終了まで3.53秒要した。
   curl_multi_performと50ms単位のcurl_multi_pollへ変更。
   **1.09秒**へ改善した。この値は5種類のライフサイクル試験と
   合計約1秒のテスト用待機時間を含み、個別中断レイテンシではない。
4. 翻訳成功キャッシュの更新を生存スレッド・フレームトークン
   の検査後へ移動。古いスクリプトの結果を流用しない。

ライフサイクルテストに32件同時要求（最大4アクティブ）と
128回の生成・停止（最大16未完了）を追加。
128回の実測ピークは3ワーカーで、16件までの飽和を
強制再現できたという意味ではない。
通常120ms遅延に加え、3秒無応答のモードを持つ
tests/differential/plamo_lifecycle_delayed.py にて、
クライアント時間2.5秒未満を検証。
正式CTestのxyo-plamo-lifecycle-stalledを追加。

検証：翻訳関連CTest **4/4 PASS（5.28秒）**。
実PLaMoモデル6/6 PASS。MATH-019と音楽CTest 3/3 PASS。
サーバー側推論の停止自体は保証されないため、
モデル提供元の推論負荷管理は引き続き必要。
モデル再ダウンロードなし、未commit・未push。


## 2026-11-09：Scratchクローンを用いた統合負荷テスト

tests/differential/plamo_clone_stress.py を追加。
Scratch SB3のcontrol_create_clone_ofで32クローンを生成し、
control_start_as_cloneで非同期のtranslate_getTranslateを実行。
翻訳結果を判定してStageの共有変数に成功数・原文復帰数・完了数を記録し、
最後に全クローンがcontrol_delete_this_cloneを実行する。
127.0.0.1の模擬APIは200ms遅延を持つ。

実測結果（LLVM JIT有効構成のCLI）：
- Interpreter: 32完了、翻訳成功4、原文復帰28、HTTP要求4、最大同時通信4
- Eager: 同上
- HotSpot: 同上

全実行設定で結果一致。翻訳レポータを含むスクリプトは安全な
Interpreterフォールバックで実行されているため、LLVMネイティブの
非同期レポータは依然として未対応。
正式CTest xyo-plamo-clone-stress を登録。
30秒以内に完了し、正確な4/28の割り振りを検証する。

翻訳関連CTestは初回登録時に5/5 PASS（6.43秒）。
クローン数32への拡張は独立実行で3/3 PASS。


## 2026-11-10：入れ子の自作ブロックに潜む非同期翻訳を検出

LLVM JITの旧判定は、イベントスクリプトから捕捉した
ScratchCompilationUnit.script.nodes のみを検査していた。
しかしこのスナップショットは procedures_call が指す
別の自作ブロック定義までは含まないため、
「イベント → 自作ブロックA → 自作ブロックB →
translate_getTranslate」で、同期JIT内から非同期翻訳が
実行される可能性があった。

src/jit/codegen.cpp のコンパイル可否判定を変更。
- トップレベルの捕捉済みノードを起点に
  procedures_call の proccode を収集し、
  Blocks::getProcedureDefinition から定義を引く。
- 定義ごとに ScratchScript::capture で入力レポータ・
  サブスタック・次ブロックを含めて検査し、
  入れ子の手続き呼び出しを再帰でなく反復処理で追跡。
- 訪問済みproccodeを記録して循環参照を抑止し、
  最大1024定義に制限する。
- 到達可能な翻訳レポータか壊れた手続きグラフを
  検出した場合のみ対象スクリプトをInterpreterへ
  フォールバック。翻訳を含まない別スクリプトはJIT可。

tests/differential/plamo_jit_procedure_gate.py で
二重に入れ子の自作ブロック内に翻訳レポータを配置したSB3を生成し、
独立する1000回の数値加算スクリプトも同じSB3に載せる。
Interpreter/eager/hotspotで結果と最終状態が一致。
eagerでは翻訳を含むスクリプトのコンパイル拒否ログと、
独立スクリプトの実際のLLVMコンパイルログの両方を確認。
正式CTest xyo-plamo-jit-procedure-gate を追加。

LLVM JIT有効構成で翻訳関連CTest **6/6 PASS（6.86秒）**、
音楽＋MATH-019 CTest **3/3 PASS（9.89秒）**。
新規コードでの実PLaMoモデル試験は、
利用者のサーバーが127.0.0.1:8000で接続拒否したため未実施。
以前の実モデル検証結果を今回の新規検証結果として流用しない。
ネイティブLLVMで非同期継続する機構は依然未実装。
未commit・未push。


## 2026-11-10：実験的LLVM Fiberによるネイティブ非同期翻訳

既存の xyo_rt_reporter はInterpreterのreporterをFiber上で評価するが、
STATUS_PROMISE_WAITをRUNNINGに戻してしまうため、
通常は全スクリプトのInterpreterフォールバックを使っていた。

新たに xyo_rt_async_reporter(JitCtx*, const char*) を追加した。
- LLVM生成コードの翻訳レポータ呼び出しは、Interpreterの当該reporterを
  一度だけ評価し、非同期中はFiberをyieldする。
- STATUS_PROMISE_WAITは維持し、main threadが非同期結果のresolveWaitを
  行うまでLLVM Fiberを再開しない。
- 復帰時にjustReportedをLLVMのboxed Valueとして返し、
  LLVMの元の呼び出し位置へ戻る。
- JITランタイムシンボルとhelperMayYieldへ新ヘルパを登録。
- 実験用環境変数 XYO_JIT_PLAMO_FIBER=1 でのみ有効。
  デフォルトは従来のInterpreterフォールバックを維持する。
  AOTは引き続き非対応。

tests/differential/plamo_native_fiber.py を追加し、
127.0.0.1の80ms遅延HTTPモックで確認：
1. 直接の非同期翻訳を含むスクリプトがLLVMに実際にコンパイルされ、
   返った訳文を変数へ保存する。
2. 入れ子手続きA→Bの内部の翻訳でもLLVMコンパイル・Fiber復帰が成功。
3. 32クローン同時翻訳で4件訳文・28件原文復帰、
   HTTP要求4、同時最大4。

正式CTest xyo-plamo-native-fiberを登録。
翻訳関連CTest 7/7 PASS（7.45秒）。
XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1でも上記3ケースPASS。
MATH-019・音楽関連CTest 3/3 PASS（12.11秒）。

制限・未検証：
- 明示GC・多量の文字列SSAルートが生存する最中の検証。
- LLVM Fiberが翻訳待機中のstopAll/クローン削除/再起動の負荷検査。
- 今回の実験経路での実PLaMoモデル推論（localhost:8000接続拒否）。
- AOT・WebAssemblyの非同期継続ABI。
以上のためデフォルトONにはしない。

試験例：
    XYO_PLAMO_TRANSLATE=1 XYO_JIT_PLAMO_FIBER=1 ./build/xyo run project.sb3 --jit eager --frames 300

モデルは取得・同梱しておらず、未commit/未push。


## 2026-11-10：ネイティブFiber待機中の停止・削除・文字列保持

実験用 XYO_JIT_PLAMO_FIBER=1 の追加回帰検証。

- tests/differential/plamo_native_stop_all.py：
  2本の旗スクリプトを持つ実SB3。片方のLLVM Fiberが翻訳レポータで
  2秒遅延HTTP待ちの間、もう片方が0.04秒後にcontrol_stop allを実行。
  Interpreter/offとLLVM/eagerの両方で停止後の変数書き込みは起こらず、
  HTTPの2秒待ちを引きずらない。実測約0.09秒。
- tests/differential/plamo_native_clone_delete.py：
  8個のクローンそれぞれが翻訳レポータと、別hatからの0.06秒待機後
  control_delete_this_cloneを持つ実SB3。4件はHTTP待ち、残り4件は
  上限による即時原文フォールバック。削除8/8、後続書き込み4件
  （即時フォールバック由来のみ）、HTTP要求4件、約0.13秒で終了。
  Interpreter/offとLLVM/eagerで一致。
- tests/differential/plamo_native_fiber.py：
  元々の直接・入れ子手続き・32クローンに加えて、
  動的文字列連結と翻訳を一つのLLVM式に混在させるSB3を追加。
  翻訳待機後も、以前に計算した文字列値を保持し、
  startabcdef|Scratchからこんにちはを生成できることを確認。
  XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1の厳格設定でも4ケースPASS。

正式CTest:
  xyo-plamo-native-stop-all、
  xyo-plamo-native-clone-delete、
  xyo-plamo-native-fiber-strict-gc
を追加。翻訳関連CTest10/10 PASS（9.74秒）。
変更後のネイティブ4ケース4/4 PASS（3.32秒）。
MATH-019と音楽CTest3/3 PASS（9.91秒）。

本結果はFiber待機中の特定の停止/削除/文字列保持を検証したもので、
大量の明示GC実行を強制して全SSAルートの健全性を証明したものではない。
実PLaMoでの新規検証も未実施。実験フラグは既定OFFを維持。
未commit/未push。


## 2026-11-10: suspended LLVM Fiber explicit-GC regression

Added tests/platform/plamo_native_gc_wait.cpp and tests/differential/plamo_native_gc_wait.py. With eager LLVM JIT, opt-in XYO_JIT_PLAMO_FIBER=1 and strict XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1, a real translation reporter blocks on a localhost HTTP mock for two seconds. The C++ harness verifies a live STATUS_PROMISE_WAIT Fiber and FiberPool::unsafeCount()>0. It requests explicit GC 128 times and verifies collectCount stays unchanged while deferred collection is pending.

Found stopAll() detached ordinary suspended native Fibers without retiring them, preventing pending explicit GC from draining until Runtime destruction. Runtime::stopAll now invokes stopThread for all threads (retireThread avoids self-destruction of the currently executing Fiber) before detaching the threads vector. After stopAll, unsafeCount is zero and collectIfNeeded drains the deferred collection. The test confirms exactly one HTTP request reached the mock, and the two-second HTTP delay does not block cancellation.

Verification: new GC CTest 1/1 PASS, all PLaMo CTests 11/11 PASS (10.46 s), runtime clone journal and two RT-001 native GC smoke tests 3/3 PASS (0.21 s), Music CTests 2/2 PASS. MATH-019 conditional i64 PHI test failed in concurrently changing shared tree with missing conditional iter PHI i64; causality with this change not established and it is not claimed PASS. These tests prove explicit collection is deferred while unsafe Fibers exist, NOT that arbitrary live LLVM SSA can be collected safely. Experimental feature remains OFF by default. No model download, commit or push.

## 2026-11-10: simultaneous suspended LLVM Fiber GC and restart recycling

Added `tests/platform/plamo_native_gc_multifiber.cpp` plus `tests/differential/plamo_native_gc_multifiber.py` as `xyo-plamo-native-gc-multifiber` CTest.

The Python harness builds a real Scratch .sb3 with 32 clones and a localhost 600ms delayed OpenAI-compatible HTTP mock. The C++ harness uses one Runtime, eager native JIT, experimental `XYO_JIT_PLAMO_FIBER=1`, and strict `XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1`. In each of 128 green-flag cycles it waits for four actual `PROMISE_WAIT` native Fiber reporters; asks for 64 explicit collections (all deferred while FiberPool::unsafeCount() >= 4); stops exactly one thread; confirms the three remaining native activations still prevent GC; then calls `stopAll()` and confirms no unsafe/active Fiber remains, and the pending GC can be drained. It resets the *test's* TranslationService between cycles to invalidate the normal repeated-string cache and guarantee a fresh HTTP request; production caching code is unchanged.

The 128 cycles also cross Runtime's existing 4096-thread reclamation threshold: peak owned Threads = 4092, final owned Threads = 131, peak recyclable pool = 2048, final reusable = 1949. Peak native Fibers = 4. The HTTP mock observed 431 accepted requests on the first successful run; exact HTTP count is not fixed because cancellation may race the server accept. All 128 cycles completed; CTest PASS (5.28s) and repeated PASS (6.40s). These values show thread ownership is reclaimed at the existing threshold, not that overall RSS or VM memory has been independently measured.

Regression: all 12/12 PLaMo CTests PASS (15.26s). Clone journal, RT-001 native GC deferral and multi-Fiber barrier, and two music CTests: 5/5 PASS (0.82s). The MATH-019 i64 conditional PHI failure noted in the previous entry was not rechecked on this turn, so its status remains unknown. Strict native GC guards defer collection, not certify arbitrary LLVM SSA as an exact GC root. `XYO_JIT_PLAMO_FIBER` remains OFF by default. No user model download, commit, or push.

## 2026-11-10: RSS telemetry and 512-cycle native-Fiber soak

Extended `tests/platform/plamo_native_gc_multifiber.cpp` to sample actual Linux `VmRSS` from `/proc/self/status` after every stopAll+deferred GC drain; logs `MEM cycle=... rss_kib=... peak_rss_kib=... owned=... reusable=... fibers=...` at 32-cycle intervals and `MEM_SUMMARY` at the end. Added `XYO_PLAMO_GC_CYCLES` optional integer override (default 128; maximum 2048) and updated the Python integration test to verify the requested count.

Example for extended run:

```bash
XYO_PLAMO_GC_CYCLES=512 python3 tests/differential/plamo_native_gc_multifiber.py \
  board/.music-implementation/cmake/tests/xyo-plamo-native-gc-multifiber \
  board/.music-implementation/cmake/tests/plamo-native-gc-multifiber
```

A 128-cycle CTest PASS: baseline 53,888 KiB, after first cycle 86,108 KiB, final 92,224 KiB, growth from cycle 1 = 6,116 KiB. A verified 512-cycle extended run PASS: baseline 54,260 KiB, after first cycle 86,652 KiB, cycle 128 = 92,624 KiB, final cycle 512 = 92,624 KiB, growth after first = 5,972 KiB. At every 32-cycle checkpoint between 128 and 512 the post-stop RSS was 92,624 KiB, without continued growth. Peak owned threads 4,092, final 131; peak recycled capacity 2,048, final reusable 1,949; peak suspended native Fibers 4, after each stopAll = 0. Mock HTTP server accepted 2,040 requests during the 512-cycle verified run. These figures describe RSS for this fixture; they do not prove a general absence of memory leaks, and a steady RSS can include cached/recycled allocations.

Regression after instrumentation: PLaMo CTests 12/12 PASS (17.48s); clone journal, RT-001 GC deferral/multifiber, and Music tests 5/5 PASS (0.64s). No production runtime changes in this turn. Previous MATH-019 PHI failure was not rechecked. Experimental native-Fiber flag stays disabled by default; no commit/push or model re-download.


## 2026-11-10: suspended native Fiber string pressure

Extended tests/platform/plamo_native_gc_wait.cpp with 10,000 distinct 160-character GC-managed SStr allocations while an actual native LLVM translation Fiber is suspended. Gc::bytesAllocated: before 8 KiB, suspended 3,855 KiB (+3,847 KiB), after stopAll/Fiber retirement and deferred collection 10 KiB. Explicit GC attempts every 500 allocations plus existing 128 attempts were deferred while unsafe SSA remained. CTest PASS.

Extended tests/differential/plamo_native_fiber.py: generates a Scratch SB3 with 400 dynamic string concatenations, then LLVM async translation and native Fiber resume; checks full accumulated string and translated result against independent Python references. Strict generated SSA root mode and compiled LLVM code confirmed PASS. PLaMO CTest 12/12 PASS; RT-001 GC, clone journal, music CTest 5/5 PASS. GC is deferred until native Fiber retirement, not made safe during arbitrary live SSA. No production code changes, model downloads, commit, or push; experimental flag remains OFF.


## 2026-11-10: mandatory GC veto for experimental native translation Fiber

Safety review found that XYO_JIT_PLAMO_FIBER=1 previously enabled LLVM async translation regardless of XYO_RT_REQUIRE_GENERATED_SSA_ROOTS. This could allow explicit GC while a suspended Fiber retains uncheckpointed LLVM SSA and no global native-Fiber GC veto is enabled. Native translation is now permitted only when BOTH environment variables are set to 1. The compile-time async gate and translation reporter lowering in src/jit/codegen.cpp enforce the same condition; otherwise translation scripts safely fall back to Interpreter. This is NOT a proof of precise live-SSA rooting or permission to collect during active Fibers: the global unsafeCount veto still defers GC until retirement.

Updated opt-in tests (plamo_native_fiber.py, plamo_native_stop_all.py, plamo_native_clone_delete.py) to explicitly request the native SSA GC guard. Added a negative case to plamo_jit_procedure_gate.py enabling Fiber=1 with GC guard=0, verifying Interpreter fallback and exact final values, with independent JIT-able scripts still compiled. LLVM build succeeded. PLaMO CTest 12/12 PASS (17.68s). Clone journal, RT-001 native GC, and music related CTest 5/5 PASS (0.75s). Default remains OFF, no commit or push. Native translation requires XYO_JIT_PLAMO_FIBER=1 plus XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1.


## 2026-11-10: LLVM async translation callsite SSA census (diagnostic only)

Added opt-in `XYO_JIT_PLAMO_SSA_AUDIT=1` to `src/jit/codegen.cpp`. For each emitted `xyo_rt_async_reporter` call, before existing IR-001 spill instrumentation, reuse the existing conservative `analyzeLlvmCheckpointLiveness` and `analyzeExtraLiveSsa` to log numeric, float, argument, pointer, pointer region, and unknown-pointer origin counts. No raw pointers, boxed SSA words or GC handles are printed. Every row explicitly marks `gc_safe=0` regardless of `classification_complete`, because i64 Scratch Values and loaded/derived pointers can still retain unrooted SStr objects, and this is not a GC safepoint proof.

Five LLVM-native mock-server SB3 integration cases passed, and `tests/differential/plamo_native_fiber.py` now enables audit and asserts that each emitted reporter has a diagnostic and matching nine-way unknown origin counts:
- Direct reporter: numeric 0, unresolved pointers 6 (host-address region 1, unknown 5).
- Nested procedure: numeric 2, unresolved pointers 2 (host 1, unknown 1).
- Combined string expression: numeric 1, unresolved pointers 2 (unknown 2).
- 400-step Scratch string churn: numeric 1, unresolved pointers 2 (unknown 2).
- 32 clone reporters: numeric 0, unresolved pointers 5 (host 1, unknown 4).
All unknown-region values in these fixtures have `load_unknown` origin. `classification_complete=1` means the static counters reconcile, NOT that all roots are captured or that GC is safe.

LLVM JIT build PASS; PLaMO CTest 12/12 PASS (17.68 s); clone journal, RT-001 GC deferral/multifiber, and music tests 5/5 PASS (0.64 s). No collection policy change: `FiberPool::unsafeCount()` continues to veto GC until every unsafe Fiber retires. Next work is to trace the unknown load chains back to their actual storage owners and identify boxed i64 String values surviving this callsite; only then consider a roots-first safepoint protocol. The experimental native Fiber remains OFF by default; no commit or push.


## 2026-11-10: tracing native PLaMo load_unknown to JitCtx::varSlotData

Extended the opt-in translation SSA diagnostic in `src/jit/codegen.cpp` to emit bounded per-callsite `[plamo-ssa-load-path]` records: compiler instruction ID, dereference count, GEP/cast derivations, terminal provenance and the exact native JitCtx field name (matched only by `offsetof`, with no pointer values). Also emits counts of host/stack/other terminals, known JitCtx field offsets and maximum dereference depth. This uses the preexisting IR-001 `tracePointerLoadChain`, remains diagnostic-only, and explicitly reports `gc_safe=0`. No change to collection policy.

`tests/differential/plamo_native_fiber.py` checks each site reconciles origin counts with load-path counts, validates bounded paths, and compares guarded variable access (`XYO_RT_GUARD_NATIVE_VAR_SLOTS=1`) to unguarded in direct translation and 400-string churn. All inspected unknown load chains were two-level chains from the `JitCtx::varSlotData` field: across the five SB3 fixtures 12+2+2+2+4=22 logged load chains, all mapped to `varSlotData`. The unguarded direct script yielded six live pointers (five unresolved) and five unknown-load paths at its first reporter site; with the guarded var-slot mode it yielded one live host pointer, zero unresolved pointers and zero unknown load chains. The 400-concatenation script likewise dropped from two unresolved loads to zero and preserved the exact translated output. This is useful *pointer lifetime* provenance, NOT an SSA boxed i64 GC root proof. Since live i64 Scratch Value bits and heap ownership remain unproven, unsafeCount continues to veto any collection until Fiber retirement.

LLVM build PASS, PLaMO CTests 12/12 PASS (18.53s), RT-001 GC/clone/music 5/5 PASS (1.03s), Python compile and git diff check PASS. No change to production collection policy or default opt-in flags, no commit or push.
