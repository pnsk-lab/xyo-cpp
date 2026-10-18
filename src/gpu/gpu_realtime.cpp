// xyo-cpp: GPU real-time ray/path プレイヤ実装。
#include "gpu_realtime.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "../blocks/pen_api.hpp"
#include "../core/cast.hpp"
#include "../core/color.hpp"
#include "../core/rng.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../engine/block_utility.hpp"
#include "../engine/runtime.hpp"
#include "../engine/thread.hpp"
#ifdef XYO_JIT
#include "../jit/jit.hpp"
#endif
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"

namespace xyo::gpu {

namespace {

constexpr const char* kOpFrame = "xyo_internal_gpu_frame";
constexpr const char* kOpModeRay = "xyo_internal_gpu_mode_ray";
constexpr const char* kOpModePath = "xyo_internal_gpu_mode_path";

constexpr const char* kProcRay = "r.render";
constexpr const char* kProcPath = "pt.pass";
constexpr const char* kProcTex = "tex setup";
constexpr const char* kProcReset = "pt.reset";

Value gpuFramePrim(const ArgValues&, BlockUtility& u) {
    Runtime* rt = u.runtime();
    if (rt && rt->onGpuFrame && rt->onGpuFrame(u.target()))
        rt->redrawRequested = true;
    else
        u.yieldTick();
    return Value::undefined();
}
Value gpuModeRayPrim(const ArgValues&, BlockUtility& u) {
    Runtime* rt = u.runtime();
    if (rt && rt->onGpuMode) rt->onGpuMode(false);
    return Value::undefined();
}
Value gpuModePathPrim(const ArgValues&, BlockUtility& u) {
    Runtime* rt = u.runtime();
    if (rt && rt->onGpuMode) rt->onGpuMode(true);
    return Value::undefined();
}

const Block* chainNext(Blocks* b, const Block* blk) {
    if (!blk || blk->next.empty()) return nullptr;
    return b->getBlock(blk->next);
}
const Block* inputBlock(Blocks* b, const Block* blk, const char* name) {
    if (!blk) return nullptr;
    const BlockInput* in = blk->inputs.find(name);
    if (!in || in->block.empty()) return nullptr;
    return b->getBlock(in->block);
}
bool subtreeHas(Blocks* bl, const Block* b, const char* opcode,
                uint32_t depth = 0) {
    if (!b || depth > 64) return false;
    if (b->opcode == opcode) return true;
    for (auto& [name, in] : b->inputs.items) {
        if (name.rfind("SUBSTACK", 0) == 0) {
            for (const Block* s = bl->getBlock(in.block); s;
                 s = chainNext(bl, s))
                if (subtreeHas(bl, s, opcode, depth + 1)) return true;
        } else {
            if (subtreeHas(bl, bl->getBlock(in.block), opcode,
                           depth + 1))
                return true;
            if (subtreeHas(bl, bl->getBlock(in.shadow), opcode,
                           depth + 1))
                return true;
        }
    }
    return false;
}

bool subtreeHasProcCall(Blocks* bl, const Block* b, const char* pc,
                        uint32_t depth = 0) {
    if (!b || depth > 64) return false;
    if (b->opcode == "procedures_call") {
        const Mutation* mu = bl->getMutation(b);
        if (mu && mu->attr("proccode") == pc) return true;
    }
    for (auto& [name, in] : b->inputs.items) {
        if (name.rfind("SUBSTACK", 0) == 0) {
            for (const Block* s = bl->getBlock(in.block); s;
                 s = chainNext(bl, s))
                if (subtreeHasProcCall(bl, s, pc, depth + 1))
                    return true;
        } else {
            if (subtreeHasProcCall(bl, bl->getBlock(in.block), pc,
                                   depth + 1))
                return true;
            if (subtreeHasProcCall(bl, bl->getBlock(in.shadow), pc,
                                   depth + 1))
                return true;
        }
    }
    return false;
}

uint64_t g_sceneEpoch = 0;

} // namespace

struct RealtimePlayer::Impl {
    Runtime* rt = nullptr;
    Target* bvh = nullptr;
    Target* stage = nullptr;
    RealtimeMode initMode = gpu::RealtimeMode::Ray;
    RealtimeMode wantMode = gpu::RealtimeMode::Ray;
    RealtimeMode mode = gpu::RealtimeMode::Ray;
    bool installed = false;
    bool active = false;
    bool needActivate = false;
    bool gpuOk = false;

    gpu::NativeProgram prog[2];
    bool progOk[2] = {false, false};
    std::string progWhy[2];

    const Block* bodyRay = nullptr;
    const Block* bodyPath = nullptr;
    const Block* bodyTex = nullptr;
    const Block* bodyReset = nullptr;

