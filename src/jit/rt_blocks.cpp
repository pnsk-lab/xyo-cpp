// xyo-cpp: JIT 生成コードから呼ばれるブロックヘルパ（rt.cpp 以外の
// カバレッジ拡張分）。意味論は blocks/core.cpp のプリミティブと
// 同一の API/共有関数を呼ぶ。リスト系は jsexecute.js の listIndex
// 意味論（rt::list*）に従う。
// yield 系は fiber 上で中断してよい（ローカル状態は fiber スタックに残る）。
#include "jit.hpp"
#include "rtprof.hpp"

#include <cmath>
#include <cstring>
#include <ctime>

#include "../blocks/core.hpp"
#include "../blocks/pen_api.hpp"
#include "../core/cast.hpp"
#include "../core/rng.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../engine/fiber.hpp"
#include "../engine/io_devices.hpp"
#include "../engine/runtime.hpp"
#include "../engine/sequencer.hpp"
#include "../engine/thread.hpp"
#include "../engine/timer.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "../rt/runtime_helpers.hpp"

using namespace xyo;

namespace {

Value toV(uint64_t bits) { Value v; v.bits = bits; return v; }
uint64_t toB(Value v) { return v.bits; }
Value V(double n) { return Value::num(n); }
Value V(bool b) { return Value::boolean(b); }
Value VS(std::u16string_view s) {
    return s.empty() ? Value::emptyStr()
                     : Value::string(SStr::create(s));
}

constexpr double kWarpTimeMs = 500.0;
bool isStuck(JitCtx* c) {
    return c->sequencer->timer.timeElapsed() > kWarpTimeMs;
}

std::u16string u16(Value v) {
    SStr* s = cast::toString(v);
    return std::u16string(s->view());
}
std::u16string u16lower(Value v) {
    return u16ToLower(cast::toString(v)->view());
}

/// 解決済み Variable* → SList（lookupOrCreateList は list を必ず持つが、
/// ロード時点で既存 var が list=null になりうるためガード+生成）。
SList* listOfVar(Variable* v) {
    if (!v) return nullptr;
    if (!v->list) v->list = SList::create();
    return v->list;
}

} // namespace

namespace {

/// broadcast/背景切替等の startedThreads 待機ループ（JS waitThreads 相当）
void waitThreads(JitCtx* c, std::vector<Thread*>& started) {
    if (started.empty()) return;
    Runtime* rt = c->runtime;
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
        c->thread->status = allWaiting ? STATUS_YIELD_TICK : STATUS_YIELD;
        xyo_yield(c);
    }
    c->thread->status = STATUS_RUNNING;
}

/// sayforsecs/thinkforsecs 相当: バブル更新 + WaitEntry + PROMISE_WAIT
void bubbleForSecs(JitCtx* c, const char* type, Value msg, double secs) {
    updateBubble(c->target, type, msg);
    Target* t = c->target;
    Runtime* rt = c->runtime;
    Runtime::WaitEntry w;
    w.thread = c->thread;
    w.deadlineMs = rt->currentMSecs + std::max(0.0, 1000.0 * secs);
    w.bubbleTarget = t;
    w.bubbleType = type;
    w.bubbleUsageId = t->bubbleUsageId;
    rt->waitQueue.push_back(w);
    c->thread->status = STATUS_PROMISE_WAIT;
    xyo_yield(c);
}

/// changeMonitorVisibility（data_show/hidevariable, show/hidelist）
void setMonitorVisible(JitCtx* c, const char* id, bool visible) {
    Runtime* rt = c->runtime;
    MonitorRecord& rec = rt->monitorState.getOrCreate(id);
    if (rec.id.empty()) rec.id = id;
    rec.visible = visible;
    rt->monitorState.dirty = true;
    if (Block* b = rt->monitorBlocks->getBlock(id)) b->isMonitored = visible;
    rt->monitorBlocks->resetCache();
}

void promiseNextTick(JitCtx* c) {
    Runtime::WaitEntry w;
    w.thread = c->thread;
    w.nextTick = true;
    c->runtime->waitQueue.push_back(w);
    c->thread->status = STATUS_PROMISE_WAIT;
    xyo_yield(c);
}

} // namespace

