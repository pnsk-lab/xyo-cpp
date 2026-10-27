// danger モード（スレッドごとの実行時間予算を外す）の意味論テスト。
// CPU ループの yield checkpoint を対象とするため、この suite 内だけ GPU
// offload を無効にする。GPU 適格ループは CPU checkpoint を通らず全反復を
// 完了し得るため、GPU の有無でこの CPU 予算テストの結果を変えない。
//
// - 通常モード: warp 手続きのループは予算（既定 500ms）を使い切ると
//   yield して他スレッドへ譲る — 数パスでは終わらない。
// - danger モード: 予算が無制限（+inf）なので同じループが 1 パスで
//   最後まで走る。
// - 非 warp のループは danger モードでも毎反復 yield する（予算ではなく
//   スクリプト種別の意味論なので外れない）。
//
// 実時間に依存しないよう、予算は 0ms に設定して「常に使い切り」にする。
// danger 側は +inf なので同じ設定でも最後まで走る。
// 観測は `stepThread`（= yield するまでの 1 パス）で行い、WORK_TIME で
// 1 ティック内に何度も再開する `step()` のループは使わない。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

extern "C" int32_t xyo_wait_should_yield(xyo::JitCtx*);

namespace {
using namespace xyo;

int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL danger: %s\n", name);
    }
}

// Restore the caller's GPU setting instead of changing later GPU suites.
struct GpuEnvGuard {
    std::string previous;
    bool existed;
    GpuEnvGuard() : existed(std::getenv("XYO_GPU") != nullptr) {
        if (existed) previous = std::getenv("XYO_GPU");
        put("0");
    }
    static void put(const char* value) {
#ifdef _WIN32
        _putenv_s("XYO_GPU", value ? value : "");
#else
        if (value) setenv("XYO_GPU", value, 1);
        else unsetenv("XYO_GPU");
#endif
    }
    ~GpuEnvGuard() { put(existed ? previous.c_str() : nullptr); }
};

constexpr int kIterations = 20000;

/// hat → warp 手続き burn（引数なし）→ repeat N { change v by 1 }
std::string projectJson(bool warp) {
    const std::string mut =
        std::string("{\"tagName\":\"mutation\",\"children\":[],"
                    "\"proccode\":\"burn\",\"argumentids\":\"[]\","
                    "\"argumentnames\":\"[]\",\"argumentdefaults\":\"[]\","
                    "\"warp\":\"") +
        (warp ? "true" : "false") + "\"}";
    const std::string blocks =
        "{\"hat\":{\"opcode\":\"event_whenflagclicked\",\"next\":\"call\","
        "\"parent\":null,\"inputs\":{},\"fields\":{},\"topLevel\":true,"
        "\"shadow\":false,\"x\":0,\"y\":0},"
        "\"call\":{\"opcode\":\"procedures_call\",\"next\":null,"
        "\"parent\":\"hat\",\"inputs\":{},\"fields\":{},\"mutation\":" +
        mut +
        ",\"topLevel\":false,\"shadow\":false},"
        "\"def\":{\"opcode\":\"procedures_definition\",\"next\":\"loop\","
        "\"parent\":null,\"inputs\":{\"custom_block\":[1,\"proto\"]},"
        "\"fields\":{},\"topLevel\":true,\"shadow\":false,\"x\":0,\"y\":400},"
        "\"proto\":{\"opcode\":\"procedures_prototype\",\"next\":null,"
        "\"parent\":\"def\",\"inputs\":{},\"fields\":{},\"topLevel\":false,"
        "\"shadow\":true,\"mutation\":" +
        mut +
        "},"
        "\"loop\":{\"opcode\":\"control_repeat\",\"next\":null,"
        "\"parent\":\"def\",\"inputs\":{\"TIMES\":[1,[6,\"" +
        std::to_string(kIterations) +
        "\"]],\"SUBSTACK\":[2,\"inc\"]},\"fields\":{},\"topLevel\":false,"
        "\"shadow\":false},"
        "\"inc\":{\"opcode\":\"data_changevariableby\",\"next\":null,"
        "\"parent\":\"loop\",\"inputs\":{\"VALUE\":[1,[4,\"1\"]]},"
        "\"fields\":{\"VARIABLE\":[\"v\",\"v\"]},\"topLevel\":false,"
        "\"shadow\":false}}";
    return "{\"targets\":["
           "{\"isStage\":true,\"name\":\"Stage\",\"variables\":{},"
           "\"lists\":{},\"broadcasts\":{},\"blocks\":{},\"comments\":{},"
           "\"costumes\":[],\"sounds\":[],\"currentCostume\":0,"
           "\"volume\":100,\"layerOrder\":0},"
           "{\"isStage\":false,\"name\":\"S\",\"variables\":{\"v\":[\"v\",0]},"
           "\"lists\":{},\"broadcasts\":{},\"blocks\":" +
           blocks +
           ",\"comments\":{},\"costumes\":[],\"sounds\":[],"
           "\"currentCostume\":0,\"volume\":100,\"visible\":true,"
           "\"x\":0,\"y\":0,\"size\":100,\"direction\":90,"
           "\"draggable\":false,\"rotationStyle\":\"all around\","
           "\"layerOrder\":1}],"
           "\"monitors\":[],\"extensions\":[],\"meta\":{\"semver\":\"3.0.0\"}}";
}