    struct Patch {
        Blocks* owner;
        std::string blockId;
        int kind;
        std::string savedOp;
        std::string inputName;
        std::string savedInput;
    };
    std::vector<Patch> patches;

    std::vector<float> varVals;
    std::vector<uint32_t> fpVarSlots;
    std::vector<uint64_t> fpVarBits;
    bool fpVarValid = false;
    std::vector<const Variable*> boundLists;
    std::vector<uint64_t> listBits;
    bool listBitsValid = false;
    std::vector<const Variable*> accumVars;
    std::vector<uint64_t> accumBits;
    bool accumBitsValid = false;
    std::vector<float> listData;
    std::vector<uint32_t> listMeta;
    bool listsPacked = false;
    uint64_t sceneVer = 0;
    void bumpScene() { sceneVer = ++g_sceneEpoch; }

    std::function<bool(Target*)> savedGpuFrame;
    std::function<void(bool)> savedGpuMode;
    std::function<void()> savedGreenFlag;
    PrimFn savedPrims[3] = {nullptr, nullptr, nullptr};

    Blocks* pbRayOwner = nullptr;
    const Block* pbRay = nullptr;
    Blocks* pbPathOwner = nullptr;
    const Block* pbPath = nullptr;
    Blocks* pbDrawOwner = nullptr;
    const Block* pbLastCall = nullptr;
    const Block* pbStopIf = nullptr;

    Thread* setupThread = nullptr;
    Thread* fbThread = nullptr;
    bool fbCounted = false;
    uint64_t lastSerial = ~0ull;
    uint64_t completed = 0, sampleCount = 0;
    uint32_t htimCount = 0;
    uint32_t errReports = 0;
    bool accumInit = false;
    bool accumReset = false;
    std::vector<float> accumData;

    const gpu::NativeProgram& curProg() const {
        return prog[mode == gpu::RealtimeMode::Path ? 1 : 0];
    }
    const gpu::NativeProgram& wantProg() const {
        return prog[wantMode == gpu::RealtimeMode::Path ? 1 : 0];
    }
    bool wantCompiled() const {
        return progOk[wantMode == gpu::RealtimeMode::Path ? 1 : 0];
    }

    Variable* varOf(const std::string& id) {
        auto it = bvh->variables.find(id);
        if (it != bvh->variables.end()) return &it->second;
        auto it2 = stage->variables.find(id);
        return it2 != stage->variables.end() ? &it2->second : nullptr;
    }
    double varNum(int32_t slot) const {
        const gpu::NativeProgram& p = curProg();
        if (slot < 0 || slot >= int32_t(p.vars.size())) return 0;
        auto it = bvh->variables.find(p.vars[slot].id);
        const Variable* v = nullptr;
        if (it != bvh->variables.end()) v = &it->second;
        else {
            auto it2 = stage->variables.find(p.vars[slot].id);
            if (it2 != stage->variables.end()) v = &it2->second;
        }
        return v ? cast::toNumber(v->value) : 0;
    }

    bool compileFor(gpu::RealtimeMode m, std::string* why) {
        int idx = m == gpu::RealtimeMode::Path ? 1 : 0;
        if (progOk[idx]) return true;
        std::string w;
        if (auto p = gpu::compileRayKernel(bvh->blocks,
                                         m == gpu::RealtimeMode::Path,
                                         &w)) {
            prog[idx] = std::move(*p);
            progOk[idx] = true;
            return true;
        }
        progWhy[idx] = w;
        if (why) *why = w;
        return false;
    }

    const Block* procBody(const char* pc) {
        std::string defId = bvh->blocks->getProcedureDefinition(pc);
        if (defId.empty()) return nullptr;
        const Block* def = bvh->blocks->getBlock(defId);
        return def && !def->next.empty()
                   ? bvh->blocks->getBlock(def->next) : nullptr;
    }

    void patchOpcode(Blocks* owner, Block* b, const char* op) {
        patches.push_back({owner, b->id, 0, b->opcode, {}, {}});
        b->opcode = op;
    }
    void patchSever(Blocks* owner, Block* b, const char* input) {
        BlockInput* in = b->inputs.find(input);
        if (!in) return;
        patches.push_back({owner, b->id, 1, {}, input, in->block});
        in->block.clear();
    }
    void restorePatches() {
        for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
            Block* b = it->owner->getBlock(it->blockId);
            if (!b) continue;
            if (it->kind == 0) b->opcode = it->savedOp;
            else if (BlockInput* in = b->inputs.find(it->inputName))
                in->block = it->savedInput;
        }
        patches.clear();
    }

