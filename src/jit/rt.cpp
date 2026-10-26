// xyo-cpp: JIT 生成コードから呼ばれる extern "C" ランタイムヘルパ。
// jsexecute.js runtimeFunctions + cast/compare/list 系の移植。
// yield = fiber switch — C++ スタックが保持されるため、ヘルパ内部で
// 中断してもローカル状態は維持される（executeInCompatibilityLayer 相当）。
#include "jit.hpp"
#include "rtprof.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "../core/cast.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../engine/block_utility.hpp"
#include "../engine/fiber.hpp"
#include "../engine/runtime.hpp"
#include "../engine/sequencer.hpp"
#include "../engine/thread.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "../rt/runtime_helpers.hpp"

using namespace xyo;

namespace xyo {

} // namespace xyo

namespace {

Value toV(uint64_t bits) { Value v; v.bits = bits; return v; }
uint64_t toB(Value v) { return v.bits; }

BlockUtility& jitUtil(JitCtx* c) {
    // 並列 worker 対策として thread_local（並列適格スクリプトは
    // compat 経路に到達しないが、保険として隔離する）
    static thread_local BlockUtility util;
    util.sequencer = c->sequencer;
    util.thread = c->thread;
    return util;
}

bool isStuck(JitCtx* c) {
    // JS jsexecute: sequencer.timer.timeElapsed() > 500（tick 単位でリセット）。
    // dangerMode はスレッドごとの時間予算そのものが無いので時計も読まない
    // （warp ループの yield 点はそもそも IR から消えている）。
    if (c->runtime->dangerMode) return false;
    return c->sequencer->timer.timeElapsed() > c->runtime->warpTimeMs;
}

/// var 系ヘルパ別カウンタ（XYO_PROF 時に内容を表示）
std::atomic<uint64_t> g_varCtrs[4] = {};
struct VarCtrDump {
    VarCtrDump() {
        if (!std::getenv("XYO_PROF")) return;
        std::atexit([]{
            std::fprintf(stderr,
                "[var] jitVar=%llu get=%llu set=%llu change=%llu\n",
                (unsigned long long)g_varCtrs[0].load(),
                (unsigned long long)g_varCtrs[1].load(),
                (unsigned long long)g_varCtrs[2].load(),
                (unsigned long long)g_varCtrs[3].load());
        });
    }
} g_varCtrDump;

/// compat opcode 別呼出数（XYO_PROF 時）
std::unordered_map<std::string, uint64_t>& compatOps() {
    static auto* m = new std::unordered_map<std::string, uint64_t>;
    static int once = []{
        std::atexit([]{
            std::vector<std::pair<std::string,uint64_t>> v(
                compatOps().begin(), compatOps().end());
            std::sort(v.begin(), v.end(),
                      [](auto& a, auto& b){ return a.second > b.second; });
            for (auto& [k,n] : v)
                std::fprintf(stderr, "[compat] %s %llu\n", k.c_str(),
                             (unsigned long long)n);
        });
        return 0; }();
    (void)once;
    return *m;
}

} // namespace

