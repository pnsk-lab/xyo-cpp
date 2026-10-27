// xyo-cpp: blocks/scratch3_*.js 移植 — コアブロックプリミティブ。
// JS との対応:
//   args.X            → args.get("X")      （フィールド+入力報告値）
//   args.VARIABLE     → args.getObj("X")   （{id,name}）
//   util.stackFrame   → util.stackFrame()  （ExecContext）
//   util.ioQuery(...) → util.xxx() メソッド
#include "core.hpp"

#include "../core/v8math.hpp"

#include <cmath>
#include <ctime>
#include <limits>

// M_PI/M_LN10 não são padrão ISO — fora do POSIX podem não existir em <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif

#include "../core/cast.hpp"
#include "../engine/block_utility.hpp"
#include "../engine/runtime.hpp"
#include "../engine/sequencer.hpp"
#include "../model/target.hpp"
#include "../rt/runtime_helpers.hpp"

namespace xyo {

// ================================================================
// 共有ブロック意味論 — インタプリタと JIT ランタイムヘルパの双方が使用。
// （core.hpp 宣言。anon 内の toStr と同等の u16 化だけここに用意）

static std::u16string u16(Value v) {
    SStr* s = cast::toString(v);
    return std::u16string(s->view());
}

std::u16string formatBubbleText(Value v) {
    std::u16string text;
    if (v.isNumber()) {
        double d = v.asNumberUnchecked();
        if (std::fabs(d) >= 0.01 && std::fmod(d, 1.0) != 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.2f", d);
            text = utf8ToU16(buf);
        } else {
            text = u16(v);
        }
    } else {
        text = u16(v);
    }
    if (text.size() > 330) text.resize(330);   // SAY_BUBBLE_LIMIT（substr）
    return text;
}

void updateBubble(Target* t, const char* type, Value message) {
    t->setSay(type, formatBubbleText(message));
}

// _setCostume — Scratch 2.0 互換の型分岐
void setCostumeArg(Target* target, Value requestedCostume, bool optZeroIndex) {
    if (requestedCostume.isNumber()) {
        target->setCostume(optZeroIndex ? requestedCostume.asNumberUnchecked()
                                        : requestedCostume.asNumberUnchecked() - 1);
        return;
    }
    std::u16string name = u16(requestedCostume);
    int costumeIndex = target->getCostumeIndexByName(name);
    if (costumeIndex != -1) {
        target->setCostume(costumeIndex);
    } else if (name == u"next costume") {
        target->setCostume(target->currentCostume + 1);
    } else if (name == u"previous costume") {
        target->setCostume(target->currentCostume - 1);
    } else {
        double d = cast::toNumberOrNaN(requestedCostume);
        if (!std::isnan(d) && !cast::isWhiteSpace(requestedCostume)) {
            target->setCostume(optZeroIndex ? d : d - 1);
        }
    }
}

// _setBackdrop — stage 対象。開始されたスレッドを返す。
std::vector<Thread*> setBackdropArg(Runtime& rt, Target* stage,
                                  Value requestedBackdrop, bool optZeroIndex) {
    if (requestedBackdrop.isNumber()) {
        stage->setCostume(optZeroIndex ? requestedBackdrop.asNumberUnchecked()
                                       : requestedBackdrop.asNumberUnchecked() - 1);
    } else {
        std::u16string name = u16(requestedBackdrop);
        int costumeIndex = stage->getCostumeIndexByName(name);
        if (costumeIndex != -1) {
            stage->setCostume(costumeIndex);
        } else if (name == u"next backdrop") {
            stage->setCostume(stage->currentCostume + 1);
        } else if (name == u"previous backdrop") {
            stage->setCostume(stage->currentCostume - 1);
        } else if (name == u"random backdrop") {
            size_t numCostumes = stage->getCostumes()->size();
            if (numCostumes > 1) {
                int next = int(rt::randomIntWithout(
                    0, double(numCostumes - 1), stage->currentCostume));
                stage->setCostume(next);
            }
        } else {
            double d = cast::toNumberOrNaN(requestedBackdrop);
            if (!std::isnan(d) && !cast::isWhiteSpace(requestedBackdrop)) {
                stage->setCostume(optZeroIndex ? d : d - 1);
            }
        }
    }
    std::u16string newName;
    if (const Costume* c = stage->getCurrentCostume()) newName = c->name;
    std::map<std::string, std::u16string> m{{"BACKDROP", newName}};
    return rt.startHats("event_whenbackdropswitchesto", &m, nullptr);
}

double clampEffect(const std::u16string& effect, double value) {
    if (effect == u"ghost")
        return std::min(std::max(value, 0.0), 100.0);
    if (effect == u"brightness")
        return std::min(std::max(value, -100.0), 100.0);
    return value;
}

void moveSteps(double steps, Target* t) {
    double radians = (M_PI * (90.0 - t->direction)) / 180.0;
    double dx = steps * v8math::cos(radians);
    double dy = steps * v8math::sin(radians);
    t->setXY(t->x + dx, t->y + dy);
}

/// getTargetXY — '_mouse_'/'_random_'/スプライト名 → [x,y]。見つからなければ false
bool getTargetXY(Value targetName, Runtime& rt, double& tx, double& ty) {
    if (targetName.isString()) {
        SStr* s = targetName.asStrPtr();
        if (s && s->view() == u"_mouse_") {
            tx = rt.ioDevices.mouse.getScratchX();
            ty = rt.ioDevices.mouse.getScratchY();
            return true;
        }
        if (s && s->view() == u"_random_") {
            tx = jsnum::jsRound(rt.stageWidth *
                            (Rng::global().nextDouble() - 0.5));
            ty = jsnum::jsRound(rt.stageHeight *
                            (Rng::global().nextDouble() - 0.5));
            return true;
        }
    }
    // JS: '_mouse_'/'_random_' は文字列比較 — 非文字列/他は sprite lookup
    std::u16string name = u16(targetName);
    Target* goTo = rt.getSpriteTargetByName(name);
    if (!goTo) return false;
    tx = goTo->x;
    ty = goTo->y;
    return true;
}

namespace {

int getSoundIndexByName(const std::u16string& name, Sprite* sprite) {
    for (size_t i = 0; i < sprite->sounds.size(); ++i)
        if (sprite->sounds[i].name == name) return int(i);
    return -1;
}

} // namespace

int getSoundIndex(Value soundName, Target* t) {
    if (!t->sprite || t->sprite->sounds.empty()) return -1;
    size_t len = t->sprite->sounds.size();
    std::u16string name = u16(soundName);
    int idx = getSoundIndexByName(name, t->sprite);
    if (idx != -1) return idx;
    // parseInt 相当: 先頭からの整数パース
    SStr* s = cast::toString(soundName);
    const char16_t* p = s->data();
    const char16_t* end = p + s->len;
    while (p < end && (*p == u' ' || *p == u'\t')) ++p;
    bool neg = false;
    if (p < end && (*p == u'+' || *p == u'-')) neg = *p++ == u'-';
    long long v = 0;
    bool any = false;
    while (p < end && *p >= u'0' && *p <= u'9') {
        v = v * 10 + (*p++ - u'0');
        any = true;
        if (v > 1e15) break;
    }
    if (any) {
        long long oneIdx = (neg ? -v : v) - 1;
        // wrapClamp(oneIdx, 0, len-1)
        double range = double(len);
        double w = double(oneIdx);
        w = w - std::floor(w / range) * range;
        return int(w);
    }
    return -1;
}

namespace {

Value V(double n) { return Value::num(n); }
Value V(bool b) { return Value::boolean(b); }
Value VS(std::u16string_view s) {
    return s.empty() ? Value::emptyStr()
                     : Value::string(SStr::create(s));
}

std::u16string toStr(Value v) {
    SStr* s = cast::toString(v);
    return std::u16string(s->view());
}
std::string toStr8(Value v) {
    return SStr::toUtf8(cast::toString(v)->view());
}

double num(const ArgValues& a, const char* n) {
    return cast::toNumber(a.get(n));
}
bool truth(const ArgValues& a, const char* n) {
    return cast::toBoolean(a.get(n));
}
Value v(const ArgValues& a, const char* n) { return a.get(n); }
std::u16string str(const ArgValues& a, const char* n) {
    return toStr(a.get(n));
}
std::u16string strLower(const ArgValues& a, const char* n) {
    return u16ToLower(toStr(a.get(n)));
}
const IdName* obj(const ArgValues& a, const char* n) {
    return a.getObj(n);
}

// ============================================================ operators ====

Value opAdd(const ArgValues& a, BlockUtility&) {
    return V(num(a, "NUM1") + num(a, "NUM2"));
}
Value opSub(const ArgValues& a, BlockUtility&) {
    return V(num(a, "NUM1") - num(a, "NUM2"));
}
Value opMul(const ArgValues& a, BlockUtility&) {
    return V(num(a, "NUM1") * num(a, "NUM2"));
}
Value opDiv(const ArgValues& a, BlockUtility&) {
    return V(num(a, "NUM1") / num(a, "NUM2"));
}
Value opLt(const ArgValues& a, BlockUtility&) {
    return V(cast::compare(v(a, "OPERAND1"), v(a, "OPERAND2")) < 0);
}
Value opEq(const ArgValues& a, BlockUtility&) {
    return V(cast::compare(v(a, "OPERAND1"), v(a, "OPERAND2")) == 0);
}
Value opGt(const ArgValues& a, BlockUtility&) {
    return V(cast::compare(v(a, "OPERAND1"), v(a, "OPERAND2")) > 0);
}
Value opAnd(const ArgValues& a, BlockUtility&) {
    return V(truth(a, "OPERAND1") && truth(a, "OPERAND2"));
}
Value opOr(const ArgValues& a, BlockUtility&) {
    return V(truth(a, "OPERAND1") || truth(a, "OPERAND2"));
}
Value opNot(const ArgValues& a, BlockUtility&) {
    return V(!truth(a, "OPERAND"));
}
Value opRandom(const ArgValues& a, BlockUtility&) {
    Value from = v(a, "FROM"), to = v(a, "TO");
    double nFrom = cast::toNumber(from), nTo = cast::toNumber(to);
    double low = nFrom <= nTo ? nFrom : nTo;
    double high = nFrom <= nTo ? nTo : nFrom;
    if (low == high) return V(low);
    if (cast::isInt(from) && cast::isInt(to)) {
        return V(rt::randomInt(low, high));
    }
    return V(rt::randomFloat(low, high));
}
Value opJoin(const ArgValues& a, BlockUtility&) {
    return rt::join(v(a, "STRING1"), v(a, "STRING2"));
}
Value opLetterOf(const ArgValues& a, BlockUtility&) {
    return rt::letterOf(v(a, "LETTER"), v(a, "STRING"));
}
Value opLength(const ArgValues& a, BlockUtility&) {
    return V(rt::strLength(v(a, "STRING")));
}
Value opContains(const ArgValues& a, BlockUtility&) {
    SStr* h = SStr::create(u16ToLower(toStr(a.get("STRING1"))));
    SStr* n = SStr::create(u16ToLower(toStr(a.get("STRING2"))));
    return V(sstrContains(h->view(), n->view()));
}
Value opMod(const ArgValues& a, BlockUtility&) {
    return V(rt::mod(num(a, "NUM1"), num(a, "NUM2")));
}
Value opRound(const ArgValues& a, BlockUtility&) {
    return V(jsnum::jsRound(num(a, "NUM")));
}
Value opMathop(const ArgValues& a, BlockUtility&) {
    std::u16string op = strLower(a, "OPERATOR");
    double n = num(a, "NUM");
    if (op == u"abs") return V(std::fabs(n));
    if (op == u"floor") return V(std::floor(n));
    if (op == u"ceiling") return V(std::ceil(n));
    if (op == u"sqrt") return V(std::sqrt(n));
    if (op == u"sin")
        return V(jsnum::jsRound(v8math::sin((M_PI * n) / 180.0) * 1e10) / 1e10);
    if (op == u"cos")
        return V(jsnum::jsRound(v8math::cos((M_PI * n) / 180.0) * 1e10) / 1e10);
    if (op == u"tan") return V(rt::tanDeg(n));
    if (op == u"asin") return V((v8math::asin(n) * 180.0) / M_PI);
    if (op == u"acos") return V((v8math::acos(n) * 180.0) / M_PI);
    if (op == u"atan") return V((v8math::atan(n) * 180.0) / M_PI);
    if (op == u"ln") return V(jsnum::jsLog(n));
    if (op == u"log") return V(jsnum::jsLog(n) / M_LN10);
    if (op == u"e ^") return V(v8math::exp(n));
    if (op == u"10 ^") return V(std::pow(10.0, n));
    return V(0.0);
}

// ============================================================= control ====

Value ctlRepeat(const ArgValues& a, BlockUtility& u) {
    double times = jsnum::jsRound(num(a, "TIMES"));
    ExecContext& sf = u.stackFrame();
    if (!sf.hasLoopCounter) {
        sf.loopCounter = times;
        sf.hasLoopCounter = true;
    }
    sf.loopCounter--;
    if (sf.loopCounter >= 0) u.startBranch(1, true);
    return Value::undefined();
}

Value ctlRepeatUntil(const ArgValues& a, BlockUtility& u) {
    if (!truth(a, "CONDITION")) u.startBranch(1, true);
    return Value::undefined();
}
Value ctlRepeatWhile(const ArgValues& a, BlockUtility& u) {
    if (truth(a, "CONDITION")) u.startBranch(1, true);
    return Value::undefined();
}
Value ctlForEach(const ArgValues& a, BlockUtility& u) {
    const IdName* varRef = obj(a, "VARIABLE");
    Variable* var = nullptr;
    if (varRef) {
        var = u.target()->lookupOrCreateVariable(varRef->id, varRef->name);
    }
    ExecContext& sf = u.stackFrame();
    if (!sf.hasIndex) { sf.index = 0; sf.hasIndex = true; }
    if (sf.index < cast::toNumber(v(a, "VALUE"))) {
        sf.index++;
        if (var) var->value = V(sf.index);
        u.startBranch(1, true);
    }
    return Value::undefined();
}
Value ctlForever(const ArgValues&, BlockUtility& u) {
    u.startBranch(1, true);
    return Value::undefined();
}
Value ctlWaitUntil(const ArgValues& a, BlockUtility& u) {
    if (!truth(a, "CONDITION")) u.yield();
    return Value::undefined();
}
Value ctlWait(const ArgValues& a, BlockUtility& u) {
    if (u.stackTimerNeedsInit()) {
        double duration = std::max(0.0, 1000.0 * num(a, "DURATION"));
        u.startStackTimer(duration);
        u.runtime()->redrawRequested = true;
        u.yield();
    } else if (!u.stackTimerFinished()) {
        u.yield();
    }
    return Value::undefined();
}
Value ctlIf(const ArgValues& a, BlockUtility& u) {
    if (truth(a, "CONDITION")) u.startBranch(1, false);
    return Value::undefined();
}
Value ctlIfElse(const ArgValues& a, BlockUtility& u) {
    u.startBranch(truth(a, "CONDITION") ? 1 : 2, false);
    return Value::undefined();
}
Value ctlStop(const ArgValues& a, BlockUtility& u) {
    std::u16string option = str(a, "STOP_OPTION");
    if (option == u"all") {
        u.stopAll();
    } else if (option == u"other scripts in sprite" ||
               option == u"other scripts in stage") {
        u.stopOtherTargetThreads();
    } else if (option == u"this script") {
        u.stopThisScript();
    }
    return Value::undefined();
}
Value ctlCreateClone(const ArgValues& a, BlockUtility& u) {
    std::u16string opt = str(a, "CLONE_OPTION");
    Target* cloneTarget;
    if (opt == u"_myself_") {
        cloneTarget = u.target();
    } else {
        cloneTarget = u.runtime()->getSpriteTargetByName(opt);
    }
    if (!cloneTarget) return Value::undefined();
    Target* c = cloneTarget->makeClone();
    if (c) {
        u.runtime()->addTarget(c);
        c->goBehindOther(cloneTarget);
    }
    return Value::undefined();
}
Value ctlDeleteClone(const ArgValues&, BlockUtility& u) {
    Target* t = u.target();
    if (t->isOriginal) return Value::undefined();
    u.runtime()->disposeTarget(t);
    u.runtime()->stopForTarget(t, nullptr);
    return Value::undefined();
}
Value ctlGetCounter(const ArgValues&, BlockUtility& u) {
    return V(u.runtime()->controlCounter);
}
Value ctlClearCounter(const ArgValues&, BlockUtility& u) {
    u.runtime()->controlCounter = 0;
    return Value::undefined();
}
Value ctlIncrCounter(const ArgValues&, BlockUtility& u) {
    u.runtime()->controlCounter++;
    return Value::undefined();
}
Value ctlAllAtOnce(const ArgValues&, BlockUtility& u) {
    u.startBranch(1, false);
    return Value::undefined();
}

// =============================================================== event ====

Value evTouchingObject(const ArgValues& a, BlockUtility& u) {
    return V(u.target()->isTouchingObject(str(a, "TOUCHINGOBJECTMENU")));
}

Value evHatGreaterThan(const ArgValues& a, BlockUtility& u) {
    std::u16string option = strLower(a, "WHENGREATERTHANMENU");
    double value = num(a, "VALUE");
    if (option == u"timer")
        return V(u.clockProjectTimer() > value);
    if (option == u"loudness")
        return V(u.runtime()->audioEngine &&
                 u.runtime()->audioEngine->getLoudness() > value);
    return V(false);
}

Value evBroadcast(const ArgValues& a, BlockUtility& u) {
    const IdName* bo = obj(a, "BROADCAST_OPTION");
    if (!bo) return Value::undefined();
    Target* stage = u.runtime()->getTargetForStage();
    if (!stage) return Value::undefined();
    Variable* bv = stage->lookupBroadcastMsg(bo->id, bo->name);
    if (bv) {
        std::map<std::string, std::u16string> m{{"BROADCAST_OPTION", bv->name}};
        u.startHats("event_whenbroadcastreceived", &m, nullptr);
    }
    return Value::undefined();
}

Value evBroadcastAndWait(const ArgValues& a, BlockUtility& u) {
    ExecContext& sf = u.stackFrame();
    if (!sf.broadcastVar) {
        const IdName* bo = obj(a, "BROADCAST_OPTION");
        Target* stage = u.runtime()->getTargetForStage();
        if (bo && stage)
            sf.broadcastVar = stage->lookupBroadcastMsg(bo->id, bo->name);
    }
    if (sf.broadcastVar) {
        if (!sf.hasStartedThreads) {
            std::map<std::string, std::u16string> m{
                {"BROADCAST_OPTION", sf.broadcastVar->name}};
            sf.startedThreads = u.startHats(
                "event_whenbroadcastreceived", &m, nullptr);
            sf.hasStartedThreads = true;
            if (sf.startedThreads.empty()) return Value::undefined();
        }
        Runtime* rt = u.runtime();
        bool waiting = false, allWaiting = true;
        for (Thread* t : sf.startedThreads) {
            bool inList = false;
            for (Thread* x : rt->threads) if (x == t) { inList = true; break; }
            if (inList) {
                waiting = true;
                if (!rt->isWaitingThread(t)) allWaiting = false;
            }
        }
        if (waiting) {
            if (allWaiting) u.yieldTick();
            else u.yield();
        }
    }
    return Value::undefined();
}

// =============================================================== data ====

Variable* lookupVar(const ArgValues& a, BlockUtility& u, const char* name,
                    bool isList) {
    const IdName* r = obj(a, name);
    if (!r) return nullptr;
    return isList ? u.target()->lookupOrCreateList(r->id, r->name)
                  : u.target()->lookupOrCreateVariable(r->id, r->name);
}
SList* listOf(Variable* v) {
    if (!v->list) v->list = SList::create();
    return v->list;
}

Value dtGetVar(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "VARIABLE", false);
    return var ? var->value : Value::emptyStr();
}
Value dtSetVar(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "VARIABLE", false);
    if (!var) return Value::undefined();
    var->value = v(a, "VALUE");
    if (var->isCloud) {
        // cloud requestUpdateVariable — 未実装（クラウド非対応）
    }
    return Value::undefined();
}
Value dtChangeVar(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "VARIABLE", false);
    if (!var) return Value::undefined();
    double nv = cast::toNumber(var->value) + num(a, "VALUE");
    var->value = V(nv);
    return Value::undefined();
}

