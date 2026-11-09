// 現在の作業ツリー diff にある「既定 OFF の実行時最適化」を一括で有効化する。
//
// 個別の XYO_* トグルは A/B 用に既定 OFF のままにしてあるものが多い。
// `--opt-all`（または XYO_OPT_ALL=1）はそれらをまとめて ON にする。
// 既に明示的に設定されている環境変数は変更しないので、個別に 0 を
// 指定したトグルはそのまま無効を保つ。
#pragma once

namespace xyo {

/// v1.2.0の検証済み高速既定プロファイル。明示した環境変数は尊重する。
int enableReleaseOptimizations();
int releaseOptimizationCount();
const char* releaseOptimizationName(int index);

/// diff の opt-in 最適化をすべて有効化する。戻り値は今回設定した数。
int enableAllOptimizations();

/// `XYO_OPT_ALL` が真値か（CLI 側の判定用）。
bool allOptimizationsRequested();

/// 一括有効化の対象（テスト・ログ用）。
int allOptimizationCount();
const char* allOptimizationName(int index);

} // namespace xyo
