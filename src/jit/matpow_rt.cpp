// xyo-cpp: matrix-exponentiation ランタイム — 線形漸化式ループの O(log n) 実行。
//
// 検出器（matpow.cpp）が作った拡大行列 Ã（(k+1)x(k+1)、int64）で
// [v; 1] を trip = floor(n + 0.5) 回更新する。計算は __int128 で行い、
// 以下を全て検証して初めて適用する（満たさなければ 0 を返して逐次へ）。
//
// 範囲証明（逐次実行と bit 一致の十分条件）:
//   1. 全状態変数が数値・有限・整数・|v| < 4.5e15・-0 でない
//      （-0 は符号が観測され得るため保存が必要 — matpow は +0 を返すので拒否）。
//   2. P = Π_j max(1, ||Ã^(2^j)||∞)（trip の全ビット位置）を計算。
//      部分積 Ã^e（e ≤ trip）の max-row-sum は P で押さえられる。
//   3. bound = 2 · (k+1) · max(1, max|v|) · P · stmtBound < 4.0e15
//      stmtBound はコンパイル時の文ノルム積（文内の部分和・内側ループ展開を
//      含む保守的上界）。これで「逐次評価の全中間値」「行列累乗の全中間
//      行列要素・最終値」が 2^53 未満の整数と分かる → 両者の結果は bit 一致。
//
// 行列累乗自体も __int128 で行う（bound 証明済みなので f64 評価と同じ
// 結果になるが、証明と同じ表現で評価する方が素直）。
// 戻り値: 0 = 逐次フォールバック、1 = out[] に結果、2 = 0 反復（書き込み無し）。
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "../core/cast.hpp"
#include "../core/sstr.hpp"
#include "../core/value.hpp"
#include "jit.hpp"

extern "C" {

// テスト用カウンタ
int64_t xyo_matpow_runs = 0;    // 適用（書き込みあり）した回数
int64_t xyo_matpow_rejects = 0; // 範囲証明が通らず逐次へ戻した回数

}

namespace {

using i128 = __int128;

constexpr int64_t kEntryLim = int64_t(1) << 60;   // matmul 内部の要素上限
constexpr double kVarLim = 4.5e15;                // 変数の初期値上限 (< 2^53)
constexpr i128 kExactLim = i128(1) << 52;  // 全中間値 < 2^52

int rejectBound() {
    ++xyo_matpow_rejects;
    if (std::getenv("XYO_MATPOW_DEBUG"))
        std::fprintf(stderr, "xyo-matpow: reject (bound)\n");
    return 0;
}

/// 実行時の有効判定。ループ 1 回の実行につき 1 度だけ呼ばれるので
/// getenv のコストは無視できる（テストが実行中に切り替えられるよう
/// 静的にキャッシュしない）。
bool matPowEnabled() {
    const char* v = std::getenv("XYO_MATPOW");
    return !(v && v[0] == '0');
}

}  // namespace

