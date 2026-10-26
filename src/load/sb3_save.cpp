// xyo-cpp: sb3 シリアライザ実装。loader（sb3.cpp）と対をなす。
#include "sb3_save.hpp"

#include <set>
#include <cmath>

#include "../core/fileio.hpp"
#include "../core/json.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../engine/runtime.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "zip.hpp"

namespace xyo {

namespace {

// 非有限値は JSON に書けない（JSON.stringify も null にする）。
// 座標/サイズは既定値へ丸める。
static double fin(double d, double fallback) {
    return std::isfinite(d) ? d : fallback;
}

// 値 → JSON。NaN / undefined は空文字列（Scratch は保存時に NaN を持たない）。
JsonValue valueToJson(const Value& v) {
    if (v.isUndefined() || v.isNaN()) return JsonValue(std::string(""));
    if (v.isNumber()) {
        double d = v.asNumberUnchecked();
        // JSON に Infinity/NaN は無い。JSON.stringify と同じく null にする
        // （Scratch の変数としては 0 扱い。非有限値は描画/座標では使わない）。
        if (!std::isfinite(d)) return JsonValue(nullptr);
        return JsonValue(d);
    }
    if (v.isBool()) return JsonValue(v.asBool());
    if (v.isString()) {
        SStr* s = v.asStrPtr();
        return s ? JsonValue(SStr::toUtf8(s->view())) : JsonValue(std::string(""));
    }
    return JsonValue(std::string(""));
}

// opcode → 拡張 id（sb3.cpp の extensionIdForOpcode と同一ロジック）
std::string extForOpcode(const std::string& opcode) {
    size_t i = opcode.find('_');
    if (i == std::string::npos) return "";
    std::string prefix = opcode.substr(0, i);
    static const std::set<std::string> kCore = {
        "argument", "colour", "control", "data", "event", "looks",
        "math", "motion", "operator", "procedures", "sensing", "sound"};
    return kCore.count(prefix) ? "" : prefix;
}

JsonObject blockToJson(Block& b) {
    JsonObject o;
    o["opcode"] = JsonValue(b.opcode);
    o["next"] = b.next.empty() ? JsonValue(nullptr) : JsonValue(b.next);
    o["parent"] = b.parent.empty() ? JsonValue(nullptr) : JsonValue(b.parent);
    o["shadow"] = JsonValue(b.shadow);
    o["topLevel"] = JsonValue(b.topLevel);
    o["x"] = JsonValue(b.x);
    o["y"] = JsonValue(b.y);

    JsonObject inputs;
    for (const auto& [name, in] : b.inputs.items) {
        JsonArray arr;
        if (!in.block.empty() && in.block == in.shadow) {
            arr.push_back(JsonValue(1.0));       // INPUT_SAME_BLOCK_SHADOW
            arr.push_back(JsonValue(in.block));
        } else if (!in.block.empty() && !in.shadow.empty()) {
            arr.push_back(JsonValue(3.0));       // INPUT_DIFF_BLOCK_SHADOW
            arr.push_back(JsonValue(in.block));
            arr.push_back(JsonValue(in.shadow));
        } else if (!in.block.empty()) {
            arr.push_back(JsonValue(2.0));       // INPUT_BLOCK_NO_SHADOW
            arr.push_back(JsonValue(in.block));
        } else if (!in.shadow.empty()) {
            arr.push_back(JsonValue(1.0));
            arr.push_back(JsonValue(in.shadow));
        } else {
            continue;   // 空入力（確保だけされた SUBSTACK 等）は省略可
        }
        inputs[name] = JsonValue(std::move(arr));
    }
    o["inputs"] = JsonValue(std::move(inputs));

    JsonObject fields;
    for (const auto& [name, f] : b.fields) {
        JsonArray arr;
        arr.push_back(valueToJson(f.value));
        if (!f.id.empty()) arr.push_back(JsonValue(f.id));
        fields[name] = JsonValue(std::move(arr));
    }
    o["fields"] = JsonValue(std::move(fields));

    if (b.mutation) {
        JsonObject m;
        m["tagName"] = JsonValue(b.mutation->tagName);
        for (const auto& [k, v] : b.mutation->attrs) {
            // `children` だけは sb3 でも配列（[{tagName:"arg",...}]）。loader は
            // 生オブジェクトを JSON 文字列として保持するので、ここで戻す。
            // 他の属性（argumentids/argumentnames/argumentdefaults/warp/proccode）は
            // sb3 でも *文字列* なので絶対にパースしない。
            if (k == "children" && !v.empty() && (v[0] == '[' || v[0] == '{')) {
                JsonValue parsed = jsonParse(v);
                if (parsed.isArray() || parsed.isObject()) {
                    m[k] = std::move(parsed);
                    continue;
                }
            }
            m[k] = JsonValue(v);
        }
        o["mutation"] = JsonValue(std::move(m));
    }
    return o;
}

JsonObject targetToJson(Runtime& rt, Target& t,
                        std::vector<std::pair<std::string, std::vector<uint8_t>>>& assets) {
    JsonObject o;
    o["isStage"] = JsonValue(t.isStage);
    o["name"] = JsonValue(SStr::toUtf8(t.getName()));

    JsonObject vars, lists, broadcasts;
    for (const auto& [id, v] : t.variables) {
        if (v.type == Variable::kListType) {
            JsonArray items;
            if (v.list)
                for (uint32_t i = 0; i < v.list->len; ++i)
                    items.push_back(valueToJson(v.list->items[i]));
            JsonArray a;
            a.push_back(JsonValue(SStr::toUtf8(v.name)));
            a.push_back(JsonValue(std::move(items)));
            lists[id] = JsonValue(std::move(a));
        } else if (v.type == Variable::kBroadcastType) {
            broadcasts[id] = JsonValue(SStr::toUtf8(v.name));
        } else {
            JsonArray a;
            a.push_back(JsonValue(SStr::toUtf8(v.name)));
            a.push_back(valueToJson(v.value));
            if (v.isCloud) a.push_back(JsonValue(true));
            vars[id] = JsonValue(std::move(a));
        }
    }
    o["variables"] = JsonValue(std::move(vars));
    o["lists"] = JsonValue(std::move(lists));
    o["broadcasts"] = JsonValue(std::move(broadcasts));

    JsonObject blocks;
    if (t.blocks)
        for (auto& [id, b] : t.blocks->allBlocks())
            blocks[id] = JsonValue(blockToJson(b));
    o["blocks"] = JsonValue(std::move(blocks));
    // ブロックコメント（Text が入っているブロックのみ）
    JsonObject comments;
    if (t.blocks)
        for (const auto& [id, b] : t.blocks->allBlocks()) {
            if (b.comment.empty()) continue;
            JsonObject c;
            c["blockId"] = JsonValue(id);
            c["x"] = JsonValue(0.0);
            c["y"] = JsonValue(0.0);
            c["width"] = JsonValue(200.0);
            c["height"] = JsonValue(100.0);
            c["minimized"] = JsonValue(false);
            c["text"] = JsonValue(b.comment);
            comments[id] = JsonValue(std::move(c));
        }
    o["comments"] = JsonValue(std::move(comments));

    JsonArray costumes, sounds;
    if (t.sprite) {
        for (const auto& c : t.sprite->costumes) {
            JsonObject co;
            co["assetId"] = JsonValue(c.assetId);
            co["name"] = JsonValue(SStr::toUtf8(c.name));
            co["bitmapResolution"] = JsonValue(fin(c.bitmapResolution, 1.0));
            co["dataFormat"] = JsonValue(c.dataFormat.empty() ? "png" : c.dataFormat);
            co["rotationCenterX"] = JsonValue(fin(c.rotationCenterX, 0.0));
            co["rotationCenterY"] = JsonValue(fin(c.rotationCenterY, 0.0));
            std::string md5 = c.md5.empty()
                ? (c.assetId + "." + (c.dataFormat.empty() ? "png" : c.dataFormat))
                : c.md5;
            co["md5ext"] = JsonValue(md5);
            costumes.push_back(JsonValue(std::move(co)));
            if (!c.data.empty()) assets.emplace_back(md5, c.data);
        }
        for (const auto& s : t.sprite->sounds) {
            JsonObject so;
            so["assetId"] = JsonValue(s.assetId);
            so["name"] = JsonValue(SStr::toUtf8(s.name));
            so["dataFormat"] = JsonValue(s.dataFormat);
            so["format"] = JsonValue(s.format);
            so["rate"] = JsonValue(s.rate);
            so["sampleCount"] = JsonValue(s.sampleCount);
            std::string md5 = s.md5.empty() ? (s.assetId + "." + s.dataFormat) : s.md5;
            so["md5ext"] = JsonValue(md5);
            sounds.push_back(JsonValue(std::move(so)));
            if (!s.data.empty()) assets.emplace_back(md5, s.data);
        }
    }
    o["costumes"] = JsonValue(std::move(costumes));
    o["sounds"] = JsonValue(std::move(sounds));
    o["currentCostume"] = JsonValue(double(t.currentCostume));
    o["volume"] = JsonValue(fin(t.volume, 100.0));
    o["layerOrder"] = JsonValue(double(t.layerOrder));
    o["visible"] = JsonValue(t.visible);
    o["x"] = JsonValue(fin(t.x, 0.0));
    o["y"] = JsonValue(fin(t.y, 0.0));
    o["size"] = JsonValue(fin(t.size, 100.0));
    o["direction"] = JsonValue(fin(t.direction, 90.0));
    o["draggable"] = JsonValue(t.draggable);
    o["rotationStyle"] = JsonValue(t.rotationStyle);
    if (t.isStage) {
        o["tempo"] = JsonValue(fin(t.tempo, 60.0));
        o["videoTransparency"] = JsonValue(fin(t.videoTransparency, 50.0));
        o["videoState"] = JsonValue(t.videoState);
        o["textToSpeechLanguage"] = JsonValue(t.textToSpeechLanguage);
    }
    return o;
}

} // namespace

Sb3SaveResult saveSb3(Runtime& runtime) {
    Sb3SaveResult result;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> assets;

    JsonArray targets;
    for (Target* t : runtime.targets)
        if (t) targets.push_back(JsonValue(targetToJson(runtime, *t, assets)));

    JsonArray monitors;
    for (const auto& [id, rec] : runtime.monitorState.records) {
        JsonObject m;
        m["id"] = JsonValue(rec.id);
        m["mode"] = JsonValue(rec.mode);
        m["opcode"] = JsonValue(rec.opcode);
        m["spriteName"] = rec.spriteName.empty()
            ? JsonValue(nullptr) : JsonValue(SStr::toUtf8(rec.spriteName));
        m["width"] = JsonValue(rec.width);
        m["height"] = JsonValue(rec.height);
        m["x"] = JsonValue(rec.x - (runtime.stageWidth - 480) / 2.0);
        m["y"] = JsonValue(rec.y - (runtime.stageHeight - 360) / 2.0);
        m["visible"] = JsonValue(rec.visible);
        m["sliderMin"] = JsonValue(rec.sliderMin);
        m["sliderMax"] = JsonValue(rec.sliderMax);
        m["isDiscrete"] = JsonValue(rec.isDiscrete);
        JsonObject params;
        for (const auto& [pk, pv] : rec.params) params[pk] = JsonValue(SStr::toUtf8(pv));
        m["params"] = JsonValue(std::move(params));
        monitors.push_back(JsonValue(std::move(m)));
    }

    std::set<std::string> exts;
    for (Target* t : runtime.targets)
        if (t && t->blocks)
            for (auto& [bid, b] : t->blocks->allBlocks()) {
                std::string e = extForOpcode(b.opcode);
                if (!e.empty()) exts.insert(e);
            }
    JsonArray extArr;
    for (const auto& e : exts) extArr.push_back(JsonValue(e));

    JsonObject meta;
    meta["semver"] = JsonValue(std::string("3.0.0"));
    meta["vm"] = JsonValue(std::string("0.2.0"));
    meta["agent"] = JsonValue(std::string("xyo-cpp"));
    meta["origin"] = JsonValue(runtime.origin);

    JsonObject root;
    root["targets"] = JsonValue(std::move(targets));
    root["monitors"] = JsonValue(std::move(monitors));
    root["extensions"] = JsonValue(std::move(extArr));
    root["meta"] = JsonValue(std::move(meta));

    std::string json = jsonStringify(JsonValue(std::move(root)));
    std::vector<uint8_t> projectJson(json.begin(), json.end());

    ZipWriter zw;
    // scratch-vm と同じ方針（project.json は常に、アセットは .svg/.wav 等のみ
    // deflate）。無圧縮で保存すると sb3 が数十 MB になり得るため。
    zw.addFileCompressed("project.json", projectJson, 6);
    for (auto& [name, data] : assets) {
        if (zipShouldCompress(name))
            zw.addFileCompressed(name, data, 6);
        else
            zw.addFile(name, data);
    }
    result.data = zw.finish();
    result.ok = !result.data.empty();
    return result;
}

bool saveSb3File(Runtime& runtime, const char* path) {
    Sb3SaveResult r = saveSb3(runtime);
    if (!r.ok) return false;
    FILE* f = fopenUtf8(path, "wb");
    if (!f) return false;
    size_t n = std::fwrite(r.data.data(), 1, r.data.size(), f);
    std::fclose(f);
    return n == r.data.size();
}

bool saveSprite3File(Runtime& runtime, Target& target, const char* path) {
    // scratch-vm exportSprite: sprite.json + そのスプライトのアセット zip
    std::vector<std::pair<std::string, std::vector<uint8_t>>> assets;
    JsonValue j(targetToJson(runtime, target, assets));
    std::string json = jsonStringify(j);
    ZipWriter zw;
    zw.addFileCompressed("sprite.json",
                         std::vector<uint8_t>(json.begin(), json.end()), 6);
    for (auto& [name, data] : assets) {
        if (zipShouldCompress(name))
            zw.addFileCompressed(name, data, 6);
        else
            zw.addFile(name, data);
    }
    std::vector<uint8_t> data = zw.finish();
    if (data.empty()) return false;
    FILE* f = fopenUtf8(path, "wb");
    if (!f) return false;
    size_t n = std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return n == data.size();
}

namespace {
void collectFragmentIds(Blocks& src, const std::string& id, std::set<std::string>& out) {
    if (id.empty() || out.count(id)) return;
    Block* b = src.getBlock(id);
    if (!b) return;
    out.insert(id);
    for (auto& [n, in] : b->inputs.items) {
        collectFragmentIds(src, in.block, out);
        collectFragmentIds(src, in.shadow, out);
    }
    collectFragmentIds(src, b->next, out);
}
} // namespace

JsonValue saveBlocksFragment(Blocks& src, const std::string& headId) {
    std::set<std::string> ids;
    collectFragmentIds(src, headId, ids);
    JsonObject blocks;
    for (const std::string& id : ids) {
        Block* b = src.getBlock(id);
        if (!b) continue;
        blocks[id] = JsonValue(blockToJson(*b));
    }
    JsonObject frag;
    frag["head"] = JsonValue(headId);
    frag["blocks"] = JsonValue(std::move(blocks));
    return JsonValue(std::move(frag));
}

} // namespace xyo