    const Block* hatChain(Blocks* bl, const char16_t* key,
                          const Block*& hatOut) {
        for (const RuntimeScriptCache& s :
             bl->getRuntimeScripts("event_whenkeypressed")) {
            const Block* h = s.block;
            if (!h) continue;
            auto f = h->fields.find("KEY_OPTION");
            if (f == h->fields.end() || !f->second.value.isString())
                continue;
            SStr* sv = f->second.value.asStrPtr();
            if (!sv || sv->view() != std::u16string_view(key))
                continue;
            hatOut = h;
            return bl->getBlock(h->next);
        }
        return nullptr;
    }

    const Block* chainSeen(Blocks* bl, const Block* b,
                           std::unordered_set<const Block*>& seen) {
        const Block* n = chainNext(bl, b);
        if (!n || !seen.insert(n).second) return nullptr;
        return n;
    }

    bool validateStructure(std::string* why) {
        const Block* hat = nullptr;
        bool gotR = false, gotP = false, gotDraw = false;
        if (const Block* c = hatChain(bvh->blocks, u"r", hat)) {
            std::unordered_set<const Block*> seen;
            const Block* last = nullptr;
            const Block* ray = nullptr;
            seen.insert(c);
            for (const Block* b = c; b;
                 b = chainSeen(bvh->blocks, b, seen)) {
                if (b->opcode != "procedures_call") continue;
                const Mutation* mu = bvh->blocks->getMutation(b);
                if (!mu) continue;
                if (mu->attr("proccode") == kProcRay) ray = b;
                last = b;
            }
            if (ray && ray == last) {
                pbRayOwner = bvh->blocks;
                pbRay = ray;
                gotR = true;
            }
        }
        if (const Block* c = hatChain(bvh->blocks, u"p", hat)) {
            std::unordered_set<const Block*> seen;
            bool resetSeen = false;
            seen.insert(c);
            for (const Block* b = c; b;
                 b = chainSeen(bvh->blocks, b, seen)) {
                if (b->opcode == "procedures_call") {
                    const Mutation* mu = bvh->blocks->getMutation(b);
                    if (mu && mu->attr("proccode") == kProcReset)
                        resetSeen = true;
                }
                if (b->opcode == "control_forever" && resetSeen) {
                    const Block* sub =
                        inputBlock(bvh->blocks, b, "SUBSTACK");
                    std::unordered_set<const Block*> sseen;
                    if (sub) sseen.insert(sub);
                    for (const Block* s = sub; s;
                         s = chainSeen(bvh->blocks, s, sseen)) {
                        if (!subtreeHasProcCall(bvh->blocks, s, kProcPath))
                            continue;
                        pbPathOwner = bvh->blocks;
                        pbPath = b;
                        gotP = true;
                        break;
                    }
                    if (gotP) break;
                }
            }
        }
        for (Target* t : rt->targets) {
            if (gotDraw || !t->blocks) break;
            for (auto& [id, blk] : t->blocks->allBlocks()) {
                if (blk.opcode != "control_forever") continue;
                const Block* sub = inputBlock(t->blocks, &blk, "SUBSTACK");
                std::unordered_set<const Block*> seen;
                const Block* stopIf = nullptr;
                const Block* lastCall = nullptr;
                const Block* lastBlock = nullptr;
                int nCalls = 0;
                bool shapeOk = true;
                seen.insert(sub);
                for (const Block* s = sub; s;
                     s = chainSeen(t->blocks, s, seen)) {
                    if (s->opcode == "procedures_call") {
                        lastCall = s;
                        ++nCalls;
                    } else if (s->opcode == "control_if") {
                        if (stopIf ||
                            !subtreeHas(t->blocks,
                                        inputBlock(t->blocks, s,
                                                   "SUBSTACK"),
                                        "control_stop") ||
                            !subtreeHas(t->blocks,
                                        inputBlock(t->blocks, s,
                                                   "CONDITION"),
                                        "sensing_keypressed"))
                            shapeOk = false;
                        else
                            stopIf = s;
                    } else {
                        shapeOk = false;
                    }
                    lastBlock = s;
                }
                if (!shapeOk || !stopIf || !lastCall || nCalls < 2 ||
                    lastBlock != stopIf)
                    continue;
                pbDrawOwner = t->blocks;
                pbLastCall = lastCall;
                pbStopIf = stopIf;
                gotDraw = true;
                break;
            }
        }
        if (why) {
            if (!gotR) *why = "r-hat r.render call not found";
            else if (!gotP) *why = "p-hat pt.reset/forever not found";
            else if (!gotDraw) *why = "draw controller not found";
        }
        return gotR && gotP && gotDraw;
    }

    void applyPatches() {
        patchOpcode(pbRayOwner, pbRayOwner->getBlock(pbRay->id),
                    kOpModeRay);
        patchOpcode(pbPathOwner, pbPathOwner->getBlock(pbPath->id),
                    kOpModePath);
        patchOpcode(pbDrawOwner, pbDrawOwner->getBlock(pbLastCall->id),
                    kOpFrame);
        patchSever(pbDrawOwner, pbDrawOwner->getBlock(pbStopIf->id),
                   "SUBSTACK");
    }

