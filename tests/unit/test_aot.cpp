// xyo-cpp: AOT コンパイルの意味論一致テスト。
//
// 同じプロジェクトを (1) 通常の JIT で実行した場合と (2) AOT オブジェクトを
// ロードして実行した場合で、実行後の全変数・リストが一致することを確認する。
// AOT の生成コードはホストの絶対アドレスを埋め込まない（slot 経由）ため、
// ここでロードが通ること自体が slot 束縛の回帰テストになる。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"

#ifdef XYO_JIT
#include "aot/aot.hpp"
#include "jit/jit.hpp"
#endif

using namespace xyo;

namespace {

int g_aotChecks = 0, g_aotFails = 0;

} // namespace

#ifdef XYO_JIT

namespace {

void check(bool cond, const char* what) {
    ++g_aotChecks;
    if (!cond) {
        ++g_aotFails;
        std::printf("FAIL aot: %s\n", what);
    }
}

std::string fixturePath(const char* rel) {
    std::string path = __FILE__;
    const size_t pos = path.find_last_of('/');
    path = path.substr(0, pos) + "/../fixtures/" + rel;
    return path;
}

bool readAll(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return !out.empty();
}

/// 全ターゲットの変数・リストを id 昇順で文字列化（比較用）。
std::string dumpState(Runtime& rt) {
    std::string s;
    for (Target* t : rt.targets) {
        s += SStr::toUtf8(t->getName());
        s += ":\n";
        std::vector<std::string> ids;
        for (auto& [id, v] : t->variables) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        for (const std::string& id : ids) {
            const Variable& v = t->variables[id];
            s += "  ";
            s += id;
            s += "=";
            if (v.type == Variable::kListType) {
                SList* l = v.list;
                s += "[";
                if (l)
                    for (uint32_t i = 0; i < l->len; ++i) {
                        SStr* e = cast::toString(l->items[i]);
                        s += e ? SStr::toUtf8(e->view()) : "";
                        s += ",";
                    }
                s += "]";
            } else {
                SStr* e = cast::toString(v.value);
                s += e ? SStr::toUtf8(e->view()) : "";
            }
            s += "\n";
        }
    }
    return s;
}

void runCase(const char* rel, int frames) {
    std::vector<uint8_t> data;
    if (!readAll(fixturePath(rel), data)) {
        check(false, "fixture read");
        return;
    }
    Runtime a;
    if (!loadSb3File(a, data.data(), data.size()).ok) {
        check(false, "fixture load");
        return;
    }
    const uint64_t hash = aot::hashBytes(data.data(), data.size());

    aot::BuildResult br;
    std::string why;
    if (!aot::compileProject(a, hash, br, &why)) {
        std::printf("FAIL aot %s: compile: %s\n", rel, why.c_str());
        ++g_aotFails;
        ++g_aotChecks;
        return;
    }
    check(br.scripts > 0, "compiled at least one script");

    a.greenFlag();
    a.runUntilDone(frames);
    const std::string ref = dumpState(a);

    Runtime b;
    if (!loadSb3File(b, data.data(), data.size()).ok || !b.jit) {
        check(false, "second load / JIT unavailable");
        return;
    }
    if (!b.jit->loadAot(b, br.object.data(), br.object.size(), hash, &why)) {
        std::printf("FAIL aot %s: loadAot: %s\n", rel, why.c_str());
        ++g_aotFails;
        ++g_aotChecks;
        return;
    }
    b.greenFlag();
    b.runUntilDone(frames);
    const bool same = ref == dumpState(b);
    check(same, rel);
    if (!same) {
        const std::string got = dumpState(b);
        size_t i = 0;
        while (i < ref.size() && i < got.size() && ref[i] == got[i]) ++i;
        std::printf("  first diff at %zu: jit=%.40s / aot=%.40s\n", i,
                    ref.c_str() + i, got.c_str() + i);
    }
    std::printf("aot %s: %d scripts, %d slots\n", rel, br.scripts,
                br.slots);
}

} // namespace

int test_aot_main() {
    runCase("basic.sb3", 200);
    runCase("par/par_indep.sb3", 60);
    std::printf("aot: %d checks, %d failures\n", g_aotChecks, g_aotFails);
    return g_aotFails;
}

#else

int test_aot_main() { return 0; }

#endif