extern "C" {

// --- 基本 -------------------------------------------------------------

void xyo_yield(JitCtx* c) {
    XYO_RT_PROF(kYield);
    c->fiber->yield();
}

int32_t xyo_is_stuck(JitCtx* c) { return isStuck(c) ? 1 : 0; }

/// loop 末尾の yield 点 — non-warp なら常に、warp なら stuck 時のみ
void xyo_loop_yield(JitCtx* c) {
    XYO_RT_PROF(kLoopY);
    if (!c->isWarp || isStuck(c)) xyo_yield(c);
}

// --- 値 ----------------------------------------------------------------

double xyo_to_num(uint64_t bits) { return cast::toNumber(toV(bits)); }

int32_t xyo_truthy(uint64_t bits) { return cast::toBoolean(toV(bits)) ? 1 : 0; }

uint64_t xyo_num(double d) { return toB(Value::numCanonical(d)); }

uint64_t xyo_undefined() { return toB(Value::undefined()); }

uint64_t xyo_to_str(uint64_t bits) {
    XYO_RT_PROF(kCast);
    return toB(Value::string(cast::toString(toV(bits))));
}

uint64_t xyo_add(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return toB(Value::numCanonical(cast::toNumber(toV(a)) +
                                   cast::toNumber(toV(b))));
}
uint64_t xyo_sub(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return toB(Value::numCanonical(cast::toNumber(toV(a)) -
                                   cast::toNumber(toV(b))));
}
uint64_t xyo_mul(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return toB(Value::numCanonical(cast::toNumber(toV(a)) *
                                   cast::toNumber(toV(b))));
}
uint64_t xyo_div(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return toB(Value::numCanonical(cast::toNumber(toV(a)) /
                                   cast::toNumber(toV(b))));
}
uint64_t xyo_mod(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    const double n = cast::toNumber(toV(a));
    const double m = cast::toNumber(toV(b));
    // JS mod: ((n % m) + m) % m — Scratch の剰余は常に正
    double r = std::fmod(n, m);
    r = std::fmod(r + m, m);
    if (r == 0) r = 0.0;  // -0 正規化
    return toB(Value::numCanonical(r));
}

int32_t xyo_lt(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return cast::compare(toV(a), toV(b)) < 0 ? 1 : 0;
}
int32_t xyo_gt(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return cast::compare(toV(a), toV(b)) > 0 ? 1 : 0;
}
static int valTag(Value v) {
    if (v.isNumber()) return 0;
    if (v.isBool()) return 1;
    if (v.isString()) return v.asStrPtr() ? 2 : 3;
    if (v.isColor()) return 4;
    return 5;  // undefined 他
}

int32_t xyo_eq(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    if (a == b) return 1;   // 同 bits は常に等値（kNan 含む）
    {
        Value va = toV(a), vb = toV(b);
        // 数値同士は compare の文字列/ゼロ判定を全部飛ばす。
        // 描画ループの色比較などホットパスで支配的（voxelRenderer の
        // display は 1 フレームに数千回）。
        if (va.isNumber() && vb.isNumber()) {
            double x = va.asNumberUnchecked();
            double y = vb.asNumberUnchecked();
            return x == y ? 1 : 0;
        }
        if (va.isString() && vb.isString() && va.asStrPtr() &&
            vb.asStrPtr()) {
            // numState は文字列内容の純関数 — 異なる state は
            // 必ず異なる内容 = 非等値（compare の文字列比較をスキップ）。
            // ただし Infinity リテラル（state4）は "infinity" 等の
            // case 違い（state2）と等しいためショートカット不可。
            uint32_t s1 = va.asStrPtr()->loadNumState();
            uint32_t s2 = vb.asStrPtr()->loadNumState();
            if (s1 && s2 && s1 != s2 && s1 != 4 && s2 != 4) return 0;
        }
    }
    return cast::compare(toV(a), toV(b)) == 0 ? 1 : 0;
}
int32_t xyo_not(uint64_t a) { return !cast::toBoolean(toV(a)) ? 1 : 0; }
int32_t xyo_and(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return (cast::toBoolean(toV(a)) && cast::toBoolean(toV(b))) ? 1 : 0;
}
int32_t xyo_or(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kArith);
    return (cast::toBoolean(toV(a)) || cast::toBoolean(toV(b))) ? 1 : 0;
}

uint64_t xyo_random(JitCtx*, uint64_t from, uint64_t to) {
    XYO_RT_PROF(kArith);
    // scratch3_operators.random 準拠（NaN 伝播・isInt 型判定・low/high 交換）
    Value fv = toV(from), tv = toV(to);
    const double nFrom = cast::toNumber(fv), nTo = cast::toNumber(tv);
    const double low = nFrom <= nTo ? nFrom : nTo;
    const double high = nFrom <= nTo ? nTo : nFrom;
    if (low == high) return toB(Value::numCanonical(low));
    if (cast::isInt(fv) && cast::isInt(tv))
        return toB(Value::numCanonical(rt::randomInt(low, high)));
    return toB(Value::numCanonical(rt::randomFloat(low, high)));
}

