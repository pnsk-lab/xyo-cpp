// xyo-cpp: sb3 書き換え（xyo rewrite）のテスト。
//   - yield しないループ入り手続きが warp 化される
//   - yield する手続き / 短い手続きは warp 化されない
//   - `set t = <純粋>` 直後が `if <t ...>` で t の読み書きが各 1 回なら
//     条件へインライン化され、set が消える
//   - 2 回読まれる変数はインライン化されない
//   - 書き換え前後で実行結果（変数）が一致する
#include <cstdio>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/json.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "load/sb3_rewrite.hpp"
#include "load/zip.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

using namespace xyo;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

std::string block(const std::string& id, const std::string& opcode,
                  const std::string& next, const std::string& parent,
                  const std::string& inputs, const std::string& fields,
                  bool topLevel = false) {
    std::string s = "{\"opcode\": \"" + opcode + "\", \"next\": ";
    s += next.empty() ? "null" : "\"" + next + "\"";
    s += ", \"parent\": ";
    s += parent.empty() ? "null" : "\"" + parent + "\"";
    s += ", \"inputs\": " + (inputs.empty() ? std::string("{}") : inputs) +
         ", \"fields\": {" + fields +
         "}, \"topLevel\": " + (topLevel ? "true" : "false") +
         ", \"shadow\": false";
    if (topLevel) s += ", \"x\": 0, \"y\": 0";
    s += "}";
    return s;
}

std::string procDef(const std::string& defId, const std::string& protoId,
                    const std::string& bodyFirst, const std::string& proccode,
                    const std::string& tail) {
    std::string s = "\"" + defId +
        "\": {\"opcode\": \"procedures_definition\", \"next\": \"" + bodyFirst +
        "\", \"parent\": null, \"inputs\": {\"custom_block\": [1, \"" + protoId +
        "\"]}, \"fields\": {}, \"topLevel\": true, \"shadow\": false,"
        " \"x\": 0, \"y\": 400},\n";
    s += "\"" + protoId +
        "\": {\"opcode\": \"procedures_prototype\", \"next\": null, \"parent\": \"" +
        defId + "\", \"inputs\": {}, \"fields\": {}, \"topLevel\": false,"
        " \"shadow\": true, \"mutation\": {\"tagName\": \"mutation\","
        " \"children\": [], \"proccode\": \"" + proccode +
        "\", \"argumentids\": \"[]\", \"argumentnames\": \"[]\","
        " \"argumentdefaults\": \"[]\", \"warp\": \"false\"}},\n";
    return s + tail;
}

