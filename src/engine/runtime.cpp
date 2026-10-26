// xyo-cpp: engine/runtime.js 移植。
#include "runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "../core/cast.hpp"
#include "../rt/runtime_helpers.hpp"
#include "fiber.hpp"
#include "sequencer.hpp"
#include "../jit/jit.hpp"

namespace xyo {

/// コアブロックパッケージのプリミティブ/ハットを登録（blocks/ 側で定義）
void registerCoreBlocks(Runtime& rt);

bool dangerModeFromEnv() {
    const char* v = std::getenv("XYO_DANGER");
    return v != nullptr && *v != 0 && std::strcmp(v, "0") != 0;
}

Runtime::Runtime()
    : sequencer(std::make_unique<Sequencer>(*this)),
      // XYO_JIT=OFF では nojit_stub.cpp の空実装が入る（常時非 null に
      // して ->jit 参照の null チェックを不要にする）
      jit(std::make_unique<JitEngine>()),
      ioDevices(this),
      monitorBlocks(std::make_unique<Blocks>(this)),
      flyoutBlocks(std::make_unique<Blocks>(this)) {
    updateCurrentMSecs();
    registerCoreBlocks(*this);
    Gc::get().addRootProvider(this, [this](Gc& g) { markRoots(g); });
}

Runtime::~Runtime() {
    Gc::get().removeRootProvider(this);
    // targets は所有ポインタ（disposeTarget/stopAll でも delete 済み要素は
    // リストから除去されている）。残りをここで回収する。
    for (Target* t : targets) delete t;
    targets.clear();
}

// ------------------------------------------------------------- スレッド --

Thread* Runtime::pushThread(const std::string& id, Target* target,
                            bool stackClick, bool updateMonitor,
                            const Block* topHint) {
    struct PProf {
        bool enabled = std::getenv("XYO_PROF") != nullptr;
        double allocNs = 0, mapNs = 0, compileNs = 0; uint64_t n = 0;
        ~PProf() {
            if (enabled)
                fprintf(stderr,
                    "[prof] pushThread alloc=%.1f map=%.1f compile=%.1f ms(%llu)\n",
                    allocNs/1e6, mapNs/1e6, compileNs/1e6,
                    (unsigned long long)n);
        }
    };
    static PProf pp;
    auto now = []{ return std::chrono::steady_clock::now(); };
    std::chrono::steady_clock::time_point t0;
    if (pp.enabled) { t0 = now(); }
    auto thread = std::make_unique<Thread>(id);
    Thread* t = thread.get();
    t->target = target;
    t->stackClick = stackClick;
    t->updateMonitor = updateMonitor;
    t->blockContainer = updateMonitor ? monitorBlocks.get() : target->blocks;
    t->pushStack(id);
    threads.push_back(t);
    ownedThreads.push_back(std::move(thread));
    if (!t->stackClick && !t->updateMonitor) {
        threadMap[t->getId()] = t;
    }
    if (pp.enabled) { pp.allocNs += std::chrono::duration<double,std::nano>(now()-t0).count(); t0 = now(); }
    if (!updateMonitor) t->tryCompile(topHint);
    if (pp.enabled) { pp.compileNs += std::chrono::duration<double,std::nano>(now()-t0).count(); ++pp.n; }
    return t;
}

void Runtime::stopThread(Thread* thread) {
    thread->isKilled = true;
    sequencer->retireThread(*thread);
}

Thread* Runtime::restartThread(Thread* thread, const Block* topHint) {
    auto nt = std::make_unique<Thread>(thread->topBlock);
    Thread* n = nt.get();
    n->target = thread->target;
    n->stackClick = thread->stackClick;
    n->updateMonitor = thread->updateMonitor;
    n->blockContainer = thread->blockContainer;
    n->pushStack(thread->topBlock);
    if (thread->triedToCompile) n->tryCompile(topHint);
    if (!n->stackClick && !n->updateMonitor) {
        threadMap[n->getId()] = n;
    }
    for (size_t i = 0; i < threads.size(); ++i) {
        if (threads[i] == thread) {
            threads[i] = n;
            ownedThreads.push_back(std::move(nt));
            return n;
        }
    }
    threads.push_back(n);
    ownedThreads.push_back(std::move(nt));
    return n;
}

bool Runtime::isActiveThread(Thread* thread) const {
    if (thread->stack.empty() || thread->status == STATUS_DONE) return false;
    for (Thread* t : threads) if (t == thread) return true;
    return false;
}

bool Runtime::isWaitingThread(Thread* thread) const {
    return thread->status == STATUS_PROMISE_WAIT ||
           thread->status == STATUS_YIELD_TICK ||
           !isActiveThread(thread);
}

void Runtime::toggleScript(const std::string& topBlockId, Target* target,
                           bool stackClick) {
    for (Thread* t : threads) {
        if (t->target == target && t->topBlock == topBlockId &&
            t->status != STATUS_DONE) {
            const Block* block = target->blocks->getBlock(topBlockId);
            std::string opcode = block ? block->opcode : std::string{};
            if (getIsEdgeActivatedHat(opcode) && t->stackClick != stackClick) {
                continue;
            }
            stopThread(t);
            return;
        }
    }
    pushThread(topBlockId, target, stackClick, false);
}

void Runtime::addMonitorScript(const std::string& topBlockId, Target* optTarget) {
    if (!optTarget) optTarget = editingTarget;
    if (!optTarget) return;
    for (Thread* t : threads) {
        if (t->topBlock == topBlockId && t->status != STATUS_DONE &&
            t->updateMonitor) {
            return;
        }
    }
    pushThread(topBlockId, optTarget, false, true);
}

void Runtime::updateThreadMap() {
    threadMap.clear();
    for (Thread* t : threads) {
        if (!t->stackClick && !t->updateMonitor) {
            threadMap[t->getId()] = t;
        }
    }
}

// ---------------------------------------------------------------- startHats --

std::vector<Thread*> Runtime::startHats(
    const std::string& requestedHatOpcode,
    const std::map<std::string, std::u16string>* optMatchFields,
    Target* optTarget) {
    std::vector<Thread*> newThreads;
    auto hit = hats.find(requestedHatOpcode);
    if (hit == hats.end()) return newThreads;
    const HatMeta& hatMeta = hit->second;

    std::map<std::string, std::u16string> matchFields;
    if (optMatchFields) {
        for (auto& [k, v] : *optMatchFields) matchFields[k] = u16ToUpper(v);
    }

    const size_t startingThreadCount = threads.size();

    forEachScriptByOpcode(requestedHatOpcode,
        [&](const RuntimeScriptCache& script, Target* target) {
            // フィールド一致（キャッシュ側は大文字化済み、JS: !== 厳密比較）
            for (auto& [mf, want] : matchFields) {
                auto it = script.fieldsOfInputs.find(mf);
                if (it == script.fieldsOfInputs.end()) return;
                const Value& fv = it->second;
                SStr* s = fv.isString() ? fv.asStrPtr() : nullptr;
                if (!s || s->view() != want) return;
            }
            const std::string& topBlockId = script.blockId;
            if (hatMeta.restartExistingThreads) {
                auto it = threadMap.find(Thread::getIdFromTargetAndBlock(target, topBlockId));
                if (it != threadMap.end()) {
                    newThreads.push_back(restartThread(it->second, script.block));
                    return;
                }
            } else {
                for (size_t j = 0; j < startingThreadCount; ++j) {
                    if (threads[j]->target == target &&
                        threads[j]->topBlock == topBlockId &&
                        !threads[j]->stackClick &&
                        threads[j]->status != STATUS_DONE) {
                        return;
                    }
                }
            }
            newThreads.push_back(
                pushThread(topBlockId, target, false, false, script.block));
        }, optTarget);

    // Scratch 2 互換: 新スレッドのハット即時評価
    for (Thread* t : newThreads) {
        if (t->isCompiled) {
            // JIT 側の処理（後で）
        } else {
            execute(*sequencer, *t);
            t->goToNextBlock();
        }
    }
    return newThreads;
}

// --------------------------------------------------------------- stop 系 --

void Runtime::stopForTarget(Target* target, Thread* optException) {
    emitStopForTarget(target, optException);
    for (Thread* t : threads) {
        if (t == optException) continue;
        if (t->target == target) stopThread(t);
    }
}

void Runtime::stopAll() {
    emitProjectStopAll();
    // クローンを廃棄（dispose が executableTargets/sprite.clones から除去）
    std::vector<Target*> keep;
    for (Target* t : targets) {
        t->onStopAll();
        if (!t->isOriginal) {
            if (onTargetRemoved) onTargetRemoved(t);
            t->dispose();
            delete t;
        } else {
            keep.push_back(t);
        }
    }
    targets = std::move(keep);
    if (sequencer->activeThread) stopThread(sequencer->activeThread);
    // JS では this.threads = [] に留めスレッドオブジェクトは GC 任せ。
    // 実行中プリミティブ（control_stop 等）が自スレッドの参照を保持する
    // ため、ownedThreads からは ~Runtime まで解放しない。
    // また JS は新しい配列に「置き換える」ため、シーケンサが捕まえた
    // 旧配列はそのパス中は実行が継続される — ここでも detach で再現する。
    if (sequencer->stepping == &threads) {
        auto old = std::make_unique<std::vector<Thread*>>();
        old->swap(threads);                  // old: 旧中身, threads: 空
        sequencer->stepping = old.get();
        sequencer->detachedLists.push_back(std::move(old));
    } else {
        threads.clear();
    }
    threadMap.clear();
}

void Runtime::greenFlag() {
    stopAll();
    emitProjectStart();
    updateCurrentMSecs();
    ioDevices.clock.resetProjectTimer();
    for (Target* t : targets) t->clearEdgeActivatedValues();
    for (Target* t : targets) t->onGreenFlag();
    startHats("event_whenflagclicked", nullptr, nullptr);
    if (onGreenFlag) onGreenFlag();
}

// ------------------------------------------------------------------ step --

void Runtime::step() {
    ++stepSerial;
    // 停止指示のあったスレッドを除去
    std::vector<Thread*> alive;
    alive.reserve(threads.size());
    for (Thread* t : threads) if (!t->isKilled) alive.push_back(t);
    threads = std::move(alive);
    // killed スレッドは ownedThreads に残す: broadcastandwait 等の
    // startedThreads が生ポインタを保持するため（解放すると ABA になる）
    updateThreadMap();

    // エッジ起動ハットを毎フレーム評価
    for (auto& [hatType, meta] : hats) {
        if (meta.edgeActivated) startHats(hatType, nullptr, nullptr);
    }
    redrawRequested = false;
    pushMonitors();
    processWaitQueue();
    std::vector<Thread*> doneThreads = sequencer->stepThreads();
    emitProjectRunStatus(
        int(threads.size() + doneThreads.size() -
            getMonitorThreadCount(threads) -
            getMonitorThreadCount(doneThreads)));
    lastStepDoneThreads = std::move(doneThreads);
    if (renderer) renderer->draw();
    if (refreshTargets) {
        emitTargetsUpdate();
        refreshTargets = false;
    }
    if (monitorState.dirty) {
        if (onMonitorsUpdate) onMonitorsUpdate(monitorState);
        monitorState.dirty = false;
    }
}

int Runtime::runUntilDone(int maxSteps) {
    int ran = 0;
    while (ran < maxSteps) {
        bool anyActive = false;
        for (Thread* t : threads) {
            if (!t->stack.empty() && t->status != STATUS_DONE) { anyActive = true; break; }
        }
        if (!anyActive) break;
        step();
        ++ran;
    }
    return ran;
}

void Runtime::updateCurrentMSecs() {
    currentMSecs = Timer::stdNow();
}

void Runtime::processWaitQueue() {
    if (waitQueue.empty()) return;
    for (auto it = waitQueue.begin(); it != waitQueue.end();) {
        WaitEntry& w = *it;
        if (!w.nextTick && w.deadlineMs > currentMSecs) { ++it; continue; }
        if (w.bubbleTarget) {
            // sayforsecs: usageId が変わっていなければバブルをクリア
            if (w.bubbleTarget->bubbleUsageId == w.bubbleUsageId) {
                w.bubbleTarget->bubbleText.clear();
                w.bubbleTarget->bubbleType = w.bubbleType;
                if (onSay) onSay(w.bubbleTarget, w.bubbleType, u"");
            }
        }
        if (w.thread && w.thread->status == STATUS_PROMISE_WAIT) {
            resolveWait(*sequencer, *w.thread, w.resolveValue);
        }
        it = waitQueue.erase(it);
    }
}

// --------------------------------------------------------------- targets --

void Runtime::addTarget(Target* target) {
    targets.push_back(target);
    executableTargets.push_back(target);
}

void Runtime::moveExecutable(Target* target, int delta) {
    auto it = std::find(executableTargets.begin(), executableTargets.end(), target);
    if (it == executableTargets.end()) return;
    int oldIndex = int(it - executableTargets.begin());
    executableTargets.erase(it);
    int newIndex = oldIndex + delta;
    if (newIndex > int(executableTargets.size()))
        newIndex = int(executableTargets.size());
    if (newIndex <= 0) {
        if (!executableTargets.empty() && executableTargets[0]->isStage) newIndex = 1;
        else newIndex = 0;
    }
    executableTargets.insert(executableTargets.begin() + newIndex, target);
}

void Runtime::setExecutablePosition(Target* target, int newIndex) {
    auto it = std::find(executableTargets.begin(), executableTargets.end(), target);
    if (it == executableTargets.end()) return;
    moveExecutable(target, newIndex - int(it - executableTargets.begin()));
}

void Runtime::removeExecutable(Target* target) {
    auto it = std::find(executableTargets.begin(), executableTargets.end(), target);
    if (it != executableTargets.end()) executableTargets.erase(it);
}

void Runtime::disposeTarget(Target* target) {
    auto it = std::find(targets.begin(), targets.end(), target);
    if (it == targets.end()) return;
    target->dispose();
    targets.erase(it);
    // editingTarget が消えたターゲットを指すと addMonitorScript が freed
    // ターゲットへスレッドを生成する（モニタ UAF）。次のスプライトへ移す。
    if (editingTarget == target) {
        editingTarget = nullptr;
        for (Target* t : targets) {
            if (!t->isStage) { editingTarget = t; break; }
        }
        if (!editingTarget && !targets.empty()) editingTarget = targets[0];
    }
    if (onTargetRemoved) onTargetRemoved(target);
    delete target;
}

Target* Runtime::getTargetForStage() const {
    for (Target* t : targets) if (t->isStage) return t;
    return nullptr;
}

Target* Runtime::getTargetById(const std::string& id) const {
    for (Target* t : targets) if (t->id == id) return t;
    return nullptr;
}

Target* Runtime::getTargetByDrawableId(int drawableId) const {
    for (Target* t : targets) if (t->drawableID == drawableId) return t;
    return nullptr;
}

Target* Runtime::getSpriteTargetByName(const std::u16string& name) const {
    for (Target* t : targets) {
        if (!t->isStage && t->sprite && t->sprite->name == name) return t;
    }
    return nullptr;
}

// --------------------------------------------------------------- monitors --

void Runtime::pushMonitors() {
    if (targets.empty()) return;
    monitorBlocks->runAllMonitored(*this);
}

int Runtime::getMonitorThreadCount(const std::vector<Thread*>& list) const {
    int n = 0;
    for (Thread* t : list) if (t->updateMonitor) ++n;
    return n;
}

void Runtime::requestUpdateMonitor(
    const std::map<std::string, std::u16string>& fields) {
    auto idIt = fields.find("id");
    if (idIt == fields.end()) return;
    std::string id = SStr::toUtf8(idIt->second);
    MonitorRecord& rec = monitorState.getOrCreate(id);
    if (rec.id.empty()) rec.id = id;
    for (auto& [k, v] : fields) {
        if (k == "id") continue;
        rec.params[k] = v;
    }
    monitorState.dirty = true;
}

void Runtime::requestUpdateMonitorValue(const std::string& id, Value v,
                                        const std::u16string& spriteName) {
    MonitorRecord& rec = monitorState.getOrCreate(id);
    if (rec.id.empty()) rec.id = id;
    // MonitorState は値が変わったときのみ dirty（文字列は内容比較）
    if (rec.value == v || rt::compareEqual(rec.value, v)) {
        if (!spriteName.empty()) rec.spriteName = spriteName;
        return;
    }
    rec.value = v;
    if (!spriteName.empty()) rec.spriteName = spriteName;
    monitorState.dirty = true;
}

void Runtime::requestAddMonitor(const MonitorRecord& rec) {
    MonitorRecord& dst = monitorState.getOrCreate(rec.id);
    // TW: 既存の位置/可視性を維持しつつ他フィールドを更新
    if (dst.id.empty()) dst = rec;
    else {
        std::string id = dst.id;
        double x = dst.x, y = dst.y;
        bool vis = dst.visible;
        dst = rec;
        dst.id = id;
        dst.x = x; dst.y = y; dst.visible = vis;
    }
    monitorState.dirty = true;
}

void Runtime::requestRemoveMonitor(const std::string& id) {
    monitorState.records.erase(id);
    monitorState.dirty = true;
}

void Runtime::requestRemoveMonitorByTargetId(const std::string& targetId) {
    for (auto it = monitorState.records.begin(); it != monitorState.records.end();) {
        if (it->second.targetId == targetId) it = monitorState.records.erase(it);
        else ++it;
    }
    monitorState.dirty = true;
}

// ------------------------------------------------------------------ misc --

void Runtime::visualReport(Target* target, const std::string& blockId, Value value) {
    if (onVisualReport) onVisualReport({target, blockId, value});
    lastVisualReport = {target, blockId, value};
}

void Runtime::emitKeyPressed(const std::u16string& scratchKey) {
    std::map<std::string, std::u16string> m1{{"KEY_OPTION", scratchKey}};
    startHats("event_whenkeypressed", &m1, nullptr);
    std::map<std::string, std::u16string> m2{{"KEY_OPTION", u"any"}};
    startHats("event_whenkeypressed", &m2, nullptr);
}

void Runtime::emitCompileError(Target* target, const std::u16string& error) {
    if (onCompileError) onCompileError(target, error);
}

void Runtime::onTargetGreenFlag(Target* target) {
    target->clearEffects();
}

// --------------------------------------------------------- ask and wait --

void Runtime::emitAnswer(const std::u16string& answerText) {
    answer = answerText;
    if (questionList.empty()) {
        if (onQuestion) onQuestion(nullptr);
        return;
    }
    AskEntry e = questionList.front();
    questionList.pop_front();
    if (e.wasVisible && !e.wasStage) e.target->setSay("say", u"");
    if (e.thread) resolveWait(*sequencer, *e.thread, Value::undefined());
    // _askNextQuestion — 次の質問を表示
    if (!questionList.empty()) {
        const AskEntry& q = questionList.front();
        if (q.wasVisible && !q.wasStage) {
            q.target->setSay("say", q.question);
            if (onQuestion) {
                static const std::u16string kEmpty;
                onQuestion(&kEmpty);
            }
        } else if (onQuestion) {
            onQuestion(&q.question);
        }
    } else if (onQuestion) {
        onQuestion(nullptr);
    }
}

void Runtime::clearAllQuestions() {
    questionList.clear();
    if (onQuestion) onQuestion(nullptr);
}

void Runtime::emitStopForTarget(Target* stopTarget, Thread*) {
    // _clearTargetQuestions — 対象の質問をキューから除去
    bool currentlyAsking =
        !questionList.empty() && questionList.front().target == stopTarget;
    for (auto it = questionList.begin(); it != questionList.end();) {
        if (it->target == stopTarget) it = questionList.erase(it);
        else ++it;
    }
    if (currentlyAsking) {
        stopTarget->setSay("say", u"");
        if (!questionList.empty()) {
            const AskEntry& q = questionList.front();
            if (q.wasVisible && !q.wasStage) {
                q.target->setSay("say", q.question);
                if (onQuestion) {
                    static const std::u16string kEmpty;
                    onQuestion(&kEmpty);
                }
            } else if (onQuestion) {
                onQuestion(&q.question);
            }
        } else if (onQuestion) {
            onQuestion(nullptr);
        }
    }
}

void Runtime::emitProjectStopAll() {
    clearAllQuestions();
    // stopAllSounds は soundBank 側（onProjectStopped 経由）
    for (Target* t : targets) {
        for (auto& [k, v] : t->soundEffects) v = 0;
    }
    if (onProjectStopped) onProjectStopped();
}

void Runtime::emitProjectStart() {
    answer.clear();
    for (Target* t : targets) {
        for (auto& [k, v] : t->soundEffects) v = 0;
    }
}

void Runtime::emitProjectRunStatus(int running) {
    projectRunStatus = running;
    if (onProjectRunStatus) onProjectRunStatus(running);
}

Variable* Runtime::createNewGlobalVariable(const std::u16string& name,
                                           const char* type,
                                           const std::string& id) {
    Target* stage = getTargetForStage();
    if (!stage) return nullptr;
    return stage->createVariable(id, name, type, false);
}

std::vector<std::u16string> Runtime::getAllVarNamesOfType(const char* type) const {
    std::vector<std::u16string> out;
    for (Target* t : targets)
        for (auto& [id, v] : t->variables)
            if (v.type == type) out.push_back(v.name);
    return out;
}

// ----------------------------------------------------------------- GC ルート --

namespace {

void markArgValues(Gc& g, const ArgValues& av) {
    for (auto& [k, v] : av.vals) g.markValue(v);
}

void markFieldMap(Gc& g, const std::map<std::string, BlockField>& fields) {
    for (auto& [k, f] : fields) g.markValue(f.value);
}

void markBlocks(Gc& g, Blocks* blocks) {
    if (!blocks) return;
    for (auto& [id, b] : blocks->allBlocks()) {
        markFieldMap(g, b.fields);
        if (b.mutation) {
            b.mutation->parseArgs();
            for (const Value& v : b.mutation->argumentDefaults) g.markValue(v);
        }
    }
    auto& c = blocks->cache();
    for (auto& [id, bc] : c.executeCached) {
        markFieldMap(g, bc->fields);
        g.markValue(bc->shadowValue);
        markArgValues(g, bc->argValues);
    }
    for (auto& [op, scripts] : c.scripts)
        for (auto& s : scripts)
            for (auto& [k, v] : s.fieldsOfInputs) g.markValue(v);
}

void markFrame(Gc& g, StackFrame& f) {
    for (auto& [k, v] : f.params) g.markValue(v);
    for (auto& e : f.reported) g.markValue(e.inputValue);
    if (f.executionContext) {
        for (auto& [k, v] : f.executionContext->vals) g.markValue(v);
        g.markValue(f.executionContext->returnValue);
    }
}

void markThread(Gc& g, Thread& t) {
    g.markValue(t.justReported);
    // ファイバ上に残るローカル（JIT の spill スロット・退避レジスタの
    // Value 等）は構造体ルートに無いため、ファイバスタック自体を走査する。
    if (t.fiber && !t.fiber->finished()) {
        void* base = t.fiber->stackBase();
        if (t.fiber == currentFiber()) {
            // 実行中ファイバ: この関数は collect の呼出側と同一スタック上
            // にあるため、現 sp から底までを走査する。
            uintptr_t lo = reinterpret_cast<uintptr_t>(__builtin_alloca(0));
            if (base && lo < reinterpret_cast<uintptr_t>(base))
                g.scanRange(reinterpret_cast<void*>(lo), base);
            // スケジューラ側の退避レジスタも走査
            auto* sr = &detail::gSchedulerRegs;
            g.scanRange(sr, reinterpret_cast<char*>(sr) + sizeof(*sr));
        } else {
            // 中断中ファイバ: 保存 sp から底まで + 退避済み callee-saved
            void* sp = t.fiber->suspendedSp();
            if (sp && base && sp < base) g.scanRange(sp, base);
            auto* r = t.fiber->regs();
            g.scanRange(r, reinterpret_cast<char*>(r) + sizeof(*r));
        }
    }
    for (auto& f : t.stackFrames) markFrame(g, *f);
    // trash は execute 中の参照生存期間のために残っている — 中身も生かす
    for (auto& f : t.trashFrames) markFrame(g, *f);
    if (t.hasCompatStackFrame) {
        for (auto& [k, v] : t.compatibilityStackFrame.vals) g.markValue(v);
        g.markValue(t.compatibilityStackFrame.returnValue);
    }
}

} // namespace

void Runtime::markRoots(Gc& g) {
    for (Target* t : targets) {
        for (auto& [id, v] : t->variables) {
            g.markValue(v.value);
            if (v.list) g.markPtr(v.list);
        }
        for (auto& [id, v] : t->edgeActivatedHatValues) g.markValue(v);
        markBlocks(g, t->blocks);
    }
    for (auto& th : ownedThreads) markThread(g, *th);
    markBlocks(g, monitorBlocks.get());
    markBlocks(g, flyoutBlocks.get());
    for (auto& [id, rec] : monitorState.records) g.markValue(rec.value);
    g.markValue(lastVisualReport.value);
    for (auto& w : waitQueue) g.markValue(w.resolveValue);
}

} // namespace xyo
