// xyo-cpp: serialization/sb3.js 移植。
#include "sb3.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "../core/gc.hpp"
#include "../core/json.hpp"
#include "../core/jsnum.hpp"
#include "../core/sstr.hpp"
#include "../engine/runtime.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "zip.hpp"

namespace xyo {

namespace {

constexpr int INPUT_SAME_BLOCK_SHADOW = 1;
constexpr int INPUT_BLOCK_NO_SHADOW = 2;
constexpr int INPUT_DIFF_BLOCK_SHADOW = 3;

// プリミティブ型コード（scratch-blocks-constants）
constexpr int PRIM_MATH_NUM = 4;
constexpr int PRIM_POSITIVE_NUM = 5;
constexpr int PRIM_WHOLE_NUM = 6;
constexpr int PRIM_INTEGER = 7;
constexpr int PRIM_ANGLE = 8;
constexpr int PRIM_COLOR = 9;
constexpr int PRIM_TEXT = 10;
constexpr int PRIM_BROADCAST = 11;
constexpr int PRIM_VAR = 12;
constexpr int PRIM_LIST = 13;

// Transient exact-UTF-8 sharing for initial list values only. Keys are owned,
// but SStr pointers are borrowed: this cache is not a GC root and must never
// free them. loadSb3's DeferCollect protects them until targets are installed;
// the cache is destroyed before that defer guard is released.
class InitialListStrings {
public:
    InitialListStrings() {
        const char* env = std::getenv("XYO_SB3_LIST_INTERN");
        enabled_ = !env || std::string_view(env) != "0";
    }

    SStr* get(const std::string& key) {
        if (!enabled_) return SStr::create(utf8ToU16(key));
        // Hits must keep working even after an admission budget is exhausted.
        auto it = strings_.find(key);
        if (it != strings_.end()) return it->second;
        if (strings_.size() >= kMaxEntries || key.size() > kMaxKeyLength ||
            key.size() > kMaxKeyBytes - keyBytes_)
            return SStr::create(utf8ToU16(key));
        SStr* value = SStr::create(utf8ToU16(key));
        strings_.emplace(key, value);
        keyBytes_ += key.size();
        return value;
    }

private:
    static constexpr size_t kMaxEntries = 4096;
    static constexpr size_t kMaxKeyBytes = 1024 * 1024;
    static constexpr size_t kMaxKeyLength = 4096;
    bool enabled_;
    size_t keyBytes_ = 0;
    std::unordered_map<std::string, SStr*> strings_;
};

uint32_t g_uidCounter = 0;
std::string newUid() {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "xyo_%08x", ++g_uidCounter);
    return buf;
}

/// JSON値 → Mutation attr 文字列（XML属性相当）。
/// 文字列はそのまま、配列/オブジェクトは JSON 化（JS は mutation を
/// 生オブジェクトとして持ち argumentids 等は JSON.parse される）。
std::string mutationAttr(const JsonValue& v) {
    if (v.isString()) return v.asString();
    if (v.isBool()) return v.asBool() ? "true" : "false";
    if (v.isNull()) return "";
    return jsonStringify(v);
}

/// StringUtil.replaceUnsafeChars（変数 id 内の <>&'" を置換）
std::string replaceUnsafeChars(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '<') out += "lt";
        else if (c == '>') out += "gt";
        else if (c == '&') out += "amp";
        else if (c == '\'') out += "apos";
        else if (c == '"') out += "quot";
        else out += c;
    }
    return out;
}

// ---------------------------------------------------------------
// ブロック JSON → Block 変換用の中間構造
// ---------------------------------------------------------------

struct BlockJson {
    std::string id;
    std::string opcode;
    std::string next;          // "" = null
    std::string parent;
    bool topLevel = false;
    bool shadow = false;
    double x = 0, y = 0;
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>>
        inputs;                // name → (block, shadow) ※ id 解決後
    std::vector<std::pair<std::string, BlockField>> fields;
    std::unique_ptr<Mutation> mutation;
};

