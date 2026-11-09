// xyo-cpp: frame-boundary hot-start snapshots. No native pointer is serialized.
// Save only between Runtime::step calls; do not serialize JIT/fiber stacks.
#include "hot_start.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../blocks/pen_api.hpp"
#include "../core/json.hpp"
#include "../core/rng.hpp"
#include "../core/slist.hpp"
#include "../core/sstr.hpp"
#include "../model/target.hpp"
#include "runtime.hpp"
#include "sequencer.hpp"

namespace xyo {
namespace {
constexpr int kVersion = 1;
constexpr size_t kMaxFile = size_t(512) * 1024 * 1024;
constexpr size_t kMaxThreads = 100000;
constexpr size_t kMaxTargets = 5000;

bool fail(std::string* why, std::string message) {
    if (why) *why = std::move(message);
    return false;
}

std::string hex64(uint64_t n) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(n));
    return buf;
}

bool unhex64(std::string_view text, uint64_t& out) {
    if (text.size() != 16) return false;
    uint64_t v = 0;
    for (char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else return false;
        v = (v << 4) | digit;
    }
    out = v;
    return true;
}

std::string hexBytes(const std::vector<uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t v : bytes) {
        out.push_back(digits[v >> 4]);
        out.push_back(digits[v & 15]);
    }
    return out;
}

bool unhexBytes(std::string_view text, std::vector<uint8_t>& output) {
    if ((text.size() & 1) || text.size() > 64 * 1024 * 1024)
        return false;
    output.resize(text.size() / 2);
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < output.size(); ++i) {
        const int hi = nibble(text[i * 2]);
        const int lo = nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        output[i] = uint8_t((hi << 4) | lo);
    }
    return true;
}

JsonValue valToJson(Value value) {
    JsonArray a;
    if (value.isString()) {
        a.emplace_back(std::string("s"));
        SStr* str = value.asStrPtr();
        a.emplace_back(str ? SStr::toUtf8(str->view()) : "");
    } else {
        a.emplace_back(std::string("b"));
        a.emplace_back(hex64(value.bits));
    }
    return JsonValue(std::move(a));
}

bool valFromJson(const JsonValue& j, Value& out) {
    if (!j.isArray() || j.size() != 2 ||
        !j.at(0).isString() || !j.at(1).isString())
        return false;
    if (j.at(0).asString() == "s") {
        const std::string& s = j.at(1).asString();
        out = s.empty() ? Value::emptyStr() : Value::string(SStr::fromUtf8(s));
        return true;
    }
    uint64_t bits;
    if (j.at(0).asString() != "b" ||
        !unhex64(j.at(1).asString(), bits))
        return false;
    // Untrusted files must never be able to smuggle a native heap address
    // into a Value. Strings are encoded by content only, not raw pointers.
    const uint64_t high = bits >> 48;
    if (high == 0x7FF9 ||
        (high == 0x7FF8 && bits > val::kUndefined) ||
        (high == 0x7FFA && (bits & val::kPtrMask) > 0xFFFFFF) ||
        (high >= 0x7FFB && high < 0x8000))
        return false;
    out = Value{bits};
    return true;
}

JsonValue mapToJson(const std::unordered_map<std::string, Value>& values) {
    JsonObject result;
    for (const auto& [key, value] : values)
        result[key] = valToJson(value);
    return result;
}

bool mapFromJson(const JsonValue& j,
                 std::unordered_map<std::string, Value>& values) {
    if (!j.isObject()) return false;
    values.clear();
    for (const auto& [key, raw] : j.asObject()) {
        Value v;
        if (!valFromJson(raw, v)) return false;
        values.emplace(key, v);
    }
    return true;
}

JsonValue varsToJson(const Target& target) {
    JsonArray rows;
    for (const auto& [id, variable] : target.variables) {
        JsonObject row{
            {"id", id},
            {"name", SStr::toUtf8(variable.name)},
            {"type", variable.type},
            {"cloud", variable.isCloud},
            {"value", valToJson(variable.value)},
            {"profile", JsonValue(double(variable.typeProfile()))},
        };
        if (variable.list) {
            JsonArray list;
            list.reserve(variable.list->len);
            for (uint32_t i = 0; i < variable.list->len; ++i)
                list.emplace_back(valToJson(variable.list->get0(i)));
            row.emplace("list", std::move(list));
        }
        rows.emplace_back(std::move(row));
    }
    return rows;
}

