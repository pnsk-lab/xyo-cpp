// xyo-cpp: sb3 minify のテスト。
//   - 既定（safe）は可逆: 未使用アセットだけ落とし、ブロック/変数/コメントは残す
//   - aggressive は未使用変数も落とす（monitor が参照する変数は残る）
//   - ID 短縮後もブロック参照グラフが壊れない
//   - 出力 zip は deflate され、ロードして実行できる
#include <cstdio>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/json.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "load/sb3_minify.hpp"
#include "load/zip.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; \
    if (!((a) == (b))) { ++g_fail; \
    std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

namespace {

const char* kAssetA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.svg";
const char* kAssetB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.png";
const char* kAssetC = "cccccccccccccccccccccccccccccccc.wav";
const char* kAssetDead = "dddddddddddddddddddddddddddddddd.png";
const char* kAssetFont = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee.ttf";

const char* kProject = R"JSON({
  "targets": [
    {
      "isStage": true, "name": "Stage",
      "variables": {
        "used": ["score", 0],
        "dead": ["dead var", 0],
        "watched": ["watched var", 0],
        "standalone": ["standalone var", 0]
      },
      "lists": {"lst": ["my list", [1, 2, 3]]},
      "broadcasts": {"bc": "go"},
      "blocks": {}, "comments": {},
      "costumes": [{"assetId": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                    "name": "backdrop", "md5ext":
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.svg",
                    "dataFormat": "svg", "bitmapResolution": 1,
                    "rotationCenterX": 0, "rotationCenterY": 0}],
      "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 0
    },
    {
      "isStage": false, "name": "Sprite1",
      "variables": {"local": ["score", 0]},
      "lists": {}, "broadcasts": {},
      "blocks": {
        "hat": {"opcode": "event_whenflagclicked", "next": "set",
                "parent": null, "inputs": {}, "fields": {},
                "topLevel": true, "shadow": false, "x": 0, "y": 0},
        "set": {"opcode": "data_setvariableto", "next": null,
                "parent": "hat",
                "inputs": {"VALUE": [1, [10, "42"]]},
                "fields": {"VARIABLE": ["score", "used"]},
                "topLevel": false, "shadow": false},
        "orphan": {"opcode": "motion_movesteps", "next": null,
                   "parent": null,
                   "inputs": {"STEPS": [1, [4, "10"]]},
                   "fields": {}, "topLevel": false, "shadow": false},
        "varget": [12, "score", "used", 100, 100],
        "varget2": [12, "standalone var", "standalone", 50, 50]
      },
      "comments": {
        "c0": {"blockId": null, "x": 10, "y": 10, "width": 200,
               "height": 100, "minimized": false, "text": "top-level"},
        "c1": {"blockId": "hat", "x": 0, "y": 0, "width": 200,
               "height": 100, "minimized": false, "text": "keep"},
        "c2": {"blockId": "orphan", "x": 0, "y": 0, "width": 200,
               "height": 100, "minimized": false, "text": "orphan"}
      },
      "costumes": [{"assetId": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                    "name": "c1", "md5ext":
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.png",
                    "dataFormat": "png", "bitmapResolution": 1,
                    "rotationCenterX": 0, "rotationCenterY": 0}],
      "sounds": [{"assetId": "cccccccccccccccccccccccccccccccc",
                  "name": "s1", "md5ext":
                  "cccccccccccccccccccccccccccccccc.wav",
                  "dataFormat": "wav", "format": "",
                  "rate": 44100, "sampleCount": 4}],
      "currentCostume": 0, "volume": 100, "layerOrder": 1,
      "visible": true, "x": 0, "y": 0, "size": 100, "direction": 90,
      "draggable": false, "rotationStyle": "all around"
    }
  ],
  "monitors": [
    {"id": "watched", "mode": "default", "opcode": "data_variable",
     "params": {"VARIABLE": "watched var"}, "spriteName": "Stage",
     "value": 0, "width": 0, "height": 0, "x": 5, "y": 5,
     "visible": true, "sliderMin": 0, "sliderMax": 100,
     "isDiscrete": true},
    {"id": "itemmon", "mode": "default", "opcode": "data_itemoflist",
     "params": {"LIST": "my list"}, "spriteName": "Stage",
     "value": 0, "width": 0, "height": 0, "x": 5, "y": 5,
     "visible": true, "sliderMin": 0, "sliderMax": 100,
     "isDiscrete": true}
  ],
  "customFonts": [
    {"system": false, "family": "Test Font", "fallback": "sans-serif",
     "md5ext": "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee.ttf"}
  ],
  "extensions": [],
  "meta": {"semver": "3.0.0", "vm": "0.2.0", "agent": "test"}
})JSON";