extern "C" {

// ============================================================ 文字列 ====

uint64_t xyo_join(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kStr);
    return toB(rt::join(toV(a), toV(b)));
}
uint64_t xyo_letterof(uint64_t letter, uint64_t s) {
    XYO_RT_PROF(kStr);
    return toB(rt::letterOf(toV(letter), toV(s)));
}
uint64_t xyo_strlen(uint64_t s) {
    XYO_RT_PROF(kStr);
    return toB(V(rt::strLength(toV(s))));
}
uint64_t xyo_strcontains(uint64_t a, uint64_t b) {
    XYO_RT_PROF(kStr);
    return toB(V(rt::strContains(toV(a), toV(b))));
}

// ============================================================= リスト ====

void xyo_list_add(JitCtx* c, Variable* v, uint64_t item) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v)) {
        l->push(toV(item));
        l->monitorDirty = true;
    }
}
void xyo_list_delete(JitCtx* c, Variable* v, uint64_t idx) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v)) rt::listDelete(l, toV(idx));
}
void xyo_list_deleteall(JitCtx* c, Variable* v) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v)) {
        rt::listDeleteAll(l);
        l->monitorDirty = true;
    }
}
void xyo_list_insert(JitCtx* c, Variable* v, uint64_t idx, uint64_t item) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v)) rt::listInsert(l, toV(idx), toV(item));
}
void xyo_list_replace(JitCtx* c, Variable* v, uint64_t idx, uint64_t item) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v)) rt::listReplace(l, toV(idx), toV(item));
}
uint64_t xyo_list_item(JitCtx* c, Variable* v, uint64_t idx) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v)) return toB(rt::listGet(l, toV(idx)));
    return toB(Value::emptyStr());
}
uint64_t xyo_list_itemnum(JitCtx* c, Variable* v, uint64_t item) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v))
        return toB(V(rt::listIndexOf(l, toV(item))));
    return toB(V(0.0));
}
uint64_t xyo_list_length(JitCtx* c, Variable* v) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v)) return toB(V(double(l->len)));
    return toB(V(0.0));
}
uint64_t xyo_list_contains(JitCtx* c, Variable* v, uint64_t item) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v))
        return toB(V(rt::listContains(l, toV(item))));
    return toB(V(false));
}
uint64_t xyo_list_contents(JitCtx* c, Variable* v) {
    XYO_RT_PROF(kList);
    if (SList* l = listOfVar(v)) return toB(rt::listContents(l));
    return toB(Value::emptyStr());
}
void xyo_var_monitor(JitCtx* c, const char* id, int32_t visible) {
    XYO_RT_PROF(kVar);
    setMonitorVisible(c, id, visible != 0);
}

// ============================================================= pen ========
// compat 経由を避けるため pen ブロックは直接ヘルパ化（実体は
// blocks/pen.cpp の共有コア — セマンティクスはプリミティブと同一）

void xyo_pen_up(JitCtx* c) {
    XYO_RT_PROF(kPen);
    penrt::up(c->target, c->runtime);
}
void xyo_pen_down(JitCtx* c) {
    XYO_RT_PROF(kPen);
    penrt::down(c->target, c->runtime);
}
void xyo_pen_clear(JitCtx* c) {
    XYO_RT_PROF(kPen);
    penrt::clear(c->runtime);
}
void xyo_pen_stamp(JitCtx* c) {
    XYO_RT_PROF(kPen);
    penrt::stamp(c->target, c->runtime);
}
void xyo_pen_setcolor(JitCtx* c, uint64_t color) {
    XYO_RT_PROF(kPen);
    penrt::setColorToColor(c->target, toV(color));
}
void xyo_pen_changeparam(JitCtx* c, uint64_t param, uint64_t v) {
    XYO_RT_PROF(kPen);
    SStr* s = cast::toString(toV(param));
    penrt::colorParam(c->target,
                      s ? s->view() : std::u16string_view{},
                      toV(v), true);
}
void xyo_pen_setparam(JitCtx* c, uint64_t param, uint64_t v) {
    XYO_RT_PROF(kPen);
    SStr* s = cast::toString(toV(param));
    penrt::colorParam(c->target,
                      s ? s->view() : std::u16string_view{},
                      toV(v), false);
}
void xyo_pen_setsize(JitCtx* c, uint64_t v) {
    XYO_RT_PROF(kPen);
    penrt::size(c->target, c->runtime, toV(v), false);
}
void xyo_pen_changesize(JitCtx* c, uint64_t v) {
    XYO_RT_PROF(kPen);
    penrt::size(c->target, c->runtime, toV(v), true);
}
/// 生成コードの inline パラメータ更新後の updatePenColor 相当
/// （penState 非 null 前提 — 生成コード側で確認済み）
void xyo_pen_recolor(JitCtx* c) {
    XYO_RT_PROF(kPen);
    penrt::recolor(c->target);
}
/// penState==null 経路（遅延生成込みのフォールバック）
void xyo_pen_paramnum(JitCtx* c, int32_t kind, int32_t change, double v) {
    XYO_RT_PROF(kPen);
    penrt::colorParamNum(c->target, kind, v, change != 0);
}

