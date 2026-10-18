// xyo-cpp: engine/block-utility.js 移植。
#include "block_utility.hpp"

#include "../model/target.hpp"
#include "runtime.hpp"
#include "sequencer.hpp"

namespace xyo {

Target* BlockUtility::target() const { return thread->target; }
Runtime* BlockUtility::runtime() const { return &sequencer->runtime; }

void BlockUtility::startStackTimer(double duration) {
    ExecContext& sf = stackFrame();
    Runtime* rt = runtime();
    if (rt) {
        sf.timer = std::make_unique<Timer>([rt] { return rt->currentMSecs; });
    } else {
        sf.timer = std::make_unique<Timer>();
    }
    sf.timer->start();
    sf.duration = duration;
}

void BlockUtility::startBranch(int branchNum, bool isLoop) {
    sequencer->stepToBranch(*thread, branchNum, isLoop);
}

void BlockUtility::promiseResolveNextTick() {
    Runtime::WaitEntry w;
    w.thread = thread;
    w.nextTick = true;
    runtime()->waitQueue.push_back(w);
    thread->status = STATUS_PROMISE_WAIT;
}

void BlockUtility::stopAll() {
    sequencer->runtime.stopAll();
}

void BlockUtility::stopOtherTargetThreads() {
    sequencer->runtime.stopForTarget(thread->target, thread);
}

void BlockUtility::startProcedure(const std::string& procedureCode) {
    sequencer->stepToProcedure(*thread, procedureCode);
}

bool BlockUtility::getProcedureParamNamesIdsAndDefaults(
    const std::string& procedureCode,
    std::vector<std::u16string>& names,
    std::vector<std::string>& ids,
    std::vector<Value>& defaults) {
    return thread->target->blocks->getProcedureParamNamesIdsAndDefaults(
        procedureCode, names, ids, defaults);
}

std::vector<Thread*> BlockUtility::startHats(
    const std::string& requestedHat,
    const std::map<std::string, std::u16string>* matchFields,
    Target* optTarget) {
    // 呼び出し側コンテキストを退避（startHats は execute を呼び util を汚す）
    Thread* callerThread = thread;
    Sequencer* callerSeq = sequencer;
    auto result = sequencer->runtime.startHats(requestedHat, matchFields, optTarget);
    thread = callerThread;
    sequencer = callerSeq;
    return result;
}

double BlockUtility::clockProjectTimer() {
    return runtime()->ioDevices.clock.projectTimer();
}
bool BlockUtility::keyboardIsDown(Value keyArg) {
    return runtime()->ioDevices.keyboard.getKeyIsDown(keyArg);
}
std::u16string BlockUtility::keyboardLastKeyPressed() {
    return runtime()->ioDevices.keyboard.getLastKeyPressed();
}
double BlockUtility::mouseScratchX() {
    return runtime()->ioDevices.mouse.getScratchX();
}
double BlockUtility::mouseScratchY() {
    return runtime()->ioDevices.mouse.getScratchY();
}
bool BlockUtility::mouseIsDown() {
    return runtime()->ioDevices.mouse.getIsDown();
}
bool BlockUtility::mouseButtonIsDown(int button) {
    return runtime()->ioDevices.mouse.getButtonIsDown(button);
}
double BlockUtility::mouseWheelX() {
    return runtime()->ioDevices.mouseWheel.getScrollX();
}
double BlockUtility::mouseWheelY() {
    return runtime()->ioDevices.mouseWheel.getScrollY();
}
std::u16string BlockUtility::username() {
    return runtime()->ioDevices.userData.getUsername();
}
bool BlockUtility::videoReady() {
    return runtime()->ioDevices.video.videoReady;
}

double BlockUtility::nowMs() {
    return runtime()->currentMSecs;
}

} // namespace xyo
