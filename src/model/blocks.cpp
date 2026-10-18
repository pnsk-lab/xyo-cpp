// xyo-cpp: engine/blocks.js 相当の実装。
#include "blocks.hpp"

#include "../core/cast.hpp"
#include "../engine/runtime.hpp"

namespace xyo {

std::u16string utf8ToU16(std::string_view utf8) {
    std::u16string out;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = utf8[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; n = 4; }
        else { cp = 0xFFFD; n = 1; }
        if (i + n <= utf8.size()) {
            for (size_t k = 1; k < n; ++k)
                cp = (cp << 6) | (utf8[i + k] & 0x3F);
        }
        i += n;
        if (cp <= 0xFFFF) {
            out.push_back(char16_t(cp));
        } else {
            cp -= 0x10000;
            out.push_back(char16_t(0xD800 + (cp >> 10)));
            out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

// ---------------------------------------------------------------- Mutation --

bool Mutation::warp() const {
    const std::string& w = attr("warp");
    if (w == "true") return true;
    if (w == "false" || w.empty()) return false;
    JsonValue j = jsonParse(w);
    return j.isBool() ? j.asBool() : false;
}

void Mutation::parseArgs() const {
    if (parsed) return;
    parsed = true;
    auto parseStrArr = [](const std::string& s, auto push) {
        JsonValue j = jsonParse(s);
        if (!j.isArray()) return;
        for (auto& e : j.asArray()) {
            if (e.isString()) push(e.asString());
        }
    };
    parseStrArr(attr("argumentids"), [&](const std::string& s) {
        argumentIds.push_back(s);
    });
    parseStrArr(attr("argumentnames"), [&](const std::string& s) {
        argumentNames.push_back(utf8ToU16(s));
    });
    JsonValue defs = jsonParse(attr("argumentdefaults"));
    if (defs.isArray()) {
        for (auto& e : defs.asArray()) {
            if (e.isNumber()) argumentDefaults.push_back(Value::num(e.asNumber()));
            else if (e.isBool()) argumentDefaults.push_back(Value::boolean(e.asBool()));
            else if (e.isString())
                argumentDefaults.push_back(Value::string(SStr::create(utf8ToU16(e.asString()))));
            else argumentDefaults.push_back(Value::emptyStr());
        }
    }
}

// ------------------------------------------------------------------ Blocks --

Blocks::Blocks(Runtime* rt, bool noGlow) : runtime(rt), forceNoGlow(noGlow) {}

Block* Blocks::getBlock(const std::string& id) {
    auto it = blocks_.find(id);
    return it == blocks_.end() ? nullptr : &it->second;
}
const Block* Blocks::getBlock(const std::string& id) const {
    auto it = blocks_.find(id);
    return it == blocks_.end() ? nullptr : &it->second;
}

std::string Blocks::getNextBlock(const std::string& id) const {
    const Block* b = getBlock(id);
    return b ? b->next : std::string{};
}

std::string Blocks::getBranch(const std::string& id, int branchNum) const {
    const Block* b = getBlock(id);
    if (!b) return {};
    if (!branchNum) branchNum = 1;
    std::string inputName = "SUBSTACK";
    if (branchNum > 1) inputName += char('0' + branchNum);
    const BlockInput* in = b->inputs.find(inputName);
    return in ? in->block : std::string{};
}

std::string Blocks::getOpcode(const Block* block) const {
    return block ? block->opcode : std::string{};
}

const std::map<std::string, BlockField>* Blocks::getFields(const Block* block) const {
    return block ? &block->fields : nullptr;
}

static constexpr std::string_view kBranchPrefix = "SUBSTACK";

const InputSet* Blocks::getInputs(const Block* block) const {
    if (!block) return nullptr;
    auto it = cache_.inputs.find(block->id);
    if (it != cache_.inputs.end()) return &it->second;
    InputSet out;
    for (auto& [name, in] : block->inputs.items) {
        if (name.compare(0, kBranchPrefix.size(), kBranchPrefix) != 0) {
            out.items.emplace_back(name, in);
        }
    }
    auto [ins, ok] = cache_.inputs.emplace(block->id, std::move(out));
    return &ins->second;
}

const Mutation* Blocks::getMutation(const Block* block) const {
    return block && block->mutation ? &*block->mutation : nullptr;
}

std::string Blocks::getTopLevelScript(const std::string& id) const {
    const Block* b = getBlock(id);
    if (!b) return {};
    while (!b->parent.empty()) {
        b = getBlock(b->parent);
        if (!b) return {};
    }
    return b->id;
}

namespace {
/// procedures_definition の内側の custom_block プロトタイプを取得
const Block* customBlockInternal(Blocks& blocks, const Block* block) {
    const BlockInput* in = block->inputs.find("custom_block");
    return in ? blocks.getBlock(in->block) : nullptr;
}
} // namespace

std::string Blocks::getProcedureDefinition(const std::string& proccode) {
    auto it = cache_.procedureDefinitions.find(proccode);
    if (it != cache_.procedureDefinitions.end()) {
        return it->second;   // 空文字列 = null キャッシュ
    }
    for (auto& [id, block] : blocks_) {
        if (block.opcode == "procedures_definition") {
            const Block* inner = customBlockInternal(*this, &block);
            if (inner && inner->mutation &&
                inner->mutation->attr("proccode") == proccode) {
                cache_.procedureDefinitions[proccode] = id;
                return id;
            }
        }
    }
    cache_.procedureDefinitions[proccode] = {};
    return {};
}

bool Blocks::getProcedureParamNamesIdsAndDefaults(
    const std::string& proccode,
    std::vector<std::u16string>& names,
    std::vector<std::string>& ids,
    std::vector<Value>& defaults) {
    for (auto& [id, block] : blocks_) {
        if (block.opcode == "procedures_prototype" && block.mutation &&
            block.mutation->attr("proccode") == proccode) {
            block.mutation->parseArgs();
            names = block.mutation->argumentNames;
            ids = block.mutation->argumentIds;
            defaults = block.mutation->argumentDefaults;
            return true;
        }
    }
    return false;
}

void Blocks::populateProcedureCache() {
    if (cache_.proceduresPopulated) return;
    for (auto& [id, block] : blocks_) {
        if (block.opcode == "procedures_prototype" && block.mutation) {
            block.mutation->parseArgs();
        }
    }
    cache_.proceduresPopulated = true;
}

const std::vector<RuntimeScriptCache>&
Blocks::getRuntimeScripts(const std::string& opcode) {
    auto it = cache_.scripts.find(opcode);
    if (it != cache_.scripts.end()) return it->second;

    std::vector<RuntimeScriptCache> out;
    for (const std::string& topId : scripts_) {
        const Block* block = getBlock(topId);
        if (!block || block->opcode != opcode) continue;
        RuntimeScriptCache rsc;
        rsc.blockId = topId;
        rsc.block = block;
        auto putFields = [&](const std::map<std::string, BlockField>& fields) {
            for (auto& [k, f] : fields) {
                if (f.value.isString()) {
                    SStr* s = cast::toString(f.value);
                    rsc.fieldsOfInputs[k] =
                        Value::string(SStr::create(u16ToUpper(s->view())));
                } else {
                    rsc.fieldsOfInputs[k] = f.value;
                }
            }
        };
        if (!block->fields.empty()) {
            putFields(block->fields);
        } else {
            const InputSet* ins = getInputs(block);
            if (ins) {
                for (auto& [name, in] : ins->items) {
                    const Block* ib = getBlock(in.block);
                    if (ib) putFields(ib->fields);
                }
            }
        }
        out.push_back(std::move(rsc));
    }
    auto [ins, ok] = cache_.scripts.emplace(opcode, std::move(out));
    return ins->second;
}

// -------------------------------------------------------------- BlockCached --

BlockCached* Blocks::getCachedExecute(const std::string& blockId) {
    auto it = cache_.executeCached.find(blockId);
    if (it != cache_.executeCached.end()) return it->second.get();

    const Block* block = getBlock(blockId);
    if (!block) return nullptr;

    auto bc = std::make_unique<BlockCached>();
    BlockCached* self = bc.get();
    self->id = block->id;
    self->opcode = block->opcode;
    self->fields = block->fields;
    // getInputs: SUBSTACK* を除くコピー
    const InputSet* ins = getInputs(block);
    if (ins) self->inputs = *ins;
    self->mutation = getMutation(block);
    self->argValues.mutation = self->mutation;

    self->isHat = runtime->getIsHat(self->opcode);
    self->blockFunction = runtime->getOpcodeFunction(self->opcode);
    self->definedBlockFunction = self->blockFunction != nullptr;

    auto flow = runtime->getFlowing(self->opcode);
    self->isConditional = flow.conditional;
    self->isLoop = flow.loop;

    self->isShadowBlock = !self->definedBlockFunction &&
        self->fields.size() == 1 && self->inputs.empty();
    if (self->isShadowBlock) {
        self->shadowValue = self->fields.begin()->second.value;
    }

    // 静的フィールドを argValues へ
    for (auto& [name, f] : self->fields) {
        if (name == "VARIABLE" || name == "LIST" || name == "BROADCAST_OPTION") {
            IdName ref;
            ref.id = f.id;
            SStr* s = cast::toString(f.value);
            ref.name = s->view();
            self->argValues.refs[name] = std::move(ref);
        } else {
            self->argValues.vals[name] = f.value;
        }
    }

    self->inputs.erase("custom_block");

    if (self->inputs.has("BROADCAST_INPUT")) {
        self->argValues.refs["BROADCAST_OPTION"] = IdName{};
        const BlockInput* bi = self->inputs.find("BROADCAST_INPUT");
        if (bi && bi->block == bi->shadow) {
            const Block* shadow = getBlock(bi->shadow);
            if (shadow) {
                auto fit = shadow->fields.find("BROADCAST_OPTION");
                if (fit != shadow->fields.end()) {
                    IdName ref;
                    ref.id = fit->second.id;
                    SStr* s = cast::toString(fit->second.value);
                    ref.name = s->view();
                    self->argValues.refs["BROADCAST_OPTION"] = std::move(ref);
                }
            }
            self->inputs.erase("BROADCAST_INPUT");
        }
    }

    // 入力子ブロックの _ops を順に連結
    for (auto& [inputName, input] : self->inputs.items) {
        if (input.block.empty()) continue;
        BlockCached* inputCached = getCachedExecute(input.block);
        if (!inputCached || inputCached->isHat) continue;

        self->ops.insert(self->ops.end(),
                         inputCached->ops.begin(), inputCached->ops.end());
        inputCached->parentKey = inputName;
        inputCached->parentValues = &self->argValues;
        if (inputCached->isShadowBlock) {
            self->argValues.vals[inputName] = inputCached->shadowValue;
        }
    }

    if (self->definedBlockFunction) {
        self->ops.push_back(self);
    }

    cache_.executeCached.emplace(blockId, std::move(bc));
    return self;
}

// ------------------------------------------------------------------ mutation --

void Blocks::createBlock(Block block) {
    if (blocks_.count(block.id)) return;
    bool top = block.topLevel;
    std::string id = block.id;
    blocks_.emplace(id, std::move(block));
    if (top) addScript(id);
    resetCache();
}

void Blocks::deleteBlock(const std::string& blockId) {
    auto it = blocks_.find(blockId);
    if (it == blocks_.end()) return;
    Block& block = it->second;
    // 子（next チェーン・入力・シャドウ）を先に消す — blocks_ への参照が
    // 無効化されるので id を先に拾う
    std::string next = block.next;
    if (!next.empty()) deleteBlock(next);
    std::vector<std::string> kids;
    for (auto& [name, in] : block.inputs.items) {
        if (!in.block.empty()) kids.push_back(in.block);
        if (!in.shadow.empty() && in.shadow != in.block) kids.push_back(in.shadow);
    }
    for (auto& k : kids) deleteBlock(k);
    deleteScript(blockId);
    blocks_.erase(blockId);
    resetCache();
}

void Blocks::addScript(const std::string& topBlockId) {
    for (auto& s : scripts_) if (s == topBlockId) return;
    scripts_.push_back(topBlockId);
}
void Blocks::deleteScript(const std::string& topBlockId) {
    for (auto it = scripts_.begin(); it != scripts_.end(); ++it)
        if (*it == topBlockId) { scripts_.erase(it); return; }
}

void Blocks::resetCache() {
    cache_.inputs.clear();
    cache_.procedureDefinitions.clear();
    cache_.procedureParamNames.clear();
    cache_.executeCached.clear();
    cache_.scripts.clear();
    cache_.monitored.reset();
    cache_.proceduresPopulated = false;
}

void Blocks::runAllMonitored(Runtime& rt) {
    if (!cache_.monitored) {
        std::vector<std::pair<std::string, std::string>> m;
        for (auto& [id, b] : blocks_)
            if (b.isMonitored) m.emplace_back(id, b.targetId);
        cache_.monitored = std::move(m);
    }
    for (auto& [blockId, targetId] : *cache_.monitored) {
        Target* t = targetId.empty() ? nullptr : rt.getTargetById(targetId);
        rt.addMonitorScript(blockId, t);
    }
}

} // namespace xyo