const char* kSensingProject = R"JSON({
  "targets": [
    {
      "isStage": true, "name": "Stage",
      "variables": {
        "v1": ["target var", 0],
        "dead2": ["nobody reads me", 0]
      },
      "lists": {}, "broadcasts": {}, "blocks": {}, "comments": {},
      "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 0
    },
    {
      "isStage": false, "name": "Sprite1",
      "variables": {}, "lists": {}, "broadcasts": {},
      "blocks": {
        "hat": {"opcode": "event_whenflagclicked", "next": "sensing",
                "parent": null, "inputs": {}, "fields": {},
                "topLevel": true, "shadow": false, "x": 0, "y": 0},
        "sensing": {"opcode": "sensing_of", "next": null,
                    "parent": "hat",
                    "inputs": {"OBJECT": [1, "obj"]},
                    "fields": {"PROPERTY": ["target var"]},
                    "topLevel": false, "shadow": false},
        "obj": {"opcode": "sensing_of_object_menu", "next": null,
                "parent": "sensing", "inputs": {},
                "fields": {"OBJECT": ["Stage", null]},
                "topLevel": false, "shadow": true}
      },
      "comments": {},
      "costumes": [], "sounds": [],
      "currentCostume": 0, "volume": 100, "layerOrder": 1,
      "visible": true, "x": 0, "y": 0, "size": 100, "direction": 90,
      "draggable": false, "rotationStyle": "all around"
    }
  ],
  "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}
})JSON";

std::vector<uint8_t> zipWithProject(const char* json) {
    ZipWriter zw;
    const std::string s = json;
    zw.addFile("project.json", std::vector<uint8_t>(s.begin(), s.end()));
    return zw.finish();
}

std::vector<uint8_t> makeSb3() {
    ZipWriter zw;
    const std::string json = kProject;
    zw.addFile("project.json",
               std::vector<uint8_t>(json.begin(), json.end()));
    auto add = [&](const char* name, const std::string& body) {
        zw.addFile(name, std::vector<uint8_t>(body.begin(), body.end()));
    };
    add(kAssetA, "<svg xmlns='http://www.w3.org/2000/svg'/>");
    add(kAssetB, std::string("\x89PNG\r\n\x1a\n", 8) + "sprite");
    add(kAssetC, std::string("RIFF....WAVEfmt ", 16));
    // 同じ内容の重複エントリ（dedupe 対象）
    add(kAssetB, std::string("\x89PNG\r\n\x1a\n", 8) + "sprite");
    // どこからも参照されない md5 名アセット（drop 対象）
    add(kAssetDead, std::string("\x89PNG\r\n\x1a\n", 8) + "dead");
    // customFonts[].md5ext だけが参照するフォント（残る）
    add(kAssetFont, "font-bytes");
    // md5 名でない未知のエントリは残す
    add("notes.txt", "unknown metadata");
    return zw.finish();
}

std::string projectJsonOf(const std::vector<uint8_t>& zipData) {
    ZipReader z;
    if (!ZipReader::open(zipData.data(), zipData.size(), z)) return {};
    const ZipEntry* e = z.find("project.json");
    if (!e) return {};
    std::vector<uint8_t> data;
    if (!z.extract(*e, data)) return {};
    return std::string(data.begin(), data.end());
}

size_t blockRefsMissing(const JsonValue& root) {
    size_t missing = 0;
    for (const JsonValue& tv : root.get("targets").asArray()) {
        const JsonObject& blocks = tv.get("blocks").asObject();
        for (const auto& [id, b] : blocks) {
            if (!b.isObject()) continue;
            for (const char* key : {"next", "parent"}) {
                const std::string& r = b.get(key).asString();
                if (!r.empty() && !blocks.count(r)) ++missing;
            }
            for (const auto& [in, desc] : b.get("inputs").asObject()) {
                (void)in;
                if (!desc.isArray()) continue;
                const JsonArray& a = desc.asArray();
                for (size_t i = 1; i < a.size(); ++i) {
                    if (a[i].isString() && !a[i].asString().empty() &&
                        !blocks.count(a[i].asString()))
                        ++missing;
                }
            }
        }
    }
    return missing;
}