double xyo_mathop(JitCtx*, int32_t fn, double x) {
    XYO_RT_PROF(kArith);
    // scratch3_operators.mathop / math-util.tan 準拠
    constexpr double kPi = 3.14159265358979323846;
    switch (fn) {
    case 0: return std::fabs(x);
    case 1: return std::floor(x);
    case 2: return std::ceil(x);
    case 3: return std::sqrt(x);
    case 4: return std::round(std::sin(kPi * x / 180.0) * 1e10) / 1e10;
    case 5: return std::round(std::cos(kPi * x / 180.0) * 1e10) / 1e10;
    case 6: {
        // MathUtil.tan: %360、±90/270 は ±Infinity、他は 1e10 丸め
        double a = std::fmod(x, 360.0);
        if (a == -270.0 || a == 90.0) return INFINITY;
        if (a == -90.0 || a == 270.0) return -INFINITY;
        return std::round(std::tan(kPi * a / 180.0) * 1e10) / 1e10;
    }
    case 7: return std::asin(x) * 180.0 / kPi;
    case 8: return std::acos(x) * 180.0 / kPi;
    case 9: return std::atan(x) * 180.0 / kPi;
    case 10: return std::log(x);
    case 11: return std::log(x) / 2.302585092994046;
    case 12: return std::exp(x);
    case 13: return std::pow(10.0, x);
    default: return 0.0;
    }
}

double xyo_round(double x) {
    XYO_RT_PROF(kArith);
    // Scratch round: .5 は正方向（JS Math.round 相当）
    return std::round(x) + 0.0;
}

// --- 変数 --------------------------------------------------------------
// clone 対応のため実行時に ctx->target で解決する（JS lookupVariableById 相当）

Variable* xyo_jitVar(JitCtx* c, const char* id, const char16_t* name,
                     int list) {
    XYO_RT_PROF(kVar);
    if (RTProf::get().enabled) ++g_varCtrs[0];
    auto& cache = c->thread->jitVarCache;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    Variable* v = list ? c->target->lookupOrCreateList(id, name)
                       : c->target->lookupOrCreateVariable(id, name);
    cache.emplace(id, v);
    return v;
}

void xyo_varslot_init(JitCtx* c, uint64_t idx, const char* id,
                      const char16_t* name, int32_t isList) {
    Thread* t = c->thread;
    if (idx >= t->jitVarSlots.size()) t->jitVarSlots.resize(size_t(idx) + 1);
    t->jitVarSlots[size_t(idx)] = xyo_jitVar(c, id, name, isList);
    c->varSlotData = t->jitVarSlots.data();
}

uint64_t xyo_var_get(JitCtx* c, const char* id, const char16_t* name) {
    XYO_RT_PROF(kVar);
    if (RTProf::get().enabled) ++g_varCtrs[1];
    Variable* v = xyo_jitVar(c, id, name, false);
    return v ? toB(v->value) : toB(Value::undefined());
}

void xyo_var_set(JitCtx* c, const char* id, const char16_t* name,
                 uint64_t value) {
    XYO_RT_PROF(kVar);
    if (RTProf::get().enabled) ++g_varCtrs[2];
    Variable* v = xyo_jitVar(c, id, name, false);
    if (v) v->value = toV(value);
}

void xyo_var_change(JitCtx* c, const char* id, const char16_t* name,
                    uint64_t value) {
    XYO_RT_PROF(kVar);
    if (RTProf::get().enabled) ++g_varCtrs[3];
    Variable* v = xyo_jitVar(c, id, name, false);
    if (v) {
        const double d = cast::toNumber(v->value) + cast::toNumber(toV(value));
        v->value = Value::numCanonical(d);
    }
}

// --- 手続き --------------------------------------------------------------

uint64_t xyo_arg_get(JitCtx* c, uint64_t index) {
    XYO_RT_PROF(kArg);
    return index < c->numArgs ? toB(c->args[index])
                              : toB(Value::undefined());
}

// --- 制御 -----------------------------------------------------------------

void xyo_rt_wait(JitCtx* c, double secs) {
    XYO_RT_PROF(kOther);
    // JS jsgen CONTROL_WAIT: timer start → redraw → yieldNotWarp →
    // while (elapsed < ms) yieldStuckOrNotWarp
    Timer t([c] { return c->runtime->currentMSecs; });
    t.start();
    const double ms = std::max(0.0, secs * 1000.0);
    c->runtime->redrawRequested = true;
    if (!c->isWarp) xyo_yield(c);
    while (t.timeElapsed() < ms) {
        if (!c->isWarp || isStuck(c)) xyo_yield(c);
    }
}

