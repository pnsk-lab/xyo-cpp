// danger モード（スレッドごとの実行時間予算を外す）の意味論テスト。
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
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "engine/thread.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

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

} // namespace

/// テストランナ（tests/unit/test_cast.cpp の main）から呼ばれる。
int test_danger_main() {
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
