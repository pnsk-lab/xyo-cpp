// xyo-cpp: 自作拡張レジストリ（Phase 0）の単体テスト。
//
// - demo 拡張を Runtime に登録し、sb3 から interp / JIT eager の両方で
//   同じ結果になること
// - BlockCached に PrimFn と primitiveContext が結び付くこと
// - id / opcode の重複・prefix 不一致・コア opcode との衝突を拒否すること
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "blockly/dynamic_catalog.hpp"
#include "core/cast.hpp"
#include "core/json.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "extensions/demo.hpp"
#include "extensions/extension.hpp"
#include "extensions/manifest.hpp"
#include "extensions/wasm_provider.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "load/sb3_save.hpp"
#include "load/zip.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const char* kProject = R"JSON({
  "targets": [
    {"isStage": true, "name": "Stage",
     "variables": {"v": ["v", 0]}, "lists": {}, "broadcasts": {},
     "blocks": {}, "costumes": [], "sounds": [], "currentCostume": 0,
     "layerOrder": 0},
    {"isStage": false, "name": "Sprite1",
     "variables": {}, "lists": {}, "broadcasts": {},
     "blocks": {
       "hat": {"opcode": "event_whenflagclicked", "next": "set1",
               "parent": null, "inputs": {}, "fields": {},
               "topLevel": true, "shadow": false, "x": 0, "y": 0},
       "set1": {"opcode": "data_setvariableto", "next": "set2",
                "parent": "hat",
                "inputs": {"VALUE": [2, "dbl"]},
                "fields": {"VARIABLE": ["v", "v"]}},
       "dbl": {"opcode": "demo_double", "next": null, "parent": "set1",
               "inputs": {"NUM": [1, [4, "21"]]}, "fields": {},
               "shadow": false},
       "set2": {"opcode": "data_setvariableto", "next": "set3",
                "parent": "set1",
                "inputs": {"VALUE": [2, "st"]},
                "fields": {"VARIABLE": ["v", "v"]}},
       "st": {"opcode": "demo_set", "next": null, "parent": "set2",
              "inputs": {"NUM": [1, [4, "7"]]}, "fields": {},
              "shadow": false},
       "set3": {"opcode": "data_setvariableto", "next": "chg",
                "parent": "set2",
                "inputs": {"VALUE": [2, "nx1"]},
                "fields": {"VARIABLE": ["v", "v"]}},
       "nx1": {"opcode": "demo_next", "next": null, "parent": "set3",
               "inputs": {}, "fields": {}, "shadow": false},
       "chg": {"opcode": "data_changevariableby", "next": null,
               "parent": "set3",
               "inputs": {"VALUE": [2, "nx2"]},
               "fields": {"VARIABLE": ["v", "v"]}},
       "nx2": {"opcode": "demo_next", "next": null, "parent": "chg",
               "inputs": {}, "fields": {}, "shadow": false}
     },
     "costumes": [], "sounds": [], "currentCostume": 0,
     "x": 0, "y": 0, "size": 100, "direction": 90,
     "rotationStyle": "all around", "visible": true, "layerOrder": 1}
  ],
  "monitors": [], "extensions": ["demo"],
  "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
})JSON";

class BadPrefixExtension final : public ExtensionInstance {
public:
    const ExtensionDef& def() const override { return def_; }
    Value call(const std::string&, const ArgValues&,
               BlockUtility&) override {
        return Value::undefined();
    }

private:
    ExtensionDef def_{"bad", "Bad", 0, "", "", {{"other_thing", "x"}}};
};

class CoreCollisionExtension final : public ExtensionInstance {
public:
    const ExtensionDef& def() const override { return def_; }
    Value call(const std::string&, const ArgValues&,
               BlockUtility&) override {
        return Value::undefined();
    }

private:
    ExtensionDef def_{"pen", "PenCollision", 0, "", "", {{"pen_penDown", "x"}}};
};