/// loop を 1 つ含む手続き（yield あり/なし・ブロック数違いを切り替え）
std::string projectJson(bool longLoop, bool yields, bool twoReads = false) {
    const std::string listId = "lst";
    std::string body;
    // ループ本体: 40 個の `change v by 1` か、短い本体
    std::string inner;
    const int n = longLoop ? 40 : 2;
    for (int i = 0; i < n; ++i) {
        const std::string id = "inc" + std::to_string(i);
        const std::string nx = i + 1 < n ? "inc" + std::to_string(i + 1) : "";
        inner += "\"" + id + "\": " +
                 block(id, "data_changevariableby", nx, i == 0 ? "loop" : "inc" + std::to_string(i - 1),
                       "{\"VALUE\": [1, [4, \"1\"]]}", "\"VARIABLE\": [\"v\", \"vv\"]") +
                 ",\n";
    }
    const std::string loopFirst = n > 0 ? "inc0" : "";
    std::string loopInputs = "{\"TIMES\": [1, [6, \"1\"]]";
    if (!loopFirst.empty()) loopInputs += ", \"SUBSTACK\": [2, \"" + loopFirst + "\"]";
    if (yields) {
        // ループ内に wait（= yield するので warp 化してはいけない）
        inner += "\"w\": " + block("w", "control_wait", loopFirst, "loop",
                                   "{\"DURATION\": [1, [4, \"0.01\"]]}", "") + ",\n";
        loopInputs = "{\"TIMES\": [1, [6, \"1\"]], \"SUBSTACK\": [2, \"w\"]";
    }
    loopInputs += "}";
    // 旗 → 手続き呼出し（p を実際に走らせる）
    body += "\"hat\": {\"opcode\": \"event_whenflagclicked\", \"next\": \"callp\","
            " \"parent\": null, \"inputs\": {}, \"fields\": {},"
            " \"topLevel\": true, \"shadow\": false, \"x\": 0, \"y\": 0},\n";
    body += "\"callp\": {\"opcode\": \"procedures_call\", \"next\": null,"
            " \"parent\": \"hat\", \"inputs\": {}, \"fields\": {},"
            " \"topLevel\": false, \"shadow\": false,"
            " \"mutation\": {\"tagName\": \"mutation\", \"children\": [],"
            " \"proccode\": \"p\", \"argumentids\": \"[]\","
            " \"argumentnames\": \"[]\", \"argumentdefaults\": \"[]\","
            " \"warp\": \"false\"}},\n";
    body += procDef("pdef", "pproto", "set", "p", "");
    body += "\"set\": " + block("set", "data_setvariableto", "cond", "pdef",
                                "{\"VALUE\": [1, [4, \"7\"]]}",
                                "\"VARIABLE\": [\"t\", \"tt\"]") + ",\n";
    body += "\"cond\": " + block("cond", "control_if", "loop", "set",
                                 "{\"CONDITION\": [2, \"cmp\"],"
                                 " \"SUBSTACK\": [2, \"ifbody\"]}",
                                 "") + ",\n";
    // 注: control_if は CONDITION と SUBSTACK の 2 入力を同時に持つ
    body += "\"cmp\": " + block("cmp", "operator_equals", "", "cond",
                                "{\"OPERAND1\": [3, [12, \"t\", \"tt\"], [10, \"\"]],"
                                " \"OPERAND2\": [1, [4, \"7\"]]}", "") + ",\n";
    body += "\"ifbody\": " + block("ifbody", "data_changevariableby", "", "cond",
                                   "{\"VALUE\": [1, [4, \"1\"]]}",
                                   "\"VARIABLE\": [\"out\", \"vvout\"]") + ",\n";
    if (twoReads) {
        // t をもう 1 回読む（opcode 直下なので total reads は 2 回）
        body += "\"cmp2\": " + block("cmp2", "operator_equals", "", "cond",
                                     "{\"OPERAND1\": [3, [12, \"t\", \"tt\"], [10, \"\"]],"
                                     " \"OPERAND2\": [1, [4, \"0\"]]}", "") + ",\n";
    }
    body += "\"loop\": " + block("loop", "control_repeat", "", "cond", loopInputs, "") + ",\n";
    body += inner;
    while (!body.empty() && (body.back() == '\n' || body.back() == ' '))
        body.pop_back();
    if (!body.empty() && body.back() == ',') body.pop_back();

    std::string prog =
        "{\"targets\": ["
        "{\"isStage\": true, \"name\": \"Stage\", \"variables\": {}, \"lists\": {},"
        " \"broadcasts\": {}, \"blocks\": {}, \"comments\": {}, \"costumes\": [],"
        " \"sounds\": [], \"currentCostume\": 0, \"volume\": 100, \"layerOrder\": 0},"
        "{\"isStage\": false, \"name\": \"S\", \"layerOrder\": 1,"
        " \"variables\": {\"vv\": [\"v\", 0], \"tt\": [\"t\", 0],"
        " \"vvout\": [\"out\", 0]}, \"lists\": {}, \"broadcasts\": {},"
        " \"blocks\": {" + body + "}, \"comments\": {}, \"costumes\": [],"
        " \"sounds\": [], \"currentCostume\": 0, \"volume\": 100,"
        " \"visible\": true, \"x\": 0, \"y\": 0, \"size\": 100,"
        " \"direction\": 90, \"draggable\": false,"
        " \"rotationStyle\": \"all around\"}],"
        " \"monitors\": [], \"extensions\": [],"
        " \"meta\": {\"semver\": \"3.0.0\"}}";
    return prog;
}

std::vector<uint8_t> makeSb3(const std::string& json) {
    ZipWriter zw;
    zw.addFile("project.json", std::vector<uint8_t>(json.begin(), json.end()));
    return zw.finish();
}

std::string extractJson(const std::vector<uint8_t>& sb3) {
    ZipReader zr;
    if (!ZipReader::open(sb3.data(), sb3.size(), zr)) return "";
    const ZipEntry* e = zr.find("project.json");
    if (!e) return "";
    std::vector<uint8_t> out;
    if (!zr.extract(*e, out)) return "";
    return std::string(out.begin(), out.end());
}

