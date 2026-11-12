// PLaMo translation: stopped/restarted threads must not receive stale values.
#include "../../src/engine/runtime.hpp"
#include "../../src/engine/thread.hpp"
#include "../../src/engine/translate_service.hpp"
#include "../../src/model/target.hpp"
#include <cassert>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>

static xyo::Target* makeTarget(xyo::Runtime& rt, bool stage) {
    auto* target = new xyo::Target(&rt);
    target->isStage = stage;
    xyo::Block hat;
    hat.id = "hat";
    hat.opcode = "event_whenflagclicked";
    hat.topLevel = true;
    target->blocks->createBlock(std::move(hat));
    target->blocks->addScript("hat");
    rt.addTarget(target);
    return target;
}

int main() {
    // Set explicit opt-in: the test runner supplies an unreachable local
    // endpoint or a delayed mock server. Workers never access Scratch data.
    setenv("XYO_PLAMO_TRANSLATE", "1", 1);
    {
        xyo::Runtime rt;
        auto* target = makeTarget(rt, true);
        auto* old = rt.pushThread("hat", target, false, false);
        rt.requestTranslation(old, "stopped request", "ja");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(rt.translationService->hasPending());
        rt.stopThread(old);
        assert(old->isKilled);
        assert(!rt.translationService->hasPending());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        rt.translationService->poll(rt);
        assert(old->isKilled);
        assert(!rt.translationService->hasPending());
        std::puts("PASS stopThread cancellation");
    }
    {
        xyo::Runtime rt;
        auto* target = makeTarget(rt, true);
        auto* old = rt.pushThread("hat", target, false, false);
        rt.requestTranslation(old, "restarted request", "ja");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(rt.translationService->hasPending());
        auto* fresh = rt.restartThread(old);
        assert(fresh != old);
        assert(!rt.translationService->hasPending());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        rt.translationService->poll(rt);
        assert(fresh->status == xyo::STATUS_RUNNING);
        assert(!rt.translationService->hasPending());
        std::puts("PASS restartThread cancellation");
    }
    {
        xyo::Runtime rt;
        auto* target = makeTarget(rt, false);
        auto* old = rt.pushThread("hat", target, false, false);
        rt.requestTranslation(old, "clone request", "ja");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(rt.translationService->hasPending());
        rt.stopForTarget(target, nullptr);
        assert(old->isKilled);
        assert(!rt.translationService->hasPending());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        rt.translationService->poll(rt);
        assert(old->isKilled);
        std::puts("PASS stopForTarget cancellation");
    }
    {
        xyo::Runtime rt;
        auto* target = makeTarget(rt, true);
        // Old canceled requests may remain in-flight; none can consume
        // a fresh Scratch run's bounded four active inference slots.
        for (int i = 0; i < 4; ++i) {
            auto* old = rt.pushThread("hat", target, false, false);
            rt.requestTranslation(old, "old concurrent request " +
                                   std::to_string(i), "ja");
            rt.stopThread(old);
        }
        assert(!rt.translationService->hasPending());
        auto* fresh = rt.pushThread("hat", target, false, false);
        rt.requestTranslation(fresh, "fresh after canceled four", "ja");
        assert(rt.translationService->hasPending());
        rt.stopThread(fresh);
        std::puts("PASS canceled requests do not exhaust active slots");
    }
    {
        xyo::Runtime rt;
        auto* target = makeTarget(rt, true);
        for (int i = 0; i < 32; ++i) {
            auto* thread = rt.pushThread("hat", target, false, false);
            rt.requestTranslation(thread, "inference concurrency " +
                                  std::to_string(i), "ja");
            // Requests beyond the four allowed active workers resolve with
            // the original string rather than starting another HTTP worker.
            assert(rt.translationService->outstandingCount() <= 4);
        }
        assert(rt.translationService->outstandingCount() == 4);
        assert(!rt.waitQueue.empty());
        rt.stopAll();
        rt.processWaitQueue();
        std::puts("PASS 32 concurrent requests bounded to four workers");
    }
    {
        xyo::Runtime rt;
        auto* target = makeTarget(rt, true);
        size_t peak = 0;
        // 128 clone-like lifetimes with a stalled inference server.
        // Canceled requests still own std::async workers until curl exits.
        for (int i = 0; i < 128; ++i) {
            auto* old = rt.pushThread("hat", target, false, false);
            rt.requestTranslation(old, "stress canceled request " +
                                   std::to_string(i), "ja");
            rt.stopThread(old);
            peak = std::max(peak, rt.translationService->outstandingCount());
            assert(!rt.translationService->hasPending());
            assert(peak <= 16);
        }
        // Next-tick fallback from the worker cap cannot revive killed threads.
        rt.processWaitQueue();
        for (auto* old : rt.threads)
            assert(old->isKilled || old->status != xyo::STATUS_RUNNING);
        std::printf("PASS bounded churn 128 peak=%zu\n", peak);
    }
    {
        xyo::Runtime rt;
        auto* target = makeTarget(rt, true);
        auto* old = rt.pushThread("hat", target, false, false);
        rt.requestTranslation(old, "greenflag request", "ja");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(rt.translationService->hasPending());
        rt.stopAll();
        assert(!rt.translationService->hasPending());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        rt.translationService->poll(rt);
        assert(!rt.translationService->hasPending());
        std::puts("PASS stopAll cancellation");
    }
}