void changeMonitorVisibility(BlockUtility& u, const IdName* r, bool visible) {
    if (!r) return;
    Runtime* rt = u.runtime();
    MonitorRecord& rec = rt->monitorState.getOrCreate(r->id);
    if (rec.id.empty()) rec.id = r->id;
    rec.visible = visible;
    rt->monitorState.dirty = true;
    // monitorBlocks 側の isMonitored フラグも更新（checkbox イベント相当）
    if (Block* b = rt->monitorBlocks->getBlock(r->id)) b->isMonitored = visible;
    rt->monitorBlocks->resetCache();
}

Value dtShowVar(const ArgValues& a, BlockUtility& u) {
    changeMonitorVisibility(u, obj(a, "VARIABLE"), true);
    return Value::undefined();
}
Value dtHideVar(const ArgValues& a, BlockUtility& u) {
    changeMonitorVisibility(u, obj(a, "VARIABLE"), false);
    return Value::undefined();
}
Value dtShowList(const ArgValues& a, BlockUtility& u) {
    changeMonitorVisibility(u, obj(a, "LIST"), true);
    return Value::undefined();
}
Value dtHideList(const ArgValues& a, BlockUtility& u) {
    changeMonitorVisibility(u, obj(a, "LIST"), false);
    return Value::undefined();
}