/// inputDesc（string id またはプリミティブ配列）→ ブロック id。
/// プリミティブなら新ブロックを allBlocks に追加する。
std::string deserializeInputDesc(
    const JsonValue& desc, const std::string& parentId, bool isShadow,
    std::unordered_map<std::string, BlockJson>& allBlocks,
    std::vector<std::string>& order) {
    if (!desc.isArray()) {
        return desc.isString() ? desc.asString() : "";
    }
    const JsonArray& arr = desc.asArray();
    if (arr.empty()) return "";
    int primType = int(arr[0].asNumber());
    BlockJson b;
    b.id = newUid();
    b.parent = parentId;
    b.shadow = isShadow;
    switch (primType) {
    case PRIM_MATH_NUM: b.opcode = "math_number"; break;
    case PRIM_POSITIVE_NUM: b.opcode = "math_positive_number"; break;
    case PRIM_WHOLE_NUM: b.opcode = "math_whole_number"; break;
    case PRIM_INTEGER: b.opcode = "math_integer"; break;
    case PRIM_ANGLE: b.opcode = "math_angle"; break;
    case PRIM_COLOR: b.opcode = "colour_picker"; break;
    case PRIM_TEXT: b.opcode = "text"; break;
    case PRIM_BROADCAST: b.opcode = "event_broadcast_menu"; break;
    case PRIM_VAR: b.opcode = "data_variable"; break;
    case PRIM_LIST: b.opcode = "data_listcontents"; break;
    default: return "";
    }
    const JsonValue& val = arr.size() > 1 ? arr[1] : JsonValue();
    auto fieldValue = [&]() -> Value {
        if (val.isNumber()) return Value::num(val.asNumber());
        if (val.isBool()) return Value::boolean(val.asBool());
        if (val.isString())
            return Value::string(SStr::create(utf8ToU16(val.asString())));
        return Value::emptyStr();
    };
    auto makeField = [&](const char* fieldName) {
        BlockField f;
        f.value = fieldValue();
        if (arr.size() > 2 && arr[2].isString()) f.id = arr[2].asString();
        b.fields.emplace_back(fieldName, f);
    };
    switch (primType) {
    case PRIM_MATH_NUM: case PRIM_POSITIVE_NUM: case PRIM_WHOLE_NUM:
    case PRIM_INTEGER: case PRIM_ANGLE:
        makeField("NUM");
        break;
    case PRIM_COLOR: makeField("COLOUR"); break;
    case PRIM_TEXT: makeField("TEXT"); break;
    case PRIM_BROADCAST: makeField("BROADCAST_OPTION"); break;
    case PRIM_VAR: makeField("VARIABLE"); break;
    case PRIM_LIST: makeField("LIST"); break;
    default: break;
    }
    // VAR/LIST はトップレベル配置情報を持つ場合がある（旧形式）
    if ((primType == PRIM_VAR || primType == PRIM_LIST) && arr.size() > 3) {
        b.topLevel = true;
        b.x = arr[3].asNumber();
        b.y = arr[4].asNumber();
    }
    std::string id = b.id;
    allBlocks.emplace(id, std::move(b));
    order.push_back(id);
    return id;
}