// ============================================================= motion ====

void xyo_mot_movesteps(JitCtx* c, uint64_t steps) {
    XYO_RT_PROF(kPen);
    moveSteps(cast::toNumber(toV(steps)), c->target);
}
void xyo_mot_gotoxy(JitCtx* c, uint64_t x, uint64_t y) {
    XYO_RT_PROF(kPen);
    c->target->setXY(cast::toNumber(toV(x)), cast::toNumber(toV(y)));
}
void xyo_mot_goto(JitCtx* c, uint64_t to) {
    XYO_RT_PROF(kPen);
    double tx, ty;
    if (getTargetXY(toV(to), *c->runtime, tx, ty))
        c->target->setXY(tx, ty);
}
void xyo_mot_turnright(JitCtx* c, uint64_t deg) {
    XYO_RT_PROF(kPen);
    c->target->setDirection(c->target->direction + cast::toNumber(toV(deg)));
}
void xyo_mot_turnleft(JitCtx* c, uint64_t deg) {
    XYO_RT_PROF(kPen);
    c->target->setDirection(c->target->direction - cast::toNumber(toV(deg)));
}
void xyo_mot_pointdir(JitCtx* c, uint64_t dir) {
    XYO_RT_PROF(kPen);
    c->target->setDirection(cast::toNumber(toV(dir)));
}
void xyo_mot_pointtowards(JitCtx* c, uint64_t towards) {
    XYO_RT_PROF(kPen);
    Value tw = toV(towards);
    Target* t = c->target;
    Runtime* rt = c->runtime;
    double tx = 0, ty = 0;
    if (tw.isString() && tw.asStrPtr() &&
        tw.asStrPtr()->view() == u"_mouse_") {
        tx = rt->ioDevices.mouse.getScratchX();
        ty = rt->ioDevices.mouse.getScratchY();
    } else if (tw.isString() && tw.asStrPtr() &&
               tw.asStrPtr()->view() == u"_random_") {
        t->setDirection(std::round(Rng::global().nextDouble() * 360) - 180);
        return;
    } else {
        Target* pt = rt->getSpriteTargetByName(u16(tw));
        if (!pt) return;
        tx = pt->x;
        ty = pt->y;
    }
    double dx = tx - t->x, dy = ty - t->y;
    t->setDirection(90.0 - (std::atan2(dy, dx) * 180.0 / M_PI));
}