    void snapshotLists() {
        boundLists.clear();
        listBits.clear();
        for (const gpu::GlobalVar& g : curProg().lists) {
            Variable* v = varOf(g.id);
            boundLists.push_back(v);
            SList* l = v ? v->list : nullptr;
            listBits.push_back(l ? l->len : 0);
            if (l)
                for (uint32_t i = 0; i < l->len; ++i)
                    listBits.push_back(l->items[i].bits);
        }
        listBitsValid = true;
    }
    bool listsChanged() {
        if (!listBitsValid) { snapshotLists(); return false; }
        size_t pos = 0;
        bool changed = false;
        for (const Variable* v : boundLists) {
            SList* l = v ? v->list : nullptr;
            size_t n = l ? l->len : 0;
            if (pos + n + 1 > listBits.size() ||
                listBits[pos] != n) { changed = true; break; }
            if (l && std::memcmp(listBits.data() + pos + 1, l->items,
                                 n * sizeof(Value)) != 0) {
                changed = true;
                break;
            }
            pos += n + 1;
        }
        if (changed) snapshotLists();
        return changed;
    }
    void snapshotAccum() {
        accumVars.clear();
        accumBits.clear();
        for (int i = 0; i < 4; ++i) {
            const gpu::GlobalVar& g = curProg().accumVars[i];
            Variable* v = g.id.empty() ? nullptr : varOf(g.id);
            accumVars.push_back(v);
            SList* l = v ? v->list : nullptr;
            accumBits.push_back(l ? l->len : 0);
            if (l)
                for (uint32_t j = 0; j < l->len; ++j)
                    accumBits.push_back(l->items[j].bits);
        }
        accumBitsValid = true;
    }
    bool accumChanged() {
        if (!accumBitsValid) { snapshotAccum(); return false; }
        size_t pos = 0;
        bool changed = false;
        for (const Variable* v : accumVars) {
            SList* l = v ? v->list : nullptr;
            size_t n = l ? l->len : 0;
            if (pos + n + 1 > accumBits.size() ||
                accumBits[pos] != n) { changed = true; break; }
            if (l && std::memcmp(accumBits.data() + pos + 1, l->items,
                                 n * sizeof(Value)) != 0) {
                changed = true;
                break;
            }
            pos += n + 1;
        }
        if (changed) snapshotAccum();
        return changed;
    }

    void snapshotFpVars() {
        fpVarBits.clear();
        const gpu::NativeProgram& p = curProg();
        for (uint32_t s : fpVarSlots) {
            Variable* v = varOf(p.vars[s].id);
            fpVarBits.push_back(v ? v->value.bits : 0);
        }
        fpVarValid = true;
    }
    bool fpVarsChanged() {
        if (!fpVarValid) { snapshotFpVars(); return false; }
        const gpu::NativeProgram& p = curProg();
        bool changed = false;
        for (size_t i = 0; i < fpVarSlots.size(); ++i) {
            Variable* v = varOf(p.vars[fpVarSlots[i]].id);
            uint64_t b = v ? v->value.bits : 0;
            if (fpVarBits[i] != b) { changed = true; break; }
        }
        if (changed) snapshotFpVars();
        return changed;
    }
    void buildFpVarSlots() {
        fpVarSlots.clear();
        const gpu::NativeProgram& p = curProg();
        std::unordered_set<int32_t> written(p.writtenVars.begin(),
                                            p.writtenVars.end());
        for (uint32_t i = 0; i < p.vars.size(); ++i) {
            if (written.count(int32_t(i))) continue;
            const std::u16string& n = p.vars[i].name;
            if (n == u"@exposure" || n == u"@tonemap") continue;
            fpVarSlots.push_back(i);
        }
    }

    bool packLists() {
        if (listsPacked) return true;
        const gpu::NativeProgram& p = curProg();
        listData.clear();
        listMeta.clear();
        for (const gpu::GlobalVar& g : p.lists) {
            Variable* v = varOf(g.id);
            SList* l = v ? v->list : nullptr;
            uint32_t n = l ? l->len : 0;
            listMeta.push_back(uint32_t(listData.size() / 2));
            listMeta.push_back(n);
            for (uint32_t i = 0; i < n; ++i) {
                Value e = l->items[i];
                double d = cast::toNumberOrNaN(e);
                float none = 0.0f;
                if (std::isnan(d)) {
                    if (e.isString()) {
                        SStr* s = e.asStrPtr();
                        std::string u8 =
                            s ? SStr::toUtf8(s->view()) : std::string();
                        for (auto& c : u8)
                            c = char(std::tolower((unsigned char)c));
                        if (u8 == "none") {
                            none = 1.0f;
                            listData.push_back(
                                std::numeric_limits<float>::quiet_NaN());
                            listData.push_back(none);
                            continue;
                        }
                        return false;
                    }
                }
                listData.push_back(float(d));
                listData.push_back(none);
            }
        }
        listsPacked = true;
        return true;
    }