/// object.blocks を全走査して BlockJson 群を構築（deserializeBlocks 相当）。
void deserializeBlocks(const JsonValue& blocksObj,
                       std::unordered_map<std::string, BlockJson>& allBlocks,
                       std::vector<std::string>& order) {
    // 1st pass: オブジェクトブロックを登録 + inputs/fields 変換
    // （プリミティブ配列エントリは deserializeInputDesc で新ブロック化）
    std::vector<std::string> standalone;   // 配列型エントリの生成 id
    for (const auto& [blockId, blockVal] : blocksObj.asObject()) {
        if (blockVal.isArray()) {
            std::string nid =
                deserializeInputDesc(blockVal, "", false, allBlocks, order);
            if (!nid.empty()) standalone.push_back(nid);
            continue;
        }
        if (!blockVal.isObject()) continue;
        BlockJson b;
        b.id = blockId;
        b.opcode = blockVal.get("opcode").asString();
        if (blockVal.get("next").isString())
            b.next = blockVal.get("next").asString();
        if (blockVal.get("parent").isString())
            b.parent = blockVal.get("parent").asString();
        b.topLevel = blockVal.get("topLevel").asBool();
        b.shadow = blockVal.get("shadow").asBool();
        b.x = blockVal.get("x").asNumber();
        b.y = blockVal.get("y").asNumber();

        // inputs — JSON 挿入順を維持（評価順）
        for (const auto& [iname, ival] :
             blockVal.get("inputs").asObject()) {
            if (!ival.isArray()) continue;
            const JsonArray& a = ival.asArray();
            if (a.empty()) continue;
            int info = int(a[0].asNumber());
            std::string block, shadow;
            if (info == INPUT_SAME_BLOCK_SHADOW) {
                block = shadow = a.size() > 1
                    ? deserializeInputDesc(a[1], blockId, true, allBlocks, order)
                    : "";
            } else if (info == INPUT_BLOCK_NO_SHADOW) {
                block = a.size() > 1
                    ? deserializeInputDesc(a[1], blockId, false, allBlocks,
                                           order)
                    : "";
            } else {   // INPUT_DIFF_BLOCK_SHADOW
                block = a.size() > 1
                    ? deserializeInputDesc(a[1], blockId, false, allBlocks,
                                           order)
                    : "";
                shadow = a.size() > 2
                    ? deserializeInputDesc(a[2], blockId, true, allBlocks,
                                           order)
                    : "";
            }
            b.inputs.emplace_back(iname, std::make_pair(block, shadow));
        }
        // fields — 値+id（refs は field 名で判定される）
        for (const auto& [fname, fval] :
             blockVal.get("fields").asObject()) {
            BlockField f;
            if (!fval.isArray() || fval.asArray().empty()) continue;
            const JsonArray& fa = fval.asArray();
            const JsonValue& fv = fa[0];
            if (fv.isNumber()) f.value = Value::num(fv.asNumber());
            else if (fv.isBool()) f.value = Value::boolean(fv.asBool());
            else if (fv.isString())
                f.value = Value::string(SStr::create(utf8ToU16(fv.asString())));
            if (fa.size() > 1 && fa[1].isString()) f.id = fa[1].asString();
            b.fields.emplace_back(fname, f);
        }
        // mutation
        const JsonValue& mut = blockVal.get("mutation");
        if (mut.isObject()) {
            Mutation m;
            m.tagName = mut.get("tagName").asString();
            if (m.tagName.empty()) m.tagName = "mutation";
            for (const auto& [k, v] : mut.asObject()) {
                // children は sb3 では配列（procedures では常に空）。JSON 文字列として
                // 保持し、serializer が配列へ戻す — これで round-trip が一致する。
                if (k == "tagName") continue;
                m.attrs[k] = mutationAttr(v);
            }
            b.mutation = std::make_unique<Mutation>(std::move(m));
        }
        allBlocks.emplace(blockId, std::move(b));
        order.push_back(blockId);
    }
}

/// fixSporkCompatibility
void fixSporkCompatibility(
    std::unordered_map<std::string, BlockJson>& allBlocks) {
    for (auto& [id, b] : allBlocks) {
        if (b.opcode == "procedures_prototype") {
            b.shadow = true;
        } else if (b.opcode == "argument_reporter_string_number" ||
                   b.opcode == "argument_reporter_boolean") {
            auto it = allBlocks.find(b.parent);
            if (it != allBlocks.end() &&
                it->second.opcode == "procedures_prototype") {
                b.shadow = true;
            }
        } else if (b.opcode == "control_stop" && !b.mutation) {
            std::u16string opt;
            for (const auto& [n, f] : b.fields)
                if (n == "STOP_OPTION" && f.value.isString())
                    opt = f.value.asStrPtr()->view();
            bool hasNext = opt == u"other scripts in sprite" ||
                           opt == u"other scripts in stage";
            Mutation m;
            m.tagName = "mutation";
            m.attrs["hasnext"] = hasNext ? "true" : "false";
            b.mutation = std::make_unique<Mutation>(std::move(m));
        }
    }
}

/// BlockJson → Block 変換（createBlock 相当）
Block makeBlock(const BlockJson& bj) {
    Block b;
    b.id = bj.id;
    b.opcode = bj.opcode;
    if (!bj.next.empty()) b.next = bj.next;
    if (!bj.parent.empty()) b.parent = bj.parent;
    b.topLevel = bj.topLevel;
    b.shadow = bj.shadow;
    b.x = bj.x;
    b.y = bj.y;
    for (const auto& [name, bs] : bj.inputs) {
        BlockInput in;
        in.block = bs.first;
        in.shadow = bs.second;
        b.inputs.set(name, std::move(in));
    }
    for (const auto& [name, f] : bj.fields) b.fields[name] = f;
    if (bj.mutation) b.mutation = *bj.mutation;
    return b;
}