/// スクリプトを 1 回実行して変数 v を返す。失敗時は -1。
double runScenario(JitMode mode, bool* loaded) {
    setJitMode(mode);
    ExtensionRegistry registry;
    Runtime rt;
    std::string error;
    if (!registerDemoExtension(rt, registry, &error)) {
        std::printf("register: %s\n", error.c_str());
        *loaded = false;
        return -1.0;
    }

    Sb3LoadResult result = loadSb3(rt, kProject, nullptr);
    *loaded = result.ok;
    if (!result.ok) {
        std::printf("load: %s\n", result.error.c_str());
        return -1.0;
    }

    // ロード時に demo_* が PrimFn + context へ束縛されていること。
    Target* sprite = nullptr;
    for (Target* t : rt.targets)
        if (!t->isStage) sprite = t;
    CHECK(sprite != nullptr);
    if (sprite) {
        BlockCached* dbl = sprite->blocks->getCachedExecute("dbl");
        CHECK(dbl != nullptr);
        if (dbl) {
            CHECK(dbl->blockFunction != nullptr);
            CHECK(dbl->primitiveContext == registry.find("demo"));
        }
    }

    rt.greenFlag();
    rt.runUntilDone(100000);

    Target* stage = rt.getTargetForStage();
    Variable* v = stage ? stage->lookupVariableById("v") : nullptr;
    CHECK(v != nullptr);
    return v ? cast::toNumber(v->value) : -1.0;
}

void test_registry_validation() {
    Runtime rt;
    ExtensionRegistry registry;
    std::string error;

    CHECK(registerDemoExtension(rt, registry, &error));
    CHECK(registry.find("demo") != nullptr);
    CHECK(registry.definitions().size() == 1);
    CHECK(registry.primitiveFor("demo_double") != nullptr);
    CHECK(registry.contextFor("demo_double") == registry.find("demo"));
    CHECK(registry.primitiveFor("demo_missing") == nullptr);

    // 同じ拡張の二重登録は拒否。
    error.clear();
    CHECK(!registerDemoExtension(rt, registry, &error));
    CHECK(error.find("duplicate extension id") != std::string::npos);

    // prefix 不一致は拒否され、途中のブロックも登録されない。
    error.clear();
    CHECK(!registry.add(rt, std::make_unique<BadPrefixExtension>(), &error));
    CHECK(error.find("does not start") != std::string::npos);
    CHECK(rt.getOpcodeFunction("other_thing") == nullptr);

    // 既存のコア opcode との衝突は拒否。
    error.clear();
    CHECK(!registry.add(rt, std::make_unique<CoreCollisionExtension>(),
                        &error));
    CHECK(error.find("already registered") != std::string::npos);
    CHECK(registry.find("pen") == nullptr);
}

const char* kManifest = R"JSON({
  "schema": 1,
  "id": "clock",
  "name": "Clock",
  "color": "#123456",
  "blocks": [
    {"opcode": "clock_now", "label": "now", "shape": "reporter"},
    {"opcode": "clock_wait", "label": "wait %1 seconds", "shape": "stack",
     "args": [{"name": "SECS", "type": "number", "default": "1"}]},
    {"opcode": "clock_mode", "label": "mode %1", "shape": "reporter",
     "args": [{"name": "MODE", "type": "dropdown",
               "options": ["fast", "slow"]}]},
    {"opcode": "clock_ready", "label": "when ready", "shape": "hat",
     "edge": true}
  ]
})JSON";

