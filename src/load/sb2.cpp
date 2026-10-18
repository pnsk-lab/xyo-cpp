// xyo-cpp: serialization/sb2.js 移植 — Scratch 2.0 JSON → モデル層。
// アセット読み込み・コメント・ScratchX 拡張は最小限（headless 向け）。
// specmap テーブルは sb2_specmap.hpp（自動生成）。
#include "sb3.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_set>

#include "../core/cast.hpp"
#include "../core/jsnum.hpp"
#include "../core/sstr.hpp"
#include "../engine/runtime.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "../rt/runtime_helpers.hpp"
#include "sb2_specmap.hpp"
#include "zip.hpp"

namespace xyo {

namespace {

// ------------------------------------------------------------------ util --

std::string uidSeq() {
    static int counter = 0;
    return "sb2-" + std::to_string(++counter);
}

/// StringUtil.replaceUnsafeChars — XML安全でない文字を名前に変換
std::string replaceUnsafe(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '<': out += "lt"; break;
        case '>': out += "gt"; break;
        case '&': out += "amp"; break;
        case '\'': out += "apos"; break;
        case '"': out += "quot"; break;
        default: out += c;
        }
    }
    return out;
}

std::u16string toU16(const JsonValue& v) {
    if (v.isNumber()) return jsnum::toString16(v.asNumber());
    if (v.isBool()) return v.asBool() ? u"true" : u"false";
    return utf8ToU16(v.asString());
}

std::string toU8(const JsonValue& v) {
    if (v.isNumber()) return jsnum::toString(v.asNumber());
    if (v.isBool()) return v.asBool() ? "true" : "false";
    return v.asString();
}

Value toValue(const JsonValue& v) {
    if (v.isNumber()) return Value::num(v.asNumber());
    if (v.isBool()) return Value::boolean(v.asBool());
    return Value::string(SStr::create(toU16(v)));
}

/// proccode の %n %s %b を走査して inputN 名の arg spec を生成
/// （JS: procCode.split(/(?=[^\d]%[nbs])/) → trim → '%'始まりのみ）
std::vector<Sb2ArgSpec> parseProcedureArgMap(const std::string& procCode) {
    std::vector<Sb2ArgSpec> argMap;
    argMap.push_back({nullptr, nullptr, nullptr, nullptr, nullptr}); // [0]=op
    static std::deque<std::string> names;  // c_str 安定性のため deque
    int inputCount = 0;
    // split 境界 = [^\d] の直後に %[nbs] が続く位置（lookahead 相当）
    std::vector<size_t> bounds;
    for (size_t i = 0; i + 2 < procCode.size(); ++i) {
        char c = procCode[i];
        if (!(c >= '0' && c <= '9') && procCode[i + 1] == '%' &&
            (procCode[i + 2] == 'n' || procCode[i + 2] == 's' ||
             procCode[i + 2] == 'b')) {
            bounds.push_back(i);
        }
    }
    for (size_t bi = 0; bi <= bounds.size(); ++bi) {
        size_t lo = bi == 0 ? 0 : bounds[bi - 1];
        size_t hi = bi == bounds.size() ? procCode.size() : bounds[bi];
        std::string part = procCode.substr(lo, hi - lo);
        size_t a = part.find_first_not_of(" \t\r\n");
        size_t b = part.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        part = part.substr(a, b - a + 1);
        if (part.empty() || part[0] != '%' || part.size() < 2) continue;
        Sb2ArgSpec arg{};
        arg.type = "input";
        names.push_back("input" + std::to_string(inputCount++));
        arg.inputName = names.back().c_str();
        char t = part[1];
        arg.inputOp = t == 'n' ? "math_number"
                    : t == 's' ? "text"
                    : t == 'b' ? "boolean" : nullptr;
        argMap.push_back(arg);
    }
    return argMap;
}

/// argumentids JSON: ["input0","input1",...]
std::string parseProcedureArgIdsJson(const std::string& procCode) {
    std::string out = "[";
    bool first = true;
    for (const Sb2ArgSpec& a : parseProcedureArgMap(procCode)) {
        if (!a.inputName) continue;
        if (!first) out += ',';
        out += '"'; out += a.inputName; out += '"';
        first = false;
    }
    out += ']';
    return out;
}