/// コア opcode → monitor id 生成（getMonitored 相当）。
/// 返り値 empty は「core でない」。
std::string coreMonitorId(const std::string& opcode, const std::string& targetId,
                          const std::map<std::string, BlockField>& fields,
                          bool& isSpriteSpecific) {
    isSpriteSpecific = false;
    auto fieldArgs = [&]() {
        std::string s;
        for (const auto& [k, f] : fields) {
            std::string v = SStr::toUtf8(
                f.value.isString() ? f.value.asStrPtr()->view()
                                   : u"");
            if (f.value.isNumber()) {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%g",
                              f.value.asNumberUnchecked());
                v = buf;
            }
            if (k == "CURRENTMENU") {
                for (char& c : v) c = char(std::tolower(c));
            }
            s += "_" + v;
        }
        return s;
    };
    if (opcode == "motion_xposition") {
        isSpriteSpecific = true;
        return targetId + "_xposition";
    }
    if (opcode == "motion_yposition") {
        isSpriteSpecific = true;
        return targetId + "_yposition";
    }
    if (opcode == "motion_direction") {
        isSpriteSpecific = true;
        return targetId + "_direction";
    }
    if (opcode == "looks_size") {
        isSpriteSpecific = true;
        return targetId + "_size";
    }
    if (opcode == "looks_costumenumbername") {
        isSpriteSpecific = true;
        return targetId + "_costumenumbername" + fieldArgs();
    }
    if (opcode == "looks_backdropnumbername")
        return "backdropnumbername" + fieldArgs();
    if (opcode == "sensing_answer") return "answer";
    if (opcode == "sensing_mousedown") return "mousedown";
    if (opcode == "sensing_mousex") return "mousex";
    if (opcode == "sensing_mousey") return "mousey";
    if (opcode == "sensing_loudness") return "loudness";
    if (opcode == "sensing_timer") return "timer";
    if (opcode == "sensing_dayssince2000") return "dayssince2000";
    if (opcode == "sensing_current") return "current" + fieldArgs();
    if (opcode == "sensing_online") return "online";
    if (opcode == "sound_volume") {
        isSpriteSpecific = true;
        return targetId + "_volume";
    }
    return "";
}

/// opcode から拡張 id（prefix_）。core 拡張は除外。
const std::unordered_set<std::string>& coreExtensions() {
    static const std::unordered_set<std::string> kCore = {
        "argument", "colour", "control", "data", "event", "looks",
        "math", "motion", "operator", "procedures", "sensing", "sound"};
    return kCore;
}
std::string extensionIdForOpcode(const std::string& opcode) {
    size_t i = opcode.find('_');
    if (i == std::string::npos) return "";
    std::string prefix = opcode.substr(0, i);
    if (coreExtensions().count(prefix)) return "";
    return prefix;
}

double clampD(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
double wrapClampD(double v, double lo, double hi) {
    double range = hi - lo + 1;
    return v - std::floor((v - lo) / range) * range;
}

void loadAsset(ZipReader const* zip, const std::string& md5,
               std::vector<uint8_t>& out) {
    if (!zip || md5.empty()) return;
    if (const ZipEntry* e = zip->find(md5)) zip->extract(*e, out);
}

} // namespace

// ---------------------------------------------------------------