void test_manifest_parse() {
    ExtensionDef def;
    std::string error;
    CHECK(parseExtensionManifest(kManifest, &def, &error));
    CHECK(def.id == "clock");
    CHECK(def.name == "Clock");
    CHECK(def.color == 0x123456u);
    CHECK(def.blocks.size() == 4);
    if (def.blocks.size() == 4) {
        CHECK(def.blocks[0].shape == "reporter");
        CHECK(def.blocks[1].args.size() == 1);
        if (!def.blocks[1].args.empty()) {
            CHECK(def.blocks[1].args[0].type == "number");
            CHECK(def.blocks[1].args[0].dflt == "1");
        }
        CHECK(def.blocks[2].args.size() == 1);
        if (!def.blocks[2].args.empty()) {
            CHECK(def.blocks[2].args[0].options.size() == 4);
            CHECK(def.blocks[2].args[0].dflt == "fast");
        }
        CHECK(def.blocks[3].hat);
        CHECK(def.blocks[3].edgeActivated);
    }

    auto rejects = [&](const char* json) {
        ExtensionDef bad;
        std::string why;
        return !parseExtensionManifest(json, &bad, &why) && !why.empty();
    };
    CHECK(rejects("{}"));
    CHECK(rejects(R"({"id":"Bad","name":"x","blocks":[{"opcode":"Bad_x","label":"x"}]})"));
    CHECK(rejects(R"({"id":"ok","name":"x","color":"#GGGGGG","blocks":[{"opcode":"ok_x","label":"x"}]})"));
    CHECK(rejects(R"({"id":"ok","name":"x","blocks":[]})"));
    CHECK(rejects(R"({"id":"ok","name":"x","blocks":[{"opcode":"other_x","label":"x"}]})"));
    CHECK(rejects(R"({"id":"ok","name":"x","blocks":[{"opcode":"ok_x","label":"x"},{"opcode":"ok_x","label":"y"}]})"));
    CHECK(rejects(R"({"id":"ok","name":"x","blocks":[{"opcode":"ok_x","label":"x","args":[{"name":"A","type":"wat"}]}]})"));
    CHECK(rejects(R"({"id":"ok","name":"x","blocks":[{"opcode":"ok_x","label":"x","args":[{"name":"A","type":"dropdown"}]}]})"));
}

void test_dynamic_catalog() {
    edit::clearDynamicExtensions();
    ExtensionDef def;
    std::string error;
    CHECK(parseExtensionManifest(kManifest, &def, &error));
    edit::registerExtensionDef(def);

    const edit::BlockDef* now = edit::findBlock("clock_now");
    CHECK(now != nullptr);
    if (now) CHECK(now->shape == edit::Shape::Reporter);
    const edit::BlockDef* wait = edit::findBlock("clock_wait");
    CHECK(wait != nullptr);
    if (wait) {
        CHECK(wait->shape == edit::Shape::Stack);
        CHECK(wait->args.size() == 1);
        if (!wait->args.empty()) {
            CHECK(wait->args[0].shadow == "math_number");
            CHECK(wait->args[0].shadowDefault == "1");
        }
    }
    CHECK(edit::isDynamicBlock("clock_now"));
    CHECK(!edit::isDynamicBlock("pen_clear"));
    const auto& cats = edit::dynamicExtensionCategories();
    CHECK(cats.size() == 1);
    if (!cats.empty()) {
        CHECK(cats[0].id == "clock");
        CHECK(cats[0].items.size() == 4);
    }
    edit::clearDynamicExtensions();
    CHECK(edit::findBlock("clock_now") == nullptr);
}

void test_bootstrap() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() /
                         ("xyo-ext-test-" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "clock", ec);
    {
        std::ofstream out(dir / "clock" / "manifest.json");
        CHECK(static_cast<bool>(out));
        out << kManifest;
    }
    const std::string list = dir.string();
#ifdef _WIN32
    _putenv_s("XYO_EXTENSION_PATH", list.c_str());
#else
    setenv("XYO_EXTENSION_PATH", list.c_str(), 1);
#endif

    ExtensionRegistry registry;
    Runtime rt;
    const ExtensionLoadReport report =
        bootstrapExtensionsFromEnvironment(rt, registry);
    CHECK(report.loaded.size() == 1);
    CHECK(report.errors.empty());
    CHECK(registry.find("clock") != nullptr);
    CHECK(rt.getOpcodeFunction("clock_now") != nullptr);
    CHECK(rt.getIsHat("clock_ready"));

#ifdef _WIN32
    _putenv_s("XYO_EXTENSION_PATH", "");
#else
    unsetenv("XYO_EXTENSION_PATH");
#endif
    fs::remove_all(dir, ec);
}

void test_extension_storage_roundtrip() {
    const char* project = R"JSON({
      "targets": [
        {"isStage": true, "name": "Stage", "variables": {}, "lists": {},
         "broadcasts": {}, "blocks": {}, "costumes": [], "sounds": [],
         "currentCostume": 0, "layerOrder": 0,
         "extensionStorage": {"clock": {"local": 5}}},
        {"isStage": false, "name": "Sprite1", "variables": {}, "lists": {},
         "broadcasts": {}, "blocks": {}, "costumes": [], "sounds": [],
         "currentCostume": 0, "layerOrder": 1,
         "extensionStorage": {"clock": {"local": 4}}}
      ],
      "monitors": [], "extensions": ["clock"],
      "extensionURLs": {"clock": "https://example.invalid/clock.js"},
      "extensionStorage": {"clock": {"global": 3}},
      "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
    })JSON";

    Runtime rt;
    Sb3LoadResult loaded = loadSb3(rt, project, nullptr);
    CHECK(loaded.ok);
    CHECK(rt.extensionStorage.count("clock") == 1);
    CHECK(rt.extensionStorage["clock"].get("global").asNumber() == 3);
    CHECK(rt.extensionURLs["clock"] == "https://example.invalid/clock.js");
    Target* sprite = nullptr;
    for (Target* t : rt.targets)
        if (!t->isStage) sprite = t;
    CHECK(sprite != nullptr);
    if (sprite) {
        CHECK(sprite->extensionStorage.count("clock") == 1);
        CHECK(sprite->extensionStorage["clock"].get("local").asNumber() == 4);
    }

    Sb3SaveResult saved = saveSb3(rt);
    CHECK(saved.ok);
    ZipReader zip;
    CHECK(ZipReader::open(saved.data.data(), saved.data.size(), zip));
    const ZipEntry* entry = zip.find("project.json");
    CHECK(entry != nullptr);
    std::vector<uint8_t> bytes;
    CHECK(entry && zip.extract(*entry, bytes));
    JsonValue root = jsonParse(std::string(bytes.begin(), bytes.end()));
    CHECK(root.get("extensionStorage").get("clock").get("global").asNumber() == 3);
    CHECK(root.get("extensionURLs").get("clock").asString() ==
          "https://example.invalid/clock.js");
    bool foundSprite = false;
    for (const JsonValue& t : root.get("targets").asArray()) {
        if (t.get("name").asString() != "Sprite1") continue;
        foundSprite = true;
        CHECK(t.get("extensionStorage").get("clock").get("local").asNumber() ==
              4);
    }
    CHECK(foundSprite);
}