/// JS string[] → JSON.stringify
std::string strArrayJson(const JsonValue& arr) {
    std::string out = "[";
    bool first = true;
    for (const JsonValue& v : arr.asArray()) {
        if (!first) out += ',';
        out += '"';
        for (char c : v.asString()) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += '"';
        first = false;
    }
    out += ']';
    return out;
}

/// 任意値配列の JSON（argumentdefaults）
std::string valArrayJson(const JsonValue& arr) {
    std::string out = "[";
    bool first = true;
    for (const JsonValue& v : arr.asArray()) {
        if (!first) out += ',';
        if (v.isNumber()) out += jsnum::toString(v.asNumber());
        else if (v.isBool()) out += v.asBool() ? "true" : "false";
        else {
            out += '"';
            for (char c : v.asString()) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            out += '"';
        }
        first = false;
    }
    out += ']';
    return out;
}

// ------------------------------------------------------------- コンテキスト --

struct Sb2Ctx {
    Runtime& rt;
    // globalBroadcastMsgStateGenerator: 全ターゲット共有（topLevel で reset）
    std::map<std::string, std::string> broadcastMsgMap;
    std::vector<BlockField*> allBroadcastFields;
    std::string emptyMsgName = uidSeq();
    // globalVariableNameMap: "name-type" → id（ステージ変数）
    std::unordered_map<std::string, std::string> globalVarIds;
    // ステージ objName が "Stage" でなかった場合の元名（モニタ target 解決用）
    std::u16string stageOrigName;
    // (target, indexInLibrary) — pane order ソート用。NaN = 未定義
    std::vector<std::pair<Target*, double>> paneOrder;

    std::string addBroadcastMsg(const std::u16string& name, BlockField* field) {
        std::string key = SStr::toUtf8(name);
        for (auto& c : key) c = char(std::tolower((unsigned char)c));
        if (key.empty()) key = emptyMsgName;
        broadcastMsgMap[key] = "broadcastMsgId-" + replaceUnsafe(key);
        allBroadcastFields.push_back(field);
        return broadcastMsgMap[key];
    }
};

/// generateVariableIdGetter: stage は global map 登録、他は参照→namer
struct VarIdGetter {
    Sb2Ctx& ctx;
    std::string targetId;
    bool topLevel;

    std::string operator()(const std::string& name, const char* type) {
        std::string key = name + "-" + type;
        if (topLevel) {
            ctx.globalVarIds[key] =
                targetId + "-" + replaceUnsafe(name) + "-" + type;
            return ctx.globalVarIds[key];
        }
        auto it = ctx.globalVarIds.find(key);
        if (it != ctx.globalVarIds.end()) return it->second;
        return targetId + "-" + replaceUnsafe(name) + "-" + type;
    }
};

struct ParseState {
    const Sb2ArgSpec* expectedArg = nullptr;
};

/// JS のパース中ブロック（children は flatten 前）
struct PB {
    Block block;
    std::vector<PB> children;
};

// ----------------------------------------------------------- parseBlock --

bool parseBlock(const JsonValue& sb2block, Sb2Ctx& ctx,
                VarIdGetter& getVarId, ParseState& st, PB& out);

/// JS parseBlockList — [block...] をパースし next/parent を連結
std::vector<PB> parseBlockList(const JsonValue& blockList, Sb2Ctx& ctx,
                               VarIdGetter& getVarId, ParseState& st) {
    std::vector<PB> result;
    size_t prev = SIZE_MAX;  // ポインタだと push_back の再確保でダングルする
    for (const JsonValue& jblock : blockList.asArray()) {
        PB pb;
        if (!parseBlock(jblock, ctx, getVarId, st, pb)) continue;
        result.push_back(std::move(pb));
        if (prev != SIZE_MAX) {
            result.back().block.parent = result[prev].block.id;
            result[prev].block.next = result.back().block.id;
        }
        prev = result.size() - 1;
    }
    return result;
}