    void packAccumIn(uint32_t total) {
        const gpu::NativeProgram& p = curProg();
        accumData.assign(size_t(total) * 4, 0.0f);
        for (int ch = 0; ch < 4; ++ch) {
            const gpu::GlobalVar& g = p.accumVars[ch];
            Variable* v = g.id.empty() ? nullptr : varOf(g.id);
            SList* l = v ? v->list : nullptr;
            if (!l) continue;
            uint32_t n = std::min<uint32_t>(l->len, total);
            for (uint32_t i = 0; i < n; ++i)
                accumData[size_t(i) * 4 + ch] =
                    float(cast::toNumber(l->items[i]));
        }
    }

    void switchMode(bool path) {
        if (setupThread && rt->isActiveThread(setupThread))
            rt->stopThread(setupThread);
        if (fbThread && rt->isActiveThread(fbThread))
            rt->stopThread(fbThread);
        needActivate = true;
        active = false;
        wantMode = path ? gpu::RealtimeMode::Path : gpu::RealtimeMode::Ray;
        bumpScene();
    }

    void startSetup() {
        const Block* body = wantMode == gpu::RealtimeMode::Ray
                                ? bodyTex : bodyReset;
        active = false;
        gpuOk = false;
        mode = wantMode;
        if (!wantCompiled()) {
            std::string w;
            compileFor(wantMode, &w);
        }
        if (body) {
            setupThread =
                rt->pushThread(body->id, bvh, false, false, body);
        } else {
            finishSetup();
        }
    }
    void finishSetup() {
        setupThread = nullptr;
        needActivate = false;
        active = true;
        bumpScene();
        listsPacked = false;
        listBitsValid = false;
        fpVarValid = false;
        accumBitsValid = false;
        accumInit = false;
        accumReset = true;
        if (mode == gpu::RealtimeMode::Path) resetSampleCount();
        buildFpVarSlots();
        snapshotFpVars();
        snapshotLists();
        snapshotAccum();
    }
    void resetSampleCount() {
        sampleCount = 0;
        const gpu::NativeProgram& p = curProg();
        if (p.slotPN >= 0) {
            Variable* v = varOf(p.vars[p.slotPN].id);
            if (v) v->value = Value::num(0);
        }
    }

