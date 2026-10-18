// xyo-cpp: engine/block-utility.js 相当 + stackFrame.executionContext。
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/value.hpp"
#include "../model/variable.hpp"
#include "thread.hpp"
#include "timer.hpp"

namespace xyo {

struct Sequencer;
struct Runtime;
struct Target;
struct Thread;
// ExecContext は thread.hpp（StackFrame の unique_ptr が完全型を要する）

/// BlockUtility — プリミティブへ渡される util インターフェース。
class BlockUtility {
public:
    Sequencer* sequencer = nullptr;
    Thread* thread = nullptr;
    /// JIT compat 層用: 合成 ExecContext（設定中は stackFrame がこれを返す）
    ExecContext* jitCompatEc = nullptr;

    Target* target() const;
    Runtime* runtime() const;

    /// util.stackFrame — 必要なら生成して返す
    ExecContext& stackFrame() {
        if (jitCompatEc) return *jitCompatEc;
        StackFrame* f = thread->peekStackFrame();
        if (!f->executionContext) f->executionContext = std::make_unique<ExecContext>();
        return *f->executionContext;
    }
    ExecContext* stackFrameOpt() {
        StackFrame* f = thread->peekStackFrame();
        return f->executionContext.get();
    }

    bool stackTimerNeedsInit() { return !stackFrame().timer; }
    void startStackTimer(double duration);
    bool stackTimerFinished() {
        ExecContext& sf = stackFrame();
        return sf.timer && sf.timer->timeElapsed() >= sf.duration;
    }

    void yield() { thread->status = STATUS_YIELD; }
    void yieldTick() { thread->status = STATUS_YIELD_TICK; }
    /// JS `return Promise.resolve()` 相当 — PROMISE_WAIT にし、
    /// 次の processWaitQueue（次ステップ冒頭）で undefined 解決する
    void promiseResolveNextTick();
    void startBranch(int branchNum, bool isLoop);
    void stopAll();
    void stopOtherTargetThreads();
    void stopThisScript() { thread->stopThisScript(); }
    void startProcedure(const std::string& procedureCode);

    bool getProcedureParamNamesIdsAndDefaults(
        const std::string& procedureCode,
        std::vector<std::u16string>& names,
        std::vector<std::string>& ids,
        std::vector<Value>& defaults);

    void initParams() { thread->initParams(); }
    void pushParam(const std::u16string& name, Value v) { thread->pushParam(name, v); }
    Value getParam(const std::u16string& name) { return thread->getParam(name); }

    std::vector<Thread*> startHats(const std::string& requestedHat,
                                   const std::map<std::string, std::u16string>* matchFields,
                                   Target* optTarget);

    /// ioQuery("clock","projectTimer") 等 — 型付きで直接呼ぶ形に統一
    double clockProjectTimer();
    bool keyboardIsDown(Value keyArg);
    std::u16string keyboardLastKeyPressed();
    double mouseScratchX();
    double mouseScratchY();
    bool mouseIsDown();
    bool mouseButtonIsDown(int button);
    double mouseWheelX();
    double mouseWheelY();
    std::u16string username();
    bool videoReady();

    double nowMs();   // runtime.currentMSecs
};

} // namespace xyo
