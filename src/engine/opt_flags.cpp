#include "opt_flags.hpp"

#include <cstdlib>
#include <cstring>

#include "../core/sstr.hpp"

namespace xyo {
namespace {

struct Toggle {
    const char* name;
    const char* value;
};

// v1.2.0: Scratch parityと実測を満たした高速経路のみを既定化する。
 // OFFへ戻す場合は個別環境変数を明示する。実験的JIT/OSR/FSRは含めない。
 // numeric-repeat SSAは「未設定」と明示的=1で受理範囲が異なるため、
 // 既存の厳格な既定判定を維持し、この表には入れない。
constexpr Toggle kReleaseDefaults[] = {
    {"XYO_JIT_EQ_NUMERIC_PROFILE_CACHE", "1"},
    {"XYO_JIT_LIST_MONOTONE_FLAG", "1"},
    {"XYO_JIT_REPLACE_NO_NULL", "1"},
    {"XYO_JIT_NUMERIC_UNTIL_SSA", "1"},
    {"XYO_JIT_NUMERIC_UNTIL_FAST", "1"},
    {"XYO_JIT_AFFINE_UNTIL", "1"},
    {"XYO_BCAST_FAST_ASCII", "1"},
    {"XYO_SKIP_SAME_EFFECT_REDRAW", "1"},
};

// 既定 OFF の実行時最適化だけを並べる。既定 ON のトグルはここに不要。
// 意味論を変える実験（XYO_EFFECT_JOURNAL）、遅くなる方向の切り替え
// （XYO_AUDIO_SYNC / XYO_AUDIO_ASYNC_ALL）、表示品位の切り替え
// （XYO_RENDER_SCALE）、ストレージ形式の選択（XYO_STRING_STORAGE /
// XYO_LIST_STORAGE）、診断（*_STATS / *_LOG / XYO_RT_TEST_*）は含めない。
constexpr Toggle kToggles[] = {
    // --- JIT codegen -------------------------------------------------
    {"XYO_JIT_IF_PHI", "1"},
    {"XYO_JIT_PHI_DEMAND", "1"},
    {"XYO_JIT_CFG_TYPE_HINTS", "1"},
    {"XYO_JIT_LOCAL_READ_FORWARD", "1"},
    {"XYO_JIT_DUPLICATE_SCALAR_NUM", "1"},
    {"XYO_JIT_TO_NUM_COLD_HELPER", "1"},
    {"XYO_JIT_INLINE_EMPTY_DIRECT", "1"},
    {"XYO_JIT_REPLACE_LIKELY_NONNULL", "1"},
    // --- JIT engine / runtime ---------------------------------------
    {"XYO_JIT_OSR", "1"},
    {"XYO_JIT_ASYNC", "1"},
    {"XYO_JIT_ASYNC_PREFETCH", "1"},
    {"XYO_JIT_CALLFRAME", "1"},
    {"XYO_JIT_COMMON_CFG", "1"},
    {"XYO_INTERP_OSR_SNAPSHOT", "1"},
    {"XYO_BCAST_RETAIN_VAR_SLOTS", "1"},
    // --- 文字列 ------------------------------------------------------
    {"XYO_STRING_SHORT_INTERN", "1"},
    // --- レンダラ（PEN-001 の FSR） ---------------------------------
    {"XYO_PEN_FSR", "1"},
    {"XYO_PEN_FSR_RCAS", "1"},
    // --- WASM（EMSCRIPTEN のみ。ネイティブでは無視される） -----------
    {"XYO_NESTED_WASM", "1"},
};

bool truthy(const char* value) {
    if (!value || !*value) return false;
    return std::strcmp(value, "0") != 0 && std::strcmp(value, "off") != 0 &&
           std::strcmp(value, "false") != 0;
}

bool setEnvIfAbsent(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 0) == 0;
#endif
}

} // namespace

int enableReleaseOptimizations() {
    int applied = 0;
    for (const Toggle& toggle : kReleaseDefaults) {
        if (std::getenv(toggle.name)) continue;
        if (setEnvIfAbsent(toggle.name, toggle.value)) ++applied;
    }
    return applied;
}

int releaseOptimizationCount() {
    return static_cast<int>(sizeof(kReleaseDefaults) / sizeof(kReleaseDefaults[0]));
}

const char* releaseOptimizationName(int index) {
    if (index < 0 || index >= releaseOptimizationCount()) return nullptr;
    return kReleaseDefaults[index].name;
}

bool allOptimizationsRequested() {
    return truthy(std::getenv("XYO_OPT_ALL"));
}

int enableAllOptimizations() {
    int applied = 0;
    bool setShortIntern = false;
    for (const Toggle& t : kToggles) {
        if (std::getenv(t.name)) continue;   // 明示設定を尊重する
        if (!setEnvIfAbsent(t.name, t.value)) continue;
        ++applied;
        if (std::strcmp(t.name, "XYO_STRING_SHORT_INTERN") == 0)
            setShortIntern = true;
    }
    // XYO_STRING_SHORT_INTERN は env ではなく sstr のフラグを直接読む経路が
    // あるため、自分で設定したときだけ setter も叩く（明示設定は尊重）。
    if (setShortIntern) setStringShortInternEnabled(true);
    return applied;
}

int allOptimizationCount() {
    return static_cast<int>(sizeof(kToggles) / sizeof(kToggles[0]));
}

const char* allOptimizationName(int index) {
    if (index < 0 || index >= allOptimizationCount()) return nullptr;
    return kToggles[index].name;
}

} // namespace xyo
