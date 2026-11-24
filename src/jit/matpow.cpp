// xyo-cpp: matpow.cpp — 線形漸化式ループの検出器。
//
// 本体の set/change を「拡大状態ベクトル [v;1] の affine 変換」として
// 順に合成し、整数行列 (M, b) を作る。全て int64 の厳密演算（途中は
// __int128 で範囲検査）で、非整数係数・非 affine 形は拒否する。
//
// 構造:
//   pass 0  本体で使われる全変数を先に登録して行列次元 k を固定する
//           （後から変数が増えると中間行列の次元がずれるため）。
//   pass 1  文列を affine 合成 → (k+1)x(k+1) 行列と文ノルム積 stmtBound。
//
// stmtBound（実行時の範囲証明に使う保守的上界）:
//   * set/change 文では「係数絶対和 + |定数|」がその文の L1 増大率。
//   * 内側 repeat（定数 t 回）を畳むときは内側文ノルム積の t 乗。
//   逐次評価の任意の文中間値 ≤ stmtBound × (ループ反復中の値の上界)
//   で押さえられる（文は順に適用される積変換なので）。
#include "matpow.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "../core/cast.hpp"
#include "../core/sstr.hpp"
#include "../core/value.hpp"
#include "../model/blocks.hpp"

namespace xyo::jit {
namespace {

using i128 = __int128;

constexpr int64_t kCap = kMatPowCoeffLimit;        // 係数・行列要素の上限
constexpr int64_t kBoundCap = int64_t(1) << 60;    // stmtBound の蓄積上限
constexpr int kMaxDepth = 4;                       // 内側 repeat の畳み込み深さ
constexpr int kMaxStmts = 512;                     // 展開後の文数上限

bool capCheck(i128 v) { return v >= -i128(kCap) && v <= i128(kCap); }

struct Aff {
    std::vector<i128> c;  // 状態変数係数（k 個、変数登録後に resize する）
    i128 k = 0;           // 定数項
};

struct Analyzer {
    Blocks& blocks;
    MatPowPlan plan;
    std::unordered_map<std::string, int> index;
    i128 stmtBound = 1;
    std::string why;
    int depth = 0;
    /// pass 0 では新規変数を登録、pass 1 では未登録変数を拒否する。
    bool collect = true;

    int k() const { return int(plan.vars.size()); }

    bool bad(const std::string& msg) {
        if (why.empty()) why = msg;
        return false;
    }
    bool growBound(i128 f) {
        if (f < 1) f = 1;
        stmtBound *= f;
        return stmtBound <= i128(kBoundCap);
    }

    /// 変数を状態ベクトルへ登録（collect 時）/ 既存 index を返す。
    /// -1 = 拒否（匿名・上限・collect 後の新規出現）。
    int varIndex(const BlockField& f) {
        if (f.id.empty()) return -1;
        auto it = index.find(f.id);
        if (it != index.end()) return it->second;
        if (!collect || int(plan.vars.size()) >= kMatPowMaxVars) return -1;
        const int i = int(plan.vars.size());
        index.emplace(f.id, i);
        plan.vars.push_back(&f);
        plan.written.push_back(0);
        plan.postSrc.push_back(int32_t(i));  // 初期値の型を引き継ぐ
        return i;
    }

    const Block* inputBlock(const BlockInput& in) {
        const std::string& id = !in.block.empty() ? in.block : in.shadow;
        if (id.empty()) return nullptr;
        return blocks.getBlock(id);
    }

    // --- pass 0: 変数収集 -------------------------------------------------