/// glide 内部で使う位置補間ループ（fiber 中断でフレーム毎に再開）
static void glideImpl(JitCtx* c, double secs, double endX, double endY) {
    Target* t = c->target;
    if (secs <= 0) { t->setXY(endX, endY); return; }
    Timer timer;
    timer.start();
    const double sx = t->x, sy = t->y;
    const double durMs = secs * 1000.0;
    for (;;) {
        double e = timer.timeElapsed();
        if (e >= durMs) { t->setXY(endX, endY); break; }
        double f = e / durMs;
        t->setXY(sx + f * (endX - sx), sy + f * (endY - sy));
        c->thread->status = STATUS_YIELD;
        xyo_yield(c);
    }
}
void xyo_mot_glidexy(JitCtx* c, uint64_t secs, uint64_t x, uint64_t y) {
    XYO_RT_PROF(kBlocks);
    glideImpl(c, cast::toNumber(toV(secs)), cast::toNumber(toV(x)),
              cast::toNumber(toV(y)));
}
void xyo_mot_glideto(JitCtx* c, uint64_t secs, uint64_t to) {
    XYO_RT_PROF(kBlocks);
    double tx, ty;
    if (getTargetXY(toV(to), *c->runtime, tx, ty))
        glideImpl(c, cast::toNumber(toV(secs)), tx, ty);
}
void xyo_mot_edgebounce(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    Target::Bounds bounds;
    if (!t->getBounds(bounds)) return;
    Runtime* rt = c->runtime;
    double stageW = rt->stageWidth, stageH = rt->stageHeight;
    double distLeft = std::max(0.0, stageW / 2 + bounds.left);
    double distTop = std::max(0.0, stageH / 2 - bounds.top);
    double distRight = std::max(0.0, stageW / 2 - bounds.right);
    double distBottom = std::max(0.0, stageH / 2 + bounds.bottom);
    const char* nearest = "";
    double minDist = std::numeric_limits<double>::infinity();
    if (distLeft < minDist) { minDist = distLeft; nearest = "left"; }
    if (distTop < minDist) { minDist = distTop; nearest = "top"; }
    if (distRight < minDist) { minDist = distRight; nearest = "right"; }
    if (distBottom < minDist) { minDist = distBottom; nearest = "bottom"; }
    if (minDist > 0) return;
    double radians = (M_PI * (90.0 - t->direction)) / 180.0;
    double dx = std::cos(radians);
    double dy = -std::sin(radians);
    if (std::strcmp(nearest, "left") == 0) dx = std::max(0.2, std::fabs(dx));
    else if (std::strcmp(nearest, "top") == 0) dy = std::max(0.2, std::fabs(dy));
    else if (std::strcmp(nearest, "right") == 0) dx = -std::max(0.2, std::fabs(dx));
    else if (std::strcmp(nearest, "bottom") == 0) dy = -std::max(0.2, std::fabs(dy));
    t->setDirection(std::atan2(dy, dx) * 180.0 / M_PI + 90.0);
    double fx, fy;
    if (t->keepInFence(t->x, t->y, fx, fy)) t->setXY(fx, fy);
}
void xyo_mot_setrotstyle(JitCtx* c, uint64_t style) {
    XYO_RT_PROF(kBlocks);
    c->target->setRotationStyle(SStr::toUtf8(u16(toV(style))));
}
void xyo_mot_changex(JitCtx* c, uint64_t dx) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    t->setXY(t->x + cast::toNumber(toV(dx)), t->y);
}
void xyo_mot_setx(JitCtx* c, uint64_t x) {
    XYO_RT_PROF(kBlocks);
    c->target->setXY(cast::toNumber(toV(x)), c->target->y);
}
void xyo_mot_changey(JitCtx* c, uint64_t dy) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    t->setXY(t->x, t->y + cast::toNumber(toV(dy)));
}
void xyo_mot_sety(JitCtx* c, uint64_t y) {
    XYO_RT_PROF(kBlocks);
    c->target->setXY(c->target->x, cast::toNumber(toV(y)));
}
uint64_t xyo_mot_getx(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(V(rt::limitPrecision(c->target->x)));
}
uint64_t xyo_mot_gety(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(V(rt::limitPrecision(c->target->y)));
}
uint64_t xyo_mot_getdir(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(V(c->target->direction));
}

// ============================================================== looks ====