void xyo_stop_all(JitCtx* c) {
    XYO_RT_PROF(kOther);
    c->runtime->stopAll();
    // 自スレッドも停止済み — 永久に中断（再 resume されない）
    c->thread->status = STATUS_DONE;
    for (;;) xyo_yield(c);
}

void xyo_stop_others(JitCtx* c) {
    XYO_RT_PROF(kOther);
    c->runtime->stopForTarget(c->target, c->thread);
}

void xyo_stop_script(JitCtx* c) {
    XYO_RT_PROF(kOther);
    // 手続き内部からでも呼ばれうる — stopped フラグで呼出し側へ伝播し
    // エントリ関数まで巻き戻す（JS の stack クリアに相当）
    c->stopped = 1;
    c->thread->status = STATUS_DONE;
    for (;;) xyo_yield(c);
}

void xyo_broadcast_v(JitCtx* c, uint64_t v) {
    XYO_RT_PROF(kBcast);
    std::map<std::string, std::u16string> m;
    m["BROADCAST_OPTION"] = cast::toString(toV(v))->view();
    if (std::getenv("XYO_JIT_VERBOSE")) {
        std::string s; for (char16_t ch : m["BROADCAST_OPTION"])
            s += ch < 128 ? char(ch) : '?';
        std::fprintf(stderr, "xyo-jit: broadcast '%s'\n", s.c_str());
    }
    c->sequencer->runtime.startHats("event_whenbroadcastreceived", &m,
                                    nullptr);
}

void xyo_broadcast_wait_v(JitCtx* c, uint64_t v) {
    XYO_RT_PROF(kOther);
    // event_broadcastandwait 相当: hats 起動 → 全スレッド終了まで待機
    Runtime* rt = c->runtime;
    Target* stage = rt->getTargetForStage();
    std::u16string name(cast::toString(toV(v))->view());
    Variable* var = stage ? stage->lookupBroadcastMsg("", name) : nullptr;
    std::map<std::string, std::u16string> m;
    m["BROADCAST_OPTION"] = var ? var->name : name;
    std::vector<Thread*> started =
        rt->startHats("event_whenbroadcastreceived", &m, nullptr);
    if (started.empty()) return;
    for (;;) {
        bool waiting = false, allWaiting = true;
        for (Thread* t : started) {
            bool inList = false;
            for (Thread* x : rt->threads)
                if (x == t) { inList = true; break; }
            if (inList) {
                waiting = true;
                if (!rt->isWaitingThread(t)) allWaiting = false;
            }
        }
        if (!waiting) break;
        if (!allWaiting && c->isWarp && !isStuck(c)) continue;
        // yield / yieldTick — status はスレッドに立ててから fiber 中断
        c->thread->status =
            allWaiting ? STATUS_YIELD_TICK : STATUS_YIELD;
        xyo_yield(c);
    }
    c->thread->status = STATUS_RUNNING;
}

double xyo_timer(JitCtx* c) {
    XYO_RT_PROF(kOther);
    return c->runtime->ioDevices.clock.projectTimer();
}

/// compat 層の入力書き込み（JS writeInputValue 相当）。
/// name はコンパイル時埋め込みの C 文字列（Blocks 所有の安定領域）。
void xyo_rt_setarg(const BlockCached* op, const char* name, uint64_t v) {
    XYO_RT_PROF(kOther);
    ArgValues* av = const_cast<ArgValues*>(&op->argValues);
    if (std::strcmp(name, "BROADCAST_INPUT") == 0) {
        IdName& bo = av->refObj("BROADCAST_OPTION");
        bo.id.clear();
        SStr* s = cast::toString(toV(v));
        bo.name = s->view();
    } else {
        av->ref(name) = toV(v);
    }
}

// --- compat 層 -----------------------------------------------------------
// JS executeInCompatibilityLayer: blockFunction(inputs, blockUtility) を
// 直接呼び、YIELD/YIELD_TICK の間再評価する。promise は PROMISE_WAIT で
// waitQueue 経由解決。スタックフレームは fiber ローカルの合成 EC。