std::string runAndGetOut(const std::vector<uint8_t>& sb3) {
    Runtime rt;
    Sb3LoadResult lr = loadSb3File(rt, sb3.data(), sb3.size());
    if (!lr.ok) return "<load error>";
    rt.greenFlag();
    rt.runUntilDone(10000);
    Target* sp = rt.getSpriteTargetByName(u"S");
    if (!sp) return "<no sprite>";
    Variable* v = sp->lookupVariableById("vvout");
    return v ? SStr::toUtf8(cast::toString(v->value)) : "<no var>";
}

bool hasWarp(const std::string& json) {
    JsonValue root = jsonParse(json);
    for (const JsonValue& t : root.get("targets").asArray())
        for (const auto& [id, b] : t.get("blocks").asObject()) {
            (void)id;
            if (b.get("opcode").asString() == "procedures_prototype" &&
                b.get("mutation").get("warp").asString() == "true")
                return true;
        }
    return false;
}

}  // namespace

int test_rewrite_main() {
    // --- warp 化 ---------------------------------------------------------
    {
        const std::vector<uint8_t> sb3 = makeSb3(projectJson(true, false));
        Sb3RewriteOptions opt;
        Sb3RewriteResult r = rewriteSb3(sb3.data(), sb3.size(), opt);
        if (!r.ok) {
            std::printf("  warp: %s\n", r.error.c_str());
            const std::string j = projectJson(true, false);
            FILE* f = std::fopen("/tmp/test_rewrite.json", "wb");
            if (f) { std::fwrite(j.data(), 1, j.size(), f); std::fclose(f); }
        }
        CHECK(r.ok);
        CHECK(r.stats.procsWarped == 1);
        const std::string json = extractJson(r.data);
        CHECK(!json.empty());
        CHECK(hasWarp(json));
        // 書き換え前後の実行結果が一致する
        CHECK(runAndGetOut(sb3) == runAndGetOut(r.data));
    }
    // --- yield する手続きは warp 化しない --------------------------------
    {
        const std::vector<uint8_t> sb3 = makeSb3(projectJson(true, true));
        Sb3RewriteResult r = rewriteSb3(sb3.data(), sb3.size(), Sb3RewriteOptions{});
        CHECK(r.ok);
        CHECK(r.stats.procsWarped == 0);
        CHECK(!hasWarp(extractJson(r.data)));
    }
    // --- 短いループは warp 化しない --------------------------------------
    {
        const std::vector<uint8_t> sb3 = makeSb3(projectJson(false, false));
        Sb3RewriteResult r = rewriteSb3(sb3.data(), sb3.size(), Sb3RewriteOptions{});
        CHECK(r.ok);
        CHECK(r.stats.procsWarped == 0);
    }
    // --- 一時変数のインライン化 ------------------------------------------
    {
        const std::vector<uint8_t> sb3 = makeSb3(projectJson(false, false));
        Sb3RewriteResult r = rewriteSb3(sb3.data(), sb3.size(), Sb3RewriteOptions{});
        CHECK(r.ok);
        CHECK(r.stats.tempsInlined == 1);
        CHECK(r.stats.blocksAfter + 1 == r.stats.blocksBefore);
        const std::string json = extractJson(r.data);
        CHECK(json.find("\"set\"") == std::string::npos);   // set が消えた
        CHECK(runAndGetOut(sb3) == runAndGetOut(r.data));
        // 実行結果: t=7 で if が成立し out が 1 増える（ループ 1 回）
        if (runAndGetOut(r.data) != "1")
            std::printf("  out(orig)='%s' out(new)='%s'\n",
                        runAndGetOut(sb3).c_str(),
                        runAndGetOut(r.data).c_str());
        CHECK(runAndGetOut(r.data) == "1");
    }
    // --- 2 回読まれる変数はインライン化しない ----------------------------
    {
        const std::vector<uint8_t> sb3 = makeSb3(projectJson(false, false, true));
        Sb3RewriteResult r = rewriteSb3(sb3.data(), sb3.size(), Sb3RewriteOptions{});
        CHECK(r.ok);
        CHECK(r.stats.tempsInlined == 0);
        CHECK(runAndGetOut(sb3) == runAndGetOut(r.data));
    }
    std::printf("rewrite: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail;
}
