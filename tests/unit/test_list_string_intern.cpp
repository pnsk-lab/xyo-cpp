// Black-box coverage of the per-load, bounded SB3 initial-list string interner.
// No cache API: admission and scope are observed through live SStr pointers.
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/gc.hpp"
#include "core/json.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "load/sb3_save.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

namespace {
using namespace xyo;
int checks = 0, failures = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::printf("FAIL list string intern %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct EnvGuard {
    std::string name, previous;
    bool existed;
    EnvGuard(const char* key, const char* value)
        : name(key), existed(std::getenv(key) != nullptr) {
        if (existed) previous = std::getenv(key);
        put(value);
    }
    void put(const char* value) {
#ifdef _WIN32
        _putenv_s(name.c_str(), value ? value : "");
#else
        if (value) setenv(name.c_str(), value, 1); else unsetenv(name.c_str());
#endif
    }
    ~EnvGuard() { put(existed ? previous.c_str() : nullptr); }
};

std::string quoted(const std::string& s) { return jsonStringify(JsonValue(s)); }
std::string array(const std::vector<std::string>& values) {
    std::string result = "[";
    for (const auto& value : values) {
        if (result.size() > 1) result += ',';
        result += quoted(value);
    }
    return result + "]";
}
std::string target(bool stage, const std::string& lists,
                   const std::string& variables = "{}", const std::string& blocks = "{}") {
    return std::string("{\"isStage\":") + (stage ? "true" : "false") +
        ",\"name\":" + quoted(stage ? "Stage" : "Sprite") +
        ",\"variables\":" + variables + ",\"lists\":" + lists +
        ",\"blocks\":" + blocks + ",\"broadcasts\":{},\"comments\":{},"
        "\"costumes\":[],\"sounds\":[],\"currentCostume\":0}";
}
std::string project(const std::string& targets) {
    return "{\"targets\":[" + targets + "],\"monitors\":[],\"extensions\":[],"
           "\"meta\":{\"semver\":\"3.0.0\"}}";
}
std::string oneList(const std::vector<std::string>& values) {
    // Exactly one initial list, no scalar strings or other list keys.
    return project(target(true, "{\"a\":[\"a\"," + array(values) + "]}"));
}
SList* list(Target* t, const char* id) {
    Variable* v = t ? t->lookupVariableById(id) : nullptr;
    CHECK(v && v->list);
    return v ? v->list : nullptr;
}
SStr* ptr(SList* l, unsigned i) { return l->items[i].asStrPtr(); }
std::string text(Value v) { return SStr::toUtf8(cast::toString(v)); }

void sharingAndContent(const char* mode) {
    EnvGuard env("XYO_SB3_LIST_INTERN", mode);
    const bool enabled = !mode || std::string(mode) != "0";
    // Same content, two JSON spellings; embedded NUL must not truncate a key.
    const std::string raw = R"JSON(["same","same","Case","case","1","01","1.0","#aabbcc","#AABBCC","é😀","\u00e9\ud83d\ude00","a\u0000b","a\u0000c","a\u0000b","","",1,true,false,1.5])JSON";
    Runtime rt;
    const auto result = loadSb3(rt, project(
        target(true, "{\"a\":[\"a\"," + raw + "],\"b\":[\"b\",[\"same\"]]}",
               "{\"scalar\":[\"scalar\",\"same\"]}") + "," +
        target(false, "{\"c\":[\"c\",[\"same\"]]}")), nullptr);
    CHECK(result.ok);
    if (!result.ok || result.targets.size() != 2) return;
    SList* a = list(result.targets[0], "a");
    SList* b = list(result.targets[0], "b");
    SList* c = list(result.targets[1], "c");
    if (!a || !b || !c || a->len != 20) { CHECK(false); return; }
    CHECK((ptr(a, 0) == ptr(a, 1)) == enabled);
    CHECK((ptr(a, 0) == ptr(b, 0)) == enabled);
    CHECK((ptr(a, 0) == ptr(c, 0)) == enabled);
    CHECK((ptr(a, 9) == ptr(a, 10)) == enabled);
    CHECK((ptr(a, 11) == ptr(a, 13)) == enabled);
    CHECK((ptr(a, 14) == ptr(a, 15)) == enabled);
    const std::vector<std::string> expected = {
        "same", "same", "Case", "case", "1", "01", "1.0", "#aabbcc", "#AABBCC",
        "é😀", "é😀", std::string("a\0b", 3), std::string("a\0c", 3),
        std::string("a\0b", 3), "", ""
    };
    for (unsigned i = 0; i < expected.size(); ++i) {
        CHECK(a->items[i].isString());
        CHECK(text(a->items[i]) == expected[i]);
    }
    // Do not canonicalize Scratch-equivalent numbers, case or RGB spellings.
    CHECK(ptr(a, 2) != ptr(a, 3));
    CHECK(ptr(a, 4) != ptr(a, 5));
    CHECK(ptr(a, 4) != ptr(a, 6));
    CHECK(ptr(a, 7) != ptr(a, 8));
    CHECK(ptr(a, 11) != ptr(a, 12));
    CHECK(ptr(a, 14) != nullptr && !a->items[14].isEmptyString());
    CHECK(a->items[16].isNumber() && a->items[16].asNumberUnchecked() == 1);
    CHECK(a->items[17].isBool() && a->items[17].asBool());
    CHECK(a->items[18].isBool() && !a->items[18].asBool());
    CHECK(a->items[19].isNumber() && a->items[19].asNumberUnchecked() == 1.5);
    Variable* scalar = result.targets[0]->lookupVariableById("scalar");
    CHECK(scalar && scalar->value.isString());
    if (scalar) CHECK(scalar->value.asStrPtr() != ptr(a, 0));
    // Lazy numeric/RGB caches must not alter the exact shared text.
    CHECK(cast::toNumber(a->items[5]) == 1);
    const auto rgb = cast::toRgbColorList(a->items[7]);
    CHECK(rgb.r == 170 && rgb.g == 187 && rgb.b == 204);
    CHECK(text(a->items[5]) == "01");
    CHECK(text(a->items[7]) == "#aabbcc");

    const auto saved = saveSb3(rt);
    CHECK(saved.ok);
    if (saved.ok) {
        Runtime reloaded;
        const auto loaded = loadSb3File(reloaded, saved.data.data(), saved.data.size());
        CHECK(loaded.ok);
        if (loaded.ok && !loaded.targets.empty()) {
            SList* again = list(loaded.targets[0], "a");
            if (again && again->len == a->len) {
                for (unsigned i = 0; i < a->len; ++i) {
                    CHECK(text(again->items[i]) == text(a->items[i]));
                    CHECK(again->items[i].isString() == a->items[i].isString());
                    CHECK(again->items[i].isBool() == a->items[i].isBool());
                }
                CHECK((ptr(again, 0) == ptr(again, 1)) == enabled);
            } else CHECK(false);
        }
    }
}

void entryLimit() {
    EnvGuard env("XYO_SB3_LIST_INTERN", "1");
    std::vector<std::string> values;
    for (unsigned i = 0; i < 4100; ++i) values.push_back("entry-" + std::to_string(i));
    values.push_back(values[0]);
    values.push_back(values[4095]);
    values.push_back(values[4096]);
    values.push_back(values[4099]);
    Runtime rt;
    auto loaded = loadSb3(rt, oneList(values), nullptr);
    CHECK(loaded.ok);
    if (!loaded.ok || loaded.targets.empty()) return;
    SList* a = list(loaded.targets[0], "a");
    if (!a || a->len != values.size()) { CHECK(false); return; }
    CHECK(ptr(a, 0) == ptr(a, 4100));
    CHECK(ptr(a, 4095) == ptr(a, 4101));
    CHECK(ptr(a, 4096) != ptr(a, 4102));
    CHECK(ptr(a, 4099) != ptr(a, 4103));
    for (unsigned i = 0; i < values.size(); ++i) CHECK(text(a->items[i]) == values[i]);
}

void byteAndLengthLimits() {
    EnvGuard env("XYO_SB3_LIST_INTERN", "1");
    std::vector<std::string> values;
    // 256 * 4096 = exactly 1MiB, well below 4096 entries. No other list keys.
    for (unsigned i = 0; i < 258; ++i) {
        std::string key = "bytes-" + std::to_string(i) + ":";
        key.resize(4096, 'x');
        values.push_back(key);
    }
    values.push_back(values[0]);
    values.push_back(values[255]);
    values.push_back(values[256]);
    Runtime rt;
    auto loaded = loadSb3(rt, oneList(values), nullptr);
    CHECK(loaded.ok);
    if (!loaded.ok || loaded.targets.empty()) return;
    SList* a = list(loaded.targets[0], "a");
    if (!a || a->len != values.size()) { CHECK(false); return; }
    CHECK(ptr(a, 0) == ptr(a, 258));
    CHECK(ptr(a, 255) == ptr(a, 259));
    CHECK(ptr(a, 256) != ptr(a, 260));
    for (unsigned i = 0; i < values.size(); ++i) CHECK(text(a->items[i]) == values[i]);

    // Overlong misses must not consume admission budget, including UTF-8 bytes
    // rather than UTF-16 code-unit lengths (2049 copies of é = 4098 bytes).
    std::string utf8;
    for (unsigned i = 0; i < 2049; ++i) utf8 += "é";
    const std::string longKey(4097, 'z'), boundary(4096, 'z');
    Runtime lengths;
    loaded = loadSb3(lengths, oneList({longKey, longKey, boundary, boundary, utf8, utf8,
                                      "short", "short"}), nullptr);
    CHECK(loaded.ok);
    if (!loaded.ok || loaded.targets.empty()) return;
    a = list(loaded.targets[0], "a");
    if (!a || a->len != 8) { CHECK(false); return; }
    CHECK(ptr(a, 0) != ptr(a, 1));
    CHECK(ptr(a, 2) == ptr(a, 3));
    CHECK(ptr(a, 4) != ptr(a, 5));
    CHECK(ptr(a, 6) == ptr(a, 7));
    CHECK(text(a->items[0]) == longKey);
    CHECK(text(a->items[4]) == utf8);
}

void loadScopeAndSlots() {
    EnvGuard env("XYO_SB3_LIST_INTERN", "1");
    Runtime first, second;
    const auto input = project(target(false, "{\"a\":[\"a\",[\"same\",\"same\"]],"
                                                     "\"b\":[\"b\",[\"same\"]]}"));
    auto r1 = loadSb3(first, input, nullptr);
    auto r2 = loadSb3(second, input, nullptr);
    CHECK(r1.ok && r2.ok);
    if (!r1.ok || !r2.ok || r1.targets.empty() || r2.targets.empty()) return;
    SList* a = list(r1.targets[0], "a");
    SList* b = list(r1.targets[0], "b");
    SList* other = list(r2.targets[0], "a");
    if (!a || !b || !other) return;
    CHECK(ptr(a, 0) == ptr(a, 1));
    CHECK(ptr(a, 0) != ptr(other, 0));
    // Importing a sprite into an existing Runtime creates a fresh per-load pool.
    auto imported = loadSb3(first, target(false, "{\"a\":[\"a\",[\"same\",\"same\"]]}"),
                            nullptr, true);
    CHECK(imported.ok && imported.targets.size() == 1);
    if (imported.ok && imported.targets.size() == 1) {
        SList* fresh = list(imported.targets[0], "a");
        if (fresh) {
            CHECK(ptr(fresh, 0) == ptr(fresh, 1));
            CHECK(ptr(fresh, 0) != ptr(a, 0));
        }
    }
    // Value storage is per slot/list, not shared mutable list state.
    const Value original = b->items[0];
    SList* cloned = a->clone();
    // Keep the standalone clone rooted across future allocations/collections.
    Gc::get().addRootProvider(&cloned, [&cloned](Gc& gc) { gc.markPtr(cloned); });
    CHECK(cloned != a && cloned->items != a->items);
    CHECK(cloned->items[0] == original);
    cloned->items[0] = Value::num(42);
    CHECK(a->items[0] == original && a->items[1] == original);
    a->items[0] = Value::boolean(true);
    CHECK(a->items[1] == original && b->items[0] == original);
    a->erase0(0);
    CHECK(a->len == 1 && a->items[0] == original);
    CHECK(cloned->len == 2 && cloned->items[0].isNumber());
    CHECK(b->len == 1 && b->items[0] == original);
    Gc::get().collect();
    CHECK(text(a->items[0]) == "same" && text(b->items[0]) == "same");
    CHECK(text(cloned->items[1]) == "same");
    Gc::get().removeRootProvider(&cloned);
}

void lastOwnerAfterCollection() {
    EnvGuard env("XYO_SB3_LIST_INTERN", "1");
    Runtime rt;
    auto loaded = loadSb3(rt, project(
        target(true, "{\"a\":[\"a\",[\"shared survivor\",\"shared survivor\"]]}") + "," +
        target(false, "{\"b\":[\"b\",[\"shared survivor\"]]}")), nullptr);
    CHECK(loaded.ok && loaded.targets.size() == 2);
    if (!loaded.ok || loaded.targets.size() != 2) return;
    SList* a = list(loaded.targets[0], "a");
    SList* b = list(loaded.targets[1], "b");
    if (!a || !b) return;
    CHECK(ptr(a, 0) == ptr(b, 0));
    // Remove all references in the first owner: the installed second target
    // must keep the shared object alive after the temporary interner is gone.
    a->clear();
    Gc::get().collect();
    CHECK(b->len == 1 && text(b->items[0]) == "shared survivor");
    SList* cloned = b->clone();
    Gc::get().addRootProvider(&cloned, [&cloned](Gc& gc) { gc.markPtr(cloned); });
    b->clear();
    Gc::get().collect();
    CHECK(cloned->len == 1 && text(cloned->items[0]) == "shared survivor");
    Gc::get().removeRootProvider(&cloned);
}

void concurrentCaches() {
    EnvGuard env("XYO_SB3_LIST_INTERN", "1");
    Runtime rt;
    const auto loaded = loadSb3(rt, oneList({"10597059", "10597059", "10597059", "10597059"}), nullptr);
    CHECK(loaded.ok);
    if (!loaded.ok || loaded.targets.empty()) return;
    SList* a = list(loaded.targets[0], "a");
    if (!a || a->len != 4) { CHECK(false); return; }
    CHECK(ptr(a, 0) == ptr(a, 1) && ptr(a, 0) == ptr(a, 2) && ptr(a, 2) == ptr(a, 3));
    // Numeric and RGB caches start cold on the SAME shared string; both cache
    // publishers race on its flags, not just on separate numeric/color strings.
    // Only the main thread records CHECKs; no GC allocations occur in workers.
    std::atomic<bool> start{false}, correct{true};
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned i = 0; i < 1000; ++i) {
                const auto rgb = cast::toRgbColorList(a->items[2 + (worker & 1)]);
                if (cast::toNumber(a->items[worker & 1]) != 10597059 ||
                    rgb.r != 161 || rgb.g != 178 || rgb.b != 195)
                    correct.store(false, std::memory_order_relaxed);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    CHECK(correct.load());
    CHECK(text(a->items[0]) == "10597059" && text(a->items[2]) == "10597059");
}

void gcPressure() {
    EnvGuard env("XYO_SB3_LIST_INTERN", "1");
    Runtime rt;
    Gc& gc = Gc::get();
    // The public GC API has no threshold getter. Restore the suite's standard
    // 2MiB threshold on every exit, just like test_e2e's pressure tests.
    struct RestoreThreshold { ~RestoreThreshold() { Gc::get().setThreshold(2 << 20); } } restore;
    const auto before = gc.collectCount();
    gc.setThreshold(1);
    const auto loaded = loadSb3(rt, project(
        target(true, "{\"a\":[\"a\",[\"rooted\",\"rooted\",\"\",\"a\\u0000b\"]]}") + "," +
        target(false, "{\"b\":[\"b\",[\"rooted\"]]}")), nullptr);
    CHECK(gc.collectCount() == before);
    CHECK(loaded.ok);
    if (!loaded.ok || loaded.targets.size() != 2) return;
    gc.collect(); // Pool has been destroyed; installed Runtime roots alone suffice.
    CHECK(gc.collectCount() == before + 1);
    SList* a = list(loaded.targets[0], "a");
    SList* b = list(loaded.targets[1], "b");
    if (!a || !b) return;
    CHECK(ptr(a, 0) == ptr(a, 1) && ptr(a, 0) == ptr(b, 0));
    CHECK(text(a->items[0]) == "rooted");
    CHECK(ptr(a, 2) != nullptr && text(a->items[2]).empty());
    CHECK(text(a->items[3]) == std::string("a\0b", 3));
}
} // namespace

int test_list_string_intern_main() {
    sharingAndContent(nullptr); // Default on, even if caller environment disabled it.
    sharingAndContent("1");
    sharingAndContent("0");
    entryLimit();
    byteAndLengthLimits();
    loadScopeAndSlots();
    lastOwnerAfterCollection();
    concurrentCaches();
    gcPressure();
    std::printf("list string intern: %d checks, %d failures\n", checks, failures);
    return failures;
}