Target* loadTargetObject(Runtime& runtime, const JsonValue& obj,
                         const ZipReader* zip, int layerOrder,
                         std::vector<std::string>& extIds,
                         InitialListStrings& listStrings) {
    if (!obj.has("name")) return nullptr;

    auto sprite = std::make_unique<Sprite>(&runtime);
    sprite->name = utf8ToU16(obj.get("name").asString());
    Sprite* spritePtr = sprite.get();
    runtime.ownedSprites.push_back(std::move(sprite));

    // costumes
    for (const auto& c : obj.get("costumes").asArray()) {
        Costume costume;
        costume.assetId = c.get("assetId").asString();
        costume.name = utf8ToU16(c.get("name").asString());
        costume.bitmapResolution = c.get("bitmapResolution").isNumber()
                                       ? c.get("bitmapResolution").asNumber()
                                       : 1;
        costume.rotationCenterX = c.get("rotationCenterX").asNumber();
        costume.rotationCenterY = c.get("rotationCenterY").asNumber();
        costume.dataFormat = c.get("dataFormat").asString();
        if (costume.dataFormat.empty()) costume.dataFormat = "png";
        costume.md5 = c.has("md5ext")
                          ? c.get("md5ext").asString()
                          : costume.assetId + "." + costume.dataFormat;
        loadAsset(zip, costume.md5, costume.data);
        spritePtr->costumes.push_back(std::move(costume));
    }
    // sounds
    for (const auto& s : obj.get("sounds").asArray()) {
        Sound sound;
        sound.assetId = s.get("assetId").asString();
        sound.name = utf8ToU16(s.get("name").asString());
        sound.dataFormat = s.get("dataFormat").asString();
        sound.format = s.get("format").asString();
        sound.rate = s.get("rate").asNumber();
        sound.sampleCount = s.get("sampleCount").asNumber();
        sound.md5 = s.get("md5ext").asString();
        if (sound.md5.empty())
            sound.md5 = sound.assetId + "." + sound.dataFormat;
        sound.soundId = int(spritePtr->sounds.size());
        loadAsset(zip, sound.md5, sound.data);
        spritePtr->sounds.push_back(std::move(sound));
    }

    // createClone(layerGroup) — オリジナル target 作成
    auto blocks = std::make_unique<Blocks>(&runtime);
    spritePtr->blocks = std::move(blocks);
    Target* target = spritePtr->createClone();
    target->id = newUid();   // sb3 には target id がない — 生成
    target->layerOrder = layerOrder;
    target->isStage = obj.get("isStage").asBool();

    // blocks
    if (obj.has("blocks")) {
        std::unordered_map<std::string, BlockJson> all;
        std::vector<std::string> order;
        deserializeBlocks(obj.get("blocks"), all, order);
        fixSporkCompatibility(all);
        for (const std::string& id : order) {
            auto it = all.find(id);
            if (it == all.end()) continue;
            Block b = makeBlock(it->second);
            std::string ext = extensionIdForOpcode(b.opcode);
            if (!ext.empty() &&
                std::find(extIds.begin(), extIds.end(), ext) == extIds.end())
                extIds.push_back(ext);
            spritePtr->blocks->createBlock(std::move(b));
        }
    }

    // ブロックコメント（blockId → text）。エディタのコメント表示・保存で使う。
    for (const auto& [cid, c] : obj.get("comments").asObject()) {
        std::string bid = c.get("blockId").asString();
        std::string text = c.get("text").asString();
        if (bid.empty() || text.empty()) continue;
        if (Block* b = spritePtr->blocks->getBlock(bid)) b->comment = text;
    }

    // ターゲットプロパティ
    if (obj.has("tempo")) target->tempo = obj.get("tempo").asNumber();
    if (obj.has("volume")) target->volume = obj.get("volume").asNumber();
    if (obj.has("videoTransparency"))
        target->videoTransparency = obj.get("videoTransparency").asNumber();
    if (obj.has("videoState"))
        target->videoState = obj.get("videoState").asString();
    if (obj.has("textToSpeechLanguage"))
        target->textToSpeechLanguage =
            obj.get("textToSpeechLanguage").asString();

    // variables: {id: [name, value, cloud?]}
    for (const auto& [varId, varArr] : obj.get("variables").asObject()) {
        const JsonArray& a = varArr.asArray();
        if (a.size() < 2) continue;
        bool isCloud = a.size() >= 3 && a[2].asBool() && target->isStage;
        Variable v = Variable::create(replaceUnsafeChars(varId),
                                      utf8ToU16(a[0].asString()),
                                      Variable::kScalarType, isCloud);
        const JsonValue& val = a[1];
        if (val.isNumber()) v.value = Value::num(val.asNumber());
        else if (val.isBool()) v.value = Value::boolean(val.asBool());
        else v.value = Value::string(SStr::create(utf8ToU16(val.asString())));
        target->variables.emplace(v.id, std::move(v));
    }
    // lists: {id: [name, [items]]}
    for (const auto& [listId, listArr] : obj.get("lists").asObject()) {
        const JsonArray& a = listArr.asArray();
        if (a.size() < 2) continue;
        Variable v = Variable::create(replaceUnsafeChars(listId),
                                      utf8ToU16(a[0].asString()),
                                      Variable::kListType, false);
        for (const JsonValue& item : a[1].asArray()) {
            if (item.isNumber()) v.list->push(Value::num(item.asNumber()));
            else if (item.isBool())
                v.list->push(Value::boolean(item.asBool()));
            else
                v.list->push(Value::string(
                    listStrings.get(item.asString())));
        }
        target->variables.emplace(v.id, std::move(v));
    }
    // broadcasts: {id: name}
    for (const auto& [bid, bval] : obj.get("broadcasts").asObject()) {
        Variable v = Variable::create(replaceUnsafeChars(bid),
                                      utf8ToU16(bval.asString()),
                                      Variable::kBroadcastType, false);
        target->variables.emplace(v.id, std::move(v));
    }

    if (obj.has("x")) target->x = obj.get("x").asNumber();
    if (obj.has("y")) target->y = obj.get("y").asNumber();
    if (obj.has("direction"))
        target->direction =
            wrapClampD(obj.get("direction").asNumber(), -179, 180);
    if (obj.has("size")) target->size = obj.get("size").asNumber();
    if (obj.has("visible")) target->visible = obj.get("visible").asBool();
    if (obj.has("currentCostume")) {
        double n = double(spritePtr->costumes.size());
        target->currentCostume =
            int(clampD(obj.get("currentCostume").asNumber(), 0,
                       n > 0 ? n - 1 : 0));
    }
    if (obj.has("rotationStyle"))
        target->rotationStyle = obj.get("rotationStyle").asString();
    if (obj.has("draggable")) target->draggable = obj.get("draggable").asBool();

    return target;
}