bool parseBlock(const JsonValue& sb2block, Sb2Ctx& ctx,
                VarIdGetter& getVarId, ParseState& st, PB& out) {
    if (!sb2block.isArray() || sb2block.asArray().empty()) return false;
    const std::string oldOpcode = sb2block.asArray()[0].asString();

    const Sb2BlockSpec* meta = nullptr;
    Sb2BlockSpec dyn;   // call 用動的 argMap
    if (oldOpcode == "whenSensorGreaterThan") {
        // specmap の関数エントリ: センサ名が 'video motion' → videoSensing
        const JsonValue& sensor = sb2block.asArray().size() > 1
            ? sb2block.asArray()[1] : JsonValue();
        if (sensor.asString() == "video motion") {
            dyn.opcode = "videoSensing_whenMotionGreaterThan";
            dyn.argMap = {{nullptr, nullptr, nullptr, nullptr, nullptr},
                          {"input", "math_number", "REFERENCE", nullptr, nullptr}};
        } else {
            dyn.opcode = "event_whengreaterthan";
            dyn.argMap = {
                {"field", nullptr, nullptr, "WHENGREATERTHANMENU", nullptr},
                {"input", "math_number", "VALUE", nullptr, nullptr}};
        }
        meta = &dyn;
    } else {
        auto it = sb2SpecMap().find(oldOpcode);
        if (it == sb2SpecMap().end()) return false;  // 未知 op はスキップ
        meta = &it->second;
    }

    out.block.id = uidSeq();
    out.block.opcode = meta->opcode;

    const Sb2ArgSpec* parentExpectedArg = st.expectedArg;

    // call は procCode から argMap を動的生成
    if (oldOpcode == "call") {
        dyn = *meta;
        dyn.argMap = parseProcedureArgMap(
            sb2block.asArray().size() > 1
                ? sb2block.asArray()[1].asString() : "");
        meta = &dyn;
    }

    const auto& arr = sb2block.asArray();
    for (size_t i = 0; i < meta->argMap.size(); ++i) {
        const Sb2ArgSpec& expectedArg = meta->argMap[i];
        const JsonValue& providedArg =
            i + 1 < arr.size() ? arr[i + 1] : JsonValue();
        bool shadowObscured = false;

        if (expectedArg.type && std::strcmp(expectedArg.type, "input") == 0) {
            std::string inputUid = uidSeq();
            std::string inputName =
                expectedArg.inputName ? expectedArg.inputName : "";
            BlockInput input;

            if (providedArg.isArray()) {
                // ブロック or ブロックリストが入力を占有
                ParseState inner;
                inner.expectedArg = &expectedArg;
                std::vector<PB> innerBlocks;
                const auto& pArr = providedArg.asArray();
                if (!pArr.empty() && pArr[0].isArray()) {
                    // ブロックリスト（substack）— [ [b1],[b2]... ]
                    innerBlocks = parseBlockList(providedArg, ctx, getVarId, inner);
                } else {
                    // 単一ブロック — [opcode, arg...]
                    PB ib;
                    if (parseBlock(providedArg, ctx, getVarId, inner, ib))
                        innerBlocks.push_back(std::move(ib));
                }
                if (!innerBlocks.empty()) {
                    for (size_t j = 0; j < innerBlocks.size(); ++j) {
                        innerBlocks[j].block.parent =
                            j == 0 ? out.block.id : innerBlocks[j - 1].block.id;
                    }
                    input.block = innerBlocks[0].block.id;
                    for (PB& b : innerBlocks)
                        out.children.push_back(std::move(b));
                }
                shadowObscured = true;
            }

            // シャドウブロック生成
            if (!expectedArg.inputOp ||
                std::strcmp(expectedArg.inputOp, "boolean") == 0 ||
                std::strcmp(expectedArg.inputOp, "substack") == 0) {
                // JS は innerBlocks が空でも inputs[name]={block:null} を残す
                out.block.inputs.set(inputName, input);
                continue;
            }
            const std::string inputOp = expectedArg.inputOp;
            std::string fieldName = inputName;
            JsonValue fieldValue = providedArg;
            if (inputOp == "math_number" || inputOp == "math_whole_number" ||
                inputOp == "math_positive_number" ||
                inputOp == "math_integer" || inputOp == "math_angle") {
                fieldName = "NUM";
                if (shadowObscured) fieldValue = JsonValue(10.0);
            } else if (inputOp == "text") {
                fieldName = "TEXT";
                if (shadowObscured) fieldValue = JsonValue(std::string());
            } else if (inputOp == "colour_picker") {
                // SB2 色は10進整数 → #rrggbb
                fieldName = "COLOUR";
                if (shadowObscured) {
                    fieldValue = JsonValue(std::string("#990000"));
                } else {
                    char buf[8];
                    uint32_t c = providedArg.isNumber()
                        ? uint32_t(int64_t(providedArg.asNumber())) : 0;
                    std::snprintf(buf, sizeof(buf), "#%06x", c & 0xFFFFFF);
                    fieldValue = JsonValue(std::string(buf));
                }
            } else if (inputOp == "event_broadcast_menu") {
                fieldName = "BROADCAST_OPTION";
                if (shadowObscured) fieldValue = JsonValue(std::string());
            } else if (inputOp == "sensing_of_object_menu") {
                if (shadowObscured) fieldValue = JsonValue(std::string("_stage_"));
                else if (fieldValue.asString() == "Stage")
                    fieldValue = JsonValue(std::string("_stage_"));
            } else if (inputOp == "note") {
                if (shadowObscured) fieldValue = JsonValue(60.0);
            } else if (inputOp == "music.menu.DRUM") {
                if (shadowObscured) fieldValue = JsonValue(1.0);
            } else if (inputOp == "music.menu.INSTRUMENT") {
                if (shadowObscured) fieldValue = JsonValue(1.0);
            } else if (inputOp == "videoSensing.menu.ATTRIBUTE") {
                if (shadowObscured) fieldValue = JsonValue(std::string("motion"));
            } else if (inputOp == "videoSensing.menu.SUBJECT") {
                if (shadowObscured) fieldValue = JsonValue(std::string("this sprite"));
            } else if (inputOp == "videoSensing.menu.VIDEO_STATE") {
                if (shadowObscured) fieldValue = JsonValue(std::string("on"));
            } else if (shadowObscured) {
                fieldValue = JsonValue(std::string());
            }

            PB shadow;
            shadow.block.id = inputUid;
            shadow.block.opcode = inputOp;
            shadow.block.shadow = true;
            shadow.block.parent = out.block.id;
            BlockField f;
            f.value = toValue(fieldValue);
            shadow.block.fields[fieldName] = f;
            if (inputOp == "event_broadcast_menu") {
                shadow.block.fields[fieldName].id = ctx.addBroadcastMsg(
                    toU16(fieldValue), &shadow.block.fields[fieldName]);
            }
            out.children.push_back(std::move(shadow));
            input.shadow = inputUid;
            if (input.block.empty()) input.block = inputUid;
            out.block.inputs.set(inputName, input);
        } else if (expectedArg.type &&
                   std::strcmp(expectedArg.type, "field") == 0) {
            std::string fieldName =
                expectedArg.fieldName ? expectedArg.fieldName : "";
            BlockField f;
            f.value = toValue(providedArg);
            if (fieldName == "CURRENTMENU") {
                std::string v = providedArg.asString();
                for (auto& c : v) c = char(std::toupper((unsigned char)c));
                if (v == "day of week") v = "DAYOFWEEK";
                else if (providedArg.asString() == "day of week") v = "DAYOFWEEK";
                f.value = Value::string(SStr::create(utf8ToU16(v)));
            }
            if (fieldName == "VARIABLE") {
                f.id = getVarId(toU8(providedArg), Variable::kScalarType);
            } else if (fieldName == "LIST") {
                f.id = getVarId(toU8(providedArg), Variable::kListType);
            } else if (fieldName == "BROADCAST_OPTION") {
                out.block.fields[fieldName] = f;
                f.id = ctx.addBroadcastMsg(
                    toU16(providedArg), &out.block.fields[fieldName]);
            }
            out.block.fields[fieldName] = f;
        }
    }

    // 新メニューを持つブロックのフィールド追加
    if (oldOpcode == "comeToFront") {
        BlockField f;
        f.value = Value::string(SStr::create(u"front"));
        out.block.fields["FRONT_BACK"] = f;
    } else if (oldOpcode == "goBackByLayers:") {
        BlockField f;
        f.value = Value::string(SStr::create(u"backward"));
        out.block.fields["FORWARD_BACKWARD"] = f;
    } else if (oldOpcode == "backgroundIndex" ||
               oldOpcode == "costumeIndex") {
        BlockField f;
        f.value = Value::string(SStr::create(u"number"));
        out.block.fields["NUMBER_NAME"] = f;
    } else if (oldOpcode == "sceneName" || oldOpcode == "costumeName") {
        BlockField f;
        f.value = Value::string(SStr::create(u"name"));
        out.block.fields["NUMBER_NAME"] = f;
    }

    // ミューテーション
    if (oldOpcode == "stopScripts") {
        const std::string opt =
            arr.size() > 1 ? arr[1].asString() : "";
        if (opt == "other scripts in sprite" ||
            opt == "other scripts in stage") {
            Mutation m;
            m.tagName = "mutation";
            m.attrs["hasnext"] = "true";
            out.block.mutation = m;
        }
    } else if (oldOpcode == "procDef") {
        // procDef: [proccode, argNames[], argDefaults[], warp]
        std::string procCode = arr.size() > 1 ? arr[1].asString() : "";
        std::string inputUid = uidSeq();
        BlockInput input;
        input.block = inputUid;
        input.shadow = inputUid;
        out.block.inputs.set("custom_block", input);

        PB proto;
        proto.block.id = inputUid;
        proto.block.opcode = "procedures_prototype";
        proto.block.shadow = true;
        Mutation m;
        m.tagName = "mutation";
        m.attrs["proccode"] = procCode;
        m.attrs["argumentnames"] =
            arr.size() > 2 ? strArrayJson(arr[2]) : "[]";
        m.attrs["argumentids"] = parseProcedureArgIdsJson(procCode);
        m.attrs["argumentdefaults"] =
            arr.size() > 3 ? valArrayJson(arr[3]) : "[]";
        bool warp = arr.size() > 4 && arr[4].isBool() && arr[4].asBool();
        m.attrs["warp"] = warp ? "true" : "false";
        proto.block.mutation = std::move(m);
        out.children.push_back(std::move(proto));
    } else if (oldOpcode == "call") {
        Mutation m;
        m.tagName = "mutation";
        m.attrs["proccode"] =
            arr.size() > 1 ? arr[1].asString() : "";
        m.attrs["argumentids"] = parseProcedureArgIdsJson(
            arr.size() > 1 ? arr[1].asString() : "");
        out.block.mutation = std::move(m);
    } else if (oldOpcode == "getParam") {
        std::string returnCode =
            arr.size() > 2 ? arr[2].asString() : "r";
        if (parentExpectedArg && parentExpectedArg->inputOp &&
            std::strcmp(parentExpectedArg->inputOp, "boolean") == 0 &&
            returnCode != "b") {
            returnCode = "b";
        }
        out.block.opcode = returnCode == "b"
            ? "argument_reporter_boolean"
            : "argument_reporter_string_number";
    }
    return true;
}

