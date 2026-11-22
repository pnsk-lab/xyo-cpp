// xyo-cpp: 静的な漸近計算量の見積り（xyo analyze --complexity）。
//
// 方針はレポート「静的解析のみで計算量・実行回数上界を自動推定する研究」の
// 簡易版である。完全な KoAT 型の ranking function / TWN closed-form /
// SMT 合成は行わない。代わりに Scratch のループ構造に特化した
//   * 局所 runtime bound（各ループの反復回数: 定数 / 記号パラメータ / 条件依存 / 無限）
//   * size パラメータ（リスト長 |L|、変数値 N）
//   * 全体合成（入れ子の積・手続き展開・走査コストの加算）
// の 3 層だけを持ち、各スクリプトの起動あたりの上界を
// 記号式 + Big-O + （定数のみなら）exact ステップ数で出す。
// until/while のカウンタ比較は線形ランキングで回数化する。
// ガード依存・非線形更新・再帰は UNKNOWN/∞ として明示し、過小評価しない。
#pragma once

#include <string>
#include <vector>

namespace xyo::analysis {

struct Report;

/// broadcast / clone による起動（送信側に記録）。
struct SpawnInfo {
    std::string kind;    // "broadcast" / "clone"
    std::string target;  // 放送名 / clone 先
    std::string mult;    // 送信側の囲み倍率（"1" / "10" / "N" …）
};

/// スクリプト 1 本の計算量見積り（1 回の起動あたり）。
struct ComplexityInfo {
    size_t scriptIndex = 0;   // Report::scripts への index
    std::string label;        // ScriptInfo::label の写し
    std::string kind;         // "hat" / "def"
    /// 人間向けの上界式（例: "10 * N * |items| (+条件U)"）。
    std::string boundExpr;
    /// 正規化したクラス（例: "O(1)", "O(N)", "O(N*|L|)", "O(?)", "O(∞)"）。
    std::string bigO;
    /// 全 bound が定数で確定したときだけ真。exactSteps が正確な上界。
    bool exact = false;
    double exactSteps = 0;
    /// 見積りの内訳（表示用）。
    size_t maxDepth = 0;      // ループネストの最大深さ
    std::vector<std::string> factors;  // 記号因子（"|items|" / "N" 等、重複あり）
    bool infinite = false;    // forever 到達可能
    bool unknown = false;     // until/while・非対応入力・動的 bound
    bool recursive = false;   // 再帰呼び出しあり（展開打ち切り）
    std::vector<SpawnInfo> spawns;  // broadcast / clone の送信
    std::vector<std::string> notes;  // 根拠（"repeat 10" / "条件依存" 等、最大 8 件）
};

struct ComplexityReport {
    std::vector<ComplexityInfo> scripts;  // hat/def のみ（reporterTop 除外）
};

/// Report から計算量を見積もる（読み取り専用）。
ComplexityReport analyzeComplexity(const Report& r);

/// --complexity の別節テキスト。
std::string formatComplexity(const Report& r, const ComplexityReport& c);
std::string formatComplexity(const Report& r);

}  // namespace xyo::analysis
