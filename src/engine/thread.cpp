// xyo-cpp: engine/thread.js 移植。
#include "thread.hpp"

#include "../jit/jit.hpp"
#include "../model/target.hpp"
#include "fiber.hpp"
#include "runtime.hpp"

namespace xyo {

Thread::Thread(std::string firstBlock) : topBlock(std::move(firstBlock)) {
    // warpTimer は遅延 init（stepThread で初めて必要になったとき start）
}

Thread::~Thread() {
    FiberPool::release(fiber);
    delete jitCtx;
}

std::string Thread::getIdFromTargetAndBlock(const Target* target,
                                            const std::string& tb) {
    return target->id + "&" + tb;
}

std::string Thread::getId() const {
    if (idCache_.empty()) idCache_ = getIdFromTargetAndBlock(target, topBlock);
    return idCache_;
}

void Thread::pushStack(const std::string& blockId) {
    stack.push_back(blockId);
    if (stack.size() > stackFrames.size()) {
        bool parentWarp = !stackFrames.empty() && stackFrames.back()->warpMode;
        if (!framePool.empty()) {
            // プール収納時に reset 済み（fresh と等価）
            stackFrames.push_back(std::move(framePool.back()));
            framePool.pop_back();
        } else {
            stackFrames.emplace_back(std::make_unique<StackFrame>());
        }
        stackFrames.back()->warpMode = parentWarp;
    }
}

void Thread::reuseStackForNextBlock(const std::string& blockId) {
    // JS では stack が空でも stack[-1]=id は無害な代入、
    // stackFrames[length-1] は残った最終フレームを再利用する。
    // retireThread 直後（edge-activated hat 不発火）にここへ来る場合がある。
    if (!stack.empty()) stack.back() = blockId;
    if (!stackFrames.empty()) stackFrames.back()->reset(true);
}

std::string Thread::popStack() {
    std::string id = stack.back();
    stack.pop_back();
    // JS GC 相当: ポップしてもオブジェクト自体は生かしておく
    trashFrames.push_back(std::move(stackFrames.back()));
    stackFrames.pop_back();
    return id;
}

void Thread::stopThisScript() {
    std::string blockID = peekStack();
    while (!blockID.empty()) {
        // レポータ形式の procedures_call — waitingReporter 中はここで止まる
        StackFrame* sf = peekStackFrame();
        if (sf && sf->waitingReporter) break;

        const Block* block = target->blocks->getBlock(blockID);
        if (block && block->opcode == "procedures_call") {
            // 手続きは完了済み — 引数が再評価されないよう次へ進める
            goToNextBlock();
            break;
        }
        popStack();
        blockID = peekStack();
    }
    if (stack.empty()) {
        requestScriptGlowInFrame = false;
        status = STATUS_DONE;
    }
}

std::string Thread::peekStack() const {
    return stack.empty() ? std::string{} : stack.back();
}

StackFrame* Thread::peekStackFrame() {
    return stackFrames.empty() ? nullptr : stackFrames.back().get();
}

StackFrame* Thread::peekParentStackFrame() {
    return stackFrames.size() > 1 ? stackFrames[stackFrames.size() - 2].get()
                                  : nullptr;
}

void Thread::initParams() {
    peekStackFrame()->hasParams = true;
}

void Thread::pushParam(const std::u16string& name, Value v) {
    peekStackFrame()->params[name] = v;
    peekStackFrame()->hasParams = true;
}

Value Thread::getParam(const std::u16string& name) const {
    for (int i = int(stackFrames.size()) - 1; i >= 0; --i) {
        const StackFrame& f = *stackFrames[i];
        if (!f.hasParams) continue;
        auto it = f.params.find(name);
        if (it != f.params.end()) return it->second;
        return Value::undefined();
    }
    return Value::undefined();
}

const std::unordered_map<std::u16string, Value>* Thread::getAllParams() const {
    const StackFrame* sf = stackFrames.empty() ? nullptr : stackFrames.back().get();
    return sf && sf->hasParams ? &sf->params : nullptr;
}

void Thread::goToNextBlock() {
    reuseStackForNextBlock(target->blocks->getNextBlock(peekStack()));
}

bool Thread::isRecursiveCall(const std::string& procedureCode) const {
    int callCount = 5;
    int sp = int(stackFrames.size()) - 1;
    for (int i = sp - 1; i >= 0; --i) {
        const BlockCached* op = stackFrames[i]->op;
        const Block* block = nullptr;
        if (op) {
            block = target->blocks->getBlock(op->id);
            if (!block && target->runtime)
                block = target->runtime->flyoutBlocks->getBlock(op->id);
        }
        if (block && block->opcode == "procedures_call" && block->mutation &&
            block->mutation->attr("proccode") == procedureCode) {
            return true;
        }
        if (--callCount < 0) return false;
    }
    return false;
}

void Thread::tryCompile(const Block* topHint) {
    triedToCompile = true;
    static const bool disabled = std::getenv("XYO_JIT_DISABLE") != nullptr;
    static const bool verbose = std::getenv("XYO_JIT_VERBOSE") != nullptr;
    if (disabled) return;
    if (!target || !target->runtime || !target->runtime->jit) return;
    if (verbose) std::fprintf(stderr,
        "xyo-jit: tryCompile top=%s target=%s\n", topBlock.c_str(),
        target->id.c_str());
    struct CProf {
        bool enabled = std::getenv("XYO_PROF") != nullptr;
        double ns = 0; uint64_t n = 0;
        ~CProf() {
            if (enabled)
                fprintf(stderr, "[prof] compile=%.1fms(%llu)\n",
                        ns / 1e6, (unsigned long long)n);
        }
    };
    static CProf cp;
    std::chrono::steady_clock::time_point t0;
    if (cp.enabled) t0 = std::chrono::steady_clock::now();
    const CompiledScript* cs =
        target->runtime->jit->compile(*this, *target->runtime,
                                      *target->runtime->sequencer, topHint);
    if (cp.enabled) {
        cp.ns += std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - t0).count();
        ++cp.n;
    }
    if (cs) {
        compiled = cs;
        isCompiled = true;
    }
}

} // namespace xyo