    void writeBack(const gpu::NativeProgram& p,
                   const gpu::NativeRequest& req,
                   const std::vector<float>& varIn, uint32_t total,
                   uint32_t cols, uint32_t rows) {
        const uint32_t nWb = uint32_t(p.writtenVars.size());
        for (uint32_t i = 0; i < nWb; ++i) {
            int32_t slot = p.writtenVars[i];
            if (slot < 0 || slot >= int32_t(p.vars.size())) continue;
            Variable* v = varOf(p.vars[slot].id);
            if (v) v->value = Value::numCanonical(double(req.scalarOut[i]));
        }
        const float* pw = req.scalarOut + nWb;
        const float* aux = req.scalarOut + nWb + 8;
        const double lastColor =
            req.nScalarOut > nWb + 10 ? double(req.scalarOut[nWb + 10]) : 0.0;
        if (!bvh->penState)
            bvh->penState = std::make_unique<PenState>();
        PenState& ps = *bvh->penState;
        ps.penSize = pw[0];
        if (p.path) {
            ps.rgba[0] = pw[1]; ps.rgba[1] = pw[2];
            ps.rgba[2] = pw[3]; ps.rgba[3] = pw[4];
            bvh->x = pw[5];
            bvh->y = pw[6];
        } else {
            double lc = std::floor(lastColor);
            if (lc < 0.0) lc = std::floor(4294967296.0 + lc);
            uint32_t ci = uint32_t(std::fmod(lc, 4294967296.0));
            uint32_t a = (ci >> 24) & 0xFF;
            ps.rgba[0] = float((ci >> 16) & 0xFF) / 255.0f;
            ps.rgba[1] = float((ci >> 8) & 0xFF) / 255.0f;
            ps.rgba[2] = float(ci & 0xFF) / 255.0f;
            ps.rgba[3] = (a > 0 && a < 255) ? float(a) / 255.0f : 1.0f;
            auto fenced = [](double v, double lo, double hi) {
                if (v > hi) return std::floor(hi);
                if (v < lo) return std::ceil(lo);
                return v;
            };
            double sx = p.slotStepX >= 0 ? varIn[p.slotStepX] : 0;
            double sy = p.slotStepY >= 0 ? varIn[p.slotStepY] : 0;
            bvh->x = fenced(aux[0] + double(cols) * sx, req.fenceL,
                            req.fenceR);
            bvh->y = fenced(aux[1] + double(rows - 1) * sy, req.fenceB,
                            req.fenceT);
        }
        Hsv hsv = color::rgbToHsv(
            {int(std::lround(ps.rgba[0] * 255.0)),
             int(std::lround(ps.rgba[1] * 255.0)),
             int(std::lround(ps.rgba[2] * 255.0))});
        ps.color = hsv.h / 360.0 * 100.0;
        ps.saturation = hsv.s * 100.0;
        ps.brightness = hsv.v * 100.0;
        ps.shade = ps.brightness / 2.0;
        ps.transparency =
            ps.rgba[3] < 1.0f ? 100.0 * (1.0 - double(ps.rgba[3])) : 0.0;
        ps.colorDirty = false;
        ps.lastSetColor = ~0ULL;
        bool down = pw[7] != 0.0f;
        if (ps.penDown != down) {
            ps.penDown = down;
            bvh->onTargetMoved = down ? penrt::movedHook() : nullptr;
        }
        auto setSlot = [&](int32_t s, double v) {
            if (s < 0 || s >= int32_t(p.vars.size())) return;
            Variable* var = varOf(p.vars[s].id);
            if (var) var->value = Value::numCanonical(v);
        };
        if (p.slotColVar >= 0 && p.slotStepX >= 0)
            setSlot(p.slotColVar,
                    double(aux[0]) + double(cols) * varIn[p.slotStepX]);
        if (p.slotRowVar >= 0 && p.slotStepY >= 0)
            setSlot(p.slotRowVar,
                    double(aux[1]) + double(rows) * varIn[p.slotStepY]);
        if (p.slotPI >= 0) setSlot(p.slotPI, double(total));
        if (p.slotLastColor >= 0) {
            Variable* lc = varOf(p.vars[p.slotLastColor].id);
            if (lc) lc->value = Value::numCanonical(lastColor);
        }
        if (req.accumOut) {
            for (int ch = 0; ch < 4; ++ch) {
                const gpu::GlobalVar& g = p.accumVars[ch];
                if (g.id.empty()) continue;
                Variable* v = varOf(g.id);
                SList* l = v ? v->list : nullptr;
                if (!l) continue;
                uint32_t n = std::min(l->len, total);
                for (uint32_t i = 0; i < n; ++i)
                    l->items[i] = Value::numCanonical(
                        double(req.accumOut[size_t(i) * 4 + ch]));
            }
            rt->monitorState.dirty = true;
            snapshotAccum();
        }
        bvh->emitVisualChange();
    }

    void report(const char* m) {
        if (errReports++ < 5)
            std::fprintf(stderr, "gpu-realtime: %s\n", m);
    }