void test_safe() {
    const std::vector<uint8_t> in = makeSb3();
    Sb3MinifyResult r = minifySb3(in.data(), in.size());
    CHECK(r.ok);
    CHECK(r.stats.outputBytes < r.stats.inputBytes);
    CHECK_EQ(r.stats.assetsDropped, 1u);
    CHECK_EQ(r.stats.assetsDeduped, 1u);
    CHECK_EQ(r.stats.assetsAfter, 5u);   // A, B, C, font, notes.txt

    ZipReader z;
    CHECK(ZipReader::open(r.data.data(), r.data.size(), z));
    CHECK(z.find(kAssetA) != nullptr);
    CHECK(z.find(kAssetB) != nullptr);
    CHECK(z.find(kAssetC) != nullptr);
    CHECK(z.find(kAssetDead) == nullptr);
    CHECK(z.find(kAssetFont) != nullptr);   // customFonts 参照は残す
    CHECK(z.find("notes.txt") != nullptr);   // 未知エントリは残す
    const ZipEntry* pj = z.find("project.json");
    CHECK(pj != nullptr);
    CHECK_EQ(pj ? pj->method : 0, 8u);       // deflate

    const std::string json = projectJsonOf(r.data);
    JsonValue root = jsonParse(json);
    CHECK(root.isObject());
    CHECK_EQ(blockRefsMissing(root), 0u);
    CHECK_EQ(root.get("targets").size(), 2u);

    const JsonValue& stage = root.get("targets").at(0);
    // safe では未使用変数も残る
    CHECK(stage.get("variables").has("dead"));
    CHECK(stage.get("variables").has("used"));
    CHECK(stage.get("variables").has("watched"));
    CHECK(stage.get("variables").has("standalone"));
    CHECK(stage.get("lists").has("lst"));

    const JsonValue& sprite = root.get("targets").at(1);
    CHECK(sprite.get("variables").has("local"));
    const JsonObject& blocks = sprite.get("blocks").asObject();
    // 到達不能ブロック（とそれに付いたコメント）は既定で落ちる。
    // 未使用変数は aggressive でのみ落ちる。
    CHECK_EQ(blocks.size(), 4u);             // hat/set + 配列 reporter x2
    bool foundOrphan = false;
    for (const auto& [id, b] : blocks) {
        (void)id;
        if (b.get("opcode").asString() == "motion_movesteps")
            foundOrphan = true;
    }
    CHECK(!foundOrphan);
    bool foundVarArray = false;
    for (const auto& [id, b] : blocks) {
        (void)id;
        if (b.isArray() && b.at(2).asString() == "used") foundVarArray = true;
    }
    CHECK(foundVarArray);   // 配列型の変数 reporter は残り、変数 ID も不変
    CHECK_EQ(sprite.get("comments").size(), 2u);   // トップレベル + hat

    // ID 短縮: トップレベルの hat は短い ID になっている
    bool topShort = false;
    for (const auto& [id, b] : blocks) {
        if (b.get("topLevel").asBool()) {
            topShort = id.size() <= 2;
            const std::string next = b.get("next").asString();
            CHECK(!next.empty() && blocks.count(next));
        }
    }
    CHECK(topShort);
    // 変数 ID は短縮しない（fields の id がそのまま）
    for (const auto& [id, b] : blocks) {
        (void)id;
        const JsonArray& f = b.get("fields").get("VARIABLE").asArray();
        if (f.size() >= 2) CHECK_EQ(f[1].asString(), std::string("used"));
    }

    // ロードして実行できる（score = 42）
    Runtime rt;
    Sb3LoadResult lr = loadSb3File(rt, r.data.data(), r.data.size());
    CHECK(lr.ok);
    if (!lr.ok) {
        std::printf("  load error: %s\n", lr.error.c_str());
        return;
    }
    Target* stageT = rt.getTargetForStage();
    CHECK(stageT != nullptr);
    rt.greenFlag();
    rt.runUntilDone(10000);
    Variable* score = stageT ? stageT->lookupVariableById("used") : nullptr;
    CHECK(score != nullptr);
    if (score) CHECK_EQ(cast::toNumber(score->value), 42.0);
}

