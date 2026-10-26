// xyo-cpp: sb3 minify 実装。JSON と zip を直接操作する（Runtime 不要）。
#include "sb3_minify.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

#include "../core/fileio.hpp"
#include "../core/json.hpp"
#include "../engine/deps.hpp"
#include "../engine/runtime.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "sb3.hpp"
#include "zip.hpp"

namespace xyo {

namespace {

// ---------------------------------------------------------------------------
// JSON ヘルパ
// ---------------------------------------------------------------------------

JsonObject* objectMember(JsonValue& v, const char* key) {
    if (!v.isObject()) return nullptr;
    auto it = v.objectRef().find(key);
    if (it == v.objectRef().end() || !it->second.isObject()) return nullptr;
    return &it->second.objectRef();
}

const JsonObject* objectMember(const JsonValue& v, const char* key) {
    if (!v.isObject()) return nullptr;
    auto it = v.asObject().find(key);
    if (it == v.asObject().end() || !it->second.isObject()) return nullptr;
    return &it->second.asObject();
}

/// 値の中の全文字列を集める（アセット参照・変数使用の保守的な検出）。
void collectStrings(const JsonValue& v, std::set<std::string>& out) {
    if (v.isString()) {
        if (!v.asString().empty()) out.insert(v.asString());
        return;
    }
    if (v.isArray()) {
        for (const JsonValue& e : v.asArray()) collectStrings(e, out);
        return;
    }
    if (v.isObject()) {
        for (const auto& [k, e] : v.asObject()) collectStrings(e, out);
    }
}

// ---------------------------------------------------------------------------
// 到達不能ブロックの除去
// ---------------------------------------------------------------------------

/// topLevel から next / inputs を辿って到達できるブロック id を返す。
/// 配列値のブロック（圧縮プリミティブ）はトップレベルの変数/リスト
/// reporter なので常に残す。
std::set<std::string> reachableBlocks(const JsonObject& blocks) {
    std::set<std::string> reachable;
    std::vector<std::string> work;
    for (const auto& [id, b] : blocks) {
        if (!b.isObject())
            reachable.insert(id);
        else if (b.get("topLevel").asBool())
            work.push_back(id);
    }
    auto push = [&](const JsonValue& v) {
        if (v.isString() && !v.asString().empty())
            work.push_back(v.asString());
    };
    while (!work.empty()) {
        std::string id = std::move(work.back());
        work.pop_back();
        if (!reachable.insert(id).second) continue;
        auto it = blocks.find(id);
        if (it == blocks.end() || !it->second.isObject()) continue;
        const JsonValue& b = it->second;
        push(b.get("next"));
        const JsonValue& inputs = b.get("inputs");
        if (!inputs.isObject()) continue;
        for (const auto& [name, in] : inputs.asObject()) {
            if (in.isArray()) {
                const JsonArray& a = in.asArray();
                for (size_t i = 1; i < a.size(); ++i) push(a[i]);
            } else {
                push(in);
            }
        }
    }
    return reachable;
}

/// 消えたブロックを指す parent/next を null にする。
void dropDanglingRefs(JsonObject& blocks,
                      const std::set<std::string>& kept) {
    for (auto& [id, b] : blocks) {
        if (!b.isObject()) continue;
        JsonObject& o = b.objectRef();
        for (const char* key : {"parent", "next"}) {
            auto it = o.find(key);
            if (it == o.end() || !it->second.isString()) continue;
            const std::string& ref = it->second.asString();
            if (!ref.empty() && !kept.count(ref))
                it->second = JsonValue(nullptr);
        }
    }
}

// ---------------------------------------------------------------------------
// 変数/リスト/ブロードキャスト
// ---------------------------------------------------------------------------

size_t countVariableDecls(const JsonObject& target) {
    size_t n = 0;
    for (const char* key : {"variables", "lists", "broadcasts"}) {
        auto it = target.find(key);
        if (it != target.end() && it->second.isObject())
            n += it->second.asObject().size();
    }
    return n;
}

bool declarationUsed(const std::string& id, const JsonValue& decl,
                     const std::set<std::string>& used) {
    if (used.count(id)) return true;
    std::string name;
    if (decl.isArray() && decl.size() > 0)
        name = decl.at(0).asString();
    else if (decl.isString())
        name = decl.asString();
    // 名前での参照（sensing_of のプロパティ、monitor の params）も拾う。
    return !name.empty() && used.count(name) != 0;
}

/// deps::analyzeVariableUsage の結果（解決済み宣言 id）。
struct VarUsageHint {
    std::set<std::string> usedIds;
    bool precise = false;
};

VarUsageHint analyzeUsedVariables(const std::string& projectJson,
                                  const ZipReader* zip) {
    VarUsageHint hint;
    Runtime rt;
    Sb3LoadResult lr = loadSb3(rt, projectJson, zip);
    if (!lr.ok) return hint;
    const VariableUsage usage = analyzeVariableUsage(rt);
    if (!usage.ok || usage.dynamic) return hint;
    for (const Variable* v : usage.used)
        if (v) hint.usedIds.insert(v->id);
    hint.precise = true;
    return hint;
}

void pruneVariables(JsonObject& target, const std::set<std::string>& used,
                    const VarUsageHint& hint) {
    for (const char* key : {"variables", "lists", "broadcasts"}) {
        auto it = target.find(key);
        if (it == target.end() || !it->second.isObject()) continue;
        // 変数/リストは deps の解決済み id で厳密に判定する。
        // broadcast は動的参照の取りこぼしを避けるため従来の文字列走査を
        // 維持する（analyzeScript は event_whenbroadcastreceived の
        // フィールドを走査しない）。
        const bool precise =
            hint.precise && std::strcmp(key, "broadcasts") != 0;
        JsonObject& m = it->second.objectRef();
        for (auto e = m.begin(); e != m.end();) {
            const bool keep =
                precise ? hint.usedIds.count(e->first) != 0
                        : declarationUsed(e->first, e->second, used);
            if (keep)
                ++e;
            else
                e = m.erase(e);
        }
    }
}

// ---------------------------------------------------------------------------
// ブロック/コメント ID の短縮（TurboWarp tw-compress-sb3.js 準拠）
// ---------------------------------------------------------------------------
//
// 参照回数が多い ID ほど短くする。変数/リスト/ブロードキャストの ID は
// エディタの backpack 等が壊れるため触らない。数値 ID は JS の列挙順で
// 先頭に来てスクリプト順を変えるため使わない（TW と同じ）。

std::string generateShortId(long long i) {
    static const char kSoup[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "!#%()*+,-./:;=?@[]^_`{|}~";
    constexpr long long kN = sizeof(kSoup) - 1;
    std::string s;
    while (i >= 0) {
        s.insert(s.begin(), kSoup[i % kN]);
        i = i / kN - 1;
    }
    return s;
}

struct IdPool {
    std::map<std::string, size_t> refs;
    std::map<std::string, size_t> order;
    std::set<std::string> skipped;

    void add(const std::string& id) {
        if (id.empty()) return;
        auto it = refs.find(id);
        if (it == refs.end()) {
            order.emplace(id, order.size());
            refs.emplace(id, 1);
        } else {
            ++it->second;
        }
    }
};

void countIdRefs(const JsonValue& root, IdPool& pool) {
    if (!root.isObject()) return;
    auto tit = root.asObject().find("targets");
    if (tit == root.asObject().end() || !tit->second.isArray()) return;
    for (const JsonValue& tv : tit->second.asArray()) {
        if (!tv.isObject()) continue;
        const JsonObject& t = tv.asObject();
        for (const char* key : {"variables", "lists", "broadcasts"}) {
            auto it = t.find(key);
            if (it == t.end() || !it->second.isObject()) continue;
            for (const auto& [id, decl] : it->second.asObject()) {
                (void)decl;
                pool.skipped.insert(id);
            }
        }
        auto bit = t.find("blocks");
        if (bit != t.end() && bit->second.isObject()) {
            for (const auto& [id, b] : bit->second.asObject()) {
                pool.add(id);
                if (!b.isObject()) continue;
                const JsonObject& o = b.asObject();
                for (const char* ref : {"parent", "next", "comment"}) {
                    auto it = o.find(ref);
                    if (it != o.end() && it->second.isString())
                        pool.add(it->second.asString());
                }
                auto iit = o.find("inputs");
                if (iit == o.end() || !iit->second.isObject()) continue;
                for (const auto& [name, in] : iit->second.asObject()) {
                    (void)name;
                    if (!in.isArray()) continue;
                    const JsonArray& a = in.asArray();
                    for (size_t i = 1; i < a.size(); ++i)
                        if (a[i].isString()) pool.add(a[i].asString());
                }
            }
        }
        auto cit = t.find("comments");
        if (cit != t.end() && cit->second.isObject()) {
            for (const auto& [id, c] : cit->second.asObject()) {
                pool.add(id);
                if (!c.isObject()) continue;
                auto it = c.asObject().find("blockId");
                if (it != c.asObject().end() && it->second.isString())
                    pool.add(it->second.asString());
            }
        }
    }
    // monitorBlocks の擬似 ID。生成 ID と衝突させない。
    pool.skipped.insert("of");
}

std::map<std::string, std::string> buildIdMap(const IdPool& pool) {
    std::vector<std::pair<std::string, size_t>> entries(pool.refs.begin(),
                                                       pool.refs.end());
    const auto& order = pool.order;
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const auto& a, const auto& b) {
                         if (a.second != b.second) return a.second > b.second;
                         return order.at(a.first) < order.at(b.first);
                     });
    std::map<std::string, std::string> map;
    long long i = 0;
    for (const auto& [oldId, count] : entries) {
        (void)count;
        std::string nid = generateShortId(i);
        while (pool.skipped.count(nid)) nid = generateShortId(++i);
        map.emplace(oldId, nid);
        ++i;
    }
    return map;
}

void remapString(JsonValue& v,
                 const std::map<std::string, std::string>& ids) {
    if (!v.isString()) return;
    auto it = ids.find(v.asString());
    if (it != ids.end()) v = JsonValue(it->second);
}

void remapBlockObject(JsonValue& b,
                      const std::map<std::string, std::string>& ids) {
    if (!b.isObject()) return;
    JsonObject& o = b.objectRef();
    for (const char* key : {"parent", "next", "comment"}) {
        auto it = o.find(key);
        if (it != o.end()) remapString(it->second, ids);
    }
    auto iit = o.find("inputs");
    if (iit == o.end() || !iit->second.isObject()) return;
    for (auto& [name, in] : iit->second.objectRef()) {
        (void)name;
        if (!in.isArray()) continue;
        for (size_t i = 1; i < in.arrayRef().size(); ++i)
            remapString(in.arrayRef()[i], ids);
    }
}

void applyIdMap(JsonObject& target,
                const std::map<std::string, std::string>& ids) {
    auto bit = target.find("blocks");
    if (bit != target.end() && bit->second.isObject()) {
        JsonObject nb;
        for (auto& [id, b] : bit->second.objectRef()) {
            remapBlockObject(b, ids);
            std::string nid = id;
            auto it = ids.find(nid);
            if (it != ids.end()) nid = it->second;
            nb.emplace(std::move(nid), std::move(b));
        }
        bit->second = JsonValue(std::move(nb));
    }
    auto cit = target.find("comments");
    if (cit != target.end() && cit->second.isObject()) {
        JsonObject nc;
        for (auto& [id, c] : cit->second.objectRef()) {
            if (c.isObject()) {
                auto it = c.objectRef().find("blockId");
                if (it != c.objectRef().end()) remapString(it->second, ids);
            }
            std::string nid = id;
            auto it = ids.find(nid);
            if (it != ids.end()) nid = it->second;
            nc.emplace(std::move(nid), std::move(c));
        }
        cit->second = JsonValue(std::move(nc));
    }
}

// ---------------------------------------------------------------------------
// アセット名
// ---------------------------------------------------------------------------

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/// `<md5>.<ext>` または `assets/<md5>.<ext>`。未参照なら捨ててよい
/// （それ以外の名前のエントリは未知のメタデータとして残す）。
bool isContentAddressedAsset(const std::string& name) {
    size_t pos = 0;
    if (name.rfind("assets/", 0) == 0) pos = 7;
    if (name.size() - pos < 34) return false;
    for (size_t i = 0; i < 32; ++i)
        if (!isHex(name[pos + i])) return false;
    return name[pos + 32] == '.' && name.size() > pos + 33;
}

std::string withoutAssetsPrefix(const std::string& name) {
    return name.rfind("assets/", 0) == 0 ? name.substr(7) : name;
}

std::string stemOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// ---------------------------------------------------------------------------
// project.json の最適化
// ---------------------------------------------------------------------------

bool optimizeProject(JsonValue& root, const Sb3MinifyOptions& opt,
                     Sb3MinifyStats& st, const VarUsageHint& varUsage) {
    if (!root.isObject()) return false;
    JsonObject& r = root.objectRef();
    auto tit = r.find("targets");
    if (tit == r.end() || !tit->second.isArray()) return false;
    JsonArray& targets = tit->second.arrayRef();

    for (const JsonValue& tv : targets) {
        if (!tv.isObject()) continue;
        if (const JsonObject* b = objectMember(tv, "blocks"))
            st.blocksBefore += b->size();
        if (const JsonObject* c = objectMember(tv, "comments"))
            st.commentsBefore += c->size();
        st.variablesBefore += countVariableDecls(tv.asObject());
    }

    // 未使用変数の判定に使う文字列集合。宣言マップ自体は除く。
    // monitors やルート直下の拡張データも「使用」とみなす（保守的）。
    std::set<std::string> used;
    if (opt.dropUnusedVariables) {
        for (const auto& [k, e] : r) {
            if (k != "targets") {
                collectStrings(e, used);
                continue;
            }
            for (const JsonValue& tv : targets) {
                if (!tv.isObject()) continue;
                for (const auto& [tk, te] : tv.asObject()) {
                    if (tk == "variables" || tk == "lists" ||
                        tk == "broadcasts")
                        continue;
                    collectStrings(te, used);
                }
            }
        }
    }

    for (JsonValue& tv : targets) {
        if (!tv.isObject()) continue;
        JsonObject& t = tv.objectRef();
        JsonObject* blocks = objectMember(tv, "blocks");

        // 1) 到達不能ブロックの除去
        if (opt.dropOrphanBlocks && blocks) {
            std::set<std::string> kept = reachableBlocks(*blocks);
            for (auto it = blocks->begin(); it != blocks->end();) {
                if (!kept.count(it->first))
                    it = blocks->erase(it);
                else
                    ++it;
            }
            dropDanglingRefs(*blocks, kept);
        }

        // 2) コメントの除去
        JsonObject* comments = objectMember(tv, "comments");
        if (comments) {
            std::set<std::string> removed;
            if (opt.stripComments) {
                for (const auto& [id, c] : *comments) {
                    (void)c;
                    removed.insert(id);
                }
                comments->clear();
            } else {
                for (auto it = comments->begin(); it != comments->end();) {
                    const std::string bid =
                        it->second.get("blockId").asString();
                    if (!bid.empty() && (!blocks || !blocks->count(bid))) {
                        removed.insert(it->first);
                        it = comments->erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (!removed.empty() && blocks) {
                for (auto& [id, b] : *blocks) {
                    (void)id;
                    if (!b.isObject()) continue;
                    JsonObject& o = b.objectRef();
                    auto it = o.find("comment");
                    if (it == o.end() || !it->second.isString()) continue;
                    if (removed.count(it->second.asString())) o.erase(it);
                }
            }
        }
        if (opt.stripComments && blocks) {
            for (auto& [id, b] : *blocks) {
                (void)id;
                if (b.isObject()) b.objectRef().erase("comment");
            }
        }

        // 3) 未使用の変数/リスト/ブロードキャスト（任意）
        if (opt.dropUnusedVariables) pruneVariables(t, used, varUsage);
    }

    // 4) ブロック/コメント ID の短縮
    if (opt.compactIds) {
        IdPool pool;
        countIdRefs(root, pool);
        const std::map<std::string, std::string> ids = buildIdMap(pool);
        for (JsonValue& tv : targets)
            if (tv.isObject()) applyIdMap(tv.objectRef(), ids);
    }

    for (const JsonValue& tv : targets) {
        if (!tv.isObject()) continue;
        if (const JsonObject* b = objectMember(tv, "blocks"))
            st.blocksAfter += b->size();
        if (const JsonObject* c = objectMember(tv, "comments"))
            st.commentsAfter += c->size();
        st.variablesAfter += countVariableDecls(tv.asObject());
    }
    return true;
}

} // namespace

Sb3MinifyResult minifySb3(const uint8_t* data, size_t size,
                          const Sb3MinifyOptions& options) {
    Sb3MinifyResult result;
    result.stats.inputBytes = size;

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
    if (!root.isObject()) {
        result.error = "project.json parse failed";
        return result;
    }

    Sb3MinifyOptions opt = options;
    if (opt.level < 0) opt.level = 0;
    if (opt.level > 9) opt.level = 9;
    VarUsageHint varUsage;
    if (opt.dropUnusedVariables) {
        const std::string projectText(
            reinterpret_cast<const char*>(jsonData.data()), jsonData.size());
        varUsage = analyzeUsedVariables(projectText, &zip);
    }
    if (!optimizeProject(root, opt, result.stats, varUsage)) {
        result.error = "not an sb3 project.json (no targets array)";
        return result;
    }

    std::string json = jsonStringify(root);
    result.stats.projectJsonOut = json.size();

    // アセット GC 用の参照集合（ID 短縮後でも md5ext は不変）。
    std::set<std::string> refs;
    collectStrings(root, refs);

    ZipWriter zw;
    const bool deflate = opt.compress && opt.level > 0;
    std::vector<uint8_t> projectBytes(json.begin(), json.end());
    if (deflate)
        zw.addFileCompressed("project.json", projectBytes, opt.level);
    else
        zw.addFile("project.json", projectBytes);

    std::map<std::string, std::pair<uLong, size_t>> seen;
    for (const ZipEntry& e : zip.entries()) {
        if (e.name == "project.json") continue;
        if (!e.name.empty() && e.name.back() == '/') continue;
        ++result.stats.assetsBefore;
        std::vector<uint8_t> asset;
        if (!zip.extract(e, asset)) {
            result.error = "asset extract failed: " + e.name;
            return result;
        }
        if (opt.dropUnusedAssets && isContentAddressedAsset(e.name)) {
            const std::string base = withoutAssetsPrefix(e.name);
            const std::string stem = stemOf(base);
            const bool used = refs.count(e.name) || refs.count(base) ||
                              refs.count(stem);
            if (!used) {
                ++result.stats.assetsDropped;
                continue;
            }
        }
        const uLong crc = crc32(0, asset.data(), (uInt)asset.size());
        auto ins = seen.emplace(e.name, std::make_pair(crc, asset.size()));
        if (!ins.second) {
            const auto& prev = ins.first->second;
            if (prev.first == crc && prev.second == asset.size()) {
                ++result.stats.assetsDeduped;
                continue;
            }
        }
        if (deflate && zipShouldCompress(e.name))
            zw.addFileCompressed(e.name, asset, opt.level);
        else
            zw.addFile(e.name, asset);
        ++result.stats.assetsAfter;
    }

    result.stats.zipCompressed = zw.compressedCount();
    result.stats.zipStored = zw.entryCount() - zw.compressedCount();
    result.data = zw.finish();
    if (result.data.empty()) {
        result.error = "zip write failed";
        return result;
    }
    result.stats.outputBytes = result.data.size();
    result.ok = true;
    return result;
}

Sb3MinifyResult minifySb3File(const char* inPath, const char* outPath,
                              const Sb3MinifyOptions& options) {
    Sb3MinifyResult result;
    std::ifstream in(pathFromUtf8(inPath), std::ios::binary);
    if (!in) {
        result.error = std::string("cannot read ") + inPath;
        return result;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        result.error = std::string("cannot read ") + inPath;
        return result;
    }
    result = minifySb3(data.data(), data.size(), options);
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