void xyo_looks_say(JitCtx* c, uint64_t msg) {
    XYO_RT_PROF(kBlocks);
    updateBubble(c->target, "say", toV(msg));
}
void xyo_looks_think(JitCtx* c, uint64_t msg) {
    XYO_RT_PROF(kBlocks);
    updateBubble(c->target, "think", toV(msg));
}
void xyo_looks_saysecs(JitCtx* c, uint64_t msg, uint64_t secs) {
    XYO_RT_PROF(kBlocks);
    bubbleForSecs(c, "say", toV(msg), cast::toNumber(toV(secs)));
}
void xyo_looks_thinksecs(JitCtx* c, uint64_t msg, uint64_t secs) {
    XYO_RT_PROF(kBlocks);
    bubbleForSecs(c, "think", toV(msg), cast::toNumber(toV(secs)));
}
void xyo_looks_show(JitCtx* c) { c->target->setVisible(true); }
void xyo_looks_hide(JitCtx* c) { c->target->setVisible(false); }
void xyo_looks_switchcostume(JitCtx* c, uint64_t costume) {
    XYO_RT_PROF(kBlocks);
    setCostumeArg(c->target, toV(costume), false);
}
void xyo_looks_nextcostume(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    setCostumeArg(c->target, V(c->target->currentCostume + 1.0), true);
}
void xyo_looks_switchbackdrop(JitCtx* c, uint64_t backdrop) {
    XYO_RT_PROF(kBlocks);
    if (Target* stage = c->runtime->getTargetForStage())
        setBackdropArg(*c->runtime, stage, toV(backdrop), false);
}
void xyo_looks_nextbackdrop(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    if (Target* stage = c->runtime->getTargetForStage())
        setBackdropArg(*c->runtime, stage,
                       V(stage->currentCostume + 1.0), true);
}
void xyo_looks_switchbackdropwait(JitCtx* c, uint64_t backdrop) {
    XYO_RT_PROF(kBlocks);
    Target* stage = c->runtime->getTargetForStage();
    if (!stage) return;
    std::vector<Thread*> started =
        setBackdropArg(*c->runtime, stage, toV(backdrop), false);
    waitThreads(c, started);
}
void xyo_looks_changeeffect(JitCtx* c, uint64_t change, uint64_t effect) {
    XYO_RT_PROF(kBlocks);
    std::u16string e = u16lower(toV(effect));
    Target* t = c->target;
    std::string e8 = SStr::toUtf8(e);
    if (!t->effects.count(e8)) return;
    double nv = cast::toNumber(toV(change)) + t->effects[e8];
    t->setEffect(e8, clampEffect(e, nv));
}
void xyo_looks_seteffect(JitCtx* c, uint64_t value, uint64_t effect) {
    XYO_RT_PROF(kBlocks);
    std::u16string e = u16lower(toV(effect));
    c->target->setEffect(SStr::toUtf8(e),
                       clampEffect(e, cast::toNumber(toV(value))));
}
void xyo_looks_cleareffects(JitCtx* c) { c->target->clearEffects(); }
void xyo_looks_changesize(JitCtx* c, uint64_t change) {
    XYO_RT_PROF(kBlocks);
    c->target->setSize(c->target->size + cast::toNumber(toV(change)));
}
void xyo_looks_setsize(JitCtx* c, uint64_t size) {
    XYO_RT_PROF(kBlocks);
    c->target->setSize(cast::toNumber(toV(size)));
}
void xyo_looks_frontback(JitCtx* c, uint64_t fb) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    if (t->isStage) return;
    if (u16(toV(fb)) == u"front") t->goToFront();
    else t->goToBack();
}
void xyo_looks_fwdbacklayers(JitCtx* c, uint64_t n, uint64_t fb) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    if (t->isStage) return;
    if (u16(toV(fb)) == u"forward")
        t->goForwardLayers(cast::toNumber(toV(n)));
    else
        t->goBackwardLayers(cast::toNumber(toV(n)));
}
uint64_t xyo_looks_size(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(V(std::round(c->target->size)));
}
uint64_t xyo_looks_costumenumname(JitCtx* c, uint64_t numname) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    if (u16(toV(numname)) == u"number")
        return toB(V(t->currentCostume + 1.0));
    const Costume* cs = t->getCurrentCostume();
    return cs ? toB(VS(cs->name)) : toB(Value::emptyStr());
}
uint64_t xyo_looks_backdropnumname(JitCtx* c, uint64_t numname) {
    XYO_RT_PROF(kBlocks);
    Target* stage = c->runtime->getTargetForStage();
    if (!stage) return toB(V(0.0));
    if (u16(toV(numname)) == u"number")
        return toB(V(stage->currentCostume + 1.0));
    const Costume* cs = stage->getCurrentCostume();
    return cs ? toB(VS(cs->name)) : toB(Value::emptyStr());
}