// -------------------------------------------------------------- flatten --

void flattenInto(std::vector<Block>& out, std::vector<PB>& pbs) {
    for (PB& pb : pbs) {
        out.push_back(std::move(pb.block));
        flattenInto(out, pb.children);
    }
}

// ------------------------------------------------------------ モニタ解析 --

void parseMonitorObject(Runtime& rt, const JsonValue& object,
                        const std::vector<Target*>& targets, Sb2Ctx& ctx,
                        Target* stage) {
    // リスト監視オブジェクトのシム（JS parseScratchObject 冒頭相当）
    std::string cmd = object.get("cmd").asString();
    JsonValue param = object.get("param");
    if (cmd.empty() && object.has("listName")) {
        cmd = "contentsOfList:";
        param = object.get("listName");
    }
    auto specIt = sb2SpecMap().find(cmd);
    if (specIt == sb2SpecMap().end()) return;
    const std::string opcode = specIt->second.opcode;
    std::string extId;
    {
        auto us = opcode.find('_');
        if (us != std::string::npos) extId = opcode.substr(0, us);
    }
    static const std::unordered_set<std::string> kCoreExt = {
        "argument", "control", "data", "event", "looks", "math",
        "motion", "operator", "procedures", "sensing", "sound"};
    if (extId == "videoSensing") return;
    if (!extId.empty() && kCoreExt.count(extId) == 0 &&
        !object.get("visible").asBool()) {
        return;
    }

    Target* target = nullptr;
    if (!object.has("target")) {
        // リストモニタ: listName が一致する変数を持つターゲットを探す
        const std::u16string listName = toU16(object.get("listName"));
        for (Target* t : targets) {
            for (auto& [id, v] : t->variables) {
                if (v.type == Variable::kListType && v.name == listName) {
                    target = t;
                    break;
                }
            }
            if (target) break;
        }
    }
    if (!target) {
        std::u16string tname = toU16(object.get("target"));
        // ステージが改名された場合、元名への参照もステージに解決する
        if (!ctx.stageOrigName.empty() && tname == ctx.stageOrigName)
            tname = u"Stage";
        for (Target* t : targets)
            if (t->sprite && t->sprite->name == tname) { target = t; break; }
    }
    if (!target) return;

    VarIdGetter getVarId{ctx, target->id, false};
    // monitor ブロック: [cmd, param]
    JsonArray blkArr;
    blkArr.push_back(JsonValue(cmd));
    blkArr.push_back(param);
    JsonValue blkJson(blkArr);
    ParseState st;
    PB pb;
    if (!parseBlock(blkJson, ctx, getVarId, st, pb)) return;

    if (cmd == "getVar:") {
        pb.block.id = getVarId(toU8(param), Variable::kScalarType);
    } else if (cmd == "contentsOfList:") {
        pb.block.id = getVarId(toU8(param), Variable::kListType);
    } else {
        pb.block.id = pb.block.opcode;
    }
    pb.block.targetId = target->isStage ? "" : target->id;
    pb.block.isMonitored = object.get("visible").asBool();

    std::vector<Block> flat;
    {
        std::vector<PB> one;
        one.push_back(std::move(pb));
        flattenInto(flat, one);
    }
    // モニタブロック自身の fields を rec.params へ（move 前に退避）
    std::map<std::string, BlockField> monFields = flat[0].fields;
    for (Block& b : flat) {
        if (rt.monitorBlocks->getBlock(b.id)) {
            Block* ex = rt.monitorBlocks->getBlock(b.id);
            ex->isMonitored = b.isMonitored;
            ex->targetId = b.targetId;
        } else {
            rt.monitorBlocks->createBlock(std::move(b));
        }
    }

    std::string mode;
    double m = object.get("mode").asNumber();
    mode = m == 2 ? "large" : m == 3 ? "slider" : "default";

    MonitorRecord rec;
    rec.id = flat[0].id;
    rec.targetId = target->isStage ? "" : target->id;
    rec.spriteName = target->isStage ? u"" : target->sprite->name;
    rec.opcode = opcode;
    for (auto& [k, f] : monFields) {
        if (f.value.isNumber())
            rec.params[k] = jsnum::toString16(f.value.asNumberUnchecked());
        else if (f.value.isString() && f.value.asStrPtr())
            rec.params[k] = f.value.asStrPtr()->view();
    }
    rec.mode = mode;
    rec.sliderMin = object.get("sliderMin").asNumber();
    rec.sliderMax = object.get("sliderMax").asNumber();
    rec.isDiscrete = !object.has("isDiscrete") ||
                     object.get("isDiscrete").asBool();
    rec.x = object.get("x").asNumber();
    rec.y = object.get("y").asNumber();
    rec.visible = object.get("visible").asBool();
    rt.requestAddMonitor(rec);
}

