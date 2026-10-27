// xyo-cpp: GPU 並列 VM バイトコードの CFG 解析（gpu_analysis.hpp 参照）。
#include "gpu_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "gpu_isa.hpp"

namespace xyo::gpu {

GlChunkPlan planGlChunks(const ExecRequest& req, uint64_t ssboLimit,
                         uint32_t groupLimit, uint32_t forcedChunk,
                         bool allowChunks, uint64_t scratchBytes) {
    GlChunkPlan plan;
    if (!req.lanes || req.lanes > (1u << 24) || !req.maxPerLane ||
        req.maxPerLane > 2048 || req.nSlots > 96 || req.nWb > 128 ||
        !req.codeLen || (req.codeLen & 1) || req.codeLen > 131072)
        return plan;

    constexpr uint64_t hostLimit = uint64_t(512) * 1024 * 1024;
    constexpr uint64_t chunkLimit = uint64_t(256) * 1024 * 1024;
    // All products below fit uint64_t after the structural checks above;
    // nGlobals is uint32_t. Subtraction-before-addition handles scratchBytes,
    // the sole unrestricted uint64_t input, without wraparound.
    const uint64_t strideBytes = uint64_t(laneOutStride(req.nWb)) * 4;
    plan.hostOutputBytes = uint64_t(req.lanes) * strideBytes;
    if (plan.hostOutputBytes > hostLimit) return plan;
    const uint64_t codeBytes = uint64_t(req.codeLen) * 4;
    const uint64_t paramBytes = uint64_t(28 + req.nWb) * 4;
    const uint64_t globalBytes = uint64_t(std::max(req.nGlobals, 1u)) * 4;
    const uint64_t fixedBytes = codeBytes + paramBytes + globalBytes;
    const uint64_t recLaneBytes = uint64_t(req.maxPerLane) * 48;
    const uint64_t idLaneBytes = uint64_t(req.maxPerLane) * 8;
    const uint64_t laneBytes = recLaneBytes + idLaneBytes + 8 + strideBytes;
    uint64_t hardwareLanes = req.lanes;
    if (ssboLimit) {
        if (codeBytes > ssboLimit || paramBytes > ssboLimit ||
            globalBytes > ssboLimit) return plan;
        for (uint64_t bytes : {recLaneBytes, idLaneBytes, strideBytes,
                               uint64_t(4)})
            hardwareLanes = std::min(hardwareLanes, ssboLimit / bytes);
    }
    if (groupLimit)
        hardwareLanes = std::min(hardwareLanes, uint64_t(groupLimit) * 64);
    // Existing renderer conservatively permits INT_MAX / 12 records.
    constexpr uint64_t maxRecords = uint64_t(2147483647) / 12;
    hardwareLanes = std::min(hardwareLanes, maxRecords / req.maxPerLane);
    if (forcedChunk) hardwareLanes = std::min(hardwareLanes,
                                             uint64_t(forcedChunk));
    auto fits = [&](uint64_t budget, uint64_t lanes) {
        return fixedBytes <= budget && scratchBytes <= budget - fixedBytes &&
               lanes <= (budget - fixedBytes - scratchBytes) / laneBytes;
    };
    if (hardwareLanes >= req.lanes && fits(hostLimit, req.lanes)) {
        plan.ok = true;
        plan.lanesPerChunk = req.lanes;
        plan.chunkBufferBytes = fixedBytes + scratchBytes +
                                uint64_t(req.lanes) * laneBytes;
        return plan;
    }
    if (!allowChunks || !fits(chunkLimit, 1)) return plan;
    const uint64_t lanes = std::min(hardwareLanes,
        (chunkLimit - fixedBytes - scratchBytes) / laneBytes);
    if (!lanes) return plan;
    plan.ok = true;
    plan.lanesPerChunk = uint32_t(lanes);
    plan.chunkBufferBytes = fixedBytes + scratchBytes + lanes * laneBytes;
    return plan;
}

namespace {

constexpr int kMaxStack = 128;    // シェーダ st[] と一致
constexpr uint64_t kMaxWork = 4000000;  // 解析発散防止

// 命令のスタック効果（pop 数, push 数）。ジャンプ/HALT は別扱い。
bool stackEffect(uint32_t op, int& pops, int& pushes) {
    pops = pushes = 0;
    switch (op) {
    case OP_HALT: return true;
    case OP_PUSH: case OP_ITER: case OP_LOADV: case OP_LOADG:
    case OP_POSX: case OP_POSY:
        pushes = 1; return true;
    case OP_STOREV: case OP_STOREVM:
        pops = 1; return true;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_LT: case OP_GT: case OP_EQ: case OP_LE: case OP_GE:
    case OP_NEQ: case OP_AND: case OP_OR: case OP_RAND:
        pops = 2; pushes = 1; return true;
    case OP_NOT: case OP_NEG: case OP_MATH:
        pops = 1; pushes = 1; return true;
    case OP_JMP: return true;
    case OP_JMPIF0: pops = 1; return true;
    case OP_GOTOXY: pops = 2; return true;
    case OP_SETX: case OP_SETY: case OP_CHANGEX: case OP_CHANGEY:
    case OP_PENSIZE: case OP_PENSIZED: case OP_PENCOLN:
    case OP_PENCOLP: case OP_PENCOLPB:
        pops = 1; return true;
    case OP_PENDOWN: case OP_PENUP: return true;
    default: return false;
    }
}

// 命令の後続 pc を列挙（戻り値 false = 不正 op/オペランド）。
bool successors(const uint32_t* code, size_t nInstr, size_t pc,
                uint32_t nSlots, uint32_t nGlobals, uint32_t nWb,
                uint32_t* succ, int& nSucc, std::string* why) {
    const uint32_t op = code[pc * 2], ar = code[pc * 2 + 1];
    auto bad = [&](const char* m) {
        if (why && why->empty()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s @%zu", m, pc);
            *why = buf;
        }
        return false;
    };
    if (op >= _OP_MAX || op == OP_HALT) {
        if (op != OP_HALT) return bad("unknown op");
        nSucc = 0;
        return true;
    }
    switch (op) {
    case OP_LOADV: case OP_STOREV:
        if (ar >= nSlots) return bad("slot oob");
        break;
    case OP_STOREVM:
        if ((ar & 0xFFFFu) >= nSlots) return bad("slot oob");
        if ((ar >> 16) >= nWb) return bad("wb oob");
        break;
    case OP_LOADG:
        if (ar >= nGlobals) return bad("global oob");
        break;
    case OP_MATH:
        if (ar >= _M_MAX) return bad("math kind oob");
        break;
    case OP_PENCOLP: case OP_PENCOLPB:
        if (ar >= 4) return bad("pen param oob");
        break;
    case OP_JMP:
        if (ar >= nInstr) return bad("jmp target oob");
        succ[nSucc++] = ar;
        return true;
    case OP_JMPIF0:
        if (ar >= nInstr) return bad("jmp target oob");
        if (pc + 1 >= nInstr) return bad("fallthrough past end");
        succ[nSucc++] = ar;
        succ[nSucc++] = uint32_t(pc + 1);
        return true;
    default:
        break;
    }
    if (pc + 1 >= nInstr) return bad("fallthrough past end");
    succ[nSucc++] = uint32_t(pc + 1);   // 直進
    return true;
}

} // namespace

bool validateBytecode(const uint32_t* code, size_t nInstr,
                      uint32_t nSlots, uint32_t nGlobals, uint32_t nWb,
                      std::string* why) {
    if (!code || nInstr == 0 || nInstr > 65536) {
        if (why) *why = "bad code length";
        return false;
    }
    if (nSlots > 96 || nWb > 128) {
        if (why) *why = "too many slots/wb";
        return false;
    }
    auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    // エントリスタック高さ（-1 = 未到達）。合流/バックエッジでは
    // 高さ一致を要求（不一致は未定義のスタック動作 → 拒否）。
    std::vector<int> height(nInstr, -1);
    height[0] = 0;
    std::vector<uint32_t> wl{0};
    uint64_t work = 0;
    while (!wl.empty()) {
        if (++work > kMaxWork) return fail("analysis work limit");
        size_t pc = wl.back();
        wl.pop_back();
        uint32_t op = code[pc * 2];
        int pops = 0, pushes = 0;
        if (!stackEffect(op, pops, pushes)) return fail("unknown op");
        uint32_t succ[2];
        int nSucc = 0;
        if (!successors(code, nInstr, pc, nSlots, nGlobals, nWb,
                        succ, nSucc, why))
            return false;
        int h = height[pc];
        if (h < pops) return fail("stack underflow");
        h += pushes - pops;
        if (h > kMaxStack) return fail("stack overflow");
        for (int i = 0; i < nSucc; ++i) {
            uint32_t s = succ[i];
            if (height[s] < 0) {
                height[s] = h;
                wl.push_back(s);
            } else if (height[s] != h) {
                return fail("stack height mismatch at join");
            }
        }
    }
    return true;
}

// ---- 独立性解析 ---------------------------------------------------------

namespace {

struct DState {
    // definite スロット（最大96 — kMaxSlots 一致）
    uint64_t defLo = 0;   // slot 0..63
    uint64_t defHi = 0;   // slot 64..127
    bool px = false, py = false;
    bool at[5] = {};
    // lanes>0 の入口状態は penUp — mayDown は「レーン>0 が
    // この点で down になり得る」か。carriedMayDown は「この点で
    // 観測されるペン状態が反復間 carried 値に依存し得る」か。
    bool mayDown = false;
    bool carriedMayDown = true;
    bool pdSet = false;   // 全経路で反復内 penUp/penDown 通過済み
    bool live = false;
};

struct Cfg {
    const uint32_t* code;
    size_t n;
    uint32_t nSlots;
    const BodyScan& scan;
    std::vector<DState> st;
    std::set<uint32_t> needsRecur;    // definite でない読みの対象
    // pdSet でない時点での移動/打刻 = carried penDown を観測
    bool carriedObs = false;
    bool endCarriedMayDown = false;   // 到達 HALT の carriedMayDown の和
    bool sawHalt = false;
    std::string why;
    bool ok = true;