// ============================================================ sensing ====

uint64_t xyo_sns_touchingobj(JitCtx* c, uint64_t menu) {
    XYO_RT_PROF(kBlocks);
    return toB(V(c->target->isTouchingObject(u16(toV(menu)))));
}
uint64_t xyo_sns_touchingcolor(JitCtx* c, uint64_t color) {
    XYO_RT_PROF(kBlocks);
    Rgb rgb = cast::toRgbColorList(toV(color));
    uint32_t p = (uint32_t(rgb.r) << 16) | (uint32_t(rgb.g) << 8) | rgb.b;
    return toB(V(c->target->isTouchingColor(p)));
}
uint64_t xyo_sns_coloristouching(JitCtx* c, uint64_t color2, uint64_t color) {
    XYO_RT_PROF(kBlocks);
    Rgb t = cast::toRgbColorList(toV(color2));
    Rgb m = cast::toRgbColorList(toV(color));
    uint32_t tp = (uint32_t(t.r) << 16) | (uint32_t(t.g) << 8) | t.b;
    uint32_t mp = (uint32_t(m.r) << 16) | (uint32_t(m.g) << 8) | m.b;
    return toB(V(c->target->colorIsTouchingColor(tp, mp)));
}
uint64_t xyo_sns_distanceto(JitCtx* c, uint64_t menu) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    if (t->isStage) return toB(V(10000.0));
    Value m = toV(menu);
    double tx = 0, ty = 0;
    if (m.isString() && m.asStrPtr() && m.asStrPtr()->view() == u"_mouse_") {
        tx = c->runtime->ioDevices.mouse.getScratchX();
        ty = c->runtime->ioDevices.mouse.getScratchY();
    } else {
        Target* other = c->runtime->getSpriteTargetByName(u16(m));
        if (!other) return toB(V(10000.0));
        tx = other->x;
        ty = other->y;
    }
    double dx = t->x - tx, dy = t->y - ty;
    return toB(V(std::sqrt(dx * dx + dy * dy)));
}
uint64_t xyo_sns_mousex(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(V(c->runtime->ioDevices.mouse.getScratchX()));
}
uint64_t xyo_sns_mousey(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(V(c->runtime->ioDevices.mouse.getScratchY()));
}
uint64_t xyo_sns_mousedown(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(V(c->runtime->ioDevices.mouse.getIsDown()));
}
uint64_t xyo_sns_keypressed(JitCtx* c, uint64_t key) {
    XYO_RT_PROF(kBlocks);
    return toB(V(c->runtime->ioDevices.keyboard.getKeyIsDown(toV(key))));
}
void xyo_sns_setdragmode(JitCtx* c, uint64_t mode) {
    XYO_RT_PROF(kBlocks);
    c->target->setDraggable(u16(toV(mode)) == u"draggable");
}
void xyo_sns_resettimer(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    c->runtime->ioDevices.clock.resetProjectTimer();
}
uint64_t xyo_sns_current(JitCtx* c, uint64_t menu) {
    XYO_RT_PROF(kBlocks);
    std::u16string option = u16lower(toV(menu));
    std::time_t now = std::time(nullptr);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);   // 引数順が POSIX localtime_r と逆
#else
    localtime_r(&now, &tm);