uint64_t xyo_rt_compat(JitCtx* c, const BlockCached* op) {
    XYO_RT_PROF(kCompat);
    static bool profOps = std::getenv("XYO_PROF") != nullptr;
    if (profOps) compatOps()[op->opcode]++;
    Thread* t = c->thread;
    BlockUtility& util = jitUtil(c);
    ExecContext ec;
    // プリミティブが stackFrame() を要求したとき合成 EC を返す
    util.jitCompatEc = &ec;

    Value rv = op->blockFunction
        ? op->blockFunction(op->argValues, util)
        : Value::undefined();
    for (;;) {
        if (t->status == STATUS_PROMISE_WAIT || t->status == STATUS_DONE) {
            util.jitCompatEc = nullptr;
            xyo_yield(c);
            return toB(Value::emptyStr());
        }
        if (t->status != STATUS_YIELD && t->status != STATUS_YIELD_TICK) break;
        if (t->status == STATUS_YIELD) {
            t->status = STATUS_RUNNING;
            if (!c->isWarp || isStuck(c)) xyo_yield(c);
        } else {
            xyo_yield(c);
        }
        rv = op->blockFunction
            ? op->blockFunction(op->argValues, util)
            : Value::undefined();
    }
    util.jitCompatEc = nullptr;
    return toB(rv);
}

/// JIT 手続き呼出し中に interpreter スタックへ残す呼出しフレーム。
/// JS では procedures_call のスタックフレームが呼出し中残り、
/// isRecursiveCall（同一 proccode 検出で再帰 yield）や warpMode
/// 継承がこれを参照する — ネイティブ呼出しでも同じ形を再現する。
void xyo_proc_push(JitCtx* c, const char* blockId, const BlockCached* op,
                   int32_t defWarp) {
    XYO_RT_PROF(kProc);
    Thread* t = c->thread;
    t->pushStack(blockId);
    if (StackFrame* sf = t->peekStackFrame()) {
        // JS stepToProcedure: warpMode は親から継承し、定義が warp なら
        // 昇格 — 非 warp 定義では降格しない（呼出し時点の isWarp は
        // まだ呼出し元の値なので定義側フラグと OR する）。
        sf->warpMode = c->isWarp != 0 || defWarp != 0;
        sf->op = op;
    }
}
void xyo_proc_pop(JitCtx* c) {
    Thread* t = c->thread;
    t->popStack();
    // JIT フレームはネイティブ呼出し境界内で完結（execute() の
    // currentStackFrame 等の外部生ポインタが跨がない）— reset して
    // 再利用プールへ移し、make_unique/destroy の往復を避ける。
    if (!t->trashFrames.empty() && t->framePool.size() < 256) {
        auto& f = t->trashFrames.back();
        f->reset();
        t->framePool.push_back(std::move(f));
    }
    t->trashFrames.pop_back();
}

/// compat 入力: ブロック id の報告値をインタプリタで評価する。
/// reporter は stack に積んで bounded stepThread で評価。
uint64_t xyo_rt_reporter(JitCtx* c, const char* blockId) {
    XYO_RT_PROF(kProc);
    Thread* t = c->thread;
    const size_t depth = t->stack.size();
    t->pushStack(blockId);
    while (t->stack.size() > depth) {
        if (t->status == STATUS_YIELD || t->status == STATUS_YIELD_TICK ||
            t->status == STATUS_PROMISE_WAIT) {
            t->status = STATUS_RUNNING;
            xyo_yield(c);
            continue;
        }
        if (t->status == STATUS_DONE) break;
        c->sequencer->stepThreadBound(*t, depth);
        // stepThreadBound は yield 境界（ブロックの u.yield / 非 warp ループ
        // 反復境界）で status を STATUS_RUNNING に戻したまま早期 return する。
        // 完了でも DONE/PROMISE_WAIT でもなく stack が残っている = yield 点に
        // 到達した → 他スレッドに実行を譲る。
        if (t->stack.size() > depth && t->status == STATUS_RUNNING)
            xyo_yield(c);
    }
    Value out = t->justReported;
    t->justReported = Value::undefined();
    return toB(out);
}

} // extern "C"