Value dtListContents(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return Value::emptyStr();
    // JS は monitor 実行時に「変更なしなら同一配列」を返して更新を抑制するが、
    // ネイティブでは報告値の等価比較で抑制するため常に contents を計算する。
    return rt::listContents(listOf(var));
}
Value dtAddToList(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return Value::undefined();
    SList* list = listOf(var);
    list->push(v(a, "ITEM"));
    list->monitorDirty = true;
    return Value::undefined();
}
Value dtDeleteOfList(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return Value::undefined();
    rt::listDelete(listOf(var), v(a, "INDEX"));
    return Value::undefined();
}
Value dtDeleteAllOfList(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return Value::undefined();
    rt::listDeleteAll(listOf(var));
    listOf(var)->monitorDirty = true;
    return Value::undefined();
}
Value dtInsertAtList(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return Value::undefined();
    SList* list = listOf(var);
    double i = cast::toListIndexD(v(a, "INDEX"), list->len + 1.0, false);
    if (i == cast::LIST_INVALID || i < 1) return Value::undefined();
    list->insert0(static_cast<uint32_t>(i - 1), v(a, "ITEM"));
    list->monitorDirty = true;
    return Value::undefined();
}
Value dtReplaceItemOfList(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return Value::undefined();
    SList* list = listOf(var);
    double i = cast::toListIndexD(v(a, "INDEX"), list->len, false);
    if (i == cast::LIST_INVALID || i < 1) return Value::undefined();
    list->items[static_cast<uint32_t>(i - 1)] = v(a, "ITEM");
    list->monitorDirty = true;
    return Value::undefined();
}
Value dtItemOfList(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return Value::emptyStr();
    SList* list = listOf(var);
    double i = cast::toListIndexD(v(a, "INDEX"), list->len, false);
    if (i == cast::LIST_INVALID || i < 1) return Value::emptyStr();
    return list->items[static_cast<uint32_t>(i - 1)];
}
Value dtItemNumOfList(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return V(0.0);
    return V(rt::listIndexOf(listOf(var), v(a, "ITEM")));
}
Value dtLengthOfList(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return V(0.0);
    return V(double(listOf(var)->len));
}
Value dtListContainsItem(const ArgValues& a, BlockUtility& u) {
    Variable* var = lookupVar(a, u, "LIST", true);
    if (!var) return V(false);
    return V(rt::listContains(listOf(var), v(a, "ITEM")));
}