// ---------------------------------------------------------------

void loadMonitor(Runtime& runtime, const JsonValue& monitorData,
                 const std::vector<Target*>& targets) {
    std::string opcode = monitorData.get("opcode").asString();
    MonitorRecord rec;
    rec.id = monitorData.get("id").asString();
    rec.opcode = opcode;
    rec.x = clampD(monitorData.get("x").asNumber() +
                       (runtime.stageWidth - 480) / 2,
                   0, runtime.stageWidth);
    rec.y = clampD(monitorData.get("y").asNumber() +
                       (runtime.stageHeight - 360) / 2,
                   0, runtime.stageHeight);
    rec.visible = monitorData.get("visible").asBool();
    // リストモニタは width/height を保存する（0 は既定 110x200）。
    rec.width = monitorData.get("width").asNumber();
    rec.height = monitorData.get("height").asNumber();
    rec.sliderMin = monitorData.get("sliderMin").asNumber();
    rec.sliderMax = monitorData.get("sliderMax").asNumber();
    rec.isDiscrete = !monitorData.has("isDiscrete") ||
                     monitorData.get("isDiscrete").asBool();
    if (monitorData.get("mode").isString())
        rec.mode = monitorData.get("mode").asString();
    std::u16string spriteName =
        utf8ToU16(monitorData.get("spriteName").asString());
    std::string targetId = monitorData.get("targetId").asString();

    // spriteName → targetId
    if (!spriteName.empty()) {
        bool found = false;
        for (Target* t : targets) {
            if (t->sprite && t->sprite->name == spriteName) {
                targetId = t->id;
                found = true;
                break;
            }
        }
        (void)found;
    }
    rec.spriteName = spriteName;
    rec.targetId = targetId;

    // params → monitor block fields + rec.params
    std::map<std::string, BlockField> fields;
    for (const auto& [pk, pv] : monitorData.get("params").asObject()) {
        BlockField f;
        std::u16string pval;
        if (pv.isNumber()) {
            f.value = Value::num(pv.asNumber());
            pval = jsnum::toString16(pv.asNumber());
        } else if (pv.isBool()) {
            f.value = Value::boolean(pv.asBool());
            pval = pv.asBool() ? u"true" : u"false";
        } else {
            pval = utf8ToU16(pv.asString());
            f.value = Value::string(SStr::create(pval));
        }
        fields[pk] = f;
        rec.params[pk] = pval;
    }

    // renamed list monitor バグ修正
    if (opcode == "data_listcontents") {
        Target* lt = nullptr;
        for (Target* t : targets) {
            if (!targetId.empty() ? t->id == targetId : t->isStage) lt = t;
        }
        if (lt) {
            auto it = lt->variables.find(rec.id);
            if (it != lt->variables.end() && fields.count("LIST"))
                fields["LIST"].value =
                    Value::string(SStr::create(it->second.name));
        }
    }

    // id 決定
    if (opcode != "data_variable" && opcode != "data_listcontents") {
        bool spriteSpecific;
        std::string mid = coreMonitorId(opcode, targetId, fields,
                                      spriteSpecific);
        if (spriteSpecific && !mid.empty()) rec.id = mid;
    }
    rec.id = replaceUnsafeChars(rec.id);

    // VARIABLE/LIST field 補完
    if (opcode == "data_variable" && fields.count("VARIABLE")) {
        fields["VARIABLE"].id = rec.id;
    } else if (opcode == "data_listcontents" && fields.count("LIST")) {
        fields["LIST"].id = rec.id;
    }

    // monitorBlocks にブロック作成（既存なら isMonitored/targetId 更新）
    if (Block* existing = runtime.monitorBlocks->getBlock(rec.id)) {
        existing->isMonitored = rec.visible;
        existing->targetId = targetId;
    } else {
        Block mb;
        mb.id = rec.id;
        mb.opcode = opcode;
        mb.topLevel = true;
        mb.isMonitored = rec.visible;
        mb.targetId = targetId;
        for (auto& [k, f] : fields) mb.fields[k] = f;
        runtime.monitorBlocks->createBlock(std::move(mb));
    }
    runtime.requestAddMonitor(rec);
}