extern "C" {

/// n 反復の線形漸化式を行列累乗で評価する。
///   vars[i] : 状態変数の Value*（読み取りのみ）
///   mat     : (dim x dim) row-major の拡大行列（int64、dim = k+1）
///   stmtBound : コンパイル時の文ノルム積
///   out[i]  : 結果（ret==1 のときだけ有効、全て有限の整数 f64）
int xyo_matpow(void* /*ctx*/, double n, const xyo::Value* const* vars,
               int64_t k, const int64_t* mat, int64_t dim,
               int64_t stmtBound, double* out, const int32_t* postSrc,
               const uint8_t* writ, int32_t flags, int32_t condKind,
               int32_t cmpKind, const int64_t* diff) {
    if (!vars || !mat || !out || !postSrc || !writ || k <= 0 ||
        dim != k + 1 || dim > 64)
        return 0;
    if (!matPowEnabled()) return 0;
    const bool dbg = std::getenv("XYO_MATPOW_DEBUG") != nullptr;
    if (dbg) {
        std::fprintf(stderr, "xyo-matpow: call n=%g k=%lld dim=%lld\n", n,
                     (long long)k, (long long)dim);
        for (int64_t r = 0; r < dim; ++r) {
            std::fprintf(stderr, "xyo-matpow: row %lld:", (long long)r);
            for (int64_t c = 0; c < dim; ++c)
                std::fprintf(stderr, " %lld",
                             (long long)mat[r * dim + c]);
            std::fprintf(stderr, "\n");
        }
    }
    // trip の決定:
    //  * repeat       → floor(n + 0.5)（逐次ループの i >= 0.5 判定と同値）。
    //  * repeat until / while → 条件（affine 同士の比較）が単調であることを
    //    検出時に証明済みなので、最初に真/偽へ変わる反復を二分探索する。
    int64_t trip = 0;

    // --- pass 1: 初期値の検査と max|v| -----------------------------------
    const int d = int(dim);
    std::vector<i128> x0(size_t(d), 0);
    std::vector<char> stringInit(size_t(d), 0);
    x0[size_t(d) - 1] = 1;
    i128 maxv = 1;
    for (int64_t i = 0; i < k; ++i) {
        const xyo::Value* v = vars[i];
        if (dbg) {
            char buf[64] = {0};
            if (v && v->isString()) {
                const xyo::SStr* sp2 = v->asStrPtr();
                if (sp2) {
                    const std::string u8 = xyo::SStr::toUtf8(sp2->view());
                    std::snprintf(buf, sizeof buf, " str='%.32s'", u8.c_str());
                }
            }
            std::fprintf(stderr, "xyo-matpow: in[%lld] bits=%016llx%s\n",
                         (long long)i,
                         (unsigned long long)(v ? v->bits : 0), buf);
        }
        if (!v || v->isNaN()) return 0;
        // xyo の JIT は `set x to 0` の数値シャドウを文字列 "0" のまま
        // 保持することがある（逐次も同じ値）。数値ならそのまま、文字列
        // なら toNumber で読む — どちらも逐次の算術と同じ値になる。
        double val = 0;
        if (v->isNumber()) {
            val = v->asNumberUnchecked();
        } else if (v->isString()) {
            if (!v->asStrPtr()) return 0;  // 空文字列は型が観測される
            val = xyo::cast::toNumber(*v);
            stringInit[size_t(i)] = 1;
        } else {
            return 0;
        }
        if (!std::isfinite(val) || val != std::floor(val)) return 0;
        // 内側 repeat の回数が状態依存（affine）のプランでは、回数が
        // 負にならないことを保証するため全状態変数 ≥ 0 を要求する。
        if ((flags & 1) && val < 0) {
            if (dbg)
                std::fprintf(stderr, "xyo-matpow: negative state for "
                                     "nonneg plan\n");
            return 0;
        }
        if (val == 0.0 && std::signbit(val)) return 0;  // -0 を拒否
        if (val > kVarLim || val < -kVarLim) return 0;
        x0[size_t(i)] = i128(int64_t(val));
        const i128 a = x0[size_t(i)] < 0 ? -x0[size_t(i)] : x0[size_t(i)];
        if (a > maxv) maxv = a;
    }

    // 符号付き A（結果の累乗用）。|A| は範囲証明で作る。
    std::vector<i128> sA(size_t(d) * d, 0);
    for (int i = 0; i < d * d; ++i) sA[size_t(i)] = i128(mat[i]);

    const i128 cap0 = i128(kEntryLim);
    auto mulRaw = [&](const std::vector<i128>& A, const std::vector<i128>& B,
                      std::vector<i128>& C) -> bool {
        for (int r = 0; r < d; ++r)
            for (int c = 0; c < d; ++c) {
                i128 sum = 0;
                for (int l = 0; l < d; ++l)
                    sum += A[size_t(r) * d + l] * B[size_t(l) * d + c];
                if (sum < -cap0 || sum > cap0) return false;
                C[size_t(r) * d + c] = sum;
            }
        return true;
    };

    if (condKind != 0) {
        if (!diff) return 0;
        constexpr int64_t kMaxTrip = int64_t(1) << 40;
        constexpr int kMaxLevels = 41;
        std::vector<std::vector<i128>> sPow(static_cast<size_t>(kMaxLevels));
        sPow[0] = sA;
        // 行列要素が上限を超えたらそこで打ち切る（その先の反復数は
        // 二分探索できない → 逐次へ）。値が小さいループは深い段まで要らない。
        int levelsOk = 1;
        while (levelsOk < kMaxLevels) {
            sPow[size_t(levelsOk)].assign(size_t(d) * d, 0);
            if (!mulRaw(sPow[size_t(levelsOk) - 1],
                        sPow[size_t(levelsOk) - 1],
                        sPow[size_t(levelsOk)]))
                break;
            ++levelsOk;
        }
        std::vector<i128> vcur(size_t(d), 0), vtmp(size_t(d), 0);
        auto evalD = [&](int64_t t, i128* outD) -> bool {
            vcur = x0;
            int64_t tt = t;
            int j = 0;
            while (tt > 0) {
                if (j >= levelsOk) return false;  // この反復数は評価不能
                if (tt & 1) {
                    for (int r = 0; r < d; ++r) {
                        i128 sum = 0;
                        for (int c = 0; c < d; ++c)
                            sum += sPow[size_t(j)][size_t(r) * d + c] *
                                   vcur[size_t(c)];
                        if (sum < -kEntryLim || sum > kEntryLim) return false;
                        vtmp[size_t(r)] = sum;
                    }
                    vcur.swap(vtmp);
                }
                tt >>= 1;
                ++j;
            }
            i128 D = 0;
            for (int c = 0; c < d; ++c) D += i128(diff[c]) * vcur[size_t(c)];
            if (D < -kEntryLim || D > kEntryLim) return false;
            *outD = D;
            return true;
        };
        auto stopAt = [&](int64_t t, bool* res) -> bool {
            i128 D = 0;
            if (!evalD(t, &D)) return false;
            const bool pred = cmpKind == 0 ? (D < 0) : (D > 0);
            *res = condKind == 1 ? pred : !pred;
            return true;
        };
        bool at0 = false;
        if (!stopAt(0, &at0)) return rejectBound();
        if (at0) return 2;
        int64_t lo = 0, hi = 0;
        bool hit = false;
        for (int64_t st = 1; st <= kMaxTrip; st <<= 1) {
            bool r = false;
            if (!stopAt(st, &r)) return rejectBound();
            if (r) {
                hi = st;
                hit = true;
                break;
            }
            lo = st;
        }
        if (!hit) return rejectBound();
        while (lo + 1 < hi) {
            const int64_t mid = lo + (hi - lo) / 2;
            bool r = false;
            if (!stopAt(mid, &r)) return rejectBound();
            if (r) hi = mid;
            else lo = mid;
        }
        trip = hi;
        if (trip <= 0) return 2;
        if (dbg)
            std::fprintf(stderr, "xyo-matpow: cond trip=%lld\n",
                         (long long)trip);
    } else {
        if (!(n > 0.4)) return 2;
        if (!(n < 4.5e18)) return 0;
        trip = int64_t(std::floor(n + 0.5));
        if (trip <= 0) return 2;
    }

    // --- pass 1b: 最終反復後の型が逐次と一致するか ------------------------
    // postSrc は 1 反復の型写像 T: T[v] = -1（算術で数値確定）または
    // 「v の新しい型は反復開始時の u の型」（u = T[v]）。T を trip 回
    // 合成した cur が、書き込み対象 v について初期値 cur[v] の型になる。
    // その初期値が文字列なら、その v は最後の反復で文字列を代入される
    // （例: fib の trip==1 で a が "1" のまま）ので数値書き戻しと食い違う。
    // T は単調（数値は数値へしか写らない）なので k+1 回で不動点に達する。
    {
        std::vector<int32_t> cur(static_cast<size_t>(k), int32_t(0));
        for (int64_t i = 0; i < k; ++i) cur[size_t(i)] = int32_t(i);
        const int64_t steps = trip < k + 1 ? trip : k + 1;
        for (int64_t st = 0; st < steps; ++st) {
            std::vector<int32_t> next(static_cast<size_t>(k), int32_t(-1));
            for (int64_t i = 0; i < k; ++i) {
                const int32_t t2 = postSrc[i];
                next[size_t(i)] =
                    t2 < 0 ? int32_t(-1) : cur[size_t(t2)];
            }
            cur.swap(next);
        }
        for (int64_t i = 0; i < k; ++i) {
            if (!writ[i]) continue;
            const int32_t src = cur[size_t(i)];
            if (src >= 0 && stringInit[size_t(src)]) {
                if (dbg)
                    std::fprintf(stderr,
                                 "xyo-matpow: type parity fail v=%lld\n",
                                 (long long)i);
                return 0;
            }
        }
    }

    // --- pass 2: 範囲証明（|A| の最大値ラダー） ---------------------------
    // 逐次実行の全中間値を押さえるには、全 e ≤ trip について
    //   |v_e| = |A^e·x0| ≤ |A|^e·|x0| ≤ W(trip) = max_{e≤trip} |A|^e·|x0|
    // が必要。|A| は非負なので W は次の再帰で **厳密に**（上界として）計算できる:
    //   W(0) = |x0|
    //   W(2t)     = max(W(t), |A|^t · W(t))      … e ≤ 2t を t 以下と t+1..2t に分割
    //   W(2t+1)   = max(W(2t), |A|^(2t) · |A|·|x0|)
    // これは A の符号に依存しない（負の係数・引き算・カウントダウンも可）。
    // A が非負なら上界は厳密値に一致する。
    const i128 cap = i128(kEntryLim);
    auto mul = [&](const std::vector<i128>& A, const std::vector<i128>& B,
                   std::vector<i128>& C) -> bool {
        for (int r = 0; r < d; ++r)
            for (int c = 0; c < d; ++c) {
                i128 sum = 0;
                for (int l = 0; l < d; ++l)
                    sum += A[size_t(r) * d + l] * B[size_t(l) * d + c];
                if (sum < -cap || sum > cap) return false;
                C[size_t(r) * d + c] = sum;
            }
        return true;
    };
    auto mulVec = [&](const std::vector<i128>& A,
                      const std::vector<i128>& v,
                      std::vector<i128>& o) -> bool {
        for (int r = 0; r < d; ++r) {
            i128 sum = 0;
            for (int c = 0; c < d; ++c)
                sum += A[size_t(r) * d + c] * v[size_t(c)];
            if (sum < -cap || sum > cap) return false;
            o[size_t(r)] = sum;
        }
        return true;
    };

    std::vector<i128> absA(size_t(d) * d, 0);
    for (int i = 0; i < d * d; ++i) {
        const i128 v = i128(mat[i]);
        absA[size_t(i)] = v < 0 ? -v : v;
    }
    std::vector<i128> absX(size_t(d), 0);
    for (int i = 0; i < d; ++i)
        absX[size_t(i)] = x0[size_t(i)] < 0 ? -x0[size_t(i)] : x0[size_t(i)];

    // |A|^(2^j) を用意（j = 0..msb(trip)）
    int msb = 0;
    while ((int64_t(1) << (msb + 1)) <= trip && msb < 62) ++msb;
    std::vector<std::vector<i128>> powAbs(size_t(msb) + 1);
    powAbs[0] = absA;
    for (int j = 1; j <= msb; ++j) {
        powAbs[size_t(j)].assign(size_t(d) * d, 0);
        if (!mul(powAbs[size_t(j) - 1], powAbs[size_t(j) - 1],
                 powAbs[size_t(j)]))
            return rejectBound();
    }
    const std::vector<i128> w1 = [&] {
        std::vector<i128> o(size_t(d), 0);
        for (int r = 0; r < d; ++r) {
            i128 sum = 0;
            for (int c = 0; c < d; ++c)
                sum += absA[size_t(r) * d + c] * absX[size_t(c)];
            if (sum > cap) sum = cap + 1;
            o[size_t(r)] = sum;
        }
        return o;
    }();

    std::vector<i128> P(size_t(d) * d, 0), Pabs(size_t(d) * d, 0),
        tmp(size_t(d) * d), sq(size_t(d) * d), W = absX, Wt(size_t(d), 0);
    for (int i = 0; i < d; ++i) {
        P[size_t(i) * d + i] = 1;
        Pabs[size_t(i) * d + i] = 1;
    }
    for (int j = msb; j >= 0; --j) {
        // e ≤ 2·prefix: |A|^prefix·W まで伸ばす
        if (!mulVec(Pabs, W, Wt)) return rejectBound();
        for (int i = 0; i < d; ++i)
            if (Wt[size_t(i)] > W[size_t(i)]) W[size_t(i)] = Wt[size_t(i)];
        // prefix ← 2·prefix
        if (!mul(Pabs, Pabs, sq)) return rejectBound();
        Pabs.swap(sq);
        if (!mul(P, P, sq)) return rejectBound();
        P.swap(sq);
        if ((trip >> j) & 1) {
            // e = 2·prefix+1: |A|^(2p)·(|A||x0|)
            if (!mulVec(Pabs, w1, Wt)) return rejectBound();
            for (int i = 0; i < d; ++i)
                if (Wt[size_t(i)] > W[size_t(i)]) W[size_t(i)] = Wt[size_t(i)];
            if (!mul(Pabs, absA, sq)) return rejectBound();
            Pabs.swap(sq);
            if (!mul(P, sA, sq)) return rejectBound();
            P.swap(sq);
        }
        // 早期打切り: 既に厳密性の上限を超えていれば逐次へ
        for (int i = 0; i < d; ++i)
            if (W[size_t(i)] > i128(int64_t(kVarLim))) return rejectBound();
    }

    // --- pass 3: 総合範囲証明 --------------------------------------------
    // 文中間値（部分和）は各文の L1 ノルム × その時点の |v| 以下。文ノルム
    // 積 stmtBound は全因子 ≥ 1 なので、逐次の中間値は stmtBound·Wmax 以下。
    i128 Wmax = 0;
    for (i128 v : W) if (v > Wmax) Wmax = v;
    const i128 bound = i128(stmtBound < 1 ? 1 : stmtBound) * Wmax;
    if (dbg)
        std::fprintf(stderr, "xyo-matpow: Wmax=%lld sb=%lld bound=%.3g\n",
                     (long long)Wmax, (long long)stmtBound, double(bound));
    if (bound > kExactLim) return rejectBound();

    // --- pass 4: r = P · x0 -----------------------------------------------
    {
        std::vector<i128> r(size_t(d), 0);
        if (!mulVec(P, x0, r)) return rejectBound();
        for (int64_t i = 0; i < k; ++i) {
            if (r[size_t(i)] < -i128(int64_t(kVarLim)) ||
                r[size_t(i)] > i128(int64_t(kVarLim)))
                return rejectBound();
            out[size_t(i)] = double(int64_t(r[size_t(i)]));
        }
    }
    ++xyo_matpow_runs;
    if (dbg) {
        std::fprintf(stderr, "xyo-matpow: apply trip=%lld ->", (long long)trip);
        for (int64_t i = 0; i < k; ++i)
            std::fprintf(stderr, " %lld%s%g", (long long)i,
                         writ[i] ? "=" : "(ro)=", out[size_t(i)]);
        std::fprintf(stderr, "\n");
    }
    return 1;
}

}  // extern "C"