    void fail(std::string m) { if (ok) { ok = false; why = std::move(m); } }
    bool isDef(const DState& s, uint32_t slot) const {
        return slot < 64 ? (s.defLo >> slot) & 1 : (s.defHi >> (slot - 64)) & 1;
    }
    void setDef(DState& s, uint32_t slot) const {
        if (slot < 64) s.defLo |= 1ull << slot;
        else s.defHi |= 1ull << (slot - 64);
    }
    // 合流: definite 系は積集合、可能性（mayDown/carriedMayDown）は和集合
    void mergeInto(DState& dst, const DState& src, bool& changed) {
        if (!dst.live) { dst = src; dst.live = true; changed = true; return; }
        uint64_t lo = dst.defLo & src.defLo, hi = dst.defHi & src.defHi;
        bool px = dst.px && src.px, py = dst.py && src.py;
        bool at[5];
        for (int k = 0; k < 5; ++k) at[k] = dst.at[k] && src.at[k];
        bool md = dst.mayDown || src.mayDown;
        bool cmd = dst.carriedMayDown || src.carriedMayDown;
        bool ps = dst.pdSet && src.pdSet;
        if (lo != dst.defLo || hi != dst.defHi || px != dst.px ||
            py != dst.py || md != dst.mayDown || cmd != dst.carriedMayDown ||
            ps != dst.pdSet || std::memcmp(at, dst.at, sizeof(at)) != 0) {
            dst.defLo = lo; dst.defHi = hi; dst.px = px; dst.py = py;
            dst.mayDown = md; dst.carriedMayDown = cmd; dst.pdSet = ps;
            std::memcpy(dst.at, at, sizeof(at));
            changed = true;
        }
    }