bool varsFromJson(const JsonValue& j, Target& t, std::string* reason) {
    if (!j.isArray()) return fail(reason, "invalid variable table");
    t.variables.clear();
    for (const JsonValue& row : j.asArray()) {
        if (!row.isObject() || !row.get("id").isString() ||
            !row.get("name").isString() || !row.get("type").isString())
            return fail(reason, "invalid variable entry");
        const auto& id = row.get("id").asString();
        const auto& kind = row.get("type").asString();
        Variable variable = Variable::create(
            id, std::u16string(SStr::fromUtf8(row.get("name").asString())->view()), kind,
            row.get("cloud").asBool());
        if (!valFromJson(row.get("value"), variable.value))
            return fail(reason, "invalid variable value");
        if (kind == Variable::kListType) {
            const JsonValue& items = row.get("list");
            if (!items.isArray() || items.size() > 2000000)
                return fail(reason, "invalid or oversized list");
            variable.list->clear();
            for (const JsonValue& cell : items.asArray()) {
                Value v;
                if (!valFromJson(cell, v))
                    return fail(reason, "invalid list item");
                variable.list->push(v);
            }
        } else if (row.has("list")) {
            return fail(reason, "list value on scalar variable");
        }
        variable.observedKinds = uint32_t(row.get("profile").asNumber());
        if (!t.variables.emplace(id, std::move(variable)).second)
            return fail(reason, "duplicate variable ID");
    }
    return true;
}

JsonValue effectsToJson(const GraphicEffects& fx) {
    JsonObject effects;
    effects["present"] = JsonValue(int(fx.present));
    JsonArray v;
    for (double n : fx.values) v.emplace_back(n);
    effects["values"] = std::move(v);
    JsonObject custom;
    for (const auto& [key, value] : fx.custom)
        custom[key] = JsonValue(value);
    effects["custom"] = std::move(custom);
    return effects;
}

bool effectsFromJson(const JsonValue& j, GraphicEffects& fx) {
    if (!j.isObject() || j.get("values").size() != 7 ||
        !j.get("custom").isObject()) return false;
    fx.present = uint8_t(j.get("present").asNumber());
    for (size_t i = 0; i < 7; ++i)
        fx.values[i] = j.get("values").at(i).asNumber();
    fx.custom.clear();
    for (const auto& [key, value] : j.get("custom").asObject())
        fx.custom[key] = value.asNumber();
    return true;
}

JsonValue penStateToJson(const PenState* p) {
    if (!p) return nullptr;
    JsonObject obj{
        {"down", p->penDown}, {"color", p->color},
        {"saturation", p->saturation}, {"brightness", p->brightness},
        {"transparency", p->transparency}, {"shade", p->shade},
        {"size", p->penSize}, {"dirty", p->colorDirty},
        {"last", hex64(p->lastSetColor)},
    };
    JsonArray rgba;
    for (float component : p->rgba) rgba.emplace_back(double(component));
    obj.emplace("rgba", std::move(rgba));
    return obj;
}

bool penStateFromJson(const JsonValue& j, Target& t) {
    t.penState.reset();
    t.onTargetMoved = nullptr;
    if (j.isNull()) return true;
    if (!j.isObject() || j.get("rgba").size() != 4)
        return false;
    t.penState = std::make_unique<PenState>();
    auto& p = *t.penState;
    p.penDown = j.get("down").asBool();
    p.color = j.get("color").asNumber();
    p.saturation = j.get("saturation").asNumber();
    p.brightness = j.get("brightness").asNumber();
    p.transparency = j.get("transparency").asNumber();
    p.shade = j.get("shade").asNumber();
    p.penSize = j.get("size").asNumber();
    p.colorDirty = j.get("dirty").asBool();
    if (!unhex64(j.get("last").asString(), p.lastSetColor))
        return false;
    for (size_t i = 0; i < 4; ++i)
        p.rgba[i] = float(j.get("rgba").at(i).asNumber());
    if (p.penDown) t.onTargetMoved = penrt::movedHook();
    return true;
}

JsonValue targetToJson(const Target& t,
                       const std::unordered_map<const Sprite*, size_t>& origins) {
    JsonObject row{
        {"id", t.id},
        {"name", SStr::toUtf8(t.getName())},
        {"original", t.isOriginal},
        {"stage", t.isStage},
        {"x", t.x}, {"y", t.y}, {"direction", t.direction},
        {"size", t.size}, {"costume", t.currentCostume},
        {"visible", t.visible}, {"draggable", t.draggable},
        {"rotation", t.rotationStyle},
        {"layer", t.layerOrder},
        {"drawableLayer", t.drawableLayerOrder},
        {"volume", t.volume}, {"tempo", t.tempo},
        {"videoTransparency", t.videoTransparency},
        {"videoState", t.videoState},
        {"textToSpeechLanguage", t.textToSpeechLanguage},
        {"bubble", SStr::toUtf8(t.bubbleText)},
        {"bubbleType", t.bubbleType},
        {"bubbleUsage", t.bubbleUsageId},
        {"variables", varsToJson(t)},
        {"effects", effectsToJson(t.effects)},
        {"pen", penStateToJson(t.penState.get())},
    };
    if (t.sprite) {
        auto it = origins.find(t.sprite);
        row["sprite"] = it == origins.end() ? JsonValue(-1)
                                            : JsonValue(int(it->second));
    } else row["sprite"] = -1;
    JsonObject sound;
    for (const auto& [key, value] : t.soundEffects)
        sound[key] = JsonValue(value);
    row["soundEffects"] = std::move(sound);
    JsonObject edges;
    for (const auto& [key, value] : t.edgeActivatedHatValues)
        edges[key] = valToJson(value);
    row["edge"] = std::move(edges);
    JsonArray collision;
    for (const auto& [key, v] : t.collidingBroadcasts)
        collision.emplace_back(JsonObject{
            {"id", key}, {"name", SStr::toUtf8(v.name)},
            {"type", v.type}, {"value", valToJson(v.value)},
        });
    row["collidingBroadcasts"] = std::move(collision);
    return row;
}