double counter(Runtime& rt) {
    Target* sp = rt.getSpriteTargetByName(u"S");
    if (!sp) return -1;
    Variable* v = sp->lookupVariableById("v");
    return v ? cast::toNumber(v->value) : -1;
}

/// スレッドを数パスだけ進めたときのカウンタ値。
/// `stepThread` は 1 パス（= yield するまで）なので、`step()` の
/// WORK_TIME ループ（1 ティック内で何度も再開する）を介さず
/// **スレッドごとの予算**そのものを観測できる。
/// warpTimeMs = 0 → 予算は必ず使い切られる（時間に依存しない）。
double onePass(bool danger, bool warp, JitMode mode) {
    setJitMode(mode);
    Runtime rt;
    const Sb3LoadResult lr = loadSb3(rt, projectJson(warp), nullptr);
    check(lr.ok, "load project");
    if (!lr.ok) return -1;
    rt.warpTimeMs = 0.0;
    rt.dangerMode = danger;
    rt.greenFlag();
    rt.sequencer->timer.start();
    for (int pass = 0; pass < 4; ++pass) {
        for (Thread* t : rt.threads) {
            if (t->status != STATUS_DONE && !t->stack.empty())
                rt.sequencer->stepThread(*t);
        }
    }
    return counter(rt);
}

const char* modeName(JitMode m) {
    return m == JitMode::Off ? "interp" : "jit";
}