// ================================================================ looks ===

/// Scratch.looks バブル状態を target に保持するための拡張（customState 相当）
struct BubbleExtra {
    // target 側に bubbleText/bubbleType/bubbleUsageId を持たせる設計。
    // ここでは Target のフィールドを直接使う。
};


Value looksSay(const ArgValues& a, BlockUtility& u) {
    updateBubble(u.target(), "say", v(a, "MESSAGE"));
    return Value::undefined();
}
Value looksThink(const ArgValues& a, BlockUtility& u) {
    updateBubble(u.target(), "think", v(a, "MESSAGE"));
    return Value::undefined();
}

Value looksSayOrThinkForSecs(const ArgValues& a, BlockUtility& u,
                             const char* type) {
    updateBubble(u.target(), type, v(a, "MESSAGE"));
    Target* t = u.target();
    int usageId = t->bubbleUsageId;
    // setTimeout(1000*SECS) 相当 — wait queue に登録し PROMISE_WAIT
    Runtime* rt = u.runtime();
    Runtime::WaitEntry w;
    w.thread = u.thread;
    w.deadlineMs = rt->currentMSecs + std::max(0.0, 1000.0 * num(a, "SECS"));
    w.bubbleTarget = t;
    w.bubbleType = type;
    w.bubbleUsageId = usageId;
    rt->waitQueue.push_back(w);
    u.thread->status = STATUS_PROMISE_WAIT;
    return Value::undefined();
}
Value looksSayForSecs(const ArgValues& a, BlockUtility& u) {
    return looksSayOrThinkForSecs(a, u, "say");
}
Value looksThinkForSecs(const ArgValues& a, BlockUtility& u) {
    return looksSayOrThinkForSecs(a, u, "think");
}

Value looksShow(const ArgValues&, BlockUtility& u) {
    u.target()->setVisible(true);
    return Value::undefined();
}
Value looksHide(const ArgValues&, BlockUtility& u) {
    u.target()->setVisible(false);
    return Value::undefined();
}


Value looksSwitchCostume(const ArgValues& a, BlockUtility& u) {
    setCostumeArg(u.target(), v(a, "COSTUME"), false);
    return Value::undefined();
}
Value looksNextCostume(const ArgValues&, BlockUtility& u) {
    setCostumeArg(u.target(), V(u.target()->currentCostume + 1.0), true);
    return Value::undefined();
}
Value looksSwitchBackdrop(const ArgValues& a, BlockUtility& u) {
    Target* stage = u.runtime()->getTargetForStage();
    if (stage) setBackdropArg(*u.runtime(), stage, v(a, "BACKDROP"), false);
    return Value::undefined();
}
Value looksSwitchBackdropAndWait(const ArgValues& a, BlockUtility& u) {
    ExecContext& sf = u.stackFrame();
    if (!sf.hasStartedThreads) {
        Target* stage = u.runtime()->getTargetForStage();
        if (!stage) return Value::undefined();
        sf.startedThreads = setBackdropArg(*u.runtime(), stage, v(a, "BACKDROP"), false);
        sf.hasStartedThreads = true;
        if (sf.startedThreads.empty()) return Value::undefined();
    }
    Runtime* rt = u.runtime();
    bool waiting = false, allWaiting = true;
    for (Thread* t : sf.startedThreads) {
        bool inList = false;
        for (Thread* x : rt->threads) if (x == t) { inList = true; break; }
        if (inList) {
            waiting = true;
            if (!rt->isWaitingThread(t)) allWaiting = false;
        }
    }
    if (waiting) {
        if (allWaiting) u.yieldTick();
        else u.yield();
    }
    return Value::undefined();
}
Value looksNextBackdrop(const ArgValues&, BlockUtility& u) {
    Target* stage = u.runtime()->getTargetForStage();
    if (stage) setBackdropArg(*u.runtime(), stage, V(stage->currentCostume + 1.0), true);
    return Value::undefined();
}

Value looksChangeEffect(const ArgValues& a, BlockUtility& u) {
    std::u16string effect = strLower(a, "EFFECT");
    double change = num(a, "CHANGE");
    Target* t = u.target();
    if (!t->effects.count(SStr::toUtf8(effect))) return Value::undefined();
    double nv = change + t->effects[SStr::toUtf8(effect)];
    nv = clampEffect(effect, nv);
    t->setEffect(SStr::toUtf8(effect), nv);
    return Value::undefined();
}
Value looksSetEffect(const ArgValues& a, BlockUtility& u) {
    std::u16string effect = strLower(a, "EFFECT");
    double value = clampEffect(effect, num(a, "VALUE"));
    u.target()->setEffect(SStr::toUtf8(effect), value);
    return Value::undefined();
}
Value looksClearEffects(const ArgValues&, BlockUtility& u) {
    u.target()->clearEffects();
    return Value::undefined();
}
Value looksChangeSize(const ArgValues& a, BlockUtility& u) {
    u.target()->setSize(u.target()->size + num(a, "CHANGE"));
    return Value::undefined();
}
Value looksSetSize(const ArgValues& a, BlockUtility& u) {
    u.target()->setSize(num(a, "SIZE"));
    return Value::undefined();
}
Value looksGoToFrontBack(const ArgValues& a, BlockUtility& u) {
    Target* t = u.target();
    if (t->isStage) return Value::undefined();
    if (str(a, "FRONT_BACK") == u"front") t->goToFront();
    else t->goToBack();
    return Value::undefined();
}
Value looksGoForwardBackwardLayers(const ArgValues& a, BlockUtility& u) {
    Target* t = u.target();
    if (t->isStage) return Value::undefined();
    if (str(a, "FORWARD_BACKWARD") == u"forward")
        t->goForwardLayers(num(a, "NUM"));
    else
        t->goBackwardLayers(num(a, "NUM"));
    return Value::undefined();
}
Value looksGetSize(const ArgValues&, BlockUtility& u) {
    return V(jsnum::jsRound(u.target()->size));
}
Value looksCostumeNumberName(const ArgValues& a, BlockUtility& u) {
    Target* t = u.target();
    if (str(a, "NUMBER_NAME") == u"number")
        return V(t->currentCostume + 1.0);
    const Costume* c = t->getCurrentCostume();
    return c ? VS(c->name) : Value::emptyStr();
}
Value looksBackdropNumberName(const ArgValues& a, BlockUtility& u) {
    Target* stage = u.runtime()->getTargetForStage();
    if (!stage) return V(0.0);
    if (str(a, "NUMBER_NAME") == u"number")
        return V(stage->currentCostume + 1.0);
    const Costume* c = stage->getCurrentCostume();
    return c ? VS(c->name) : Value::emptyStr();
}