bool targetFromJson(const JsonValue& row, Target& t, std::string* why) {
    if (!row.isObject() || !row.get("variables").isArray() ||
        !row.get("id").isString())
        return fail(why, "invalid target entry");
    t.restoreId(row.get("id").asString());
    t.isOriginal = row.get("original").asBool();
    t.x = row.get("x").asNumber();
    t.y = row.get("y").asNumber();
    t.direction = row.get("direction").asNumber();
    t.size = row.get("size").asNumber();
    t.currentCostume = int(row.get("costume").asNumber());
    t.visible = row.get("visible").asBool();
    t.draggable = row.get("draggable").asBool();
    t.rotationStyle = row.get("rotation").asString();
    t.layerOrder = int(row.get("layer").asNumber());
    t.drawableLayerOrder = int(row.get("drawableLayer").asNumber());
    t.volume = row.get("volume").asNumber();
    t.tempo = row.get("tempo").asNumber();
    t.videoTransparency = row.get("videoTransparency").asNumber();
    t.videoState = row.get("videoState").asString();
    t.textToSpeechLanguage =
        row.get("textToSpeechLanguage").asString();
    t.bubbleText = SStr::fromUtf8(row.get("bubble").asString())->view();
    t.bubbleType = row.get("bubbleType").asString();
    t.bubbleUsageId = int(row.get("bubbleUsage").asNumber());
    if (!varsFromJson(row.get("variables"), t, why)) return false;
    if (!effectsFromJson(row.get("effects"), t.effects))
        return fail(why, "invalid target graphics effects");
    if (!penStateFromJson(row.get("pen"), t))
        return fail(why, "invalid target pen state");
    t.soundEffects.clear();
    for (const auto& [key, value] : row.get("soundEffects").asObject())
        t.soundEffects[key] = value.asNumber();
    t.edgeActivatedHatValues.clear();
    for (const auto& [key, value] : row.get("edge").asObject()) {
        Value v;
        if (!valFromJson(value, v)) return fail(why, "invalid edge-hat value");
        t.edgeActivatedHatValues.emplace(key, v);
    }
    t.collidingBroadcasts.clear();
    for (const JsonValue& b : row.get("collidingBroadcasts").asArray()) {
        Variable v = Variable::create(
            b.get("id").asString(),
            std::u16string(SStr::fromUtf8(b.get("name").asString())->view()),
            b.get("type").asString(), false);
        if (!valFromJson(b.get("value"), v.value))
            return fail(why, "invalid broadcast value");
        t.collidingBroadcasts.emplace(v.id, std::move(v));
    }
    return true;
}

struct ThreadHandleTable {
    std::vector<Thread*> all;
    std::unordered_map<const Thread*, size_t> index;
    void add(Thread* t) {
        if (t && !index.contains(t)) {
            index.emplace(t, all.size());
            all.push_back(t);
        }
    }
    int of(Thread* t) const {
        if (!t) return -1;
        auto it = index.find(t);
        return it == index.end() ? -1 : int(it->second);
    }
};

SoundBankAPI* soundBankFor(Runtime& rt) {
    for (Target* t : rt.targets)
        if (t && t->sprite && t->sprite->soundBank)
            return t->sprite->soundBank;
    return nullptr;
}

