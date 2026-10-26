// xyo-cpp: sb3 の「機械的に最適化しやすい形」への書き換え。
//
// minify と同じく project.json を直接書き換える（Runtime を経由しない）ので、
// ローダが解釈しないフィールドも保持される。適用する変換はすべて
// **意味を変えない**ものだけ:
//
//  1. warp 化: ループを含み、yield する op（wait / ask / broadcast-and-wait /
//     play-until-done 等）を含まない手続きに「画面を更新せずに実行」を立てる。
//     インタプリタでもループごとの yield が消える（JIT の非 warp ループ
//     境界と同じ条件）。ブロック数が閾値未満の手続きは対象外。
//  2. 一時変数のインライン化: `set v = <純粋な式>` の直後が `if/until/while`
//     で、v の書き込みがプロジェクト全体で 1 箇所・読み出しもその条件の
//     1 箇所だけなら式を条件へ直接埋め込み、set を消す（変数スロットの
//     store/load と、共有変数への書き込みそのものが消える）。
//
// どちらも「速くなる可能性を潰さない」方向の書き換えで、warp 化は
// スケジューリング（画面更新の粒度）を変える点にだけ注意。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xyo {

struct Sb3RewriteOptions {
    /// ループを含み yield しない手続きを warp 化する。
    bool warp = true;
    /// warp 化する最小ブロック数（これ未満は効果が薄いので触らない）。
    size_t warpMinBlocks = 40;
    /// 一時変数（1 書き込み・条件でのみ 1 読み出し）をインライン化する。
    bool inlineTemps = true;
    /// 書き換え後に minify（到達不能ブロック除去 + zip 圧縮 + ID 短縮）もかける。
    bool minify = false;
    /// minify で未使用の変数/リスト/ブロードキャストも消す。
    bool minifyAggressive = false;
};

struct Sb3RewriteStats {
    size_t procsScanned = 0;
    size_t procsWarped = 0;
    size_t tempsInlined = 0;
    size_t blocksBefore = 0;
    size_t blocksAfter = 0;
    size_t projectJsonIn = 0;
    size_t projectJsonOut = 0;
    size_t outputBytes = 0;
    /// 人間向けの変更点（`--report` で stderr に出す）。
    std::vector<std::string> log;
};

struct Sb3RewriteResult {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> data;
    Sb3RewriteStats stats;
};

Sb3RewriteResult rewriteSb3(const uint8_t* data, size_t size,
                            const Sb3RewriteOptions& options);
Sb3RewriteResult rewriteSb3File(const char* inPath, const char* outPath,
                                const Sb3RewriteOptions& options);

} // namespace xyo