// ---------------------------------------------------------------

/// TurboWarp プロジェクトオプション（` // _twconfig_` で終わる行を
/// 持つステージ/スプライトのコメント）を Runtime へ適用する。
/// runtime.js の findProjectOptionsComment/parseStoredProjectOptions
/// 相当。ターゲットやモニタのロードより先に呼ぶこと。
void applyTwProjectOptions(Runtime& runtime,
                           const JsonArray& targetObjs) {
    static constexpr std::string_view kMagic = " // _twconfig_";
    for (const JsonValue& tobj : targetObjs) {
        for (const auto& [cid, comment] :
                 tobj.get("comments").asObject()) {
            const std::string& text = comment.get("text").asString();
            if (text.find(kMagic) == std::string::npos) continue;
            // マジックで終わる行を探し、その手前を JSON として読む
            std::string_view jsonText;
            size_t pos = 0;
            while (pos <= text.size()) {
                size_t nl = text.find('\n', pos);
                size_t end =
                    nl == std::string::npos ? text.size() : nl;
                std::string_view line(text.data() + pos, end - pos);
                if (line.size() >= kMagic.size() &&
                    line.substr(line.size() - kMagic.size()) == kMagic) {
                    jsonText =
                        line.substr(0, line.size() - kMagic.size());
                    break;
                }
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
            JsonValue opt =
                jsonText.empty() ? JsonValue() : jsonParse(jsonText);
            if (!opt.isObject()) continue;

            // framerate: setFramerate（>250→250、<0→1、0=垂直同期相当）
            if (opt.get("framerate").isNumber()) {
                double f = opt.get("framerate").asNumber();
                if (f > 250) f = 250;
                if (f < 0) f = 1;
                runtime.currentStepTime =
                    f == 0 ? 1000.0 / 60.0 : 1000.0 / f;
            }
            if (opt.get("turbo").asBool()) runtime.turboMode = true;
            if (opt.get("interpolation").asBool())
                runtime.interpolationEnabled = true;
            // runtimeOptions: maxClones/miscLimits/fencing
            const JsonValue& ro = opt.get("runtimeOptions");
            if (ro.get("maxClones").isNumber())
                runtime.maxClones = int(ro.get("maxClones").asNumber());
            if (ro.get("miscLimits").isBool())
                runtime.miscLimits = ro.get("miscLimits").asBool();
            if (ro.get("fencing").isBool())
                runtime.fencing = ro.get("fencing").asBool();
            // +parsed.width || stageWidth（0/非数値は既定値のまま）
            auto optNum = [](const JsonValue& v) -> double {
                if (v.isNumber()) return v.asNumber();
                if (v.isString())
                    return std::strtod(v.asString().c_str(), nullptr);
                return 0.0;
            };
            double w = optNum(opt.get("width"));
            double h = optNum(opt.get("height"));
            if (w > 0) runtime.stageWidth = int(w);
            if (h > 0) runtime.stageHeight = int(h);
            return;   // TW: 最初に見つかった設定コメントのみ
        }
    }
}

Sb3LoadResult loadSb3(Runtime& runtime, const std::string& projectJson,
                      const ZipReader* zip, bool isSingleSprite) {
    // ロード中は Target が runtime.targets に未登録の区間があり、
    // そこへ格納した SStr/SList が GC から到達不能に見える。
    // collect を遅延させ、全てがルートへ接続されてから回収可能にする。
    Gc::DeferCollect gcDefer;
    InitialListStrings listStrings;
    Sb3LoadResult result;
    JsonValue json = jsonParse(projectJson);
    if (!json.isObject()) {
        result.error = "project.json parse failed";
        return result;
    }
    runtime.origin = json.get("meta").get("origin").asString();

    // targets: layerOrder ソートで parse → targetPaneOrder で並び戻す
    JsonArray targetObjs;
    if (isSingleSprite) targetObjs.push_back(json);
    else targetObjs = json.get("targets").asArray();

    applyTwProjectOptions(runtime, targetObjs);

    std::vector<const JsonValue*> sorted;
    for (const auto& t : targetObjs) sorted.push_back(&t);
    // targetPaneOrder = 元順序
    std::vector<int> paneOrder(sorted.size());
    for (size_t i = 0; i < sorted.size(); ++i) paneOrder[i] = int(i);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const JsonValue* a, const JsonValue* b) {
                         return a->get("layerOrder").asNumber() <
                                b->get("layerOrder").asNumber();
                     });
    // layerOrder 順に parse（描画順）してから pane 順に戻す
    std::vector<std::pair<Target*, int>> parsed;   // target → paneOrder
    for (const JsonValue* tobj : sorted) {
        int li = int(parsed.size());   // layerOrder = parse 順
        Target* t = loadTargetObject(runtime, *tobj, zip, li,
                                     result.extensionIds, listStrings);
        int po = 0;
        for (size_t i = 0; i < targetObjs.size(); ++i)
            if (&targetObjs[i] == tobj) po = int(i);
        if (t) parsed.emplace_back(t, po);
    }
    std::stable_sort(parsed.begin(), parsed.end(),
                     [](const auto& a, const auto& b) {
                         return a.second < b.second;
                     });
    for (auto& [t, po] : parsed) {
        runtime.addTarget(t);
        result.targets.push_back(t);
    }
    // installTargets: executableTargets を layerOrder でソート
    std::stable_sort(runtime.executableTargets.begin(),
                     runtime.executableTargets.end(),
                     [](Target* a, Target* b) {
                         return a->layerOrder < b->layerOrder;
                     });
    // editingTarget = 最初のスプライト（なければ先頭ターゲット）
    if (!result.targets.empty()) {
        runtime.editingTarget =
            result.targets.size() > 1 ? result.targets[1]
                                      : result.targets[0];
    }

    // monitors
    for (const auto& m : json.get("monitors").asArray())
        loadMonitor(runtime, m, result.targets);

    result.ok = true;
    return result;
}