bool snapshotEligible(const Runtime& rt, const ThreadHandleTable& table,
                      const std::vector<SoundPlaybackState>& soundStates,
                      std::string* why) {
    if (rt.sequencer->stepping || rt.sequencer->activeThread ||
        !rt.sequencer->detachedLists.empty())
        return fail(why, "snapshot must occur between runtime frames");
    if (!rt.questionList.empty())
        return fail(why, "ask-and-wait is not checkpointable");
    if (rt.targets.size() > kMaxTargets || table.all.size() > kMaxThreads)
        return fail(why, "snapshot exceeds target/thread limits");
    for (Thread* t : table.all) {
        if (t->isCompiled || t->fiber || t->jitCtx || t->numericSession ||
            t->wasmLoopPhase || t->wasmRegionIndex || t->hasCompatStackFrame)
            return fail(why, "active JIT/fiber continuation: save with --jit=off");
        if (t->stack.size() != t->stackFrames.size())
            return fail(why, "missing interpreter stack frame");
        if (t->status == STATUS_PROMISE_WAIT) {
            const bool inWait = std::any_of(rt.waitQueue.begin(), rt.waitQueue.end(),
                [t](const auto& w) { return w.thread == t; });
            const bool inAudio = std::any_of(
                soundStates.begin(), soundStates.end(),
                [t](const auto& voice) { return voice.wait == t; });
            if (!inWait && !inAudio)
                return fail(why, "unsupported external promise wait: " +
                            t->topBlock);
        }
    }
    return true;
}

JsonValue frameToJson(const StackFrame& f, const ThreadHandleTable& table,
                      const Runtime& rt) {
    JsonArray reported;
    for (const auto& r : f.reported)
        reported.emplace_back(JsonArray{r.opCachedId, valToJson(r.inputValue)});
    JsonObject params;
    for (const auto& [key, value] : f.params)
        params[SStr::toUtf8(key)] = valToJson(value);
    JsonObject row{
        {"loop", f.isLoop}, {"warp", f.warpMode},
        {"reporting", f.reporting}, {"hasReported", f.hasReported},
        {"reported", std::move(reported)},
        {"waitingReporter", f.waitingReporter},
        {"suspendLast", f.suspendLast},
        {"hasParams", f.hasParams}, {"params", std::move(params)},
        {"op", f.op ? f.op->id : ""},
        {"current", f.currentBlock ? f.currentBlock->id : ""},
    };
    if (f.executionContext) {
        const ExecContext& c = *f.executionContext;
        JsonObject ctx{
            {"vals", mapToJson(c.vals)}, {"duration", c.duration},
            {"executed", c.executed}, {"return", valToJson(c.returnValue)},
            {"hasLoopCounter", c.hasLoopCounter}, {"loopCounter", c.loopCounter},
            {"hasIndex", c.hasIndex}, {"index", c.index},
            {"startX", c.startX}, {"startY", c.startY},
            {"endX", c.endX}, {"endY", c.endY},
            {"hasStartedThreads", c.hasStartedThreads},
        };
        if (c.timer) {
            const double elapsed = c.timer->timeElapsed();
            ctx["timerElapsed"] = JsonValue(elapsed);
        }
        if (c.broadcastVar) {
            for (size_t i = 0; i < rt.targets.size(); ++i) {
                for (const auto& [id, variable] : rt.targets[i]->variables)
                    if (&variable == c.broadcastVar) {
                        ctx["broadcastVar"] =
                            JsonArray{JsonValue(int(i)), JsonValue(id)};
                    }
            }
        }
        JsonArray started;
        for (Thread* t : c.startedThreads)
            started.emplace_back(table.of(t));
        ctx["startedThreads"] = std::move(started);
        row["context"] = JsonValue(std::move(ctx));
    }
    return row;
}

bool frameFromJson(const JsonValue& j, StackFrame& f, Runtime& rt,
                   Blocks* blocks, std::string* why) {
    if (!j.isObject() || !j.get("reported").isArray() ||
        !j.get("params").isObject())
        return fail(why, "invalid stack frame");
    f.isLoop = j.get("loop").asBool();
    f.warpMode = j.get("warp").asBool();
    f.reporting = j.get("reporting").asString();
    f.hasReported = j.get("hasReported").asBool();
    f.waitingReporter = j.get("waitingReporter").asBool();
    f.suspendLast = j.get("suspendLast").asBool();
    f.hasParams = j.get("hasParams").asBool();
    for (const auto& [key, raw] : j.get("params").asObject()) {
        Value v;
        if (!valFromJson(raw, v)) return fail(why, "invalid procedure parameter");
        f.params.emplace(SStr::fromUtf8(key)->view(), v);
    }
    for (const auto& entry : j.get("reported").asArray()) {
        Value v;
        if (entry.size() != 2 || !valFromJson(entry.at(1), v))
            return fail(why, "invalid reported entry");
        f.reported.push_back({entry.at(0).asString(), v});
    }
    const std::string& op = j.get("op").asString();
    const std::string& current = j.get("current").asString();
    f.op = op.empty() ? nullptr : blocks->getCachedExecute(op);
    f.currentBlock = current.empty() ? nullptr : blocks->getCachedExecute(current);
    if ((!op.empty() && !f.op) || (!current.empty() && !f.currentBlock))
        return fail(why, "missing cached block in restored script");
    const auto& rawCtx = j.get("context");
    if (rawCtx.isNull()) return true;
    if (!rawCtx.isObject()) return fail(why, "invalid execution context");
    auto ctx = std::make_unique<ExecContext>();
    if (!mapFromJson(rawCtx.get("vals"), ctx->vals))
        return fail(why, "invalid execution context values");
    ctx->duration = rawCtx.get("duration").asNumber();
    ctx->executed = rawCtx.get("executed").asBool();
    if (!valFromJson(rawCtx.get("return"), ctx->returnValue))
        return fail(why, "invalid return value");
    ctx->hasLoopCounter = rawCtx.get("hasLoopCounter").asBool();
    ctx->loopCounter = rawCtx.get("loopCounter").asNumber();
    ctx->hasIndex = rawCtx.get("hasIndex").asBool();
    ctx->index = rawCtx.get("index").asNumber();
    ctx->startX = rawCtx.get("startX").asNumber();
    ctx->startY = rawCtx.get("startY").asNumber();
    ctx->endX = rawCtx.get("endX").asNumber();
    ctx->endY = rawCtx.get("endY").asNumber();
    ctx->hasStartedThreads = rawCtx.get("hasStartedThreads").asBool();
    if (rawCtx.has("timerElapsed")) {
        const double elapsed = rawCtx.get("timerElapsed").asNumber();
        ctx->timer = std::make_unique<Timer>([&rt] { return rt.currentMSecs; });
        ctx->timer->setStartTime(rt.currentMSecs - elapsed);
    }
    f.executionContext = std::move(ctx);
    return true;
}

} // namespace

