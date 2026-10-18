// xyo-cpp: engine/execute.js 移植 — _ops フラットインタプリタ。
//
// プリミティブが返すのは常に Value。非同期プリミティブは
// thread.status = STATUS_PROMISE_WAIT を自ら立てる（JS Promise 相当）。
// その場合実行を中断し、runtime.resolveThread() で再開する。
#include "../core/cast.hpp"
#include "../model/target.hpp"
#include "block_utility.hpp"
#include "runtime.hpp"
#include "sequencer.hpp"

namespace xyo {

// 並列実行対策として thread_local（通常インタプリタはメインのみだが、
// 万一 worker で compat が走っても破壊しないようにする）
static thread_local BlockUtility g_util;

/// JS truthiness（`if (v)` / `!v`）。Scratch キャストではなく JS の意味。
static bool jsTruthy(Value v) {
    if (v.isUndefined() || v.isNaN()) return false;
    if (v.isBool()) return v.asBool();
    if (v.isNumber()) return v.asNumberUnchecked() != 0;
    if (v.isString()) {
        SStr* s = v.asStrPtr();
        return s && s->len > 0;
    }
    return true;   // color 等のオブジェクトは truthy
}

/// handleReport — 報告値の処理（ハット判定・分岐・モニタ・ビジュアル報告）
static void handleReport(Value resolvedValue, Sequencer& seq, Thread& thread,
                         const BlockCached* op, bool lastOperation) {
    const std::string& currentBlockId = op->id;
    Runtime& runtime = seq.runtime;

    thread.pushReportedValue(resolvedValue);
    if (op->isHat) {
        if (thread.stackClick) {
            thread.status = STATUS_RUNNING;
        } else if (runtime.getIsEdgeActivatedHat(op->opcode)) {
            const bool hasOld = thread.target->hasEdgeActivatedValue(currentBlockId);
            Value oldV = thread.target->updateEdgeActivatedValue(currentBlockId, resolvedValue);
            const bool edge = hasOld
                ? (!jsTruthy(oldV) && jsTruthy(resolvedValue))
                : jsTruthy(resolvedValue);
            if (edge) thread.status = STATUS_RUNNING;
            else seq.retireThread(thread);
        } else if (jsTruthy(resolvedValue)) {
            thread.status = STATUS_RUNNING;
        } else {
            seq.retireThread(thread);
        }
    } else if ((op->isConditional || op->isLoop) && !resolvedValue.isUndefined()) {
        seq.stepToBranch(thread, int(cast::toNumber(resolvedValue)), op->isLoop);
    } else {
        if (lastOperation && !resolvedValue.isUndefined() && thread.atStackTop()) {
            if (thread.stackClick) {
                runtime.visualReport(thread.target, currentBlockId, resolvedValue);
            }
            if (thread.updateMonitor) {
                const Block* mb = runtime.monitorBlocks->getBlock(currentBlockId);
                std::u16string spriteName;
                if (mb && !mb->targetId.empty()) {
                    Target* t = runtime.getTargetById(mb->targetId);
                    if (!t) return;
                    spriteName = t->getName();
                }
                runtime.requestUpdateMonitorValue(currentBlockId, resolvedValue, spriteName);
            }
        }
        thread.status = STATUS_RUNNING;
    }
}

/// BROADCAST_INPUT 入力書き込み（JS: argValues.BROADCAST_OPTION.name = toString(v)）
static void writeInputValue(ArgValues* parentValues, const std::string& inputName,
                            Value inputValue) {
    if (inputName == "BROADCAST_INPUT") {
        IdName& bo = parentValues->refObj("BROADCAST_OPTION");
        bo.id.clear();
        SStr* s = cast::toString(inputValue);
        bo.name = s->view();
    } else {
        parentValues->ref(inputName) = inputValue;
    }
}

/// 中断した _ops を promise 解決相当で再開（handlePromiseResolution 移植）
static void resolveThreadOps(Value resolvedValue, Sequencer& seq, Thread& thread,
                             const BlockCached* op, bool lastOperation) {
    handleReport(resolvedValue, seq, thread, op, lastOperation);
    if (lastOperation && (!op->isHat || thread.stackClick)) {
        StackFrame* stackFrame;
        std::string nextBlockId;
        do {
            const std::string popped = thread.popStack();
            if (popped.empty()) return;
            nextBlockId = thread.target->blocks->getNextBlock(popped);
            if (!nextBlockId.empty()) break;
            stackFrame = thread.peekStackFrame();
        } while (stackFrame != nullptr && !stackFrame->isLoop);
        thread.pushStack(nextBlockId);
    }
}

void execute(Sequencer& sequencer, Thread& thread) {
    Runtime& runtime = sequencer.runtime;
    g_util.sequencer = &sequencer;
    g_util.thread = &thread;

    const std::string currentBlockId = thread.peekStack();
    StackFrame* currentStackFrame = thread.peekStackFrame();

    Blocks* blockContainer = thread.blockContainer;
    BlockCached* blockCached = blockContainer->getCachedExecute(currentBlockId);
    if (!blockCached) {
        blockContainer = runtime.flyoutBlocks.get();
        blockCached = blockContainer->getCachedExecute(currentBlockId);
        if (!blockCached) {
            sequencer.retireThread(thread);
            thread.trashFrames.clear();
            return;
        }
    }

    auto& ops = blockCached->ops;
    const size_t length = ops.size();
    size_t i = 0;

    if (currentStackFrame->hasReported) {
        auto& reported = currentStackFrame->reported;
        for (; i < reported.size(); ++i) {
            const ReportedEntry& rep = reported[i];
            BlockCached* found = nullptr;
            for (BlockCached* o : ops) {
                if (o->id == rep.opCachedId) { found = o; break; }
            }
            if (found && found->parentValues) {
                writeInputValue(found->parentValues, found->parentKey, rep.inputValue);
            }
        }
        if (!reported.empty()) {
            // 最後に報告した op の次から再開
            size_t resume = 0;
            for (int r = int(reported.size()) - 1; r >= 0; --r) {
                for (size_t oi = 0; oi < ops.size(); ++oi) {
                    if (ops[oi]->id == reported[r].opCachedId) { resume = oi + 1; goto found; }
                }
            }
            i = 0;
            goto after_resume;
        found:;
            i = resume;
        after_resume:;
        } else {
            i = 0;
        }

        // 中断中リポータの報告値を注入
        if (!thread.justReported.isUndefined() && i < ops.size() &&
            ops[i]->id == currentStackFrame->reporting) {
            BlockCached* opCached = ops[i];
            Value inputValue = thread.justReported;
            thread.justReported = Value::undefined();
            if (opCached->parentValues) {
                writeInputValue(opCached->parentValues, opCached->parentKey, inputValue);
            }
            i += 1;
        }

        currentStackFrame->reporting.clear();
        currentStackFrame->hasReported = false;
        currentStackFrame->reported.clear();
        currentStackFrame->waitingReporter = false;
    }

    for (; i < length; ++i) {
        const bool lastOperation = (i == length - 1);
        BlockCached* opCached = ops[i];
        currentStackFrame->op = opCached;

        if (!blockContainer->forceNoGlow) {
            thread.requestScriptGlowInFrame = true;
        }

        const Value rv = opCached->blockFunction
            ? opCached->blockFunction(opCached->argValues, g_util)
            : Value::undefined();

        if (thread.status == STATUS_PROMISE_WAIT ||
            currentStackFrame->waitingReporter) {
            // 非同期中断 — 既報告値を凍結して次回再開に備える
            thread.justReported = Value::undefined();
            currentStackFrame->reporting = opCached->id;
            currentStackFrame->suspendLast = lastOperation;
            currentStackFrame->reported.clear();
            for (size_t r = 0; r < i; ++r) {
                BlockCached* rc = ops[r];
                if (!rc->parentValues) continue;
                ReportedEntry e;
                e.opCachedId = rc->id;
                if (rc->parentKey == "BROADCAST_INPUT") {
                    IdName& bo = rc->parentValues->refObj("BROADCAST_OPTION");
                    e.inputValue = Value::string(SStr::create(bo.name));
                } else {
                    e.inputValue = rc->parentValues->get(rc->parentKey);
                }
                currentStackFrame->reported.push_back(e);
            }
            currentStackFrame->hasReported = true;
            break;
        } else if (thread.status == STATUS_RUNNING) {
            if (lastOperation) {
                handleReport(rv, sequencer, thread, opCached, lastOperation);
            } else if (opCached->parentValues) {
                writeInputValue(opCached->parentValues, opCached->parentKey, rv);
            }
        } else if (thread.status == STATUS_DONE) {
            break;
        }
    }

    // ポップ済みフレームは execute 内のポインタ生存期間を守るためだけに
    // 保持していた — ここで解放（JS の _StackFrame.release 相当）
    thread.trashFrames.clear();
}

/// 非同期プリミティブ完了時の再開（JS promise .then 相当）。
/// handlePromiseResolution: 中断した op に報告値を渡し、最終 op なら
/// スタックを次ブロックまでポップする。スレッドは次の stepThreads で再開。
void resolveWait(Sequencer& seq, Thread& thread, Value resolvedValue) {
    StackFrame* sf = thread.peekStackFrame();
    const BlockCached* op = sf ? sf->op : nullptr;
    if (!op) {
        thread.status = STATUS_RUNNING;
        thread.pushReportedValue(resolvedValue);
        return;
    }
    resolveThreadOps(resolvedValue, seq, thread, op, sf->suspendLast);
    thread.status = STATUS_RUNNING;
    thread.trashFrames.clear();
}

} // namespace xyo