#endif
    if (option == u"year") return toB(V(tm.tm_year + 1900.0));
    if (option == u"month") return toB(V(tm.tm_mon + 1.0));
    if (option == u"date") return toB(V(double(tm.tm_mday)));
    if (option == u"dayofweek") return toB(V(tm.tm_wday + 1.0));
    if (option == u"hour") return toB(V(double(tm.tm_hour)));
    if (option == u"minute") return toB(V(double(tm.tm_min)));
    if (option == u"second") return toB(V(double(tm.tm_sec)));
    return toB(V(0.0));
}
uint64_t xyo_sns_dayssince2000(JitCtx*) {
    XYO_RT_PROF(kBlocks);
    return toB(V(rt::daysSince2000()));
}
uint64_t xyo_sns_loudness(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    Runtime* rt = c->runtime;
    if (!rt->audioEngine) return toB(V(-1.0));
    double since =
        rt->loudnessTimer.timeElapsed() - rt->cachedLoudnessTimestamp;
    if (since < rt->currentStepTime) return toB(V(rt->cachedLoudness));
    rt->cachedLoudnessTimestamp = rt->loudnessTimer.timeElapsed();
    rt->cachedLoudness = rt->audioEngine->getLoudness();
    return toB(V(rt->cachedLoudness));
}
uint64_t xyo_sns_loud(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    uint64_t r = xyo_sns_loudness(c);
    return toB(V(cast::toNumber(toV(r)) > 10));
}
uint64_t xyo_sns_answer(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(VS(c->runtime->answer));
}
void xyo_sns_askwait(JitCtx* c, uint64_t question) {
    XYO_RT_PROF(kBlocks);
    Runtime* rt = c->runtime;
    Target* t = c->target;
    Runtime::AskEntry e;
    e.question = u16(toV(question));
    e.thread = c->thread;
    e.target = t;
    e.wasVisible = t->visible;
    e.wasStage = t->isStage;
    bool wasEmpty = rt->questionList.empty();
    rt->questionList.push_back(e);
    if (wasEmpty) {
        if (e.wasVisible && !e.wasStage) {
            t->setSay("say", e.question);
            if (rt->onQuestion) {
                static const std::u16string kEmpty;
                rt->onQuestion(&kEmpty);
            }
        } else if (rt->onQuestion) {
            rt->onQuestion(&e.question);
        }
    }
    c->thread->status = STATUS_PROMISE_WAIT;
    xyo_yield(c);
}
uint64_t xyo_sns_of(JitCtx* c, uint64_t obj, uint64_t prop) {
    XYO_RT_PROF(kBlocks);
    Runtime* rt = c->runtime;
    Value objArg = toV(obj);
    Target* attrTarget;
    if (objArg.isString() && objArg.asStrPtr() &&
        objArg.asStrPtr()->view() == u"_stage_") {
        attrTarget = rt->getTargetForStage();
    } else {
        attrTarget = rt->getSpriteTargetByName(u16(objArg));
    }
    if (!attrTarget) return toB(V(0.0));
    std::u16string p = u16(toV(prop));
    if (attrTarget->isStage) {
        if (p == u"background #" || p == u"backdrop #")
            return toB(V(attrTarget->currentCostume + 1.0));
        if (p == u"backdrop name") {
            const Costume* cs = attrTarget->getCurrentCostume();
            return cs ? toB(VS(cs->name)) : toB(Value::emptyStr());
        }
        if (p == u"volume") return toB(V(attrTarget->volume));
    } else {
        if (p == u"x position") return toB(V(attrTarget->x));
        if (p == u"y position") return toB(V(attrTarget->y));
        if (p == u"direction") return toB(V(attrTarget->direction));
        if (p == u"costume #") return toB(V(attrTarget->currentCostume + 1.0));
        if (p == u"costume name") {
            const Costume* cs = attrTarget->getCurrentCostume();
            return cs ? toB(VS(cs->name)) : toB(Value::emptyStr());
        }
        if (p == u"size") return toB(V(attrTarget->size));
        if (p == u"volume") return toB(V(attrTarget->volume));
    }
    Variable* var = attrTarget->lookupVariableByNameAndType(
        p, Variable::kScalarType, true);
    if (var) return toB(var->value);
    return toB(V(0.0));
}
uint64_t xyo_sns_username(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(VS(c->runtime->ioDevices.userData.getUsername()));
}
uint64_t xyo_sns_online(JitCtx*) {
    XYO_RT_PROF(kBlocks);
    return toB(V(true));
}

// ============================================================ control ====

uint64_t xyo_ctl_getcounter(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(V(c->runtime->controlCounter));
}
void xyo_ctl_incrcounter(JitCtx* c) { c->runtime->controlCounter++; }
void xyo_ctl_clearcounter(JitCtx* c) { c->runtime->controlCounter = 0; }