void test_wasm_extension() {
#if defined(XYO_WASM_EXT) && defined(XYO_SOURCE_DIR)
    CHECK(wasmExtensionProviderAvailable());
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::path(XYO_SOURCE_DIR) / "examples" / "extensions" / "wasm_demo";
    ExtensionDef def;
    std::string error;
    CHECK(loadExtensionManifestFile((dir / "manifest.json").string(), &def,
                                    &error));
    CHECK(def.provider == "wasm");
    CHECK(def.blocks.size() == 5);

    const char* project = R"JSON({
      "targets": [
        {"isStage": true, "name": "Stage",
         "variables": {"v": ["v", 0], "s": ["s", ""], "t": ["t", ""],
                       "w": ["w", 0]},
         "lists": {}, "broadcasts": {}, "blocks": {}, "costumes": [],
         "sounds": [], "currentCostume": 0, "layerOrder": 0},
        {"isStage": false, "name": "Sprite1",
         "variables": {}, "lists": {}, "broadcasts": {},
         "blocks": {
           "hat": {"opcode": "event_whenflagclicked", "next": "setv1",
                   "parent": null, "inputs": {}, "fields": {},
                   "topLevel": true, "shadow": false, "x": 0, "y": 0},
           "setv1": {"opcode": "data_setvariableto", "next": "setv2",
                     "parent": "hat", "inputs": {"VALUE": [2, "dbl"]},
                     "fields": {"VARIABLE": ["v", "v"]}},
           "dbl": {"opcode": "wasmdemo_double", "next": null,
                   "parent": "setv1", "inputs": {"NUM": [1, [4, "21"]]},
                   "fields": {}, "shadow": false},
           "setv2": {"opcode": "data_setvariableto", "next": "sets",
                     "parent": "setv1", "inputs": {"VALUE": [2, "add"]},
                     "fields": {"VARIABLE": ["v", "v"]}},
           "add": {"opcode": "wasmdemo_add", "next": null, "parent": "setv2",
                   "inputs": {"A": [1, [4, "1"]], "B": [1, [4, "2"]]},
                   "fields": {}, "shadow": false},
           "sets": {"opcode": "data_setvariableto", "next": "sett",
                    "parent": "setv2", "inputs": {"VALUE": [2, "hello"]},
                    "fields": {"VARIABLE": ["s", "s"]}},
           "hello": {"opcode": "wasmdemo_hello", "next": null,
                     "parent": "sets", "inputs": {}, "fields": {},
                     "shadow": false},
           "sett": {"opcode": "data_setvariableto", "next": "setw",
                    "parent": "sets", "inputs": {"VALUE": [2, "echo"]},
                    "fields": {"VARIABLE": ["t", "t"]}},
           "echo": {"opcode": "wasmdemo_echo", "next": null, "parent": "sett",
                    "inputs": {"TEXT": [1, [10, "hi"]]}, "fields": {},
                    "shadow": false},
           "setw": {"opcode": "data_setvariableto", "next": null,
                    "parent": "sett", "inputs": {"VALUE": [2, "sum"]},
                    "fields": {"VARIABLE": ["w", "w"]}},
           "sum": {"opcode": "wasmdemo_sumcount", "next": null,
                   "parent": "setw", "inputs": {"N": [1, [4, "5"]]},
                   "fields": {}, "shadow": false}
         },
         "costumes": [], "sounds": [], "currentCostume": 0,
         "x": 0, "y": 0, "size": 100, "direction": 90,
         "rotationStyle": "all around", "visible": true, "layerOrder": 1}
      ],
      "monitors": [], "extensions": ["wasmdemo"],
      "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
    })JSON";

    for (JitMode mode : {JitMode::Off, JitMode::Eager}) {
        setJitMode(mode);
        ExtensionRegistry registry;
        Runtime rt;
        error.clear();
        const bool wasmOk = registerWasmExtension(
            rt, registry, def, (dir / "ext.wasm").string(), &error);
        if (!wasmOk) std::printf("wasm register: %s\n", error.c_str());
        CHECK(wasmOk);
        CHECK(registry.find("wasmdemo") != nullptr);
        Sb3LoadResult loaded = loadSb3(rt, project, nullptr);
        CHECK(loaded.ok);
        rt.greenFlag();
        rt.runUntilDone(100000);

        Target* stage = rt.getTargetForStage();
        Variable* v = stage ? stage->lookupVariableById("v") : nullptr;
        Variable* s = stage ? stage->lookupVariableById("s") : nullptr;
        CHECK(v != nullptr);
        if (v) {
            const double got = cast::toNumber(v->value);
            if (got != 3.0) std::printf("wasm v=%g\n", got);
            CHECK(got == 3.0);
        }
        CHECK(s != nullptr);
        if (s) {
            SStr* str = cast::toString(s->value);
            CHECK(str != nullptr);
            if (str) CHECK(SStr::toUtf8(str->view()) == "hello from wasm");
        }
        Variable* t = stage ? stage->lookupVariableById("t") : nullptr;
        CHECK(t != nullptr);
        if (t) {
            SStr* str = cast::toString(t->value);
            CHECK(str != nullptr);
            if (str) CHECK(SStr::toUtf8(str->view()) == "hi");
        }
        Variable* w = stage ? stage->lookupVariableById("w") : nullptr;
        CHECK(w != nullptr);
        if (w) CHECK(cast::toNumber(w->value) == 6.0);
    }

    ExtensionRegistry missingRegistry;
    Runtime missingRuntime;
    error.clear();
    CHECK(!registerWasmExtension(missingRuntime, missingRegistry, def,
                                 (dir / "missing.wasm").string(), &error));
    CHECK(!error.empty());