/// warp 手続き内で `broadcast and wait` → `wait until v > 0` → done=1。
/// 受信側は非 warp で `wait 0.05` してから v=1 を書く（他スレッドが
/// 進まないと待機が解けない構図）。
std::string waitProjectJson() {
    const std::string mut =
        "{\"tagName\":\"mutation\",\"children\":[],\"proccode\":\"go\","
        "\"argumentids\":\"[]\",\"argumentnames\":\"[]\","
        "\"argumentdefaults\":\"[]\",\"warp\":\"true\"}";
    const std::string blocks =
        "{\"hat\":{\"opcode\":\"event_whenflagclicked\",\"next\":\"call\","
        "\"parent\":null,\"inputs\":{},\"fields\":{},\"topLevel\":true,"
        "\"shadow\":false,\"x\":0,\"y\":0},"
        "\"call\":{\"opcode\":\"procedures_call\",\"next\":null,"
        "\"parent\":\"hat\",\"inputs\":{},\"fields\":{},\"topLevel\":false,"
        "\"shadow\":false,\"mutation\":" + mut + "},"
        "\"def\":{\"opcode\":\"procedures_definition\",\"next\":\"bw\","
        "\"parent\":null,\"inputs\":{\"custom_block\":[1,\"proto\"]},"
        "\"fields\":{},\"topLevel\":true,\"shadow\":false,\"x\":0,\"y\":400},"
        "\"proto\":{\"opcode\":\"procedures_prototype\",\"next\":null,"
        "\"parent\":\"def\",\"inputs\":{},\"fields\":{},\"topLevel\":false,"
        "\"shadow\":true,\"mutation\":" + mut + "},"
        "\"bw\":{\"opcode\":\"event_broadcastandwait\",\"next\":\"wu\","
        "\"parent\":\"def\",\"inputs\":{\"BROADCAST_INPUT\":[1,[11,\"msg\",\"msg-id\"]]},"
        "\"fields\":{},\"topLevel\":false,\"shadow\":false},"
        "\"wu\":{\"opcode\":\"control_wait_until\",\"next\":\"done\","
        "\"parent\":\"bw\",\"inputs\":{\"CONDITION\":[2,\"gt\"]},\"fields\":{},"
        "\"topLevel\":false,\"shadow\":false},"
        "\"gt\":{\"opcode\":\"operator_gt\",\"next\":null,\"parent\":\"wu\","
        "\"inputs\":{\"OPERAND1\":[3,\"varv\",[4,\"0\"]],\"OPERAND2\":[1,[4,\"0\"]]},"
        "\"fields\":{},\"topLevel\":false,\"shadow\":false},"
        "\"varv\":{\"opcode\":\"data_variable\",\"next\":null,\"parent\":\"gt\","
        "\"inputs\":{},\"fields\":{\"VARIABLE\":[\"v\",\"v\"]},\"topLevel\":false,"
        "\"shadow\":false},"
        "\"done\":{\"opcode\":\"data_setvariableto\",\"next\":null,"
        "\"parent\":\"wu\",\"inputs\":{\"VALUE\":[1,[4,\"1\"]]},"
        "\"fields\":{\"VARIABLE\":[\"done\",\"done\"]},\"topLevel\":false,"
        "\"shadow\":false},"
        "\"rhat\":{\"opcode\":\"event_whenbroadcastreceived\",\"next\":\"rwait\","
        "\"parent\":null,\"inputs\":{},\"fields\":{\"BROADCAST_OPTION\":[\"msg\",\"msg-id\"]},"
        "\"topLevel\":true,\"shadow\":false,\"x\":300,\"y\":0},"
        "\"rwait\":{\"opcode\":\"control_wait\",\"next\":\"rset\","
        "\"parent\":\"rhat\",\"inputs\":{\"DURATION\":[1,[4,\"0.05\"]]},\"fields\":{},"
        "\"topLevel\":false,\"shadow\":false},"
        "\"rset\":{\"opcode\":\"data_setvariableto\",\"next\":null,"
        "\"parent\":\"rwait\",\"inputs\":{\"VALUE\":[1,[4,\"1\"]]},"
        "\"fields\":{\"VARIABLE\":[\"v\",\"v\"]},\"topLevel\":false,\"shadow\":false}}";
    return "{\"targets\":["
           "{\"isStage\":true,\"name\":\"Stage\",\"variables\":{},\"lists\":{},"
           "\"broadcasts\":{\"msg-id\":\"msg\"},\"blocks\":{},\"comments\":{},"
           "\"costumes\":[],\"sounds\":[],\"currentCostume\":0,\"volume\":100,"
           "\"layerOrder\":0},"
           "{\"isStage\":false,\"name\":\"S\",\"variables\":{\"done\":[\"done\",0],"
           "\"v\":[\"v\",0]},\"lists\":{},\"broadcasts\":{},\"blocks\":" + blocks +
           ",\"comments\":{},\"costumes\":[],\"sounds\":[],\"currentCostume\":0,"
           "\"volume\":100,\"visible\":true,\"x\":0,\"y\":0,\"size\":100,"
           "\"direction\":90,\"draggable\":false,\"rotationStyle\":\"all around\","
           "\"layerOrder\":1}],"
           "\"monitors\":[],\"extensions\":[],\"meta\":{\"semver\":\"3.0.0\"}}";
}