void xyo_ctl_createclone(JitCtx* c, uint64_t opt) {
    XYO_RT_PROF(kBlocks);
    std::u16string o = u16(toV(opt));
    Target* cloneTarget;
    if (o == u"_myself_") {
        cloneTarget = c->target;
    } else {
        cloneTarget = c->runtime->getSpriteTargetByName(o);
    }
    if (!cloneTarget) return;
    Target* cl = cloneTarget->makeClone();
    if (cl) {
        c->runtime->addTarget(cl);
        cl->goBehindOther(cloneTarget);
    }
}
void xyo_ctl_deleteclone(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    if (t->isOriginal) return;
    c->runtime->disposeTarget(t);
    c->runtime->stopForTarget(t, nullptr);
    // このスレッドは削除済み — 二度と resume されないよう永久中断
    c->stopped = 1;
    c->thread->status = STATUS_DONE;
    for (;;) xyo_yield(c);
}

// ============================================================== sound ====

void xyo_snd_play(JitCtx* c, uint64_t menu) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    if (!t->sprite || !t->sprite->soundBank) return;
    int idx = getSoundIndex(toV(menu), t);
    if (idx >= 0)
        t->sprite->soundBank->playSound(t, t->sprite->sounds[idx].soundId);
}
void xyo_snd_playwait(JitCtx* c, uint64_t menu) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    if (!t->sprite || !t->sprite->soundBank) return;
    int idx = getSoundIndex(toV(menu), t);
    if (idx >= 0) {
        c->thread->status = STATUS_PROMISE_WAIT;
        t->sprite->soundBank->playSoundAndWait(
            t, t->sprite->sounds[idx].soundId, c->thread);
        xyo_yield(c);
    }
}
void xyo_snd_stopall(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    for (Target* t : c->runtime->targets) {
        if (t->sprite && t->sprite->soundBank)
            t->sprite->soundBank->stopAllSounds(t);
    }
}

static void sndEffectImpl(JitCtx* c, uint64_t value, uint64_t effect,
                        bool change) {
    std::u16string e = u16lower(toV(effect));
    std::string e8 = SStr::toUtf8(e);
    double val = cast::toNumber(toV(value));
    Target* t = c->target;
    if (e8 != "pitch" && e8 != "pan") return;
    if (!t->soundEffects.count(e8)) {
        t->soundEffects["pitch"] = 0;
        t->soundEffects["pan"] = 0;
    }
    if (change) t->soundEffects[e8] += val;
    else t->soundEffects[e8] = val;
    Runtime* rt = c->runtime;
    double lo, hi;
    if (e8 == "pitch") { lo = rt->miscLimits ? -360 : -1000;
                         hi = rt->miscLimits ? 360 : 1000; }
    else { lo = -100; hi = 100; }
    t->soundEffects[e8] = std::min(std::max(t->soundEffects[e8], lo), hi);
    if (rt->miscLimits) promiseNextTick(c);
    else rt->redrawRequested = true;
}
void xyo_snd_seteffect(JitCtx* c, uint64_t value, uint64_t effect) {
    XYO_RT_PROF(kBlocks);
    sndEffectImpl(c, value, effect, false);
}
void xyo_snd_changeeffect(JitCtx* c, uint64_t value, uint64_t effect) {
    XYO_RT_PROF(kBlocks);
    sndEffectImpl(c, value, effect, true);
}
void xyo_snd_cleareffects(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    Target* t = c->target;
    t->soundEffects["pitch"] = 0;
    t->soundEffects["pan"] = 0;
}

static void sndVolumeImpl(JitCtx* c, uint64_t vol, bool change) {
    double volume = cast::toNumber(toV(vol));
    if (change) volume += c->target->volume;
    volume = std::min(std::max(volume, 0.0), 100.0);
    c->target->volume = volume;
    Runtime* rt = c->runtime;
    if (rt->miscLimits) promiseNextTick(c);
    else rt->redrawRequested = true;
}
void xyo_snd_setvolume(JitCtx* c, uint64_t vol) {
    XYO_RT_PROF(kBlocks);
    sndVolumeImpl(c, vol, false);
}
void xyo_snd_changevolume(JitCtx* c, uint64_t vol) {
    XYO_RT_PROF(kBlocks);
    sndVolumeImpl(c, vol, true);
}
uint64_t xyo_snd_getvolume(JitCtx* c) {
    XYO_RT_PROF(kBlocks);
    return toB(V(c->target->volume));
}

} // extern "C"