    /// reporter 木に出てくる data_variable を全て登録する。
    /// 文の TARGET/入力ツリー両方を見る（内側 repeat も再帰）。
    void collectInputs(const Block* b) {
        if (!b) return;
        for (const auto& [name, in] : b->inputs.items) {
            (void)name;
            collectReporter(inputBlock(in));
        }
    }
    void collectReporter(const Block* b) {
        if (!b) return;
        if (b->opcode == "data_variable") {
            auto it = b->fields.find("VARIABLE");
            if (it != b->fields.end()) (void)varIndex(it->second);
        }
        for (const auto& [name, in] : b->inputs.items) {
            (void)name;
            collectReporter(inputBlock(in));
        }
    }
    /// 文鎖を先読みして変数を全部登録する（内側 repeat の本体も潜る）。
    void collectChain(const Block* head) {
        for (const Block* s = head; s;) {
            const std::string& op = s->opcode;
            if (op == "data_setvariableto" ||
                op == "data_changevariableby") {
                auto it = s->fields.find("VARIABLE");
                if (it != s->fields.end()) (void)varIndex(it->second);
            }
            collectInputs(s);
            // control_* の SUBSTACK も一応潜る（repeat 以外は pass 1 で
            // 拒否されるが、変数登録だけなら害は無い）。
            for (const auto& [name, in] : s->inputs.items) {
                if (name.rfind("SUBSTACK", 0) != 0) continue;
                if (const Block* sh = blocks.getBlock(in.block))
                    collectChain(sh);
            }
            if (!s->next.empty()) s = blocks.getBlock(s->next);
            else s = nullptr;
        }
    }

    // --- pass 1: affine 式の走査 -----------------------------------------

    /// reporter 式を affine 形へ。整数係数・整数定数のみ受理。
    bool affine(const BlockInput& in, Aff& out) {
        if (in.block.empty() && in.shadow.empty())
            return bad("empty input");
        out.c.assign(size_t(k()), i128(0));
        out.k = 0;
        return affineExpr(inputBlock(in), out);
    }
    bool affineInput(const Block* b, const char* name, Aff& out) {
        const BlockInput* in = b->inputs.find(name);
        if (!in) return bad(std::string("missing ") + name);
        if (!affine(*in, out)) return false;
        return true;
    }

    bool affineExpr(const Block* b, Aff& out) {
        if (!b) return bad("missing reporter");
        const std::string& op = b->opcode;
        if (op == "math_number" || op == "math_integer" ||
            op == "math_whole_number" || op == "math_positive_number" ||
            op == "math_angle" || op == "text") {
            auto it = b->fields.find(op == "text" ? "TEXT" : "NUM");
            if (it == b->fields.end()) return bad("numeric shadow w/o field");
            const double d = cast::toNumber(it->second.value);
            // 整数でないリテラルは拒否（逐次の丸めは行列累乗で再現不可）。
            // 文字列リテラルは cast::toNumber 経由 — "3" は 3、
            // "abc"/"" は 0 として厳密に扱える。
            if (!std::isfinite(d) || d != std::floor(d) ||
                d > double(kCap) || d < -double(kCap))
                return bad("non-integer literal");
            out.c.assign(size_t(k()), i128(0));
            out.k = i128(int64_t(d));
            return true;
        }
        if (op == "data_variable") {
            auto it = b->fields.find("VARIABLE");
            if (it == b->fields.end()) return bad("variable w/o field");
            const int i = varIndex(it->second);
            if (i < 0) return bad("unregistered variable");
            out.c.assign(size_t(k()), i128(0));
            out.c[size_t(i)] = 1;
            out.k = 0;
            return true;
        }
        if (op == "operator_add" || op == "operator_subtract") {
            Aff a, e;
            if (!affineInput(b, "NUM1", a) || !affineInput(b, "NUM2", e))
                return false;
            const i128 sgn = op == "operator_add" ? 1 : -1;
            out.c.assign(size_t(k()), i128(0));
            for (int i = 0; i < k(); ++i)
                out.c[size_t(i)] = a.c[size_t(i)] + sgn * e.c[size_t(i)];
            out.k = a.k + sgn * e.k;
            return checkAll(out) ? true : bad("coefficient overflow");
        }
        if (op == "operator_multiply") {
            Aff a, e;
            if (!affineInput(b, "NUM1", a) || !affineInput(b, "NUM2", e))
                return false;
            const bool aConst = isConst(a), eConst = isConst(e);
            out.c.assign(size_t(k()), i128(0));
            if (aConst && eConst) {
                out.k = a.k * e.k;
                return checkAll(out) ? true : bad("coefficient overflow");
            }
            if (!aConst && !eConst)
                return bad("nonlinear: var * var");
            const Aff& src = aConst ? e : a;
            const i128 f = aConst ? a.k : e.k;
            for (int i = 0; i < k(); ++i)
                out.c[size_t(i)] = src.c[size_t(i)] * f;
            out.k = src.k * f;
            return checkAll(out) ? true : bad("coefficient overflow");
        }
        if (op == "operator_divide") {
            Aff a, e;
            if (!affineInput(b, "NUM1", a) || !affineInput(b, "NUM2", e))
                return false;
            // 分母は非零定数、分子は定数 or 全係数が分母で割り切れる
            // affine 形のみ受理（x/2 のような逐次丸めは再現不可）。
            if (!isConst(e) || e.k == 0)
                return bad("non-affine divide");
            const i128 d = e.k;
            for (int i = 0; i < k(); ++i)
                if (a.c[size_t(i)] % d != 0)
                    return bad("non-exact divide");
            if (a.k % d != 0) return bad("non-exact divide");
            out.c.assign(size_t(k()), i128(0));
            for (int i = 0; i < k(); ++i) out.c[size_t(i)] = a.c[size_t(i)] / d;
            out.k = a.k / d;
            return true;
        }
        if (op == "operator_mathop") {
            // floor/ceiling/round は整数値に対して恒等。範囲証明が全中間値
            // の整数性を保証するので、引数が affine なら同じ affine を返す。
            auto fit = b->fields.find("OPERATOR");
            if (fit == b->fields.end()) return bad("mathop without OPERATOR");
            const std::string fn =
                fit->second.value.isString() && fit->second.value.asStrPtr()
                    ? SStr::toUtf8(fit->second.value.asStrPtr()->view())
                    : std::string();
            if (fn != "floor" && fn != "ceiling" && fn != "round")
                return bad("non-affine mathop: " + fn);
            const BlockInput* in = b->inputs.find("NUM");
            if (!in) return bad("mathop without NUM");
            return affine(*in, out);
        }
        return bad("non-affine op: " + op);
    }