void test_aggressive() {
    const std::vector<uint8_t> in = makeSb3();
    Sb3MinifyOptions opt;
    opt.dropUnusedVariables = true;
    opt.stripComments = true;
    Sb3MinifyResult r = minifySb3(in.data(), in.size(), opt);
    CHECK(r.ok);
    const JsonValue root = jsonParse(projectJsonOf(r.data));
    CHECK(root.isObject());
    const JsonValue& stage = root.get("targets").at(0);
    CHECK(stage.get("variables").has("used"));
    CHECK(!stage.get("variables").has("dead"));
    // monitor が参照する変数は「使用中」として残す
    CHECK(stage.get("variables").has("watched"));
    // トップレベルの変数 reporter（配列）だけが参照する変数も残す
    CHECK(stage.get("variables").has("standalone"));
    // item-of-list モニタは params.LIST の名前でリストを参照する
    CHECK(stage.get("lists").has("lst"));

    const JsonValue& sprite = root.get("targets").at(1);
    // ステージ変数と同名でも、id が参照されていないローカル変数は落とす
    // （deps::analyzeScript の解決済み Variable* による判定）
    CHECK(!sprite.get("variables").has("local"));
    const JsonObject& blocks = sprite.get("blocks").asObject();
    CHECK_EQ(blocks.size(), 4u);             // hat/set + 配列 reporter x2
    CHECK_EQ(sprite.get("comments").size(), 0u);
    CHECK_EQ(blockRefsMissing(root), 0u);
}

void test_sensing_of_fallback() {
    // sensing_of は任意の変数を動的に読むため、deps 解析は varDynamic を
    // 立てる。minify はその場合だけ従来の文字列走査へフォールバックし、
    // PROPERTY に書かれた変数名を残す。
    const std::vector<uint8_t> in = zipWithProject(kSensingProject);
    Sb3MinifyOptions opt;
    opt.dropUnusedVariables = true;
    Sb3MinifyResult r = minifySb3(in.data(), in.size(), opt);
    CHECK(r.ok);
    const JsonValue root = jsonParse(projectJsonOf(r.data));
    const JsonValue& vars = root.get("targets").at(0).get("variables");
    CHECK(vars.has("v1"));
    CHECK(!vars.has("dead2"));
}

void test_determinism() {
    const std::vector<uint8_t> in = makeSb3();
    Sb3MinifyResult a = minifySb3(in.data(), in.size());
    Sb3MinifyResult b = minifySb3(in.data(), in.size());
    CHECK(a.ok && b.ok);
    CHECK(a.data == b.data);
    // 2 回目も意味論は同じ（サイズは再割当てで僅かに変わり得る）
    Sb3MinifyResult c = minifySb3(a.data.data(), a.data.size());
    CHECK(c.ok);
    const JsonValue root = jsonParse(projectJsonOf(c.data));
    CHECK(root.isObject());
    CHECK_EQ(blockRefsMissing(root), 0u);
}

void test_keep_options() {
    const std::vector<uint8_t> in = makeSb3();
    Sb3MinifyOptions opt;
    opt.compress = false;
    opt.dropUnusedAssets = false;
    opt.dropOrphanBlocks = false;
    opt.compactIds = false;
    Sb3MinifyResult r = minifySb3(in.data(), in.size(), opt);
    CHECK(r.ok);
    ZipReader z;
    CHECK(ZipReader::open(r.data.data(), r.data.size(), z));
    const ZipEntry* pj = z.find("project.json");
    CHECK(pj != nullptr);
    CHECK_EQ(pj ? pj->method : 8u, 0u);      // --store
    CHECK(z.find(kAssetDead) != nullptr);
    const JsonValue root = jsonParse(projectJsonOf(r.data));
    const JsonObject& blocks =
        root.get("targets").at(1).get("blocks").asObject();
    CHECK_EQ(blocks.size(), 5u);             // hat/set/orphan + 配列 reporter x2
    CHECK(blocks.count("hat") == 1);         // compactIds=false で ID 不変
    CHECK(blocks.count("orphan") == 1);
}

void test_errors() {
    const uint8_t junk[] = {1, 2, 3, 4};
    Sb3MinifyResult r = minifySb3(junk, sizeof(junk));
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

} // namespace

int test_minify_main() {
    test_safe();
    test_aggressive();
    test_sensing_of_fallback();
    test_determinism();
    test_keep_options();
    test_errors();
    std::printf("minify: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