    bool frame() {
        if (lastSerial == rt->stepSerial) return false;
        lastSerial = rt->stepSerial;

        if (setupThread) {
            if (rt->isActiveThread(setupThread)) return false;
            if (!setupThread->isKilled) finishSetup();
            else { setupThread = nullptr; needActivate = true; }
        }
        if (fbThread) {
            if (rt->isActiveThread(fbThread)) return false;
            if (!fbCounted && !fbThread->isKilled) {
                ++completed;
                if (mode == gpu::RealtimeMode::Path) ++sampleCount;
            }
            fbCounted = false;
            fbThread = nullptr;
            listsPacked = false;
            listBitsValid = false;
            accumBitsValid = false;
            accumInit = false;
            fpVarValid = false;
            return false;
        }
        if (!active) {
            if (!needActivate) needActivate = true;
            startSetup();
            return false;
        }

        const gpu::NativeProgram& p = curProg();
        if (!progOk[mode == gpu::RealtimeMode::Path ? 1 : 0])
            return startFallback();

        const bool htim = htimCount < 8 && std::getenv("XYO_GPU_TIMING");
        auto hnow = []() { return std::chrono::steady_clock::now(); };
        auto hms = [](std::chrono::steady_clock::time_point a,
                      std::chrono::steady_clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const auto ht0 = hnow();
        bool listCh = listsChanged();
        bool varCh = fpVarsChanged();
        bool accCh = mode == gpu::RealtimeMode::Path && accumChanged();
        if (listCh) {
            bumpScene();
            listsPacked = false;
            startSetup();
            return false;
        }
        if (mode == gpu::RealtimeMode::Path) {
            if (varCh) {
                accumReset = true;
                resetSampleCount();
            }
            if (accCh) {
                accumInit = false;
                accumReset = false;
            }
        }
        const double hScan = htim ? hms(ht0, hnow()) : 0.0;

        double res = p.slotRes >= 0 ? varNum(p.slotRes) : 1.0;
        if (!std::isfinite(res) || res <= 0) {
            report("invalid @res — dispatch skipped");
            return false;
        }
        double colsD = std::floor(rt->stageWidth / res + 0.5);
        double rowsD = std::floor(rt->stageHeight / res + 0.5);
        if (!(colsD > 0 && rowsD > 0) ||
            colsD * rowsD > double(UINT32_MAX)) {
            report("invalid dispatch dimensions — skipped");
            return false;
        }
        const uint32_t cols = uint32_t(colsD);
        const uint32_t rows = uint32_t(rowsD);
        const uint32_t total = cols * rows;

        const auto ht1 = hnow();
        if (!packLists()) return startFallback();
        const double hPack = htim ? hms(ht1, hnow()) : 0.0;
        const auto ht2 = hnow();

        varVals.resize(p.vars.size());
        for (size_t i = 0; i < p.vars.size(); ++i) {
            Variable* v = varOf(p.vars[i].id);
            varVals[i] = v ? float(cast::toNumber(v->value)) : 0.0f;
        }

        gpu::NativeRequest req;
        req.program = &p;
        req.vars = varVals.data();
        req.nVars = uint32_t(p.vars.size());
        req.listData = listData.data();
        req.listElems = listData.size() / 2;
        req.listMeta = listMeta.data();
        req.nLists = uint32_t(p.lists.size());
        req.sceneVersion = sceneVer;
        req.cols = cols;
        req.rows = rows;
        req.res = float(res);
        req.baseSeed = Rng::global().next();
        if (rt->renderer) {
            double fr[4];
            rt->renderer->getFenceRect(bvh->drawableID, fr);
            req.fenceL = float(fr[0]); req.fenceR = float(fr[1]);
            req.fenceB = float(fr[2]); req.fenceT = float(fr[3]);
        }
        if (!rt->fencing) {
            req.fenceL = req.fenceB = -1e30f;
            req.fenceR = req.fenceT = 1e30f;
        }
        req.startX = float(bvh->x);
        req.startY = float(bvh->y);
        if (bvh->penState) {
            PenState& ps = *bvh->penState;
            if (ps.colorDirty) penrt::recolor(bvh);
            req.penSize = float(ps.penSize);
            req.penRGBA[0] = ps.rgba[0]; req.penRGBA[1] = ps.rgba[1];
            req.penRGBA[2] = ps.rgba[2]; req.penRGBA[3] = ps.rgba[3];
            req.penDown = ps.penDown;
        }
        req.miscLimits = rt->miscLimits;
        req.lastColor = float(varNum(p.slotLastColor));

        bool hasAccum = false;
        for (int i = 0; i < 4; ++i)
            if (!p.accumVars[i].id.empty()) hasAccum = true;
        if (hasAccum) {
            for (int i = 0; i < 4; ++i) {
                Variable* v = p.accumVars[i].id.empty()
                                  ? nullptr : varOf(p.accumVars[i].id);
                SList* l = v ? v->list : nullptr;
                req.accumLens[i] = l ? l->len : 0;
            }
            if (!accumInit) {
                packAccumIn(total);
                req.accumIn = accumData.data();
            }
            req.accumReset = accumReset;
        }
        const double hVars = htim ? hms(ht2, hnow()) : 0.0;

        bool ok = rt->renderer && rt->renderer->gpuExecNative(req) &&
                  req.executed && !req.overflow;
        if (!ok) return startFallback();

        gpuOk = true;
        const auto ht3 = hnow();
        writeBack(p, req, varVals, total, cols, rows);
        const double hCommit = htim ? hms(ht3, hnow()) : 0.0;
        if (hasAccum) accumInit = true;
        accumReset = false;
        ++completed;
        if (mode == gpu::RealtimeMode::Path) ++sampleCount;
        if (htim) {
            ++htimCount;
            std::fprintf(stderr,
                "xyo-native-host scan_ms=%.3f pack_ms=%.3f vars_ms=%.3f "
                "commit_ms=%.3f\n", hScan, hPack, hVars, hCommit);
        }
        return true;
    }

    bool startFallback() {
        const Block* body = mode == gpu::RealtimeMode::Path
                                ? bodyPath : bodyRay;
        if (!body) { report("no fallback procedure body"); return false; }
        fbThread = rt->pushThread(body->id, bvh, false, false, body);
        fbCounted = false;
        gpuOk = false;
        return false;
    }
};

RealtimePlayer::RealtimePlayer() : impl_(std::make_unique<Impl>()) {}
RealtimePlayer::~RealtimePlayer() { uninstall(); }

bool RealtimePlayer::init(Runtime& rt, RealtimeMode mode,
                          std::string* why) {
    uninstall();
    impl_ = std::make_unique<Impl>();
    Impl& im = *impl_;
    im.rt = &rt;
    im.initMode = mode;
    im.wantMode = mode;
    im.mode = mode;
    im.stage = rt.getTargetForStage();
    auto fail = [&](const std::string& m) {
        if (why) *why = m;
        im.restorePatches();
        return false;
    };
    if (!im.stage) return fail("no stage target");
    for (Thread* t : rt.threads)
        if (!t->isKilled)
            return fail("cannot install: runtime threads active");
    if (rt.getOpcodeFunction(kOpFrame) ||
        rt.getOpcodeFunction(kOpModeRay) ||
        rt.getOpcodeFunction(kOpModePath))
        return fail("internal opcodes already registered");
    std::string cw;
    for (Target* t : rt.targets) {
        if (t->isStage || !t->blocks) continue;
        if (auto p = gpu::compileRayKernel(
                t->blocks, mode == gpu::RealtimeMode::Path, &cw)) {
            im.bvh = t;
            im.prog[mode == gpu::RealtimeMode::Path ? 1 : 0] =
                std::move(*p);
            im.progOk[mode == gpu::RealtimeMode::Path ? 1 : 0] = true;
            break;
        }
    }
    if (!im.bvh)
        return fail("no supported render target: " + cw);
    im.bodyRay = im.procBody(kProcRay);
    im.bodyPath = im.procBody(kProcPath);
    im.bodyTex = im.procBody(kProcTex);
    im.bodyReset = im.procBody(kProcReset);
    if (!im.bodyRay || !im.bodyPath)
        return fail("render procedure not found");
    {
        std::string vw;
        if (!im.validateStructure(&vw))
            return fail("controller/key-hat structure: " + vw);
    }

    im.savedGpuFrame = rt.onGpuFrame;
    im.savedGpuMode = rt.onGpuMode;
    im.savedGreenFlag = rt.onGreenFlag;
    im.applyPatches();
#ifdef XYO_JIT
    if (rt.jit) rt.jit->invalidate();
#endif

    rt.registerPrimitive(kOpFrame, gpuFramePrim);
    rt.registerPrimitive(kOpModeRay, gpuModeRayPrim);
    rt.registerPrimitive(kOpModePath, gpuModePathPrim);

    Impl* self = impl_.get();
    rt.onGpuFrame = [self](Target* t) {
        if (self->frame()) return true;
        return self->savedGpuFrame && self->savedGpuFrame(t);
    };
    rt.onGpuMode = [self](bool path) {
        self->switchMode(path);
        if (self->savedGpuMode) self->savedGpuMode(path);
    };
    rt.onGreenFlag = [self]() {
        self->setupThread = nullptr;
        self->fbThread = nullptr;
        self->fbCounted = false;
        self->active = false;
        self->needActivate = true;
        self->wantMode = self->initMode;
        self->lastSerial = ~0ull;
        self->completed = 0;
        self->sampleCount = 0;
        self->listsPacked = false;
        self->listBitsValid = false;
        self->fpVarValid = false;
        self->accumBitsValid = false;
        self->accumInit = false;
        self->accumReset = false;
        self->boundLists.clear();
        self->accumVars.clear();
        self->bumpScene();
        if (self->savedGreenFlag) self->savedGreenFlag();
    };
    im.bvh->blocks->resetCache();
    for (Target* t : rt.targets)
        if (t->blocks && t->blocks != im.bvh->blocks)
            t->blocks->resetCache();
    im.installed = true;
    return true;
}

uint64_t RealtimePlayer::completedFrames() const {
    return impl_ ? impl_->completed : 0;
}
uint64_t RealtimePlayer::samples() const {
    return impl_ ? impl_->sampleCount : 0;
}
bool RealtimePlayer::usingGpu() const { return impl_ && impl_->gpuOk; }
bool RealtimePlayer::active() const { return impl_ && impl_->active; }

void RealtimePlayer::uninstall() {
    if (!impl_ || !impl_->installed || !impl_->rt) return;
    Runtime& rt = *impl_->rt;
    rt.stopAll();
#ifdef XYO_JIT
    if (rt.jit) rt.jit->invalidate();
#endif
    impl_->restorePatches();
    for (Target* t : rt.targets)
        if (t->blocks) t->blocks->resetCache();
    rt.onGpuFrame = std::move(impl_->savedGpuFrame);
    rt.onGpuMode = std::move(impl_->savedGpuMode);
    rt.onGreenFlag = std::move(impl_->savedGreenFlag);
    rt.primitives.erase(kOpFrame);
    rt.primitives.erase(kOpModeRay);
    rt.primitives.erase(kOpModePath);
    for (int i = 0; i < 3; ++i) {
        const char* op = i == 0 ? kOpFrame
                                : (i == 1 ? kOpModeRay : kOpModePath);
        if (impl_->savedPrims[i])
            rt.registerPrimitive(op, impl_->savedPrims[i]);
    }
    impl_->installed = false;
}

} // namespace xyo::gpu