    static bool isConst(const Aff& a) {
        for (i128 x : a.c) if (x != 0) return false;
        return true;
    }
    static bool checkAll(const Aff& a) {
        if (!capCheck(a.k)) return false;
        for (i128 x : a.c) if (!capCheck(x)) return false;
        return true;
    }
    /// 式の係数絶対和 + |定数|（文中間値の増大率として使う）
    static i128 l1(const Aff& a) {
        i128 s = a.k < 0 ? -a.k : a.k;
        for (i128 x : a.c) s += x < 0 ? -x : x;
        return s;
    }

    // --- (M,b): v_cur = M·v0 + b の合成（行列は k x k）-------------------

    /// 文 v_j := aff(v_cur) を合成へ適用:
    ///   M' の j 行 = Σ aff.c[r]·M[r]、b'_j = Σ aff.c[r]·b[r] + aff.k
    bool applyAssign(int j, const Aff& aff, std::vector<i128>& M,
                     std::vector<i128>& bv) {
        const int kv = k();
        for (int c = 0; c < kv; ++c) {
            i128 s = 0;
            for (int r = 0; r < kv; ++r)
                s += aff.c[size_t(r)] * M[size_t(r) * kv + c];
            if (!capCheck(s)) return bad("matrix entry overflow");
            M[size_t(j) * kv + c] = s;
        }
        i128 s = aff.k;
        for (int r = 0; r < kv; ++r) s += aff.c[size_t(r)] * bv[size_t(r)];
        if (!capCheck(s)) return bad("matrix entry overflow");
        bv[size_t(j)] = s;
        return true;
    }

