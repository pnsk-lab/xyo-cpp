# JIT文字列大小比較キャッシュ最適化（2026-11-03）

## 変更内容

`src/jit/rt.cpp` の `xyo_lt` と `xyo_gt` に、2つの文字列の数値解析キャッシュ `SStr::numState` と `numCache` を再利用する `cachedStringOrder` を追加した。

- 最初に両辺がヒープ文字列かを判定。通常の数値・真偽値・空文字列は従来の `cast::compare` へ戻す。
- 片方でも `numState==0`（未解析）の場合も従来の `cast::compare` へ戻す。
- 両辺が既知の数値状態（`numState==1` または `4`）であれば、キャッシュされた double でIEEE数値比較を実行する。
- それ以外（非数値、空白からの0、Infinityの大文字小文字違いなど）は `compareStringsCaseInsensitive` へ直接渡す。Unicodeの大小文字処理は既存実装を使用する。
- 診断用に `XYO_JIT_STRING_ORDER_CACHE=0` で高速経路を無効化できる。既定は有効。
- 数値・真偽値主体の呼び出しでは、比較前の早期タグ検査により診断スイッチや文字列状態へのアクセスを行わない。

## 実測性能

i7-12700/Linux上で `build/jit-string-order-frozen-xyo` の同一Releaseバイナリを使い、ScratchBenchの単独SB3をCPU固定で各6回ずつ交互測定。元のSB3は一切書換えていない。表はops/msの中央値ON/OFF比。

| ワークロード | ON/OFF |
| --- | ---: |
| `math_strcompare` | **1.2627倍** |
| `math_compare` | 0.9901倍 |
| `algo_primes` | 0.9963倍 |
| `app_raycast` | 0.9976倍 |

4ケース計48実行のチェックサムは各ケースですべて一致。小差は測定ノイズと区別できない。生データ `build/jit-string-order-ab-20261103.json`。

フルScratchBench Performanceを同一バイナリでON/OFF各1回実行し、両方とも **47項目完走**、時間依存のscheduler8項目を除く **39/39項目でチェックサムが一致**。単発であるため速度比較の幾何平均は参考値（約1.011倍）。ログ: `build/jit-string-order-full-1-20261103.log` と `build/jit-string-order-full-0-20261103.log`。

## 正しさ

`tests/unit/test_type_profile.cpp` に、キャッシュ未計算と計算済みの両状態で `xyo_lt/xyo_gt` を `cast::compare` と照合する回帰テストを追加。数字（負数、指数、Infinity）、数値とみなされない空白、ASCIIとUnicode、大文字小文字混在を含む。既存のEager/Interpreter型変更差分テストは維持する。

- CTest **7/7**: `build/jit-string-order-ctest-20261103.log`
- Eager JIT Parity **2123/2123**: `build/jit-string-order-eager-parity-20261103.log`
- HotSpot JIT Parity: 初回は乱数確率テスト `Pick Random Rules 2 #5` の1件のみ失敗、再実行は **2123/2123**、高速経路を無効にした対照も **2123/2123**。これは前回から確認されている乱数テストの揺れであり、新規回帰とは判断しない。ログ `build/jit-string-order-hotspot-{parity,retry,control}-20261103.log`。

### 最終ビルド（タグ早期判定の整理後）

- Releaseビルド成功: build/jit-string-order-final-build-20261103.log
- CTest 7/7成功: build/jit-string-order-final-ctest-20261103.log
- HotSpot Parity 2123/2123: build/jit-string-order-final-hotspot-parity-20261103.log
- Eager Parity 2123/2123: build/jit-string-order-final-eager-parity-20261103.log
- 最終バイナリ: build/jit-string-order-final-xyo（共有ビルドへの他作業の干渉を避けるため保存）

## 状態

本作業はローカルワークツリーへ適用済み。別作業の未コミット変更を保持。コミット・pushは行っていない。
