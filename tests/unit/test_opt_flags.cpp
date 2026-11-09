// --opt-all / XYO_OPT_ALL: 作業ツリー diff の opt-in 最適化を一括有効化する。
//
// 検証すること:
//   * 一覧の全トグルが未設定から "1" へ入る（set-if-absent）。
//   * 明示的に設定済みの XYO_* は上書きしない（A/B 用に 0 を残せる）。
//   * 2 回目は何も設定しない（冪等）。
//   * 一覧に「意味論を変える実験・診断・表示品位」のトグルが混ざらない。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/sstr.hpp"
#include "engine/opt_flags.hpp"

namespace {
using namespace xyo;
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL opt_flags: %s\n", what);
    }
}

const char* getEnv(const char* name) { return std::getenv(name); }

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

bool isOptInToggle(const char* name) {
    for (int i = 0; i < allOptimizationCount(); ++i) {
        const char* n = allOptimizationName(i);
        if (n && std::strcmp(n, name) == 0) return true;
    }
    return false;
}
} // namespace

int test_opt_flags_main() {
    // v1.2.0 production profile respects explicit overrides.
    {
        const int n = releaseOptimizationCount();
        check(n >= 5, "release profile includes verified defaults");
        check(releaseOptimizationName(-1) == nullptr, "release negative index");
        check(releaseOptimizationName(n) == nullptr, "release upper index");
        std::vector<std::string> names, old;
        std::vector<bool> present;
        for (int i = 0; i < n; ++i) {
            const char* name = releaseOptimizationName(i);
            names.emplace_back(name);
            const char* before = getEnv(name);
            present.push_back(before != nullptr);
            old.emplace_back(before ? before : "");
            unsetEnv(name);
        }
        setEnv(names.front().c_str(), "0");
        check(enableReleaseOptimizations() == n - 1,
              "release profile respects explicit override");
        check(std::strcmp(getEnv(names.front().c_str()), "0") == 0,
              "release profile preserves explicit 0");
        for (int i = 1; i < n; ++i) {
            const char* value = getEnv(names[i].c_str());
            check(value && std::strcmp(value, "1") == 0,
                  "release profile enables verified optimization");
        }
        check(enableReleaseOptimizations() == 0, "release profile idempotent");
        for (int i = 0; i < n; ++i) {
            if (present[i]) setEnv(names[i].c_str(), old[i].c_str());
            else unsetEnv(names[i].c_str());
        }
    }
    const int count = allOptimizationCount();
    check(count > 0, "toggle table is not empty");
    check(allOptimizationName(-1) == nullptr, "negative index rejected");
    check(allOptimizationName(count) == nullptr, "past-the-end index rejected");

    // 明示設定・opt-all 自体・フラグの直接状態を保存して、テスト後に戻す。
    std::vector<std::string> savedValues;
    std::vector<bool> savedPresent;
    auto saveVar = [&](const char* name) {
        const char* v = getEnv(name);
        savedPresent.push_back(v != nullptr);
        savedValues.push_back(v ? v : "");
    };
    saveVar("XYO_OPT_ALL");
    for (int i = 0; i < count; ++i) saveVar(allOptimizationName(i));
    saveVar("XYO_STRING_SHORT_INTERN");
    const bool savedShortIntern = stringShortInternEnabled();

    // --- 未設定から全トグルが入る -------------------------------------
    unsetEnv("XYO_OPT_ALL");
    for (int i = 0; i < count; ++i) unsetEnv(allOptimizationName(i));
    unsetEnv("XYO_STRING_SHORT_INTERN");
    setStringShortInternEnabled(false);

    const int applied = enableAllOptimizations();
    check(applied == count, "first call sets every toggle");
    bool allSet = true;
    for (int i = 0; i < count; ++i) {
        const char* v = getEnv(allOptimizationName(i));
        if (!v || std::strcmp(v, "1") != 0) {
            allSet = false;
            std::printf("FAIL opt_flags: %s not set to 1\n",
                        allOptimizationName(i));
        }
    }
    check(allSet, "every toggle env is \"1\"");
    check(stringShortInternEnabled(), "short-intern setter applied directly");

    // --- 冪等 ---------------------------------------------------------
    check(enableAllOptimizations() == 0, "second call is a no-op");

    // --- 明示設定を上書きしない ---------------------------------------
    const char* pinned = allOptimizationName(0);
    setEnv(pinned, "0");
    // 他のトグルを一度消して、「設定済みの 1 個だけ」が残るかを見る。
    for (int i = 1; i < count; ++i) unsetEnv(allOptimizationName(i));
    const int reapplied = enableAllOptimizations();
    check(reapplied == count - 1, "explicitly set toggle is skipped");
    {
        const char* v = getEnv(pinned);
        check(v && std::strcmp(v, "0") == 0, "explicit 0 wins over opt-all");
    }
    check(isOptInToggle(pinned), "pinned name still belongs to the table");

    // --- XYO_OPT_ALL の真偽判定 ---------------------------------------
    unsetEnv("XYO_OPT_ALL");
    check(!allOptimizationsRequested(), "unset XYO_OPT_ALL is false");
    setEnv("XYO_OPT_ALL", "1");
    check(allOptimizationsRequested(), "XYO_OPT_ALL=1 is true");
    setEnv("XYO_OPT_ALL", "0");
    check(!allOptimizationsRequested(), "XYO_OPT_ALL=0 is false");
    setEnv("XYO_OPT_ALL", "off");
    check(!allOptimizationsRequested(), "XYO_OPT_ALL=off is false");

    // --- 意味論・診断・品位のトグルを含めない -------------------------
    check(!isOptInToggle("XYO_EFFECT_JOURNAL"), "semantics experiment excluded");
    check(!isOptInToggle("XYO_RENDER_SCALE"), "render quality excluded");
    check(!isOptInToggle("XYO_STRING_STORAGE"), "storage selector excluded");
    check(!isOptInToggle("XYO_LIST_STORAGE"), "storage selector excluded");
    check(isOptInToggle("XYO_STRING_SHORT_INTERN"), "short-intern in table");
    check(!isOptInToggle("XYO_JIT_VERBOSE"), "diagnostic excluded");
    // 既定 ON は A/B 用に "0" で切れるので、一括 ON の対象ではない。
    check(!isOptInToggle("XYO_BCAST_FAST_ASCII"), "default-on toggle excluded");

    // --- 復元 ---------------------------------------------------------
    for (int i = 0, s = 1; i < count; ++i, ++s) {
        const char* name = allOptimizationName(i);
        if (savedPresent[s]) setEnv(name, savedValues[s].c_str());
        else unsetEnv(name);
    }
    if (savedPresent[0]) setEnv("XYO_OPT_ALL", savedValues[0].c_str());
    else unsetEnv("XYO_OPT_ALL");
    if (savedPresent[count + 1])
        setEnv("XYO_STRING_SHORT_INTERN", savedValues[count + 1].c_str());
    else unsetEnv("XYO_STRING_SHORT_INTERN");
    setStringShortInternEnabled(savedShortIntern);

    std::printf("opt_flags: %d checks, %d failures\n", checks, failures);
    return failures;
}