uint64_t hotStartFingerprint(const uint8_t* bytes, size_t length) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool saveHotStart(Runtime& rt, const std::string& path, uint64_t fingerprint,
                  std::string* why) {
    ThreadHandleTable handles;
    std::vector<SoundPlaybackState> activeSounds;
    if (SoundBankAPI* bank = soundBankFor(rt))
        activeSounds = bank->captureActiveSounds();
    for (Thread* t : rt.threads) handles.add(t);
    // A stopped script may leave a stale waiter in the SDL playback queue.
    // Never dereference or resurrect it: the audio mixer already ignores
    // wait handles that are no longer present in rt.threads.
    for (auto& voice : activeSounds) {
        if (voice.wait &&
            std::find(rt.threads.begin(), rt.threads.end(), voice.wait) ==
                rt.threads.end())
            voice.wait = nullptr;
        handles.add(voice.wait);
    }
    for (const auto& wait : rt.waitQueue) handles.add(wait.thread);
    for (size_t i = 0; i < handles.all.size(); ++i) {
        const Thread* t = handles.all[i];
        for (const auto& f : t->stackFrames)
            if (f->executionContext)
                for (Thread* started : f->executionContext->startedThreads)
                    handles.add(started);
    }
    if (!snapshotEligible(rt, handles, activeSounds, why)) return false;
    std::unordered_map<const Target*, size_t> targetIndex;
    std::unordered_map<const Sprite*, size_t> originals;
    for (size_t i = 0; i < rt.targets.size(); ++i) {
        targetIndex.emplace(rt.targets[i], i);
        if (rt.targets[i]->isOriginal && rt.targets[i]->sprite)
            originals.emplace(rt.targets[i]->sprite, i);
    }

    std::unordered_map<int, int> drawableRanks;
    if (rt.renderer) {
        const auto order = rt.renderer->drawableOrder();
        for (size_t i = 0; i < order.size(); ++i)
            drawableRanks.emplace(order[i], int(i));
    }
    JsonArray targets;
    for (const Target* target : rt.targets) {
        JsonValue snapshot = targetToJson(*target, originals);
        if (auto it = drawableRanks.find(target->drawableID);
            it != drawableRanks.end())
            snapshot.objectRef()["renderOrder"] = JsonValue(it->second);
        targets.push_back(std::move(snapshot));
    }
    JsonArray running;
    for (Thread* t : rt.threads)
        running.emplace_back(handles.of(t));
    JsonArray threads;
    for (Thread* t : handles.all) {
        auto it = targetIndex.find(t->target);
        if (it == targetIndex.end()) return fail(why, "thread target is unavailable");
        JsonArray stack;
        JsonArray frames;
        for (const auto& block : t->stack) stack.emplace_back(block);
        for (const auto& frame : t->stackFrames)
            frames.emplace_back(frameToJson(*frame, handles, rt));
        JsonObject row{
            {"target", JsonValue(int(it->second))},
            {"top", t->topBlock}, {"stack", std::move(stack)},
            {"frames", std::move(frames)}, {"status", t->status},
            {"killed", t->isKilled}, {"stackClick", t->stackClick},
            {"updateMonitor", t->updateMonitor},
            {"interpreterStarted", t->interpreterStarted},
            {"justReported", valToJson(t->justReported)},
            {"hasWarpTimer", t->hasWarpTimer},
            {"warpElapsed", t->hasWarpTimer
                ? std::max(0.0, t->warpTimer.timeElapsed()) : 0.0},
        };
        threads.emplace_back(std::move(row));
    }
    JsonArray waits;
    for (const auto& w : rt.waitQueue) {
        const auto at = targetIndex.find(w.bubbleTarget);
        if (w.bubbleTarget && at == targetIndex.end())
            return fail(why, "untracked bubble target in promise wait");
        const std::string bubbleType = w.bubbleType ? w.bubbleType : "";
        if (bubbleType != "" && bubbleType != "say" && bubbleType != "think")
            return fail(why, "unsupported timed bubble type");
        waits.emplace_back(JsonObject{
            {"thread", handles.of(w.thread)},
            {"remaining", w.deadlineMs - rt.currentMSecs},
            {"nextTick", w.nextTick},
            {"value", valToJson(w.resolveValue)},
            {"bubbleTarget", at == targetIndex.end() ? -1 : int(at->second)},
            {"bubbleType", bubbleType},
            {"bubbleUsage", w.bubbleUsageId},
        });
    }
    JsonArray sounds;
    for (const SoundPlaybackState& sound : activeSounds) {
        const auto at = targetIndex.find(sound.target);
        if (at == targetIndex.end())
            return fail(why, "audio voice references an absent clone");
        sounds.emplace_back(JsonObject{
            {"target", int(at->second)},
            {"wait", handles.of(sound.wait)},
            {"soundId", sound.soundId},
            {"position", sound.position},
            {"pending", sound.pending},
        });
    }
    JsonArray executable;
    for (Target* t : rt.executableTargets) {
        auto it = targetIndex.find(t);
        if (it == targetIndex.end()) return fail(why, "invalid executable target");
        executable.emplace_back(int(it->second));
    }
    std::vector<uint8_t> pen;
    if (rt.renderer && !rt.renderer->capturePen(pen))
        return fail(why, "unable to capture the GPU pen layer");
    JsonObject root{
        {"format", std::string("xyo-hot-start")},
        {"version", kVersion}, {"project", hex64(fingerprint)},
        {"targets", std::move(targets)}, {"threads", std::move(threads)},
        {"running", std::move(running)}, {"waits", std::move(waits)},
        {"sounds", std::move(sounds)}, {"executable", std::move(executable)},
        {"stepSerial", hex64(rt.stepSerial)}, {"pen", hexBytes(pen)},
        {"rngState", hex64(Rng::global().state())},
        {"rngInc", hex64(Rng::global().increment())},
        {"controlCounter", rt.controlCounter},
        {"answer", SStr::toUtf8(rt.answer)},
        {"clock", rt.ioDevices.clock.projectTimer()},
        {"cloneCounter", rt.cloneCounter},
    };
    const std::string payload = jsonStringify(root);
    if (payload.size() > kMaxFile)
        return fail(why, "checkpoint larger than 512 MiB");
    const auto output = std::filesystem::path(path);
    const auto temporary = std::filesystem::path(path + ".part");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(payload.data(), std::streamsize(payload.size()))) {
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return fail(why, "cannot write checkpoint file");
        }
        out.close();
        if (!out) return fail(why, "checkpoint write failed");
    }
    std::error_code ec;
    std::filesystem::rename(temporary, output, ec);
    if (ec) return fail(why, "cannot atomically replace checkpoint: " + ec.message());
    return true;
}

