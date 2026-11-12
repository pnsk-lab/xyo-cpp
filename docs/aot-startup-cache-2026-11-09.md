# START-002: 起動高速化のための内容アドレス指定AOTキャッシュ

2026-11-09。前回のSTART-001はGUIで巨大スクリプトのJIT手続き生成を遅延し、
ParityプロジェクトのEager起動を中央値12.12秒から2.62秒へ削減した。
ただしコンパイル負荷を実行中へ移すため、完走時間が悪化する場合がある。

今回追加した `tools/xyo-aot-cache.ts` は、**繰り返し起動するプロジェクトを
前もってAOTコンパイル**するための、明示的に有効化する起動ランチャーである。
`xyo` の既存AOT出力とローダーを利用するため、新しいC++ VM/LLVM ABIは追加しない。

## 使い方

```bash
# 同一プロジェクト・同一実行ファイルで一度だけ準備する。
# この準備はLLVMのAOTコンパイル時間を要する。
bun tools/xyo-aot-cache.ts prepare project.sb3 --bin ./build/xyo

# 2回目以降、前回準備したネイティブコードを読み込んで起動する。
bun tools/xyo-aot-cache.ts play project.sb3 --bin ./build/xyo

# xyo play に引数を渡す場合は -- の後へ指定する。
bun tools/xyo-aot-cache.ts play project.sb3 --bin ./build/xyo -- --fps 60

# GUIなしの実行にも利用可能。
bun tools/xyo-aot-cache.ts run project.sb3 --bin ./build/xyo -- --print-list output

# キャッシュ場所を変える場合
bun tools/xyo-aot-cache.ts prepare project.sb3 --bin ./build/xyo \
  --cache-dir ~/.cache/xyo-cpp/aot
```

環境変数 `XYO_AOT_CACHE_DIR` または
`XDG_CACHE_HOME` で保存場所を指定できる。
既定は `~/.cache/xyo-cpp/aot`。

- キャッシュキー: `SHA256(xyoの全バイト)` と
  `SHA256(sb3の全バイト)`。未コミット変更を含むビルドの差分も検出する。
- 保存内容: ネイティブ `.o` とSHA-256照合用 `.json` メタデータ。
  **準備成功後に一時ファイルを名前変更**し、不完全なオブジェクトを再利用しない。
  破損・差し替え・シンボリックリンクの場合はキャッシュミスとする。
- 別ビルドまたは変更したSB3はキャッシュミスになり、起動時のコンパイルを
  **自動実行しない**。代わりに通常のJITで即座に起動する。
  新しい内容をAOT化するときだけ `prepare` を再実行する。
- キャッシュ作成時のLLVM CPU設定は移動可能性を重視して
  `XYO_AOT_TARGET_CPU=generic` に固定する。
- キャッシュヒット時には `xyo play/run --aot <obj>` を使用し、
  `aot: N scripts precompiled` のロード記録を検証する。
  ロード失敗は**JITへ無言で切り替えず、エラーとして通知する**。

## 実測

同一バイナリ:
`/home/kicky/.cache/xyo-startup-snapshot-build-20261109/xyo`
（この時点のハッシュは
`42103fafc2b3967ac7ee04b3f37a22a6295acf05c102fde7226d3805a4bda49d`）。
対象: 2656件ScratchBench Parity GUI、
SDL `offscreen`、オーディオ `dummy`。

| 経路 | 最初のScratch出力 | 完走時間 | Parity |
| --- | ---: | ---: | ---: |
| Eager JIT（START-001後） | 2292.2 ms | 32272.8 ms | 2656/2656 |
| AOTキャッシュヒット | 313.9 ms | 14499.1 ms | 2656/2656 |

同一バイナリで起動のみを交互に4回ずつ追加測定した結果、
AOTキャッシュヒットは **中央値333.7 ms**
（334.4、327.6、333.1、372.1 ms）、
Eager JITは **中央値2518.3 ms**
（2547.5、2489.2、2462.9、2624.9 ms）。
起動時間は **約7.55倍高速、86.8%削減**。
最初の出力を測定後にプロセスを停止するため、この4組の値は完走時間ではない。

この完走比較は各1回。AOTロードログには
`aot: 479 scripts precompiled (8423 slots)` があり、JITフォールバックではない。
AOT生成物は13,864,912バイト。生成時間は起動時間・完走時間に含めず、
**事前準備のコストとして別に負担する**。

## 検証と制限

`bun test tools/xyo-aot-cache.test.ts`:
キャッシュミス・ヒット、バイナリとSB3の更新による失効、
破損オブジェクト、シンボリックリンク、暗黙のJITフォールバック拒否、
コンパイル失敗時の一時ファイル片付け、引数検証を確認。
加えて実プロジェクトのAOT起動でScratch期待値2656/2656 PASS。

既存のAOTローダーの一部スクリプト未対応は従来どおり、
ロード完了後に必要なJITコードを生成する場合がある。
このツールは「全スクリプトを必ずAOT化する」ものではない。
キャッシュは**ユーザーが信頼する専用ディレクトリ**へ保存すること。
有効なネイティブオブジェクトは実行可能コードなので、
第三者に書き込み可能な場所のキャッシュは使わない。

CIにはキャッシュランチャーの単体テストを追加。
GitHub Actions上での実実行は未検証。変更は未コミット・未push。