// =============================================================== motion ====

Value motMoveSteps(const ArgValues& a, BlockUtility& u) {
    moveSteps(num(a, "STEPS"), u.target());
    return Value::undefined();
}
Value motGoToXY(const ArgValues& a, BlockUtility& u) {
    u.target()->setXY(num(a, "X"), num(a, "Y"));
    return Value::undefined();
}
Value motGoTo(const ArgValues& a, BlockUtility& u) {
    double tx, ty;
    if (getTargetXY(v(a, "TO"), *u.runtime(), tx, ty)) u.target()->setXY(tx, ty);
    return Value::undefined();
}
Value motTurnRight(const ArgValues& a, BlockUtility& u) {
    u.target()->setDirection(u.target()->direction + num(a, "DEGREES"));
    return Value::undefined();
}
Value motTurnLeft(const ArgValues& a, BlockUtility& u) {
    u.target()->setDirection(u.target()->direction - num(a, "DEGREES"));
    return Value::undefined();
}
Value motPointInDirection(const ArgValues& a, BlockUtility& u) {
    u.target()->setDirection(num(a, "DIRECTION"));
    return Value::undefined();
}
Value motPointTowards(const ArgValues& a, BlockUtility& u) {
    Value towards = v(a, "TOWARDS");
    double tx = 0, ty = 0;
    if (towards.isString() && towards.asStrPtr() &&
        towards.asStrPtr()->view() == u"_mouse_") {
        tx = u.mouseScratchX();
        ty = u.mouseScratchY();
    } else if (towards.isString() && towards.asStrPtr() &&
               towards.asStrPtr()->view() == u"_random_") {
        u.target()->setDirection(
            jsnum::jsRound(Rng::global().nextDouble() * 360) - 180);
        return Value::undefined();
    } else {
        std::u16string name = toStr(towards);
        Target* pt = u.runtime()->getSpriteTargetByName(name);
        if (!pt) return Value::undefined();
        tx = pt->x;
        ty = pt->y;
    }
    double dx = tx - u.target()->x;
    double dy = ty - u.target()->y;
    double direction = 90.0 - (v8math::atan2(dy, dx) * 180.0 / M_PI);
    u.target()->setDirection(direction);
    return Value::undefined();
}

Value motGlide(const ArgValues& a, BlockUtility& u) {
    ExecContext& sf = u.stackFrame();
    Target* t = u.target();
    if (sf.timer) {
        double timeElapsed = sf.timer->timeElapsed();
        if (timeElapsed < sf.duration * 1000.0) {
            double frac = timeElapsed / (sf.duration * 1000.0);
            double dx = frac * (sf.endX - sf.startX);
            double dy = frac * (sf.endY - sf.startY);
            t->setXY(sf.startX + dx, sf.startY + dy);
            u.yield();
        } else {
            t->setXY(sf.endX, sf.endY);
        }
    } else {
        sf.timer = std::make_unique<Timer>();
        sf.timer->start();
        sf.duration = num(a, "SECS");
        sf.startX = t->x;
        sf.startY = t->y;
        sf.endX = num(a, "X");
        sf.endY = num(a, "Y");
        if (sf.duration <= 0) {
            t->setXY(sf.endX, sf.endY);
            return Value::undefined();
        }
        u.yield();
    }
    return Value::undefined();
}
Value motGlideTo(const ArgValues& a, BlockUtility& u) {
    double tx, ty;
    if (!getTargetXY(v(a, "TO"), *u.runtime(), tx, ty)) return Value::undefined();
    // glide({SECS, X: tx, Y: ty}) — args を差し替えて motGlide を再利用
    ArgValues a2;
    a2.vals["SECS"] = v(a, "SECS");
    a2.vals["X"] = V(tx);
    a2.vals["Y"] = V(ty);
    return motGlide(a2, u);
}
Value motIfOnEdgeBounce(const ArgValues&, BlockUtility& u) {
    Target* t = u.target();
    Target::Bounds bounds;
    if (!t->getBounds(bounds)) return Value::undefined();
    Runtime* rt = u.runtime();
    double stageW = rt->stageWidth, stageH = rt->stageHeight;
    double distLeft = std::max(0.0, stageW / 2 + bounds.left);
    double distTop = std::max(0.0, stageH / 2 - bounds.top);
    double distRight = std::max(0.0, stageW / 2 - bounds.right);
    double distBottom = std::max(0.0, stageH / 2 + bounds.bottom);
    std::string nearest;
    double minDist = std::numeric_limits<double>::infinity();
    if (distLeft < minDist) { minDist = distLeft; nearest = "left"; }
    if (distTop < minDist) { minDist = distTop; nearest = "top"; }
    if (distRight < minDist) { minDist = distRight; nearest = "right"; }
    if (distBottom < minDist) { minDist = distBottom; nearest = "bottom"; }
    if (minDist > 0) return Value::undefined();
    double radians = (M_PI * (90.0 - t->direction)) / 180.0;
    double dx = v8math::cos(radians);
    double dy = -v8math::sin(radians);
    if (nearest == "left") dx = std::max(0.2, std::fabs(dx));
    else if (nearest == "top") dy = std::max(0.2, std::fabs(dy));
    else if (nearest == "right") dx = -std::max(0.2, std::fabs(dx));
    else if (nearest == "bottom") dy = -std::max(0.2, std::fabs(dy));
    double newDir = v8math::atan2(dy, dx) * 180.0 / M_PI + 90.0;
    t->setDirection(newDir);
    double fx, fy;
    if (t->keepInFence(t->x, t->y, fx, fy)) t->setXY(fx, fy);
    return Value::undefined();
}
Value motSetRotationStyle(const ArgValues& a, BlockUtility& u) {
    u.target()->setRotationStyle(SStr::toUtf8(str(a, "STYLE")));
    return Value::undefined();
}
Value motChangeX(const ArgValues& a, BlockUtility& u) {
    Target* t = u.target();
    t->setXY(t->x + num(a, "DX"), t->y);
    return Value::undefined();
}
Value motSetX(const ArgValues& a, BlockUtility& u) {
    u.target()->setXY(num(a, "X"), u.target()->y);
    return Value::undefined();
}
Value motChangeY(const ArgValues& a, BlockUtility& u) {
    Target* t = u.target();
    t->setXY(t->x, t->y + num(a, "DY"));
    return Value::undefined();
}
Value motSetY(const ArgValues& a, BlockUtility& u) {
    u.target()->setXY(u.target()->x, num(a, "Y"));
    return Value::undefined();
}
Value motGetX(const ArgValues&, BlockUtility& u) {
    return V(rt::limitPrecision(u.target()->x));
}
Value motGetY(const ArgValues&, BlockUtility& u) {
    return V(rt::limitPrecision(u.target()->y));
}
Value motGetDirection(const ArgValues&, BlockUtility& u) {
    return V(u.target()->direction);
}
Value noop(const ArgValues&, BlockUtility&) { return Value::undefined(); }