    /// 合成 (A,a)∘(B,c) = (A·B, A·c + a)。|要素| ≤ 2^60 で検査。
    static bool compose(const std::vector<i128>& A,
                        const std::vector<i128>& a,
                        const std::vector<i128>& B,
                        const std::vector<i128>& c,
                        std::vector<i128>& M, std::vector<i128>& bv, int n) {
        const i128 lim = i128(1) << 60;
        M.assign(size_t(n) * n, 0);
        bv.assign(size_t(n), 0);
        for (int r = 0; r < n; ++r) {
            i128 s = a[size_t(r)];
            for (int c = 0; c < n; ++c) {
                i128 t = 0;
                for (int l = 0; l < n; ++l)
                    t += A[size_t(r) * n + l] * B[size_t(l) * n + c];
                if (t < -lim || t > lim) return false;
                M[size_t(r) * n + c] = t;
            }
            for (int l = 0; l < n; ++l)
                s += A[size_t(r) * n + l] * c[size_t(l)];
            if (s < -lim || s > lim) return false;
            bv[size_t(r)] = s;
        }
        return true;
    }

    /// (A,a)^t（binary exponentiation）。失敗 = 要素が 2^60 超。
    static bool applyPow(const std::vector<i128>& A,
                         const std::vector<i128>& a, int64_t t,
                         std::vector<i128>& M, std::vector<i128>& bv,
                         int n) {
        M.assign(size_t(n) * n, 0);
        bv.assign(size_t(n), 0);
        for (int i = 0; i < n; ++i) M[size_t(i) * n + i] = 1;
        std::vector<i128> cur = A, curb = a, tm, tb;
        int64_t e = t;
        while (e > 0) {
            if (e & 1) {
                if (!compose(cur, curb, M, bv, tm, tb, n)) return false;
                M.swap(tm);
                bv.swap(tb);
            }
            e >>= 1;
            if (e > 0) {
                if (!compose(cur, curb, cur, curb, tm, tb, n)) return false;
                cur.swap(tm);
                curb.swap(tb);
            }
        }
        return true;
    }

