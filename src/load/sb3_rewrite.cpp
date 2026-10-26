// xyo-cpp: sb3 の機械的な書き換え（意味を変えない範囲）。
#include "load/sb3_rewrite.hpp"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/fileio.hpp"
#include "core/json.hpp"
#include "load/sb3_minify.hpp"
#include "load/zip.hpp"

namespace xyo {
namespace {

const std::string& str(const JsonValue& v) { return v.asString(); }

bool isLoopOp(const std::string& op) {
    return op == "control_forever" || op == "control_repeat" ||
           op == "control_repeat_until" || op == "control_while" ||
           op == "control_for_each";
}

/// ループ内で他のスクリプトへ実行機会を渡す op（= その手続きは warp にできない）
bool isYieldOp(const std::string& op) {
    static const std::unordered_set<std::string> k = {
        "control_wait", "control_wait_until",
        "event_broadcastandwait",
        "looks_sayforsecs", "looks_thinkforsecs",
        "looks_switchbackdroptoandwait",
        "sound_playuntildone",
        "sound_seteffectto", "sound_changeeffectby",
        "sound_setvolumeto", "sound_changevolumeby",
        "sensing_askandwait",
        "motion_glideto", "motion_glidesecstoxy",
        "control_deleteclone",
    };
    return k.count(op) != 0;
}

struct Subtree {
    size_t blocks = 0;
    bool loop = false;
    bool yields = false;
};

void walkTree(const JsonObject& blocks, const std::string& id, Subtree& st,
              int depth = 0) {
    if (id.empty() || depth > 1000) return;
    auto it = blocks.find(id);
    if (it == blocks.end()) return;
    const JsonValue& b = it->second;
    ++st.blocks;
    const std::string op = str(b.get("opcode"));
    if (isLoopOp(op)) st.loop = true;
    if (isYieldOp(op)) st.yields = true;
    for (const auto& [name, arr] : b.get("inputs").asObject()) {
        (void)name;
        if (arr.isArray() && arr.size() >= 2 && arr.at(1).isString())
            walkTree(blocks, arr.at(1).asString(), st, depth + 1);
    }
    walkTree(blocks, str(b.get("next")), st, depth + 1);
}

/// 変数 id → 書き込み/読み出し回数
struct VarUse {
    size_t writes = 0;
    size_t reads = 0;
    std::string setBlock;   // 唯一の書き込みが data_setvariableto のときその id
};

std::string varIdOf(const JsonValue& b) {
    const JsonValue& f = b.get("fields").get("VARIABLE");
    if (!f.isArray() || f.size() < 2) return "";
    return f.at(1).isString() ? f.at(1).asString() : "";
}

bool isWriteOp(const std::string& op) {
    return op == "data_setvariableto" || op == "data_changevariableby";
}

/// 式が純粋か（評価位置を数ブロック動かしても観測できないか）。
bool isPureExpr(const JsonObject& blocks, const std::string& id,
                int depth = 0) {
    if (id.empty() || depth > 64) return true;
    auto it = blocks.find(id);
    if (it == blocks.end()) return true;   // プリミティブ配列は定数
    const JsonValue& b = it->second;
    const std::string op = str(b.get("opcode"));
    if (op == "procedures_call" || op.rfind("sensing_", 0) == 0) return false;
    for (const char* pre : {"control_", "event_", "looks_", "motion_", "pen_",
                            "sound_", "music_", "video_"}) {
        if (op.rfind(pre, 0) == 0) return false;
    }
    if (isWriteOp(op) || op == "data_replaceitemoflist" ||
        op == "data_addtolist" || op == "data_deleteoflist" ||
        op == "data_deletealloflist" || op == "data_insertatlist")
        return false;
    for (const auto& [name, arr] : b.get("inputs").asObject()) {
        (void)name;
        if (arr.isArray() && arr.size() >= 2 && arr.at(1).isString() &&
            !isPureExpr(blocks, arr.at(1).asString(), depth + 1))
            return false;
    }
    return true;
}

/// 入力に埋め込まれた変数レポータ `[12, name, id]` を数える。
void countPrimReads(const JsonValue& v, std::map<std::string, size_t>* out) {
    if (v.isArray()) {
        const JsonArray& a = v.asArray();
        if (a.size() == 3 && a[0].isNumber() && a[0].asNumber() == 12 &&
            a[2].isString())
            ++(*out)[a[2].asString()];
        for (const JsonValue& e : a) countPrimReads(e, out);
    } else if (v.isObject()) {
        for (const auto& [k, e] : v.asObject()) {
            (void)k;
            if (k == "fields") continue;   // VARIABLE/LIST フィールドは読み出しではない
            countPrimReads(e, out);
        }
    }
}

/// ブロック木の中で var を読む入力サイト（owner, 入力名）を集める。
void collectSites(const JsonObject& blocks, const std::string& id,
                  const std::string& var,
                  std::vector<std::pair<std::string, std::string>>* out,
                  int depth) {
    if (id.empty() || depth > 64) return;
    auto it = blocks.find(id);
    if (it == blocks.end()) return;
    const JsonValue& b = it->second;
    if (str(b.get("opcode")) == "data_variable" && varIdOf(b) == var) {
        const std::string p = str(b.get("parent"));
        if (!p.empty()) {
            auto pit = blocks.find(p);
            if (pit == blocks.end()) return;
            for (const auto& [n2, a2] : pit->second.get("inputs").asObject()) {
                if (a2.isArray() && a2.size() >= 2 && a2.at(1).isString() &&
                    a2.at(1).asString() == id) {
                    out->push_back({p, n2});
                    return;
                }
            }
        }
        return;
    }
    for (const auto& [name, arr] : b.get("inputs").asObject()) {
        if (!arr.isArray() || arr.size() < 2) continue;
        const JsonValue& e = arr.at(1);
        if (e.isArray() && e.size() == 3 && e.at(0).isNumber() &&
            e.at(0).asNumber() == 12 && e.at(2).isString() &&
            e.at(2).asString() == var) {
            out->push_back({id, name});
        } else if (e.isString()) {
            collectSites(blocks, e.asString(), var, out, depth + 1);
        }
    }
}

/// ブロック id の parent を差し替える（next 連鎖 or SUBSTACK 入力のどちらか）。
void repointParent(JsonObject& blocks, const std::string& parent,
                   const std::string& from, const std::string& to) {
    auto pit = blocks.find(parent);
    if (pit == blocks.end()) return;
    JsonValue& p = pit->second;
    if (p.get("next").isString() && p.get("next").asString() == from) {
        p.objectRef()["next"] = to;
        return;
    }
    for (auto& [name, arr] : p.objectRef()["inputs"].objectRef()) {
        (void)name;
        if (arr.isArray() && arr.size() >= 2 && arr.at(1).isString() &&
            arr.at(1).asString() == from) {
            arr.arrayRef()[1] = to;
            return;
        }
    }
}


} // namespace

Sb3RewriteResult rewriteSb3(const uint8_t* data, size_t size,
                           const Sb3RewriteOptions& options) {
    Sb3RewriteResult result;
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
    result.stats.projectJsonIn = jsonData.size();
    JsonValue root = jsonParse(std::string_view(
        reinterpret_cast<const char*>(jsonData.data()), jsonData.size()));
    if (!root.isObject() || !root.get("targets").isArray()) {
        result.error = "not an sb3 project.json";
        return result;
    }

    JsonArray& targets = root.objectRef()["targets"].arrayRef();
    for (const JsonValue& t : targets)
        result.stats.blocksBefore += t.get("blocks").size();

    // ---- 事前集計: 変数の使用回数・monitor・動的参照 --------------------
    std::map<std::string, VarUse> uses;
    std::set<std::string> monitored;
    bool hasSensingOf = false;
    for (JsonValue& t : targets) {
        for (const auto& [bid, b] : t.get("blocks").asObject()) {
            (void)bid;
            const std::string op = str(b.get("opcode"));
            if (op == "sensing_of") hasSensingOf = true;
            if (isWriteOp(op)) {
                const std::string v = varIdOf(b);
                if (v.empty()) continue;
                VarUse& u = uses[v];
                ++u.writes;
                if (op == "data_setvariableto") u.setBlock = bid;
            } else if (op == "data_variable") {
                const std::string v = varIdOf(b);
                if (!v.empty()) ++uses[v].reads;
            }
        }
    }
    for (const JsonValue& m : root.get("monitors").asArray()) {
        const JsonValue& v = m.get("params").get("VARIABLE");
        if (v.isString() && !v.asString().empty()) monitored.insert(v.asString());
    }

    // ---- 1) warp 化 ----------------------------------------------------
    for (JsonValue& t : targets) {
        JsonObject& blocks = t.objectRef()["blocks"].objectRef();
        std::vector<std::string> defs;
        for (const auto& [bid, b] : blocks)
            if (str(b.get("opcode")) == "procedures_definition")
                defs.push_back(bid);
        for (const std::string& defId : defs) {
            JsonValue& def = blocks[defId];
            const std::string body = str(def.get("next"));
            if (body.empty()) continue;
            ++result.stats.procsScanned;
            // prototype（mutation はこちらにある）
            std::string proto;
            const JsonValue& cb = def.get("inputs").get("custom_block");
            if (cb.isArray() && cb.size() >= 2 && cb.at(1).isString())
                proto = cb.at(1).asString();
            if (proto.empty()) {
                // `custom_block` は shadow 側に入ることがある
                const JsonValue& sh = cb.isArray() && cb.size() >= 3
                                          ? cb.at(2) : JsonValue();
                (void)sh;
            }
            if (proto.empty() || !blocks.count(proto)) continue;
            JsonValue& mut = blocks[proto].objectRef()["mutation"];
            if (!mut.isObject()) continue;
            if (str(mut.get("warp")) == "true") continue;
            if (!options.warp) continue;
            Subtree st;
            walkTree(blocks, body, st);
            if (!st.loop || st.yields) continue;
            if (st.blocks < options.warpMinBlocks) continue;
            mut.objectRef()["warp"] = std::string("true");
            ++result.stats.procsWarped;
            const std::string proccode = str(mut.get("proccode"));
            result.stats.log.push_back("warp: " + std::string(t.get("name").asString()) +
                                       " '" + proccode + "' (" +
                                       std::to_string(st.blocks) + " blocks)");
        }
    }

    // ---- 2) 一時変数のインライン化 -------------------------------------
    // 変数の読み出しは `data_variable` ブロックと、入力に直接埋め込まれた
    // プリミティブ `[12, name, id]`（TW/Scratch 3 の圧縮表現）の両方がある。
    if (options.inlineTemps && !hasSensingOf) {
        // project 全体の読み出し数を数える（プリミティブ + ブロック）。
        std::map<std::string, size_t> reads;
        for (const JsonValue& t : targets)
            for (const auto& [bid, b] : t.get("blocks").asObject()) {
                (void)bid;
                countPrimReads(b, &reads);
            }
        for (JsonValue& t : targets) {
            JsonObject& blocks = t.objectRef()["blocks"].objectRef();
            std::vector<std::string> candidates;
            for (const auto& [bid, b] : blocks) {
                if (str(b.get("opcode")) != "data_setvariableto") continue;
                const std::string v = varIdOf(b);
                if (v.empty() || monitored.count(v)) continue;
                auto u = uses.find(v);
                if (u == uses.end() || u->second.writes != 1) continue;
                if (u->second.setBlock != bid) continue;
                auto r = reads.find(v);
                if (r == reads.end() || r->second != 1) continue;
                candidates.push_back(bid);
            }
            for (const std::string& setId : candidates) {
                auto sit = blocks.find(setId);
                if (sit == blocks.end()) continue;
                const JsonValue setBlk = sit->second;   // erase するのでコピー
                const std::string var = varIdOf(setBlk);
                const std::string next = str(setBlk.get("next"));
                if (next.empty() || !blocks.count(next)) continue;
                const std::string nextOp = str(blocks[next].get("opcode"));
                if (nextOp != "control_if" && nextOp != "control_if_else" &&
                    nextOp != "control_repeat_until" && nextOp != "control_while" &&
                    nextOp != "control_wait_until")
                    continue;
                const JsonValue cond = blocks[next].get("inputs").get("CONDITION");
                if (!cond.isArray() || cond.size() < 2) continue;
                // 条件の中の読み出しサイト（owner ブロック + 入力名）を集める
                std::vector<std::pair<std::string, std::string>> sites;
                if (cond.at(1).isString()) {
                    if (!blocks.count(cond.at(1).asString())) continue;
                    collectSites(blocks, cond.at(1).asString(), var, &sites, 0);
                }
                if (sites.size() != 1) continue;   // 条件内で 1 回だけ読む
                // 値の式（VALUE の 1 番目）が純粋であること
                const JsonValue& valArr = setBlk.get("inputs").get("VALUE");
                if (!valArr.isArray() || valArr.size() < 2) continue;
                const JsonValue val = valArr.at(1);
                if (val.isString() && !isPureExpr(blocks, val.asString()))
                    continue;
                // 置換
                const auto& [owner, input] = sites[0];
                JsonValue& dst = blocks[owner].objectRef()["inputs"]
                                        .objectRef()[input];
                JsonArray repl;
                if (val.isString()) {
                    blocks[val.asString()].objectRef()["parent"] = owner;
                    repl.push_back(JsonValue(3.0));
                    repl.push_back(val);
                    if (dst.isArray() && dst.size() >= 3)
                        repl.push_back(dst.at(2));   // 元の shadow を引き継ぐ
                } else {
                    repl.push_back(JsonValue(1.0));
                    repl.push_back(val);
                }
                dst = JsonValue(repl);
                // set を鎖から外す
                const std::string setParent = str(setBlk.get("parent"));
                if (setParent.empty()) {
                    JsonValue& nb = blocks[next];
                    nb.objectRef()["parent"] = JsonValue();
                    nb.objectRef()["topLevel"] = true;
                    nb.objectRef()["x"] = setBlk.get("x");
                    nb.objectRef()["y"] = setBlk.get("y");
                } else if (blocks.count(setParent)) {
                    repointParent(blocks, setParent, setId, next);
                    blocks[next].objectRef()["parent"] = setParent;
                } else {
                    continue;
                }
                blocks.erase(setId);
                ++result.stats.tempsInlined;
                result.stats.log.push_back(
                    "inline temp: " + std::string(t.get("name").asString()) + " " +
                    var + " -> " + nextOp);
            }
        }
    }

    // ---- 書き出し -------------------------------------------------------
    for (const JsonValue& t : targets)
        result.stats.blocksAfter += t.get("blocks").size();

    const std::string json = jsonStringify(root);
    result.stats.projectJsonOut = json.size();

    ZipWriter zw;
    const std::vector<uint8_t> projectBytes(json.begin(), json.end());
    zw.addFileCompressed("project.json", projectBytes, 9);
    for (const ZipEntry& e : zip.entries()) {
        if (e.name == "project.json") continue;
        if (!e.name.empty() && e.name.back() == '/') continue;
        std::vector<uint8_t> asset;
        if (!zip.extract(e, asset)) {
            result.error = "asset extract failed: " + e.name;
            return result;
        }
        if (zipShouldCompress(e.name))
            zw.addFileCompressed(e.name, asset, 9);
        else
            zw.addFile(e.name, asset);
    }
    result.data = zw.finish();
    if (result.data.empty()) {
        result.error = "zip write failed";
        return result;
    }
    // ---- 仕上げの minify（任意） ---------------------------------------
    if (options.minify) {
        Sb3MinifyOptions mo;
        mo.compactIds = true;
        mo.dropOrphanBlocks = true;
        mo.dropUnusedVariables = options.minifyAggressive;
        Sb3MinifyResult m = minifySb3(result.data.data(), result.data.size(), mo);
        if (!m.ok) {
            result.error = "minify failed: " + m.error;
            return result;
        }
        result.stats.blocksBefore = result.stats.blocksBefore;   // 変更前の値を保つ
        result.data = m.data;
    }
    result.stats.outputBytes = result.data.size();
    result.ok = true;
    return result;
}

Sb3RewriteResult rewriteSb3File(const char* inPath, const char* outPath,
                               const Sb3RewriteOptions& options) {
    Sb3RewriteResult result;
    FILE* in = fopenUtf8(inPath, "rb");
    if (!in) {
        result.error = std::string("cannot read ") + inPath;
        return result;
    }
    std::vector<uint8_t> data;
    uint8_t buf[1 << 16];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0)
        data.insert(data.end(), buf, buf + n);
    std::fclose(in);
    result = rewriteSb3(data.data(), data.size(), options);
    if (!result.ok) return result;
    FILE* o = fopenUtf8(outPath, "wb");
    if (!o) {
        result.ok = false;
        result.error = std::string("cannot write ") + outPath;
        return result;
    }
    const size_t wrote =
        std::fwrite(result.data.data(), 1, result.data.size(), o);
    const bool closeOk = std::fclose(o) == 0;
    if (wrote != result.data.size() || !closeOk) {
        result.ok = false;
        result.error = std::string("cannot write ") + outPath;
    }
    return result;
}

} // namespace xyo