    void transfer(size_t pc, DState& s) {
        const uint32_t op = code[pc * 2], ar = code[pc * 2 + 1];
        int pops, pushes;
        stackEffect(op, pops, pushes);
        auto posOk = [&]() -> bool {
            // 各成分は反復内確定か本体未書込み（=全レーン一様）必須。
            return (s.px || !scan.writtenX) && (s.py || !scan.writtenY);
        };
        auto attrOk = [&]() -> bool {
            for (int k = 0; k < 5; ++k)
                if (!s.at[k] && scan.writtenAttr[k]) return false;
            return true;
        };
        auto strokeGate = [&] {
            // pdSet でない実行経路は carried penDown を観測する
            // （終端が carried 依存になり得る場合のみ最終集約で拒否）。
            if (!s.pdSet) carriedObs = true;
            // lanes>0 で stroke が実際に起き得るのは mayDown 時のみ。
            if (!s.mayDown) return;
            if (!posOk()) fail("stroke before position set");
            if (!attrOk()) fail("stroke before attributes set");
        };
        switch (op) {
        case OP_LOADV:
            if (!isDef(s, ar)) {
                if (ar < scan.slotWritten.size() && scan.slotWritten[ar])
                    needsRecur.insert(ar);
                else
                    fail("load of never-written slot");
            }
            break;
        case OP_STOREV: case OP_STOREVM:
            setDef(s, ar & 0xFFFFu);
            break;
        case OP_POSX:
            if (!s.px && scan.writtenX) fail("xpos carried read");
            break;
        case OP_POSY:
            if (!s.py && scan.writtenY) fail("ypos carried read");
            break;
        case OP_GOTOXY:
            strokeGate();
            s.px = s.py = true;
            break;
        case OP_SETX: case OP_CHANGEX:
            strokeGate();
            if (op == OP_CHANGEX && !s.px) fail("changex carried pos");
            s.px = true;
            break;
        case OP_SETY: case OP_CHANGEY:
            strokeGate();
            if (op == OP_CHANGEY && !s.py) fail("changey carried pos");
            s.py = true;
            break;
        case OP_PENDOWN:
            // CPU 意味論では up→down 遷移でのみドット打刻。
            // lanes>0 は up 開始なので必ず打刻 — carried 観測でもある。
            if (!s.pdSet) carriedObs = true;
            if (!posOk()) { fail("pendown before position set"); break; }
            if (!attrOk()) { fail("pendown before attributes set"); break; }
            s.mayDown = true;
            s.carriedMayDown = true;
            s.pdSet = true;
            break;
        case OP_PENUP:
            s.mayDown = false;
            s.carriedMayDown = false;
            s.pdSet = true;
            break;
        case OP_PENSIZE:
            s.at[0] = true;
            break;
        case OP_PENSIZED:
            if (!s.at[0]) fail("pen size carried accumulation");
            break;
        case OP_PENCOLN:
            s.at[1] = s.at[2] = s.at[3] = s.at[4] = true;
            break;
        case OP_PENCOLP:
            s.at[int(ar) + 1] = true;
            break;
        case OP_PENCOLPB:
            if (!s.at[int(ar) + 1])
                fail("pen param carried accumulation");
            break;
        case OP_HALT:
            sawHalt = true;
            if (s.carriedMayDown) endCarriedMayDown = true;
            break;
        default:
            break;
        }
        (void)pops; (void)pushes;
    }
};

} // namespace

CfgResult analyzeCfg(const uint32_t* code, size_t nInstr,
                     uint32_t nSlots, const BodyScan& scan,
                     const std::vector<uint32_t>& initiallyDefined) {
    CfgResult bad;
    {
        std::string vw;
        if (!validateBytecode(code, nInstr, nSlots, UINT32_MAX, 128, &vw)) {
            bad.ok = false; bad.why = vw; return bad;
        }
    }
    for (uint32_t s0 : initiallyDefined)
        if (s0 >= nSlots) { bad.why = "bad initial slot"; return bad; }
    Cfg c{code, nInstr, nSlots, scan};
    c.st.resize(nInstr);
    DState entry;
    Cfg tmp{code, nInstr, nSlots, scan};
    for (uint32_t s : initiallyDefined) tmp.setDef(entry, s);
    entry.live = true;
    c.st[0] = entry;
    std::vector<uint32_t> wl{0};
    std::vector<char> inWl(nInstr, 0);
    inWl[0] = 1;
    uint64_t work = 0;
    while (!wl.empty() && c.ok) {
        if (++work > kMaxWork) { c.fail("analysis work limit"); break; }
        size_t pc = wl.back();
        wl.pop_back();
        inWl[pc] = 0;
        DState s = c.st[pc];
        c.transfer(pc, s);
        uint32_t succ[2];
        int nSucc = 0;
        std::string sw;
        if (!successors(code, nInstr, pc, nSlots, 1u << 20, 1u << 20,
                        succ, nSucc, &sw)) {
            c.fail(sw.empty() ? "bad successor" : sw);
            break;
        }
        for (int i = 0; i < nSucc; ++i) {
            bool changed = false;
            c.mergeInto(c.st[succ[i]], s, changed);
            if (changed && !inWl[succ[i]]) {
                inWl[succ[i]] = 1;
                wl.push_back(succ[i]);
            }
        }
    }
    CfgResult r;
    r.ok = c.ok;
    r.why = std::move(c.why);
    if (c.ok) {
        // carried penDown を観測した移動/打刻がある場合、終端 pd が
        // carried に依存し得る（直列 lane>0 の入口状態が VM の
        // penUp 開始と異なり得る）ものは拒否。
        if (scan.hasPenOps && c.carriedObs && c.endCarriedMayDown) {
            r.ok = false;
            r.why = "stroke may observe carried pen state";
        }
        r.needsRecurrence.assign(c.needsRecur.begin(), c.needsRecur.end());
    }
    return r;
}

// ---- affine 再帰 ---------------------------------------------------------

namespace {

struct AbsV {
    enum K : uint8_t { Unk, Const, Self } k = Unk;
    int64_t v = 0;   // Const: 値 / Self: オフセット
    bool operator==(const AbsV&) const = default;
};

struct SState {
    bool live = false;
    std::vector<AbsV> slots;
    std::vector<AbsV> stack;
};

bool constInt(double d, int64_t& out) {
    if (!std::isfinite(d) || d != std::floor(d) ||
        d < -16777216.0 || d > 16777216.0)
        return false;
    out = int64_t(d);
    return true;
}

struct Sym {
    const uint32_t* code;
    size_t n;
    uint32_t nSlots, target;
    const std::vector<uint32_t>* rounded = nullptr;
    std::vector<SState> st;
    int64_t step = 0;
    bool haveStep = false, conflict = false;
    int64_t minOff = 0, maxOff = 0;
    uint64_t work = 0;
    static constexpr uint64_t kWork = 2000000;