    /// 文列を走査して合成 (M,b) を作る。内側 repeat（定数回数）は
    /// 畳み込んで 1 文として扱う。M/bv は k x k / k。
    bool analyzeChain(const Block* head, std::vector<i128>& M,
                      std::vector<i128>& bv, int& nStmts) {
        const int kv = k();
        for (const Block* s = head; s;) {
            const std::string& op = s->opcode;
            if (++nStmts > kMaxStmts) return bad("too many statements");
            if (op == "data_setvariableto" ||
                op == "data_changevariableby") {
                auto it = s->fields.find("VARIABLE");
                if (it == s->fields.end())
                    return bad("set/change without VARIABLE");
                const int j = varIndex(it->second);
                if (j < 0) return bad("unregistered variable");
                Aff aff;
                const BlockInput* vin = s->inputs.find("VALUE");
                if (!vin) return bad("set/change without VALUE");
                if (!affine(*vin, aff)) return false;
                if (op == "data_changevariableby") {
                    aff.c[size_t(j)] += 1;
                    if (!capCheck(aff.c[size_t(j)]))
                        return bad("coefficient overflow");
                }
                // 型の出所: 裸のコピー `set v to w` は w の型を引き継ぎ、
                // それ以外（算術・change）は数値に確定する。
                plan.written[size_t(j)] = 1;
                if (op == "data_setvariableto") {
                    int32_t src = -1;
                    int copyOf = -1, nonzero = 0;
                    for (int r = 0; r < kv; ++r) {
                        const i128 c = aff.c[size_t(r)];
                        if (c == 0) continue;
                        ++nonzero;
                        if (c == 1) copyOf = r;
                    }
                    if (nonzero == 1 && copyOf >= 0 && aff.k == 0)
                        src = plan.postSrc[size_t(copyOf)];
                    plan.postSrc[size_t(j)] = src;
                } else {
                    plan.postSrc[size_t(j)] = -1;
                }
                if (!applyAssign(j, aff, M, bv)) return false;
                if (!growBound(l1(aff)))
                    return bad("statement bound overflow");
            } else if (op == "control_repeat" && depth < kMaxDepth) {
                // 内側 repeat:
                //  * TIMES が数値定数 → 内側行列の t 乗を厳密に畳む。
                //  * TIMES が affine（状態依存）→ 本体が「定数加算だけ」なら
                //    A - I が定数列だけになるので A^t = I + t·(A-I) が成り立ち、
                //    t が affine のまま全体も affine に畳める（t ≥ 0 を要求）。
                const BlockInput* tin = s->inputs.find("TIMES");
                const Block* tb = tin ? inputBlock(*tin) : nullptr;
                if (!tb) return bad("inner repeat without TIMES");
                const bool constTimes =
                    tb->opcode == "math_number" ||
                    tb->opcode == "math_integer" ||
                    tb->opcode == "math_whole_number" ||
                    tb->opcode == "math_positive_number" ||
                    tb->opcode == "math_angle";
                double n = 0;
                Aff countAff;
                const bool dynamic = !constTimes;
                if (constTimes) {
                    auto fit = tb->fields.find("NUM");
                    if (fit == tb->fields.end())
                        return bad("inner repeat TIMES not constant");
                    n = cast::toNumber(fit->second.value);
                    if (!std::isfinite(n) || n < -9.0e15 || n > 9.0e15)
                        return bad("inner repeat TIMES out of range");
                } else {
                    if (!affine(*tin, countAff))
                        return bad("inner repeat TIMES not affine");
                    // 実行時に t ≥ 0 を保証する（係数・定数が非負で、
                    // かつ全状態変数 ≥ 0 のとき）。
                    if (countAff.k < 0)
                        return bad("inner repeat count may be negative");
                    for (i128 c : countAff.c)
                        if (c < 0)
                            return bad("inner repeat count may be negative");
                }
                const int64_t t = constTimes
                    ? (n < 0.0 ? 0 : int64_t(std::floor(n + 0.5)))
                    : 0;
                const BlockInput* sin = s->inputs.find("SUBSTACK");
                const Block* shead =
                    sin ? blocks.getBlock(sin->block) : nullptr;
                ++depth;
                const i128 savedBound = stmtBound;
                stmtBound = 1;
                const std::vector<char> savedWritten = plan.written;
                const std::vector<int32_t> savedPost = plan.postSrc;
                std::vector<i128> Mi(size_t(kv) * kv, i128(0)),
                    bi(size_t(kv), i128(0));
                for (int i = 0; i < kv; ++i) Mi[size_t(i) * kv + i] = 1;
                int innerStmts = 0;
                const bool ok =
                    !shead || analyzeChain(shead, Mi, bi, innerStmts);
                const i128 innerBound = stmtBound;
                stmtBound = savedBound;
                --depth;
                if (!ok) return false;
                if (dynamic) {
                    // 定数加算ループか: 状態ブロックが恒等、つまり
                    // 各変数は「自分 + 定数」だけ増える。
                    for (int r = 0; r < kv; ++r)
                        for (int c = 0; c < kv; ++c) {
                            const i128 want = r == c ? 1 : 0;
                            if (Mi[size_t(r) * kv + c] != want)
                                return bad("inner repeat count is dynamic "
                                           "and body is not constant-add");
                        }
                    // v_out = v + t·s（s = bi）。t = a·v + c0（affine）。
                    // 現在の合成 (M,bv) に代入して affine のまま畳む:
                    //   M' = M + s·(a·M),  b' = bv + s·(a·bv + c0)
                    std::vector<i128> aM(size_t(kv), 0);
                    i128 aB = countAff.k;
                    for (int l = 0; l < kv; ++l) {
                        i128 sum = 0;
                        for (int j = 0; j < kv; ++j)
                            sum += countAff.c[size_t(j)] *
                                   M[size_t(j) * kv + l];
                        aM[size_t(l)] = sum;
                    }
                    for (int j = 0; j < kv; ++j)
                        aB += countAff.c[size_t(j)] * bv[size_t(j)];
                    std::vector<i128> M2 = M, b2 = bv;
                    for (int i = 0; i < kv; ++i) {
                        const i128 si = bi[size_t(i)];
                        if (si == 0) continue;
                        for (int l = 0; l < kv; ++l) {
                            M2[size_t(i) * kv + l] += si * aM[size_t(l)];
                            if (!capCheck(M2[size_t(i) * kv + l]))
                                return bad("inner affine compose overflow");
                        }
                        b2[size_t(i)] += si * aB;
                        if (!capCheck(b2[size_t(i)]))
                            return bad("inner affine compose overflow");
                    }
                    M.swap(M2);
                    bv.swap(b2);
                    // t == 0 なら内側は走らない → 型は変化しないとみなす
                    // （保守側: 文字列初期値は最終チェックで拒否される）。
                    plan.postSrc = savedPost;
                    for (size_t i = 0; i < plan.written.size(); ++i)
                        plan.written[i] =
                            char(savedWritten[i] || plan.written[i]);
                    plan.needsNonnegState = true;
                    if (!growBound(innerBound < 1 ? 1 : innerBound))
                        return bad("statement bound overflow");
                } else {
                    std::vector<i128> Ti, ti, M2, b2;
                    if (t <= 0) {
                        plan.written = savedWritten;
                        plan.postSrc = savedPost;
                        Ti.assign(size_t(kv) * kv, 0);
                        ti.assign(size_t(kv), 0);
                        for (int i = 0; i < kv; ++i)
                            Ti[size_t(i) * kv + i] = 1;
                    } else {
                        if (!applyPow(Mi, bi, t, Ti, ti, kv))
                            return bad("inner matrix pow overflow");
                        for (i128 v : Ti)
                            if (!capCheck(v))
                                return bad("inner matrix entry overflow");
                        for (i128 v : ti)
                            if (!capCheck(v))
                                return bad("inner matrix entry overflow");
                    }
                    if (!compose(Ti, ti, M, bv, M2, b2, kv))
                        return bad("inner compose overflow");
                    M.swap(M2);
                    bv.swap(b2);
                    // 文ノルム積: 内側ループ分は innerBound の t 乗
                    i128 f = 1;
                    for (int64_t i = 0; i < t; ++i) {
                        f *= innerBound < 1 ? 1 : innerBound;
                        if (f > i128(kBoundCap)) {
                            f = i128(kBoundCap);
                            break;
                        }
                    }
                    if (!growBound(f)) return bad("statement bound overflow");
                }
            } else {
                return bad("statement op: " + op);
            }
            if (!s->next.empty()) s = blocks.getBlock(s->next);
            else s = nullptr;
        }
        return true;
    }
};

}  // namespace

std::optional<MatPowPlan> analyzeMatPow(Blocks& blocks, const Block* loop,
                                      std::string* why) {
    auto reject = [&](const std::string& msg) -> std::optional<MatPowPlan> {
        if (why) *why = msg;
        return std::nullopt;
    };
    if (!loop) return reject("no loop");
    const bool isRepeat = loop->opcode == "control_repeat";
    const bool isUntil = loop->opcode == "control_repeat_until";
    const bool isWhile = loop->opcode == "control_while";
    if (!isRepeat && !isUntil && !isWhile)
        return reject("not a counted/conditional repeat");
    const BlockInput* sub = loop->inputs.find("SUBSTACK");
    if (!sub || sub->block.empty()) return reject("empty body");
    const Block* head = blocks.getBlock(sub->block);
    if (!head) return reject("body head missing");

    Analyzer an{blocks, {}, {}, 1, {}, 0, true};

    // pass 0: 全変数を登録して k を固定
    an.collectChain(head);
    const int kv = an.k();
    if (kv == 0) return reject("no variables");

    // pass 1: 合成
    an.collect = false;
    std::vector<i128> M(size_t(kv) * kv, i128(0)),
        bv(size_t(kv), i128(0));
    for (int i = 0; i < kv; ++i) M[size_t(i) * kv + i] = 1;
    int nStmts = 0;
    if (!an.analyzeChain(head, M, bv, nStmts))
        return reject(an.why);

    // (kv)x(kv) + affine 列 → (kv+1)²（int64）
    const int dim = kv + 1;
    an.plan.loop = loop;
    an.plan.mat.assign(size_t(dim) * dim, 0);
    for (int r = 0; r < kv; ++r) {
        for (int c = 0; c < kv; ++c)
            an.plan.mat[size_t(r) * dim + c] =
                int64_t(M[size_t(r) * kv + c]);
        an.plan.mat[size_t(r) * dim + kv] = int64_t(bv[size_t(r)]);
    }
    an.plan.mat[size_t(kv) * dim + kv] = 1;
    an.plan.stmtBound = int64_t(an.stmtBound);

    // --- 条件付きループ: 条件が単調な affine 比較なら反復回数を二分探索 ---
    if (!isRepeat) {
        const BlockInput* cin = loop->inputs.find("CONDITION");
        const Block* cb =
            cin ? (!cin->block.empty() ? blocks.getBlock(cin->block)
                                       : blocks.getBlock(cin->shadow))
                : nullptr;
        if (!cb) return reject("missing CONDITION");
        const std::string& cop = cb->opcode;
        if (cop != "operator_lt" && cop != "operator_gt")
            return reject("condition op: " + cop);
        Aff L, R;
        if (!an.affineInput(cb, "OPERAND1", L) ||
            !an.affineInput(cb, "OPERAND2", R))
            return reject(an.why.empty() ? "condition not affine" : an.why);
        if (an.k() != kv)
            return reject("condition uses new variables");
        const int dim = kv + 1;
        std::vector<i128> d(size_t(dim), 0);
        for (int j = 0; j < kv; ++j) d[size_t(j)] = L.c[size_t(j)] - R.c[size_t(j)];
        d[size_t(kv)] = L.k - R.k;
        // g = d·Ã - d。これが定数なら D(e) は厳密に単調、状態依存なら
        // 「v ≥ 0 かつ g の符号が一定」で単調（Ã ≥ 0 をコンパイル時に要求）。
        std::vector<i128> g(size_t(dim), 0);
        for (int c = 0; c < dim; ++c) {
            i128 sum = 0;
            for (int r = 0; r < dim; ++r)
                sum += d[size_t(r)] * i128(an.plan.mat[size_t(r) * dim + c]);
            g[size_t(c)] = sum - d[size_t(c)];
        }
        bool gStateZero = true;
        for (int j = 0; j < kv; ++j)
            if (g[size_t(j)] != 0) gStateZero = false;
        int mono = 0;
        if (gStateZero) {
            if (g[size_t(kv)] > 0) mono = 1;
            else if (g[size_t(kv)] < 0) mono = -1;
        } else {
            bool pos = g[size_t(kv)] >= 0, neg = g[size_t(kv)] <= 0;
            for (int j = 0; j < kv; ++j) {
                if (g[size_t(j)] < 0) pos = false;
                if (g[size_t(j)] > 0) neg = false;
            }
            if (pos && !neg) mono = 1;
            else if (neg && !pos) mono = -1;
            if (mono != 0) {
                // v ≥ 0 を反復中ずっと保つため Ã ≥ 0 を要求する。
                for (int64_t m : an.plan.mat)
                    if (m < 0)
                        return reject("condition needs nonneg matrix");
                an.plan.needsNonnegState = true;
            }
        }
        if (mono == 0) return reject("condition not monotone");
        // 停止条件: until は条件が真、while は偽になった最初の反復。
        //   until + <   → D が非増加
        //   until + >   → D が非減少
        //   while + <   → D が非減少
        //   while + >   → D が非増加
        const int want = isUntil ? (cop == "operator_lt" ? -1 : 1)
                                 : (cop == "operator_lt" ? 1 : -1);
        if (mono != want) return reject("condition monotonicity mismatch");
        an.plan.condKind = isUntil ? 1 : 2;
        an.plan.cmpKind = cop == "operator_lt" ? 0 : 1;
        an.plan.diff.resize(size_t(dim));
        for (int j = 0; j < dim; ++j)
            an.plan.diff[size_t(j)] = int64_t(d[size_t(j)]);
    }

    if (why) why->clear();
    return an.plan;
}
}  // namespace xyo::jit