std::vector<std::string> loadBlocksFragment(Blocks& out, const JsonValue& fragment,
                                            std::string* headOut) {
    std::vector<std::string> ids;
    if (!fragment.isObject()) return ids;
    if (headOut) *headOut = fragment.get("head").asString();
    const JsonValue& blocksObj = fragment.has("blocks") ? fragment.get("blocks") : fragment;
    std::unordered_map<std::string, BlockJson> all;
    std::vector<std::string> order;
    deserializeBlocks(blocksObj, all, order);
    fixSporkCompatibility(all);
    // 旧 id → 新 id（断片を複数回貼り付けても衝突しないように）。
    // newUid() の連番はプロジェクト読込時にも使われるため、out 内に同じ id が
    // あれば createBlock が既存ブロックを上書きしてしまう — out に無い id が
    // 出るまで回す。
    std::unordered_map<std::string, std::string> remap;
    for (const std::string& id : order) {
        std::string nid;
        do { nid = newUid(); } while (out.getBlock(nid));
        remap[id] = nid;
    }
    auto mapId = [&](const std::string& s) -> std::string {
        if (s.empty()) return "";
        auto it = remap.find(s);
        return it == remap.end() ? std::string() : it->second;
    };
    for (const std::string& id : order) {
        auto it = all.find(id);
        if (it == all.end()) continue;
        BlockJson& bj = it->second;   // BlockJson は unique_ptr を持ちコピー不可 — 参照で書き換える
        bj.id = mapId(id);
        bj.next = mapId(bj.next);
        bj.parent = mapId(bj.parent);
        for (auto& [name, bs] : bj.inputs) {
            bs.first = mapId(bs.first);
            bs.second = mapId(bs.second);
        }
        bj.topLevel = false;
        Block b = makeBlock(bj);
        ids.push_back(b.id);
        out.createBlock(std::move(b));
    }
    if (headOut && !headOut->empty()) *headOut = mapId(*headOut);
    return ids;
}

Sb3LoadResult loadSb3File(Runtime& runtime, const uint8_t* data,
                          size_t size) {    Sb3LoadResult result;
    ZipReader zip;
    if (!ZipReader::open(data, size, zip)) {
        result.error = "zip open failed";
        return result;
    }
    const ZipEntry* pj = zip.find("project.json");
    if (!pj) {
        result.error = "project.json not found";
        return result;
    }
    std::vector<uint8_t> jsonData;
    if (!zip.extract(*pj, jsonData)) {
        result.error = "project.json extract failed";
        return result;
    }
    // Scratch 2.0 (.sb2) は objName+children スキーマ — sb2 ローダへ
    JsonValue peek = jsonParse(
        std::string_view(reinterpret_cast<const char*>(jsonData.data()),
                         jsonData.size()));
    if (peek.isObject() && peek.has("objName") && !peek.has("targets")) {
        return loadSb2(runtime, peek, &zip);
    }
    return loadSb3(runtime,
                   std::string(jsonData.begin(), jsonData.end()), &zip);
}

} // namespace xyo