    void noteSelf(const AbsV& v) {
        if (v.k == AbsV::Self) {
            minOff = std::min(minOff, v.v);
            maxOff = std::max(maxOff, v.v);
        }
    }
    AbsV mergeV(const AbsV& a, const AbsV& b) const {
        return a == b ? a : AbsV{};
    }
    void mergeInto(SState& d, const SState& s, bool& changed) {
        if (!d.live) { d = s; changed = true; return; }
        for (size_t i = 0; i < d.slots.size(); ++i) {
            AbsV m = mergeV(d.slots[i], s.slots[i]);
            if (!(m == d.slots[i])) { d.slots[i] = m; changed = true; }
        }
        for (size_t i = 0; i < d.stack.size(); ++i) {
            AbsV m = mergeV(d.stack[i], s.stack[i]);
            if (!(m == d.stack[i])) { d.stack[i] = m; changed = true; }
        }
    }
    AbsV pop(SState& s) { AbsV v = s.stack.back(); s.stack.pop_back(); return v; }
    void transfer(size_t pc, SState& s) {
        const uint32_t op = code[pc * 2], ar = code[pc * 2 + 1];
        switch (op) {
        case OP_PUSH: {
            float f;
            std::memcpy(&f, &ar, 4);
            AbsV v;
            int64_t iv;
            // ソースリテラルが f32 に丸められた PUSH は厳密整数
            // として扱えない（double ソースの情報損失）
            bool rounded_ = false;
            if (rounded)
                for (uint32_t q : *rounded)
                    if (q == uint32_t(pc)) { rounded_ = true; break; }
            if (!rounded_ && constInt(f, iv)) {
                v.k = AbsV::Const; v.v = iv;
            }
            s.stack.push_back(v);
            break;
        }
        case OP_LOADV:
            s.stack.push_back(ar < s.slots.size() ? s.slots[ar] : AbsV{});
            break;
        case OP_STOREV: case OP_STOREVM: {
            AbsV v = pop(s);
            uint32_t slot = ar & 0xFFFFu;
            if (slot < s.slots.size()) {
                s.slots[slot] = v;
                if (slot == target) noteSelf(v);
            }
            break;
        }
        case OP_ITER: case OP_LOADG: case OP_POSX: case OP_POSY:
        case OP_RAND:
            if (op == OP_RAND) { pop(s); pop(s); }
            s.stack.push_back(AbsV{});
            break;
        case OP_ADD: case OP_SUB: {
            AbsV b = pop(s), a = pop(s), r;
            int64_t sign = op == OP_ADD ? 1 : -1;
            if (a.k == AbsV::Const && b.k == AbsV::Const) {
                int64_t vv = a.v + sign * b.v;
                if (vv >= -16777216 && vv <= 16777216) {
                    r.k = AbsV::Const; r.v = vv;
                }
            } else if (a.k == AbsV::Self && b.k == AbsV::Const) {
                int64_t vv = a.v + sign * b.v;
                if (vv >= -16777216 && vv <= 16777216) {
                    r.k = AbsV::Self; r.v = vv;
                }
            } else if (op == OP_ADD && a.k == AbsV::Const &&
                       b.k == AbsV::Self) {
                int64_t vv = b.v + a.v;
                if (vv >= -16777216 && vv <= 16777216) {
                    r.k = AbsV::Self; r.v = vv;
                }
            } else if (op == OP_SUB && a.k == AbsV::Const &&
                       b.k == AbsV::Self) {
                // const - self は self の affine ではない
            }
            s.stack.push_back(r);
            break;
        }
        case OP_MUL: case OP_DIV: case OP_MOD: {
            AbsV b = pop(s), a = pop(s), r;
            if (a.k == AbsV::Const && b.k == AbsV::Const) {
                int64_t vv = 0;
                bool okv = true;
                if (op == OP_MUL) vv = a.v * b.v;
                else if (op == OP_DIV) {
                    if (b.v != 0 && a.v % b.v == 0) vv = a.v / b.v;
                    else okv = false;
                } else {
                    // Scratch floor-mod と C の trunc-mod は負数で異
                    // なる — 証明では保守的に Unknown
                    okv = false;
                }
                if (okv && vv >= -16777216 && vv <= 16777216) {
                    r.k = AbsV::Const; r.v = vv;
                }
            }
            s.stack.push_back(r);
            break;
        }
        case OP_NEG: {
            AbsV a = pop(s), r;
            if (a.k == AbsV::Const) { r.k = AbsV::Const; r.v = -a.v; }
            s.stack.push_back(r);
            break;
        }
        case OP_MATH: {
            AbsV a = pop(s), r;
            if (a.k == AbsV::Const &&
                (ar == M_ABS || ar == M_FLOOR || ar == M_CEIL ||
                 ar == M_ROUND)) {
                int64_t vv = a.v;
                if (ar == M_ABS) vv = vv < 0 ? -vv : vv;
                if (vv >= -16777216 && vv <= 16777216) {
                    r.k = AbsV::Const; r.v = vv;
                }
            }
            s.stack.push_back(r);
            break;
        }
        case OP_LT: case OP_GT: case OP_EQ: case OP_LE: case OP_GE:
        case OP_NEQ: case OP_AND: case OP_OR:
            pop(s); pop(s);
            s.stack.push_back(AbsV{});
            break;
        case OP_NOT:
            pop(s);
            s.stack.push_back(AbsV{});
            break;
        case OP_JMPIF0:
            pop(s);
            break;
        case OP_GOTOXY:
            pop(s); pop(s);
            break;
        case OP_SETX: case OP_SETY: case OP_CHANGEX: case OP_CHANGEY:
        case OP_PENSIZE: case OP_PENSIZED: case OP_PENCOLN:
        case OP_PENCOLP: case OP_PENCOLPB:
            pop(s);
            break;
        case OP_HALT: {
            AbsV v = s.slots[target];
            noteSelf(v);
            if (v.k != AbsV::Self) { conflict = true; break; }
            if (!haveStep) { haveStep = true; step = v.v; }
            else if (step != v.v) conflict = true;
            break;
        }
        default:
            break;
        }
        // 中間式の Self オフセットも exactness 境界に含める
        // （i+2^24-2^24 のような打ち消しで i0±2^24 が要ることを
        //   実行時ガードへ伝える）
        for (const AbsV& v : s.stack) noteSelf(v);
        for (const AbsV& v : s.slots) noteSelf(v);
    }
};

} // namespace

Recurrence analyzeRecurrence(const uint32_t* code, size_t nInstr,
                             uint32_t nSlots, uint32_t slot,
                             const std::vector<uint32_t>* roundedPushes) {
    Recurrence out;
    if (slot >= nSlots) return out;
    {
        std::string vw;
        if (!validateBytecode(code, nInstr, nSlots, UINT32_MAX, 128, &vw))
            return out;
    }
    Sym c{code, nInstr, nSlots, slot};
    c.rounded = roundedPushes;
    c.st.resize(nInstr);
    for (auto& s : c.st) s.slots.assign(nSlots, AbsV{});
    SState entry;
    entry.slots.assign(nSlots, AbsV{});
    entry.slots[slot] = AbsV{AbsV::Self, 0};
    entry.live = true;
    c.st[0] = entry;
    std::vector<uint32_t> wl{0};
    std::vector<char> inWl(nInstr, 0);
    inWl[0] = 1;
    bool sawHalt = false;
    while (!wl.empty() && !c.conflict && c.work < Sym::kWork) {
        ++c.work;
        size_t pc = wl.back();
        wl.pop_back();
        inWl[pc] = 0;
        SState s = c.st[pc];
        c.noteSelf(s.slots[slot]);
        if (code[pc * 2] == OP_HALT) sawHalt = true;
        c.transfer(pc, s);
        if (c.conflict) break;
        uint32_t succ[2];
        int nSucc = 0;
        std::string sw;
        if (!successors(code, nInstr, pc, nSlots, 1u << 20, 1u << 20,
                        succ, nSucc, &sw))
            break;
        for (int i = 0; i < nSucc; ++i) {
            bool changed = false;
            c.mergeInto(c.st[succ[i]], s, changed);
            if (changed && !inWl[succ[i]]) {
                inWl[succ[i]] = 1;
                wl.push_back(succ[i]);
            }
        }
    }
    if (c.work >= Sym::kWork) return out;   // 収束しない → 昇格しない
    out.proven = c.haveStep && !c.conflict && sawHalt;
    out.step = c.step;
    out.minOff = c.minOff;
    out.maxOff = c.maxOff;
    return out;
}

bool penOrderOffsets(const uint32_t* counts, uint32_t lanes,
                     uint32_t maxPerLane, uint32_t* offsetsOut,
                     uint64_t* totalOut) {
    if (!offsetsOut || !totalOut) return false;
    if (lanes && !counts) return false;
    uint64_t total = 0;
    for (uint32_t i = 0; i < lanes; ++i) {
        uint32_t n = counts[i] & 0x7FFFFFFFu;
        if (counts[i] & 0x80000000u) return false;   // guarded lane
        if (n > maxPerLane) return false;
        offsetsOut[i] = uint32_t(total);
        total += n;
        if (total > UINT32_MAX) return false;
    }
    *totalOut = total;
    return true;
}

} // namespace xyo::gpu