bool loadHotStart(Runtime& rt, const std::string& path, uint64_t fingerprint,
                  std::string* why) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return fail(why, "cannot open checkpoint");
    const auto bytes = input.tellg();
    if (bytes <= 0 || uint64_t(bytes) > kMaxFile)
        return fail(why, "checkpoint file too large or empty");
    std::string contents(size_t(bytes), '\0');
    input.seekg(0);
    if (!input.read(contents.data(), bytes))
        return fail(why, "checkpoint file truncated");
    const JsonValue root = jsonParse(contents);
    if (!root.isObject() || root.get("format").asString() != "xyo-hot-start" ||
        root.get("version").asNumber() != kVersion)
        return fail(why, "unsupported checkpoint format/version");
    if (root.get("project").asString() != hex64(fingerprint))
        return fail(why, "SB3 contents differ from checkpoint project");
    const auto& trows = root.get("targets").asArray();
    const auto& rows = root.get("threads").asArray();
    const auto& running = root.get("running").asArray();
    if (trows.size() > kMaxTargets || rows.size() > kMaxThreads ||
        !root.get("targets").isArray() || !root.get("threads").isArray() ||
        !root.get("running").isArray() || !root.get("waits").isArray() ||
        !root.get("sounds").isArray() ||
        root.get("sounds").size() > 200000 ||
        trows.size() < rt.targets.size())
        return fail(why, "invalid checkpoint target/thread count");
    uint64_t randomState, randomInc, step;
    if (!unhex64(root.get("rngState").asString(), randomState) ||
        !unhex64(root.get("rngInc").asString(), randomInc) ||
        !unhex64(root.get("stepSerial").asString(), step))
        return fail(why, "invalid checkpoint RNG/step serial");

    // Validate originals before mutating runtime. Runtime must be freshly
    // loaded from the matching SB3, before greenFlag() is ever called.
    const size_t baseTargets = rt.targets.size();
    for (size_t i = 0; i < baseTargets; ++i) {
        if (!trows[i].get("original").asBool() ||
            trows[i].get("name").asString() !=
                SStr::toUtf8(rt.targets[i]->getName()))
            return fail(why, "original targets do not match checkpoint");
    }
    rt.updateCurrentMSecs();
    for (size_t i = baseTargets; i < trows.size(); ++i) {
        const JsonValue& row = trows[i];
        int src = int(row.get("sprite").asNumber());
        if (row.get("original").asBool() || src < 0 ||
            src >= int(baseTargets) || !rt.targets[src]->sprite)
            return fail(why, "invalid clone source in checkpoint");
        Target* target = rt.targets[src]->sprite->createClone();
        rt.addTarget(target);
        if (rt.onTargetCreated) rt.onTargetCreated(target);
    }
    for (size_t i = 0; i < trows.size(); ++i) {
        if (!targetFromJson(trows[i], *rt.targets[i], why)) return false;
    }
    rt.cloneCounter = int(root.get("cloneCounter").asNumber());
    rt.executableTargets.clear();
    for (const auto& at : root.get("executable").asArray()) {
        int i = int(at.asNumber());
        if (i < 0 || i >= int(rt.targets.size()))
            return fail(why, "invalid executable layer index");
        rt.executableTargets.push_back(rt.targets[size_t(i)]);
    }
    if (rt.renderer) {
        std::vector<std::pair<int, Target*>> ordered;
        for (size_t i = 0; i < trows.size(); ++i)
            if (trows[i].get("renderOrder").isNumber() &&
                rt.targets[i]->drawableID >= 0)
                ordered.emplace_back(
                    int(trows[i].get("renderOrder").asNumber()), rt.targets[i]);
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        // Move every drawable to the top in its saved order. This preserves
        // Scratch layer permutations, not just the SB3's initial layerOrder.
        for (const auto& [rank, t] : ordered)
            rt.renderer->setDrawableOrder(
                t->drawableID, std::numeric_limits<double>::infinity(),
                t->isStage ? "backdrop" : "sprite", true);
    }

    rt.threads.clear();
    rt.threadMap.clear();
    rt.ownedThreads.clear();
    rt.reusableThreads.clear();
    rt.waitQueue.clear();
    rt.questionList.clear();
    rt.lastStepDoneThreads.clear();
    std::vector<Thread*> handles;
    handles.reserve(rows.size());
    for (const auto& row : rows) {
        const int target = int(row.get("target").asNumber());
        if (target < 0 || target >= int(rt.targets.size()))
            return fail(why, "invalid thread target index");
        auto holder = std::make_unique<Thread>(row.get("top").asString());
        Thread* t = holder.get();
        t->target = rt.targets[size_t(target)];
        t->stackClick = row.get("stackClick").asBool();
        t->updateMonitor = row.get("updateMonitor").asBool();
        t->blockContainer = t->updateMonitor
            ? rt.monitorBlocks.get() : t->target->blocks;
        t->status = int(row.get("status").asNumber());
        t->isKilled = row.get("killed").asBool();
        t->interpreterStarted = row.get("interpreterStarted").asBool();
        // Never re-enter saved interpreter state via compiled native entry.
        t->triedToCompile = true;
        if (!valFromJson(row.get("justReported"), t->justReported))
            return fail(why, "invalid thread reported value");
        const auto& blocks = row.get("stack").asArray();
        const auto& frames = row.get("frames").asArray();
        if (!row.get("stack").isArray() || !row.get("frames").isArray() ||
            blocks.size() != frames.size() || blocks.size() > 1000)
            return fail(why, "invalid thread call stack");
        for (size_t i = 0; i < blocks.size(); ++i) {
            const std::string& blockId = blocks[i].asString();
            if (!blockId.empty() && !t->blockContainer->getBlock(blockId) &&
                !rt.flyoutBlocks->getBlock(blockId))
                return fail(why, "missing script block after restore");
            t->pushStack(blockId);
            if (!frameFromJson(frames[i], *t->stackFrames.back(),
                               rt, t->blockContainer, why))
                return false;
        }
        t->hasWarpTimer = row.get("hasWarpTimer").asBool();
        if (t->hasWarpTimer)
            t->warpTimer.setStartTime(Timer::stdNow() -
                row.get("warpElapsed").asNumber());
        handles.push_back(t);
        rt.ownedThreads.emplace_back(std::move(holder));
    }

    // Started-thread references (broadcast-and-wait) refer to stable
    // Thread objects. Resolve only after all threads are allocated.
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& frames = rows[i].get("frames").asArray();
        for (size_t k = 0; k < frames.size(); ++k) {
            const auto& context = frames[k].get("context");
            if (context.isNull()) continue;
            ExecContext& dest =
                *handles[i]->stackFrames[k]->executionContext;
            const auto& started = context.get("startedThreads");
            if (!started.isArray()) return fail(why, "invalid started threads");
            for (const auto& index : started.asArray()) {
                const int n = int(index.asNumber());
                if (n < 0 || n >= int(handles.size()))
                    return fail(why, "unresolved started thread");
                dest.startedThreads.push_back(handles[size_t(n)]);
            }
            if (context.has("broadcastVar")) {
                const auto& pair = context.get("broadcastVar");
                const int target = int(pair.at(0).asNumber());
                if (target < 0 || target >= int(rt.targets.size()))
                    return fail(why, "unresolved broadcast variable target");
                auto it = rt.targets[size_t(target)]->variables.find(
                    pair.at(1).asString());
                if (it == rt.targets[size_t(target)]->variables.end())
                    return fail(why, "unresolved broadcast variable");
                dest.broadcastVar = &it->second;
            }
        }
    }
    for (const auto& entry : running) {
        int index = int(entry.asNumber());
        if (index < 0 || index >= int(handles.size()))
            return fail(why, "invalid runnable thread index");
        Thread* t = handles[size_t(index)];
        t->runtimeIndex = rt.threads.size();
        rt.threads.push_back(t);
    }
    for (const auto& entry : root.get("waits").asArray()) {
        const int index = int(entry.get("thread").asNumber());
        if (index < 0 || index >= int(handles.size()))
            return fail(why, "invalid wait thread");
        Runtime::WaitEntry wait;
        wait.thread = handles[size_t(index)];
        wait.nextTick = entry.get("nextTick").asBool();
        wait.deadlineMs = rt.currentMSecs +
            entry.get("remaining").asNumber();
        if (!valFromJson(entry.get("value"), wait.resolveValue))
            return fail(why, "invalid wait value");
        int bubble = int(entry.get("bubbleTarget").asNumber());
        if (bubble >= 0 && bubble < int(rt.targets.size()))
            wait.bubbleTarget = rt.targets[size_t(bubble)];
        const std::string& type = entry.get("bubbleType").asString();
        if (type == "say") wait.bubbleType = "say";
        else if (type == "think") wait.bubbleType = "think";
        else if (!type.empty()) return fail(why, "invalid bubble type");
        wait.bubbleUsageId = int(entry.get("bubbleUsage").asNumber());
        rt.waitQueue.push_back(wait);
    }
    std::vector<SoundPlaybackState> sounds;
    for (const auto& entry : root.get("sounds").asArray()) {
        const int target = int(entry.get("target").asNumber());
        const int thread = int(entry.get("wait").asNumber());
        const int soundId = int(entry.get("soundId").asNumber());
        const double position = entry.get("position").asNumber();
        if (target < 0 || target >= int(rt.targets.size()) ||
            thread < -1 || thread >= int(handles.size()) ||
            soundId < 0 || !std::isfinite(position) || position < 0)
            return fail(why, "invalid checkpoint audio reference");
        sounds.push_back({rt.targets[size_t(target)],
                          thread < 0 ? nullptr : handles[size_t(thread)],
                          soundId, position, entry.get("pending").asBool()});
    }
    if (SoundBankAPI* bank = soundBankFor(rt)) {
        if (!bank->restoreActiveSounds(sounds))
            return fail(why, "unable to restore audio playback state");
    } else if (!sounds.empty()) {
        return fail(why, "audio device required to restore active sounds");
    }
    rt.updateThreadMap();
    rt.invalidateBroadcastIndex();
    rt.stepSerial = step;
    rt.controlCounter = root.get("controlCounter").asNumber();
    rt.answer = SStr::fromUtf8(root.get("answer").asString())->view();
    rt.ioDevices.clock.restoreProjectTimer(root.get("clock").asNumber());
    Rng::global().restore(randomState, randomInc);
    std::vector<uint8_t> pen;
    if (!unhexBytes(root.get("pen").asString(), pen) ||
        (rt.renderer && !rt.renderer->restorePen(pen)) ||
        (!rt.renderer && !pen.empty()))
        return fail(why, "failed to restore pen bitmap");
    rt.redrawRequested = true;
    return true;
}

} // namespace xyo
