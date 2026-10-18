// xyo-cpp: engine/blocks.js 相当 — ブロックコンテナ + 実行キャッシュ。
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/json.hpp"
#include "../core/value.hpp"

namespace xyo {

/// UTF-8 → UTF-16 変換（sb3 JSON 文字列用）
std::u16string utf8ToU16(std::string_view utf8);

struct BlockCached;
struct Sequencer;
struct BlockUtility;
struct Runtime;

/// プリミティブ関数型: fn(args, util) → Value（undefined = 報告なし）
struct ArgValues;
using PrimFn = Value (*)(const ArgValues&, BlockUtility&);

/// VARIABLE/LIST/BROADCAST_OPTION 等の {id, name} 参照。
struct IdName {
    std::string id;
    std::u16string name;
};

/// BlockCached._argValues に対応。フィールド値と入力報告値を保持。
struct ArgValues {
    std::map<std::string, Value> vals;
    std::map<std::string, IdName> refs;   // VARIABLE/LIST/BROADCAST_OPTION
    const struct Mutation* mutation = nullptr;

    /// args.NAME — 未設定は undefined
    Value get(const std::string& name) const {
        auto it = vals.find(name);
        return it == vals.end() ? Value::undefined() : it->second;
    }
    /// hasOwnProperty(args, name) 相当
    bool has(const std::string& name) const {
        return vals.count(name) != 0 || refs.count(name) != 0;
    }
    Value& ref(const std::string& name) { return vals[name]; }
    IdName& refObj(const std::string& name) { return refs[name]; }
    const IdName* getObj(const std::string& name) const {
        auto it = refs.find(name);
        return it == refs.end() ? nullptr : &it->second;
    }
};

struct BlockField {
    Value value = Value::emptyStr();   // フィールド値（数値または文字列）
    std::string id;                    // VARIABLE/LIST/BROADCAST_OPTION 参照 id（空=なし）
};

struct BlockInput {
    std::string block;    // 接続ブロック id（空=なし）
    std::string shadow;   // シャドウブロック id（空=なし）
};

/// 入力名→BlockInput。挿入順を保持（JS オブジェクトのキー順＝評価順）。
struct InputSet {
    std::vector<std::pair<std::string, BlockInput>> items;

    const BlockInput* find(const std::string& n) const {
        for (auto& [k, v] : items) if (k == n) return &v;
        return nullptr;
    }
    BlockInput* find(const std::string& n) {
        for (auto& [k, v] : items) if (k == n) return &v;
        return nullptr;
    }
    bool has(const std::string& n) const { return find(n) != nullptr; }
    void set(const std::string& n, BlockInput in) {
        if (auto* p = find(n)) { *p = std::move(in); return; }
        items.emplace_back(n, std::move(in));
    }
    void erase(const std::string& n) {
        for (auto it = items.begin(); it != items.end(); ++it)
            if (it->first == n) { items.erase(it); return; }
    }
    bool empty() const { return items.empty(); }
    size_t size() const { return items.size(); }
};

/// ミューテーション（プロシージャ・control_stop hasnext 等）。
/// sb3 の mutation 属性をそのまま保持 + プロシージャ用パース済み配列。
struct Mutation {
    std::string tagName;
    std::map<std::string, std::string> attrs;   // 生属性（XML 属性値、文字列）

    // procedures_* 用パース済みキャッシュ（最初のアクセスで構築）
    mutable bool parsed = false;
    mutable std::vector<std::string> argumentIds;       // arg0.. → 入力名
    mutable std::vector<std::u16string> argumentNames;  // 表示名
    mutable std::vector<Value> argumentDefaults;

    const std::string& attr(const std::string& key) const {
        static const std::string kEmpty;
        auto it = attrs.find(key);
        return it == attrs.end() ? kEmpty : it->second;
    }
    bool has(const std::string& key) const { return attrs.count(key) != 0; }
    /// warp は bool でも "true"/"false" 文字列でも来る（sb3）
    bool warp() const;
    /// procedures_call の return 属性の有無（reporter 判定）
    bool isReporterCall() const { return has("return"); }
    /// argumentids/argumentnames/argumentdefaults を JSON パース
    void parseArgs() const;
};

struct Block {
    std::string id;
    std::string opcode;
    InputSet inputs;
    std::map<std::string, BlockField> fields;
    std::string next;      // "" = null
    std::string parent;    // "" = null
    bool topLevel = false;
    double x = 0;
    double y = 0;
    bool shadow = false;
    bool isMonitored = false;
    std::string targetId;  // モニタブロック用
    std::optional<Mutation> mutation;
    std::string comment;
};

/// BlocksRuntimeCache.RuntimeScriptCache — startHats 用トップブロック情報。
struct RuntimeScriptCache {
    std::string blockId;
    /// トップブロック（blocks_ は unordered_map でポインタ安定 —
    /// JIT の compile ヒントとして使う）
    const Block* block = nullptr;
    /// fields（空なら全入力ブロックの fields をマージ）。
    /// 文字列値は大文字化済み。JS の {id,value}.value === match の厳密比較
    /// を再現するため Value として保持（数値フィールドはそのまま）。
    std::map<std::string, Value> fieldsOfInputs;
};

class Blocks {
public:
    explicit Blocks(Runtime* runtime, bool forceNoGlow = false);