/// fn を別スレッドで実行し、ms 以内に終わらなければ false（ハング検出）。
/// タイムアウト時のフラグと fn のコピーはヒープに置く（detach したスレッドが
/// 呼出し側のスタックを触らないようにする）。
bool runWithin(std::function<void()> fn, int ms) {
    auto* done = new std::atomic<bool>(false);
    std::thread th([fn, done]() mutable {
        fn();
        done->store(true);
    });
    for (int waited = 0; waited <= ms; waited += 10) {
        if (done->load()) {
            th.join();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    th.detach();
    return false;
}

/// danger でも warp 内の待機系が完了すること（ハングしないこと）。
void testWarpWaits(JitMode mode) {
    setJitMode(mode);
    std::shared_ptr<Runtime> rt(new Runtime);
    if (!loadSb3(*rt, waitProjectJson(), nullptr).ok) {
        check(false, "load wait project");
        return;
    }
    rt->dangerMode = true;
    rt->greenFlag();
    const bool finished = runWithin([rt]() { rt->runUntilDone(10000); }, 5000);
    if (!finished)
        std::printf("  %s: warp waits did not finish (deadlock?)\n",
                    modeName(mode));
    check(finished, "danger keeps warp waits live (no deadlock)");
    if (!finished) return;   // 結果は判定しない（ランタイムはリーク）
    Target* sp = rt->getSpriteTargetByName(u"S");
    Variable* done = sp ? sp->lookupVariableById("done") : nullptr;
    Variable* v = sp ? sp->lookupVariableById("v") : nullptr;
    check(done && cast::toNumber(done->value) == 1,
          "warp wait-until proceeded after the broadcast");
    check(v && cast::toNumber(v->value) == 1,
          "broadcast receiver ran while the warp sender waited");
}

} // namespace

/// テストランナ（tests/unit/test_cast.cpp の main）から呼ばれる。
int test_danger_main() {
    GpuEnvGuard gpuDisabled;
    const JitMode modes[] = {JitMode::Off, JitMode::Eager};
    for (JitMode mode : modes) {
        // 予算あり: 数パスでは終わらない（予算切れで yield して譲る）。
        const double limited = onePass(false, true, mode);
        if (!(limited > 0 && limited < kIterations))
            std::printf("  %s: limited counter after 4 passes = %g\n",
                        modeName(mode), limited);
        check(limited > 0 && limited < kIterations,
              "warp loop is preempted by the time budget");

        // danger: 同じパス数で最後まで走る（予算で譲らない）。
        const double danger = onePass(true, true, mode);
        if (danger != kIterations)
            std::printf("  %s: danger counter after 4 passes = %g\n",
                        modeName(mode), danger);
        check(danger == kIterations,
              "danger mode runs the warp loop to completion without yielding");

        // 非 warp ループは danger でも毎反復 yield する（パスごとに 1 反復）。
        const double plain = onePass(true, false, mode);
        if (plain != 4)
            std::printf("  %s: plain counter after 4 passes = %g\n",
                        modeName(mode), plain);
        check(plain == 4, "non-warp loop still yields in danger mode");

        // 意味論は不変 — 最後まで回せば同じ結果になる。
        {
            setJitMode(mode);
            Runtime rt;
            if (loadSb3(rt, projectJson(true), nullptr).ok) {
                rt.warpTimeMs = 0.0;
                rt.dangerMode = true;
                rt.greenFlag();
                rt.runUntilDone(10000);
                check(counter(rt) == kIterations,
                      "danger mode preserves the final result");
            } else {
                check(false, "load project (run to completion)");
            }
        }
    }

    // 待機系は danger でも譲る（ハングしない）— interp / JIT 両方。
    for (JitMode mode : modes) testWarpWaits(mode);

    // 待機 yield の判定（テスト用フック）: 通常 warp は stuck 時のみ、
    // danger の warp は常に、非 warp は常に譲る。
    {
        Runtime rt;
        rt.sequencer->timer.start();
        JitCtx c{};
        c.runtime = &rt;
        c.sequencer = rt.sequencer.get();
        c.isWarp = 1;
        rt.dangerMode = false;
        check(xyo_wait_should_yield(&c) == 0,
              "normal warp waits yield only when stuck");
        rt.dangerMode = true;
        check(xyo_wait_should_yield(&c) == 1,
              "danger warp waits always yield");
        c.isWarp = 0;
        check(xyo_wait_should_yield(&c) == 1,
              "non-warp waits always yield");
    }

    // 環境変数の既定値（XYO_DANGER）— POSIX のみ検証する。
#ifndef _WIN32
    {
        setenv("XYO_DANGER", "1", 1);
        Runtime rt;
        check(rt.dangerMode, "XYO_DANGER=1 enables danger mode by default");
        setenv("XYO_DANGER", "0", 1);
        Runtime rt2;
        check(!rt2.dangerMode, "XYO_DANGER=0 keeps the budget");
        unsetenv("XYO_DANGER");
    }
#endif

    setJitMode(JitMode::Eager);
    std::printf("test_danger: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