// ---------------------------------------------------------- ターゲット解析 --

Target* parseScratchObject(Sb2Ctx& ctx, const JsonValue& object,
                           const ZipReader* zip, bool topLevel,
                           int parseIndex) {
    Runtime& rt = ctx.rt;
    JsonValue obj = object;  // コピー（objName 書き換えのため）
    if (!obj.has("objName")) return nullptr;

    auto sprite = std::make_unique<Sprite>(&rt);
    std::string objName = obj.get("objName").asString();
    if (topLevel && objName != "Stage") {
        // ステージ名が "Stage" でない — 改名 + 元名を記録（JS 同様）
        ctx.stageOrigName = utf8ToU16(objName);
        objName = "Stage";
    }
    sprite->name = utf8ToU16(objName);
    Sprite* spritePtr = sprite.get();
    rt.ownedSprites.push_back(std::move(sprite));

    // costumes（アセットデータは zip があれば読む、無くても作成）
    for (const auto& c : obj.get("costumes").asArray()) {
        Costume costume;
        costume.name = utf8ToU16(c.get("costumeName").asString());
        costume.md5 = c.get("baseLayerMD5").asString();
        costume.assetId = costume.md5;
        costume.bitmapResolution =
            c.get("bitmapResolution").isNumber()
                ? c.get("bitmapResolution").asNumber() : 1;
        costume.rotationCenterX = c.get("rotationCenterX").asNumber();
        costume.rotationCenterY = c.get("rotationCenterY").asNumber();
        if (zip && !costume.md5.empty()) {
            if (const ZipEntry* e = zip->find(costume.md5))
                zip->extract(*e, costume.data);
            auto dot = costume.md5.rfind('.');
            if (dot != std::string::npos)
                costume.dataFormat = costume.md5.substr(dot + 1);
        }
        if (costume.dataFormat.empty()) costume.dataFormat = "png";
        spritePtr->costumes.push_back(std::move(costume));
    }
    for (const auto& s : obj.get("sounds").asArray()) {
        Sound sound;
        sound.name = utf8ToU16(s.get("soundName").asString());
        sound.md5 = s.get("md5").asString();
        sound.assetId = sound.md5;
        sound.rate = s.get("rate").asNumber();
        sound.sampleCount = s.get("sampleCount").asNumber();
        sound.format = s.get("format").asString();
        sound.soundId = int(spritePtr->sounds.size());
        if (zip && !sound.md5.empty()) {
            if (const ZipEntry* e = zip->find(sound.md5))
                zip->extract(*e, sound.data);
            auto dot = sound.md5.rfind('.');
            if (dot != std::string::npos)
                sound.dataFormat = sound.md5.substr(dot + 1);
        }
        spritePtr->sounds.push_back(std::move(sound));
    }

    spritePtr->blocks = std::make_unique<Blocks>(&rt);
    Target* target = spritePtr->createClone();
    target->isStage = topLevel;
    target->layerOrder = parseIndex;

    VarIdGetter getVarId{ctx, target->id, topLevel};

    // variables: [{name, value, isPersistent}]
    for (const auto& var : obj.get("variables").asArray()) {
        const std::string name = var.get("name").asString();
        bool isCloud = var.get("isPersistent").asBool() && topLevel &&
                       rt.cloudVariableCount < rt.maxCloudVariables;
        Variable v = Variable::create(
            getVarId(name, Variable::kScalarType),
            utf8ToU16(name), Variable::kScalarType, isCloud);
        const JsonValue& val = var.get("value");
        if (val.isNumber()) v.value = Value::num(val.asNumber());
        else if (val.isBool()) v.value = Value::boolean(val.asBool());
        else v.value = Value::string(SStr::create(utf8ToU16(val.asString())));
        if (isCloud) rt.cloudVariableCount++;
        target->variables.emplace(v.id, std::move(v));
    }

    // scripts: [[x, y, [[block...]]]...]
    for (const auto& script : obj.get("scripts").asArray()) {
        const auto& s = script.asArray();
        if (s.size() < 3 || !s[2].isArray()) continue;
        ParseState st;
        std::vector<PB> parsed =
            parseBlockList(s[2], ctx, getVarId, st);
        if (!parsed.empty()) {
            parsed[0].block.x = s[0].asNumber() * 1.5;
            parsed[0].block.y = s[1].asNumber() * 2.2;
            parsed[0].block.topLevel = true;
            parsed[0].block.parent.clear();
        }
        std::vector<Block> flat;
        flattenInto(flat, parsed);
        for (Block& b : flat) spritePtr->blocks->createBlock(std::move(b));
    }

    // ステージ固有ブロック変換（sprite↔stage clicked）
    for (auto& [id, b] : spritePtr->blocks->allBlocks()) {
        if (topLevel && b.opcode == "event_whenthisspriteclicked")
            b.opcode = "event_whenstageclicked";
        else if (!topLevel && b.opcode == "event_whenstageclicked")
            b.opcode = "event_whenthisspriteclicked";
    }

    // lists: [{listName, contents}]
    for (const auto& list : obj.get("lists").asArray()) {
        const std::string name = list.get("listName").asString();
        Variable v = Variable::create(
            getVarId(name, Variable::kListType),
            utf8ToU16(name), Variable::kListType, false);
        if (v.list) {
            for (const JsonValue& e : list.get("contents").asArray()) {
                v.list->push(toValue(e));
            }
        }
        target->variables.emplace(v.id, std::move(v));
    }

    if (obj.has("scratchX")) target->x = obj.get("scratchX").asNumber();
    if (obj.has("scratchY")) target->y = obj.get("scratchY").asNumber();
    if (obj.has("direction")) {
        const double dd = obj.get("direction").asNumber();
        target->direction = dd - std::floor((dd + 179) / 360) * 360;
    }
    if (obj.has("isDraggable"))
        target->draggable = obj.get("isDraggable").asBool();
    if (obj.has("scale")) target->size = obj.get("scale").asNumber() * 100;
    if (obj.has("visible")) target->visible = obj.get("visible").asBool();
    if (obj.has("currentCostumeIndex")) {
        double n = std::floor(obj.get("currentCostumeIndex").asNumber());
        int hi = int(spritePtr->costumes.size()) - 1;
        target->currentCostume = int(std::min(std::max(n, 0.0), double(hi)));
        if (target->currentCostume < 0) target->currentCostume = 0;
    }
    if (obj.has("rotationStyle")) {
        const std::string rs = obj.get("rotationStyle").asString();
        target->rotationStyle =
            rs == "none" ? "don't rotate"
          : rs == "leftRight" ? "left-right"
          : "all around";
    }
    if (obj.has("tempoBPM")) target->tempo = obj.get("tempoBPM").asNumber();
    if (obj.has("videoAlpha"))
        target->videoTransparency = 100 - 100 * obj.get("videoAlpha").asNumber();
    if (obj.has("info") && obj.get("info").has("videoOn"))
        target->videoState = obj.get("info").get("videoOn").asBool() ? "on" : "off";
    // pane order（indexInLibrary。NaN = 未定義 → 元順序維持）
    double po = std::numeric_limits<double>::quiet_NaN();
    if (obj.has("indexInLibrary"))
        po = obj.get("indexInLibrary").asNumber();
    ctx.paneOrder.emplace_back(target, po);
    target->isOriginal = true;
    return target;
}

} // namespace