// ============================================================== sensing ====

Value snsTouchingColor(const ArgValues& a, BlockUtility& u) {
    Rgb c = cast::toRgbColorList(v(a, "COLOR"));
    uint32_t packed = (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b;
    return V(u.target()->isTouchingColor(packed));
}
Value snsColorTouchingColor(const ArgValues& a, BlockUtility& u) {
    Rgb t = cast::toRgbColorList(v(a, "COLOR2"));
    Rgb m = cast::toRgbColorList(v(a, "COLOR"));
    uint32_t tp = (uint32_t(t.r) << 16) | (uint32_t(t.g) << 8) | t.b;
    uint32_t mp = (uint32_t(m.r) << 16) | (uint32_t(m.g) << 8) | m.b;
    return V(u.target()->colorIsTouchingColor(tp, mp));
}
Value snsDistanceTo(const ArgValues& a, BlockUtility& u) {
    Target* t = u.target();
    if (t->isStage) return V(10000.0);
    Value menu = v(a, "DISTANCETOMENU");
    double tx = 0, ty = 0;
    if (menu.isString() && menu.asStrPtr() &&
        menu.asStrPtr()->view() == u"_mouse_") {
        tx = u.mouseScratchX();
        ty = u.mouseScratchY();
    } else {
        Target* other = u.runtime()->getSpriteTargetByName(toStr(menu));
        if (!other) return V(10000.0);
        tx = other->x;
        ty = other->y;
    }
    double dx = t->x - tx, dy = t->y - ty;
    return V(std::sqrt(dx * dx + dy * dy));
}
Value snsTimer(const ArgValues&, BlockUtility& u) {
    return V(u.clockProjectTimer());
}
Value snsResetTimer(const ArgValues&, BlockUtility& u) {
    u.runtime()->ioDevices.clock.resetProjectTimer();
    return Value::undefined();
}
Value snsMouseX(const ArgValues&, BlockUtility& u) {
    return V(u.mouseScratchX());
}
Value snsMouseY(const ArgValues&, BlockUtility& u) {
    return V(u.mouseScratchY());
}
Value snsMouseDown(const ArgValues&, BlockUtility& u) {
    return V(u.mouseIsDown());
}
Value snsSetDragMode(const ArgValues& a, BlockUtility& u) {
    u.target()->setDraggable(str(a, "DRAG_MODE") == u"draggable");
    return Value::undefined();
}
Value snsKeyPressed(const ArgValues& a, BlockUtility& u) {
    return V(u.keyboardIsDown(v(a, "KEY_OPTION")));
}
Value snsCurrent(const ArgValues& a, BlockUtility&) {
    std::u16string option = strLower(a, "CURRENTMENU");
    std::time_t now = std::time(nullptr);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);      // ordem invertida da POSIX localtime_r
#else
    localtime_r(&now, &tm);
#endif
    if (option == u"year") return V(tm.tm_year + 1900.0);
    if (option == u"month") return V(tm.tm_mon + 1.0);
    if (option == u"date") return V(double(tm.tm_mday));
    if (option == u"dayofweek") return V(tm.tm_wday + 1.0);
    if (option == u"hour") return V(double(tm.tm_hour));
    if (option == u"minute") return V(double(tm.tm_min));
    if (option == u"second") return V(double(tm.tm_sec));
    return V(0.0);
}
Value snsDaysSince2000(const ArgValues&, BlockUtility&) {
    return V(rt::daysSince2000());
}
Value snsLoudness(const ArgValues&, BlockUtility& u) {
    Runtime* rt = u.runtime();
    if (!rt->audioEngine) return V(-1.0);
    double since =
        rt->loudnessTimer.timeElapsed() - rt->cachedLoudnessTimestamp;
    if (since < rt->currentStepTime) return V(rt->cachedLoudness);
    rt->cachedLoudnessTimestamp = rt->loudnessTimer.timeElapsed();
    rt->cachedLoudness = rt->audioEngine->getLoudness();
    return V(rt->cachedLoudness);
}
Value snsLoud(const ArgValues& a, BlockUtility& u) {
    return V(cast::toNumber(snsLoudness(a, u)) > 10);
}
Value snsAnswer(const ArgValues&, BlockUtility& u) {
    return VS(u.runtime()->answer);
}
Value snsAskAndWait(const ArgValues& a, BlockUtility& u) {
    Runtime* rt = u.runtime();
    Target* t = u.target();
    Runtime::AskEntry e;
    e.question = toStr(v(a, "QUESTION"));
    e.thread = u.thread;
    e.target = t;
    e.wasVisible = t->visible;
    e.wasStage = t->isStage;
    bool wasEmpty = rt->questionList.empty();
    rt->questionList.push_back(e);
    if (wasEmpty) {
        // _askNextQuestion
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
    u.thread->status = STATUS_PROMISE_WAIT;
    return Value::undefined();
}
Value snsOf(const ArgValues& a, BlockUtility& u) {
    Runtime* rt = u.runtime();
    Value objArg = v(a, "OBJECT");
    Target* attrTarget;
    if (objArg.isString() && objArg.asStrPtr() &&
        objArg.asStrPtr()->view() == u"_stage_") {
        attrTarget = rt->getTargetForStage();
    } else {
        attrTarget = rt->getSpriteTargetByName(toStr(objArg));
    }
    if (!attrTarget) return V(0.0);
    std::u16string prop = str(a, "PROPERTY");
    if (attrTarget->isStage) {
        if (prop == u"background #" || prop == u"backdrop #")
            return V(attrTarget->currentCostume + 1.0);
        if (prop == u"backdrop name") {
            const Costume* c = attrTarget->getCurrentCostume();
            return c ? VS(c->name) : Value::emptyStr();
        }
        if (prop == u"volume") return V(attrTarget->volume);
    } else {
        if (prop == u"x position") return V(attrTarget->x);
        if (prop == u"y position") return V(attrTarget->y);
        if (prop == u"direction") return V(attrTarget->direction);
        if (prop == u"costume #") return V(attrTarget->currentCostume + 1.0);
        if (prop == u"costume name") {
            const Costume* c = attrTarget->getCurrentCostume();
            return c ? VS(c->name) : Value::emptyStr();
        }
        if (prop == u"size") return V(attrTarget->size);
        if (prop == u"volume") return V(attrTarget->volume);
    }
    Variable* var = attrTarget->lookupVariableByNameAndType(
        prop, Variable::kScalarType, true);
    if (var) return var->value;
    return V(0.0);
}
Value snsUsername(const ArgValues&, BlockUtility& u) {
    return VS(u.username());
}
Value snsOnline(const ArgValues&, BlockUtility&) {
    return V(true);   // 非ブラウザ環境: JS は常に true を返す
}

// ================================================================ sound ====

Value sndPlay(const ArgValues& a, BlockUtility& u) {
    Target* t = u.target();
    if (!t->sprite || !t->sprite->soundBank) return Value::undefined();
    int idx = getSoundIndex(v(a, "SOUND_MENU"), t);
    if (idx >= 0)
        t->sprite->soundBank->playSound(t, t->sprite->sounds[idx].soundId);
    return Value::undefined();
}
Value sndPlayUntilDone(const ArgValues& a, BlockUtility& u) {
    Target* t = u.target();
    if (!t->sprite || !t->sprite->soundBank) return Value::undefined();
    int idx = getSoundIndex(v(a, "SOUND_MENU"), t);
    if (idx >= 0) {
        u.thread->status = STATUS_PROMISE_WAIT;
        t->sprite->soundBank->playSoundAndWait(
            t, t->sprite->sounds[idx].soundId, u.thread);
    }
    return Value::undefined();
}
Value sndStopAll(const ArgValues&, BlockUtility& u) {
    for (Target* t : u.runtime()->targets) {
        if (t->sprite && t->sprite->soundBank)
            t->sprite->soundBank->stopAllSounds(t);
    }
    return Value::undefined();
}

struct SoundFxRange { double min, max; };
SoundFxRange soundFxRange(const std::string& effect, bool miscLimits) {
    if (effect == "pitch")
        return miscLimits ? SoundFxRange{-360, 360} : SoundFxRange{-1000, 1000};
    return {-100, 100};   // pan
}

Value sndUpdateEffect(const ArgValues& a, BlockUtility& u, bool change) {
    std::u16string effectU = strLower(a, "EFFECT");
    std::string effect = SStr::toUtf8(effectU);
    double value = num(a, "VALUE");
    Target* t = u.target();
    if (effect != "pitch" && effect != "pan") return Value::undefined();
    if (!t->soundEffects.count(effect)) {
        t->soundEffects["pitch"] = 0;
        t->soundEffects["pan"] = 0;
    }
    if (change) t->soundEffects[effect] += value;
    else t->soundEffects[effect] = value;
    Runtime* rt = u.runtime();
    SoundFxRange r = soundFxRange(effect, rt->miscLimits);
    t->soundEffects[effect] =
        std::min(std::max(t->soundEffects[effect], r.min), r.max);
    if (rt->miscLimits) {
        // JS: Promise.resolve() — 次ティックに解決
        u.promiseResolveNextTick();
    } else {
        rt->redrawRequested = true;
    }
    return Value::undefined();
}
Value sndSetEffect(const ArgValues& a, BlockUtility& u) {
    return sndUpdateEffect(a, u, false);
}
Value sndChangeEffect(const ArgValues& a, BlockUtility& u) {
    return sndUpdateEffect(a, u, true);
}
Value sndClearEffects(const ArgValues&, BlockUtility& u) {
    Target* t = u.target();
    t->soundEffects["pitch"] = 0;
    t->soundEffects["pan"] = 0;
    return Value::undefined();
}
Value sndUpdateVolume(const ArgValues& a, BlockUtility& u, bool change) {
    double volume = num(a, "VOLUME");
    if (change) volume += u.target()->volume;
    volume = std::min(std::max(volume, 0.0), 100.0);
    u.target()->volume = volume;
    Runtime* rt = u.runtime();
    if (rt->miscLimits) {
        // JS: Promise.resolve() — 次ティックに解決
        u.promiseResolveNextTick();
    } else {
        rt->redrawRequested = true;
    }
    return Value::undefined();
}
Value sndSetVolume(const ArgValues& a, BlockUtility& u) {
    return sndUpdateVolume(a, u, false);
}
Value sndChangeVolume(const ArgValues& a, BlockUtility& u) {
    return sndUpdateVolume(a, u, true);
}
Value sndGetVolume(const ArgValues&, BlockUtility& u) {
    return V(u.target()->volume);
}
Value sndSoundsMenu(const ArgValues& a, BlockUtility&) {
    return v(a, "SOUND_MENU");
}
Value sndBeatsMenu(const ArgValues& a, BlockUtility&) {
    return v(a, "BEATS");
}
Value sndEffectsMenu(const ArgValues& a, BlockUtility&) {
    return v(a, "EFFECT");
}

// =========================================================== procedures ====

Value procDefinition(const ArgValues&, BlockUtility&) {
    return Value::undefined();
}

Value procCall(const ArgValues& a, BlockUtility& u) {
    ExecContext& sf = u.stackFrame();
    bool isReporter = a.mutation && a.mutation->isReporterCall();
    if (sf.executed) {
        if (isReporter) {
            Value ret = sf.returnValue;
            StackFrame* tsf = u.thread->peekStackFrame();
            if (tsf) tsf->params.clear();
            sf.returnValue = Value::undefined();
            sf.executed = false;
            return ret;
        }
        return Value::undefined();
    }
    const std::string& proccode =
        a.mutation ? a.mutation->attr("proccode") : "";

    std::vector<std::u16string> names;
    std::vector<std::string> ids;
    std::vector<Value> defaults;
    if (!u.getProcedureParamNamesIdsAndDefaults(proccode, names, ids,
                                              defaults)) {
        return isReporter ? Value::emptyStr() : Value::undefined();
    }

    u.initParams();
    for (size_t i = 0; i < ids.size(); ++i) {
        if (a.has(ids[i])) {
            u.pushParam(names[i], a.get(ids[i]));
        } else if (i < defaults.size()) {
            u.pushParam(names[i], defaults[i]);
        }
    }
    sf.executed = true;
    if (isReporter) {
        u.thread->peekStackFrame()->waitingReporter = true;
        sf.returnValue = Value::emptyStr();
    }
    u.startProcedure(proccode);
    return Value::undefined();
}

Value procReturn(const ArgValues& a, BlockUtility& u) {
    u.stopThisScript();
    if (u.thread->peekStackFrame()) {
        u.stackFrame().returnValue = v(a, "VALUE");
    }
    return Value::undefined();
}

Value procArgStringNumber(const ArgValues& a, BlockUtility& u) {
    Value name = v(a, "VALUE");
    Value p = u.thread->getParam(toStr(name));
    if (p.isUndefined()) {
        if (u16ToLower(toStr(name)) == u"last key pressed")
            return VS(u.keyboardLastKeyPressed());
        return V(0.0);
    }
    return p;
}
Value procArgBoolean(const ArgValues& a, BlockUtility& u) {
    Value name = v(a, "VALUE");
    Value p = u.thread->getParam(toStr(name));
    if (p.isUndefined()) {
        std::u16string lower = u16ToLower(toStr(name));
        if (lower == u"is compiled?" &&
            u.runtime()->compilerOptionsEnabled)
            return V(true);
        if (lower == u"is turbowarp?") return V(true);
        return V(0.0);
    }
    return p;
}

} // namespace