#else
    CHECK(!wasmExtensionProviderAvailable());
#endif
}


void test_wasm_gas_guard() {
#if defined(XYO_WASM_EXT) && defined(XYO_SOURCE_DIR)
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::path(XYO_SOURCE_DIR) / "tests" / "fixtures" / "extensions" / "spin";
    ExtensionDef def;
    std::string error;
    CHECK(loadExtensionManifestFile((dir / "manifest.json").string(), &def,
                                    &error));

    const char* project = R"JSON({
      "targets": [
        {"isStage": true, "name": "Stage",
         "variables": {"v": ["v", 0]}, "lists": {}, "broadcasts": {},
         "blocks": {}, "costumes": [], "sounds": [], "currentCostume": 0,
         "layerOrder": 0},
        {"isStage": false, "name": "S", "variables": {}, "lists": {},
         "broadcasts": {}, "blocks": {
           "hat": {"opcode": "event_whenflagclicked", "next": "spin",
                   "parent": null, "inputs": {}, "fields": {},
                   "topLevel": true, "shadow": false, "x": 0, "y": 0},
           "spin": {"opcode": "spin_loop", "next": "set", "parent": "hat",
                    "inputs": {}, "fields": {}, "shadow": false},
           "set": {"opcode": "data_setvariableto", "next": null,
                   "parent": "spin", "inputs": {"VALUE": [1, [4, "1"]]},
                   "fields": {"VARIABLE": ["v", "v"]}}
         }, "costumes": [], "sounds": [], "currentCostume": 0,
         "x": 0, "y": 0, "size": 100, "direction": 90,
         "rotationStyle": "all around", "visible": true, "layerOrder": 1}
      ],
      "monitors": [], "extensions": ["spin"],
      "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": ""}
    })JSON";

    setJitMode(JitMode::Eager);
    ExtensionRegistry registry;
    Runtime rt;
    CHECK(registerWasmExtension(rt, registry, def,
                                (dir / "ext.wasm").string(), &error));
    Sb3LoadResult loaded = loadSb3(rt, project, nullptr);
    CHECK(loaded.ok);
    rt.greenFlag();
    rt.runUntilDone(100000);
    Target* stage = rt.getTargetForStage();
    Variable* v = stage ? stage->lookupVariableById("v") : nullptr;
    CHECK(v != nullptr);
    if (v) CHECK(cast::toNumber(v->value) == 1.0);
#endif
}

} // namespace

int main() {
    test_registry_validation();
    test_manifest_parse();
    test_dynamic_catalog();
    test_bootstrap();
    test_extension_storage_roundtrip();
    test_wasm_extension();
    test_wasm_gas_guard();

    for (JitMode mode : {JitMode::Off, JitMode::Eager}) {
        bool loaded = false;
        const double v = runScenario(mode, &loaded);
        CHECK(loaded);
        CHECK(v == 15.0);
    }

    setJitMode(JitMode::HotSpot);
    std::printf("extensions: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