// ------------------------------------------------------------------ 本体 --

Sb3LoadResult loadSb2(Runtime& runtime, const JsonValue& json,
                      const ZipReader* zip) {
    Sb3LoadResult result;
    Sb2Ctx ctx{runtime};

    // 子オブジェクト: objName あり→sprite、なし→deferred monitor
    std::vector<const JsonValue*> monitorObjects;
    std::vector<const JsonValue*> spriteObjects;
    for (const auto& child : json.get("children").asArray()) {
        if (child.has("objName")) spriteObjects.push_back(&child);
        else monitorObjects.push_back(&child);
    }

    std::vector<Target*> targets;
    int parseIndex = 0;
    Target* stage = parseScratchObject(ctx, json, zip, true, parseIndex++);
    if (!stage) {
        result.error = "sb2: no stage";
        return result;
    }
    targets.push_back(stage);
    for (const JsonValue* c : spriteObjects) {
        if (Target* t = parseScratchObject(ctx, *c, zip, false, parseIndex++))
            targets.push_back(t);
    }

    // ブロードキャストメッセージ変数（ステージ上に作成）
    {
        std::string emptyKey = ctx.emptyMsgName;
        auto it = ctx.broadcastMsgMap.find(emptyKey);
        if (it != ctx.broadcastMsgMap.end()) {
            // 空メッセージ名に fresh 'messageN' を割当
            int idx = 1;
            std::string fresh;
            for (;;) {
                fresh = "message" + std::to_string(idx);
                if (!ctx.broadcastMsgMap.count(fresh)) break;
                ++idx;
            }
            std::string oldId = it->second;
            ctx.broadcastMsgMap.erase(it);
            ctx.broadcastMsgMap[fresh] = oldId;
            for (BlockField* f : ctx.allBroadcastFields) {
                if (f->value.isString() && f->value.asStrPtr() &&
                    f->value.asStrPtr()->view().empty()) {
                    f->value = Value::string(
                        SStr::create(utf8ToU16(fresh)));
                }
            }
        }
        for (auto& [name, id] : ctx.broadcastMsgMap) {
            Variable v = Variable::create(id, utf8ToU16(name),
                                          Variable::kBroadcastType, false);
            stage->variables.emplace(v.id, std::move(v));
        }
    }

    // monitors（全ターゲット作成後）
    for (const JsonValue* m : monitorObjects)
        parseMonitorObject(runtime, *m, targets, ctx, stage);

    // reorderParsedTargets: pane 順でソート。NaN 比較は常に false →
    // 未定義要素は元の相対順序を維持（JS sort + NaN と同等の挙動）
    std::stable_sort(ctx.paneOrder.begin(), ctx.paneOrder.end(),
                     [](const auto& a, const auto& b) {
                         return a.second < b.second;
                     });
    for (auto& [t, po] : ctx.paneOrder) {
        runtime.addTarget(t);
        result.targets.push_back(t);
    }
    std::stable_sort(runtime.executableTargets.begin(),
                     runtime.executableTargets.end(),
                     [](Target* a, Target* b) {
                         return a->layerOrder < b->layerOrder;
                     });
    if (!result.targets.empty()) {
        runtime.editingTarget =
            result.targets.size() > 1 ? result.targets[1]
                                      : result.targets[0];
    }
    result.ok = true;
    return result;
}

} // namespace xyo