// ============================================================ 登録 ====

void registerCoreBlocks(Runtime& rt) {
    auto prim = [&](const char* op, PrimFn fn) { rt.registerPrimitive(op, fn); };
    auto hat = [&](const char* op, bool edge, bool restart) {
        HatMeta m; m.edgeActivated = edge; m.restartExistingThreads = restart;
        rt.registerHat(op, m);
    };

    prim("operator_add", opAdd);
    prim("operator_subtract", opSub);
    prim("operator_multiply", opMul);
    prim("operator_divide", opDiv);
    prim("operator_lt", opLt);
    prim("operator_equals", opEq);
    prim("operator_gt", opGt);
    prim("operator_and", opAnd);
    prim("operator_or", opOr);
    prim("operator_not", opNot);
    prim("operator_random", opRandom);
    prim("operator_join", opJoin);
    prim("operator_letter_of", opLetterOf);
    prim("operator_length", opLength);
    prim("operator_contains", opContains);
    prim("operator_mod", opMod);
    prim("operator_round", opRound);
    prim("operator_mathop", opMathop);

    prim("control_repeat", ctlRepeat);
    prim("control_repeat_until", ctlRepeatUntil);
    prim("control_while", ctlRepeatWhile);
    prim("control_for_each", ctlForEach);
    prim("control_forever", ctlForever);
    prim("control_wait", ctlWait);
    prim("control_wait_until", ctlWaitUntil);
    prim("control_if", ctlIf);
    prim("control_if_else", ctlIfElse);
    prim("control_stop", ctlStop);
    prim("control_create_clone_of", ctlCreateClone);
    prim("control_delete_this_clone", ctlDeleteClone);
    prim("control_get_counter", ctlGetCounter);
    prim("control_incr_counter", ctlIncrCounter);
    prim("control_clear_counter", ctlClearCounter);
    prim("control_all_at_once", ctlAllAtOnce);
    hat("control_start_as_clone", false, false);

    prim("event_whentouchingobject", evTouchingObject);
    prim("event_broadcast", evBroadcast);
    prim("event_broadcastandwait", evBroadcastAndWait);
    prim("event_whengreaterthan", evHatGreaterThan);
    hat("event_whenflagclicked", false, true);
    hat("event_whenkeypressed", false, false);
    hat("event_whenthisspriteclicked", false, true);
    hat("event_whentouchingobject", true, false);
    hat("event_whenstageclicked", false, true);
    hat("event_whenbackdropswitchesto", false, true);
    hat("event_whengreaterthan", true, false);
    hat("event_whenbroadcastreceived", false, true);

    prim("data_variable", dtGetVar);
    prim("data_setvariableto", dtSetVar);
    prim("data_changevariableby", dtChangeVar);
    prim("data_hidevariable", dtHideVar);
    prim("data_showvariable", dtShowVar);
    prim("data_listcontents", dtListContents);
    prim("data_addtolist", dtAddToList);
    prim("data_deleteoflist", dtDeleteOfList);
    prim("data_deletealloflist", dtDeleteAllOfList);
    prim("data_insertatlist", dtInsertAtList);
    prim("data_replaceitemoflist", dtReplaceItemOfList);
    prim("data_itemoflist", dtItemOfList);
    prim("data_itemnumoflist", dtItemNumOfList);
    prim("data_lengthoflist", dtLengthOfList);
    prim("data_listcontainsitem", dtListContainsItem);
    prim("data_hidelist", dtHideList);
    prim("data_showlist", dtShowList);

    prim("looks_say", looksSay);
    prim("looks_sayforsecs", looksSayForSecs);
    prim("looks_think", looksThink);
    prim("looks_thinkforsecs", looksThinkForSecs);
    prim("looks_show", looksShow);
    prim("looks_hide", looksHide);
    prim("looks_hideallsprites", noop);
    prim("looks_switchcostumeto", looksSwitchCostume);
    prim("looks_switchbackdropto", looksSwitchBackdrop);
    prim("looks_switchbackdroptoandwait", looksSwitchBackdropAndWait);
    prim("looks_nextcostume", looksNextCostume);
    prim("looks_nextbackdrop", looksNextBackdrop);
    prim("looks_changeeffectby", looksChangeEffect);
    prim("looks_seteffectto", looksSetEffect);
    prim("looks_cleargraphiceffects", looksClearEffects);
    prim("looks_changesizeby", looksChangeSize);
    prim("looks_setsizeto", looksSetSize);
    prim("looks_changestretchby", noop);
    prim("looks_setstretchto", noop);
    prim("looks_gotofrontback", looksGoToFrontBack);
    prim("looks_goforwardbackwardlayers", looksGoForwardBackwardLayers);
    prim("looks_size", looksGetSize);
    prim("looks_costumenumbername", looksCostumeNumberName);
    prim("looks_backdropnumbername", looksBackdropNumberName);

    prim("motion_movesteps", motMoveSteps);
    prim("motion_gotoxy", motGoToXY);
    prim("motion_goto", motGoTo);
    prim("motion_turnright", motTurnRight);
    prim("motion_turnleft", motTurnLeft);
    prim("motion_pointindirection", motPointInDirection);
    prim("motion_pointtowards", motPointTowards);
    prim("motion_glidesecstoxy", motGlide);
    prim("motion_glideto", motGlideTo);
    prim("motion_ifonedgebounce", motIfOnEdgeBounce);
    prim("motion_setrotationstyle", motSetRotationStyle);
    prim("motion_changexby", motChangeX);
    prim("motion_setx", motSetX);
    prim("motion_changeyby", motChangeY);
    prim("motion_sety", motSetY);
    prim("motion_xposition", motGetX);
    prim("motion_yposition", motGetY);
    prim("motion_direction", motGetDirection);
    prim("motion_scroll_right", noop);
    prim("motion_scroll_up", noop);
    prim("motion_align_scene", noop);
    prim("motion_xscroll", noop);
    prim("motion_yscroll", noop);

    prim("sensing_touchingobject", evTouchingObject);
    prim("sensing_touchingcolor", snsTouchingColor);
    prim("sensing_coloristouchingcolor", snsColorTouchingColor);
    prim("sensing_distanceto", snsDistanceTo);
    prim("sensing_timer", snsTimer);
    prim("sensing_resettimer", snsResetTimer);
    prim("sensing_of", snsOf);
    prim("sensing_mousex", snsMouseX);
    prim("sensing_mousey", snsMouseY);
    prim("sensing_setdragmode", snsSetDragMode);
    prim("sensing_mousedown", snsMouseDown);
    prim("sensing_keypressed", snsKeyPressed);
    prim("sensing_current", snsCurrent);
    prim("sensing_dayssince2000", snsDaysSince2000);
    prim("sensing_loudness", snsLoudness);
    prim("sensing_loud", snsLoud);
    prim("sensing_askandwait", snsAskAndWait);
    prim("sensing_answer", snsAnswer);
    prim("sensing_username", snsUsername);
    prim("sensing_userid", noop);
    prim("sensing_online", snsOnline);

    prim("sound_play", sndPlay);
    prim("sound_playuntildone", sndPlayUntilDone);
    prim("sound_stopallsounds", sndStopAll);
    prim("sound_seteffectto", sndSetEffect);
    prim("sound_changeeffectby", sndChangeEffect);
    prim("sound_cleareffects", sndClearEffects);
    prim("sound_sounds_menu", sndSoundsMenu);
    prim("sound_beats_menu", sndBeatsMenu);
    prim("sound_effects_menu", sndEffectsMenu);
    prim("sound_setvolumeto", sndSetVolume);
    prim("sound_changevolumeby", sndChangeVolume);
    prim("sound_volume", sndGetVolume);

    prim("procedures_definition", procDefinition);
    prim("procedures_call", procCall);
    prim("procedures_return", procReturn);
    prim("argument_reporter_string_number", procArgStringNumber);
    prim("argument_reporter_boolean", procArgBoolean);

    registerPenBlocks(rt);
}

} // namespace xyo