    Runtime* runtime;
    bool forceNoGlow;

    Block* getBlock(const std::string& id);
    const Block* getBlock(const std::string& id) const;
    std::string getNextBlock(const std::string& id) const;
    /// Cブロックの分岐先（SUBSTACK / SUBSTACK2...）
    std::string getBranch(const std::string& id, int branchNum) const;
    std::string getOpcode(const Block* block) const;
    const std::map<std::string, BlockField>* getFields(const Block* block) const;
    /// 非ブランチ入力（SUBSTACK* を除く）— キャッシュ
    const InputSet* getInputs(const Block* block) const;
    const Mutation* getMutation(const Block* block) const;
    std::string getTopLevelScript(const std::string& id) const;

    /// トップレベルスクリプト id 一覧
    const std::vector<std::string>& getScripts() const { return scripts_; }

    /// proccode は UTF-8（mutation の生属性値と同型）で比較
    std::string getProcedureDefinition(const std::string& proccode);
    /// [names, ids, defaults] — 見つからなければ false
    bool getProcedureParamNamesIdsAndDefaults(
        const std::string& proccode,
        std::vector<std::u16string>& names,
        std::vector<std::string>& ids,
        std::vector<Value>& defaults);
    void populateProcedureCache();

    /// BlocksExecuteCache.getCached — ブロックごとの BlockCached（ops フラット化済み）
    BlockCached* getCachedExecute(const std::string& blockId);
    /// BlocksRuntimeCache.getScripts — opcode 一致するトップレベルスクリプト
    const std::vector<RuntimeScriptCache>& getRuntimeScripts(const std::string& opcode);

    void createBlock(Block block);
    void deleteBlock(const std::string& blockId);
    void resetCache();
    void runAllMonitored(Runtime& rt);

    size_t size() const { return blocks_.size(); }
    /// 内部イテレーション用（fixSporkCompatibility 等）
    std::unordered_map<std::string, Block>& allBlocks() { return blocks_; }

private:
    std::unordered_map<std::string, Block> blocks_;
    std::vector<std::string> scripts_;

    struct Cache {
        std::unordered_map<std::string, InputSet> inputs;
        std::unordered_map<std::string, std::string> procedureDefinitions; // proccode(u8キー) → blockId
        std::unordered_map<std::string, std::string> procedureParamNames;  // → JSON文字列として [names,ids,defaults] エンコード
        std::unordered_map<std::string, std::unique_ptr<BlockCached>> executeCached;
        std::unordered_map<std::string, std::vector<RuntimeScriptCache>> scripts;
        std::optional<std::vector<std::pair<std::string, std::string>>> monitored; // blockId → targetId
        bool proceduresPopulated = false;
    };

private:
    mutable Cache cache_;   // 遅延構築キャッシュ（const メソッドからも充填）

    void addScript(const std::string& topBlockId);
    void deleteScript(const std::string& topBlockId);

public:
    /// GC マーク用にキャッシュを公開
    Cache& cache() { return cache_; }
};

/// execute.js BlockCached — _ops フラット化実行ユニット。
/// 各入力子ブロックの _ops が順に連結され、最後に自分が push される。
struct BlockCached {
    std::string id;
    std::string opcode;
    std::map<std::string, BlockField> fields;
    InputSet inputs;
    const Mutation* mutation = nullptr;

    bool isHat = false;
    PrimFn blockFunction = nullptr;
    bool definedBlockFunction = false;
    bool isConditional = false;   // _flowing.conditional（拡張用）
    bool isLoop = false;
    bool isShadowBlock = false;
    Value shadowValue = Value::undefined();

    ArgValues argValues;

    /// この op の報告値を親ブロックの argValues[inputName] に書き込むリンク。
    std::string parentKey;
    ArgValues* parentValues = nullptr;

    /// フラット化済み実行列（子 ops …→ 自分）。所有権は Blocks のキャッシュ内
    /// 個別 BlockCached。_ops は非所有ポインタ列。
    std::vector<BlockCached*> ops;
};

} // namespace xyo
