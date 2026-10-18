// xyo-cpp: target.js / rendered-target.js / sprite.js 移植。
#include "target.hpp"

#include <climits>
#include <cmath>
#include <limits>

#include "../core/cast.hpp"
#include "../engine/runtime.hpp"

namespace xyo {

static int g_nextTargetId = 0;
static std::string genId() {
    return "target-" + std::to_string(++g_nextTargetId);
}

Sprite::Sprite(Runtime* rt) : runtime(rt), blocks(std::make_unique<Blocks>(rt)) {}

Target* Sprite::createClone(const std::string& layerGroup) {
    auto* c = new Target(runtime, this);
    c->isOriginal = clones.empty();
    clones.push_back(c);
    (void)layerGroup;
    return c;
}

void Sprite::removeClone(Target* clone) {
    for (auto it = clones.begin(); it != clones.end(); ++it)
        if (*it == clone) { clones.erase(it); return; }
}

Target::Target(Runtime* rt, Sprite* spr)
    : runtime(rt), id(genId()), sprite(spr),
      blocks(spr ? spr->blocks.get() : new Blocks(rt)) {
    // RenderedTarget 相当 — エフェクト7種を 0 で初期化
    for (const char* e : {"color", "fisheye", "whirl", "pixelate",
                          "mosaic", "brightness", "ghost"})
        effects[e] = 0;
}

// ---------------------------------------------------------------- 変数 --

Variable* Target::lookupVariableById(const std::string& vid) {
    auto it = variables.find(vid);
    if (it != variables.end()) return &it->second;
    if (runtime && !isStage) {
        Target* stage = runtime->getTargetForStage();
        if (stage) {
            auto sit = stage->variables.find(vid);
            if (sit != stage->variables.end()) return &sit->second;
        }
    }
    return nullptr;
}

Variable* Target::lookupVariableByNameAndType(const std::u16string& name,
                                              const char* type,
                                              bool skipStage) {
    for (auto& [id, v] : variables)
        if (v.name == name && v.type == type) return &v;
    if (!skipStage && runtime && !isStage) {
        Target* stage = runtime->getTargetForStage();
        if (stage) {
            for (auto& [id, v] : stage->variables)
                if (v.name == name && v.type == type) return &v;
        }
    }
    return nullptr;
}

Variable* Target::lookupOrCreateVariable(const std::string& id,
                                         const std::u16string& name) {
    if (Variable* v = lookupVariableById(id)) return v;
    if (Variable* v = lookupVariableByNameAndType(name, Variable::kScalarType)) return v;
    Variable nv = Variable::create(id, name, Variable::kScalarType, false);
    auto [it, ok] = variables.emplace(id, std::move(nv));
    return &it->second;
}

Variable* Target::lookupOrCreateList(const std::string& id,
                                     const std::u16string& name) {
    if (Variable* v = lookupVariableById(id)) return v;
    if (Variable* v = lookupVariableByNameAndType(name, Variable::kListType)) return v;
    Variable nv = Variable::create(id, name, Variable::kListType, false);
    auto [it, ok] = variables.emplace(id, std::move(nv));
    return &it->second;
}

Variable* Target::lookupBroadcastMsg(const std::string& id,
                                     const std::u16string& name) {
    Variable* msg = nullptr;
    if (!id.empty()) msg = lookupVariableById(id);
    else if (!name.empty()) msg = lookupBroadcastByInputValue(name);
    if (msg) return msg;
    return nullptr;
}

Variable* Target::lookupBroadcastByInputValue(const std::u16string& name) {
    std::u16string lower = u16ToLower(name);
    for (auto& [id, v] : variables) {
        if (v.type == Variable::kBroadcastType &&
            u16ToLower(v.name) == lower) return &v;
    }
    return nullptr;
}

Variable* Target::createVariable(const std::string& id, const std::u16string& name,
                                 const char* type, bool isCloud) {
    auto it = variables.find(id);
    if (it != variables.end()) return &it->second;
    Variable nv = Variable::create(id, name, type, isCloud);
    auto [ins, ok] = variables.emplace(id, std::move(nv));
    return &ins->second;
}

void Target::renameVariable(const std::string& id, const std::u16string& newName) {
    auto it = variables.find(id);
    if (it == variables.end()) return;
    it->second.name = newName;
}

void Target::deleteVariable(const std::string& id) {
    variables.erase(id);
}

// -------------------------------------------------------- RenderedTarget --

static double wrapClamp(double n, double min, double max) {
    const double range = (max - min) + 1;
    return n - std::floor((n - min) / range) * range;
}

void Target::setXY(double nx, double ny, bool force) {
    if (isStage) return;
    if (dragging && !force) return;
    const double oldX = x, oldY = y;
    if (runtime && runtime->renderer) {
        // fencing は renderer の責務（getFencedPositionOfDrawable）
        runtime->renderer->getFencedPosition(drawableID, nx, ny, runtime->fencing);
    }
    x = nx; y = ny;
    if (onTargetMoved) onTargetMoved(this, oldX, oldY, force);
    if (runtime) {
        if (runtime->renderer && visible) {
            emitVisualChange();
            runtime->redrawRequested = true;
        }
        runtime->requestTargetsUpdate(this);
    }
}

void Target::setDirection(double dir) {
    if (isStage || !std::isfinite(dir)) return;
    direction = wrapClamp(dir, -179, 180);
    if (runtime) {
        if (runtime->renderer && visible) {
            emitVisualChange();
            runtime->redrawRequested = true;
        }
        runtime->requestTargetsUpdate(this);
    }
}

void Target::setDraggable(bool d) {
    if (isStage) return;
    draggable = d;
    if (runtime) runtime->requestTargetsUpdate(this);
}

void Target::setRotationStyle(const std::string& style) {
    if (style == "none" || style == "all around" || style == "left-right") {
        rotationStyle = style;
    }
    if (runtime) {
        if (runtime->renderer && visible) {
            emitVisualChange();
            runtime->redrawRequested = true;
        }
        runtime->requestTargetsUpdate(this);
    }
}

void Target::setVisible(bool v) {
    if (isStage) return;
    visible = v;
    if (runtime) {
        if (runtime->renderer && visible) {
            emitVisualChange();
            runtime->redrawRequested = true;
        }
        runtime->requestTargetsUpdate(this);
    }
}

void Target::setSize(double s) {
    if (isStage) return;
    if (runtime && runtime->renderer) {
        double w = 1, h = 1;
        runtime->renderer->getCurrentSkinSize(drawableID, w, h);
        const double origW = w > 0 ? w : 1;
        const double origH = h > 0 ? h : 1;
        const double minScale = runtime->fencing
            ? std::min(1.0, std::max(5.0 / origW, 5.0 / origH)) : 0;
        const double maxScale = runtime->fencing
            ? std::min(1.5 * runtime->stageWidth / origW,
                       1.5 * runtime->stageHeight / origH)
            : std::numeric_limits<double>::infinity();
        s = std::min(std::max(s / 100.0, minScale), maxScale) * 100.0;
        if (visible) {
            emitVisualChange();
            runtime->redrawRequested = true;
        }
    }
    size = s;
    if (runtime) runtime->requestTargetsUpdate(this);
}

void Target::setCostume(double index) {
    if (!sprite) return;
    index = std::round(index);
    if (!std::isfinite(index) || index == 0) index = 0;  // JS: !index → 0
    int n = int(sprite->costumes.size());
    if (n <= 0) { currentCostume = 0; return; }
    currentCostume = int(wrapClamp(index, 0, n - 1));
    if (runtime) {
        if (runtime->renderer && visible) {
            emitVisualChange();
            runtime->redrawRequested = true;
        }
        runtime->requestTargetsUpdate(this);
    }
}

int Target::getCostumeIndexByName(const std::u16string& name) const {
    if (!sprite) return -1;
    for (size_t i = 0; i < sprite->costumes.size(); ++i)
        if (sprite->costumes[i].name == name) return int(i);
    return -1;
}

const Costume* Target::getCurrentCostume() const {
    if (!sprite || sprite->costumes.empty()) return nullptr;
    int i = currentCostume;
    if (i < 0) i = 0;
    if (i >= int(sprite->costumes.size())) i = int(sprite->costumes.size()) - 1;
    return &sprite->costumes[i];
}

void Target::setEffect(const std::string& name, double value) {
    if (!effects.count(name)) return;
    effects[name] = value;
    if (runtime && runtime->renderer && visible) {
        emitVisualChange();
        runtime->redrawRequested = true;
    }
}

void Target::clearEffects() {
    for (auto& [k, v] : effects) v = 0;
    if (runtime && runtime->renderer && visible) {
        emitVisualChange();
        runtime->redrawRequested = true;
    }
}

void Target::updateAllDrawableProperties() {
    if (runtime) {
        if (runtime->renderer && visible) {
            emitVisualChange();
            runtime->redrawRequested = true;
        }
        runtime->requestTargetsUpdate(this);
    }
}

void Target::emitVisualChange() {
    if (runtime && runtime->renderer) runtime->renderer->emitVisualChange(this);
}

void Target::setSay(const char* type, const std::u16string& text) {
    bubbleText = text;
    bubbleType = type;
    bubbleUsageId++;
    if (runtime && runtime->onSay) runtime->onSay(this, type, bubbleText);
}

void Target::postSpriteInfo() {}

void Target::goToFront() {
    if (runtime && runtime->renderer)
        runtime->renderer->setDrawableOrder(drawableID,
            std::numeric_limits<double>::infinity(), "sprite", true);
    if (runtime) runtime->setExecutablePosition(
        this, std::numeric_limits<int>::max() / 2);
}

void Target::goToBack() {
    if (runtime && runtime->renderer)
        runtime->renderer->setDrawableOrder(drawableID,
            -std::numeric_limits<double>::infinity(), "sprite", false);
    if (runtime) runtime->setExecutablePosition(this, -INT_MAX / 2);
}

void Target::goForwardLayers(double n) {
    if (runtime && runtime->renderer)
        runtime->renderer->setDrawableOrder(drawableID, n, "sprite", true);
    if (runtime) runtime->moveExecutable(this, int(n));
}

void Target::goBackwardLayers(double n) {
    if (runtime && runtime->renderer)
        runtime->renderer->setDrawableOrder(drawableID, -n, "sprite", true);
    if (runtime) runtime->moveExecutable(this, int(-n));
}

void Target::goBehindOther(Target* other) {
    if (!other || !runtime) return;
    if (runtime->renderer) {
        double otherLayer = runtime->renderer->setDrawableOrder(
            other->drawableID, 0, "sprite", true);
        runtime->renderer->setDrawableOrder(drawableID, otherLayer, "sprite", false);
    }
    int pos = -1;
    for (size_t i = 0; i < runtime->executableTargets.size(); ++i)
        if (runtime->executableTargets[i] == other) { pos = int(i); break; }
    runtime->setExecutablePosition(this, pos);
}

bool Target::getBounds(Bounds& out) const {
    if (!runtime || !runtime->renderer) return false;
    double b[4] = {0, 0, 0, 0};
    runtime->renderer->getDrawableBounds(drawableID, b);
    out = {b[0], b[1], b[2], b[3]};
    return runtime->renderer->hasDrawable(drawableID);
}

bool Target::isTouchingObject(const std::u16string& obj) {
    if (obj == u"_mouse_") {
        if (!runtime) return false;
        return isTouchingPoint(runtime->ioDevices.mouse.getClientX(),
                               runtime->ioDevices.mouse.getClientY());
    }
    if (obj == u"_edge_") return isTouchingEdge();
    return isTouchingSprite(obj);
}

bool Target::isTouchingPoint(double px, double py) const {
    if (!runtime || !runtime->renderer) return false;
    return runtime->renderer->drawableTouching(drawableID, px, py);
}

bool Target::isTouchingEdge() const {
    if (!runtime || !runtime->renderer) return false;
    Bounds b;
    if (!getBounds(b)) return false;
    return b.left < -runtime->stageWidth / 2.0 ||
           b.right > runtime->stageWidth / 2.0 ||
           b.top > runtime->stageHeight / 2.0 ||
           b.bottom < -runtime->stageHeight / 2.0;
}

bool Target::isTouchingSprite(const std::u16string& spriteName) {
    if (!runtime || !runtime->renderer) return false;
    Target* first = runtime->getSpriteTargetByName(spriteName);
    if (!first || !first->sprite) return false;
    std::vector<int> candidates;
    for (Target* c : first->sprite->clones)
        if (!c->dragging) candidates.push_back(c->drawableID);
    return runtime->renderer->isTouchingDrawables(
        drawableID, candidates.data(), candidates.size());
}

bool Target::isTouchingColor(uint32_t rgb) const {
    if (!runtime || !runtime->renderer) return false;
    return runtime->renderer->isTouchingColor(drawableID, rgb);
}

bool Target::colorIsTouchingColor(uint32_t targetRgb, uint32_t maskRgb) const {
    if (!runtime || !runtime->renderer) return false;
    return runtime->renderer->colorTouchingColor(targetRgb, maskRgb, drawableID);
}

bool Target::keepInFence(double nx, double ny, double& fx, double& fy) const {
    Bounds b;
    if (!getBounds(b)) return false;
    const double fl = -runtime->stageWidth / 2.0;
    const double fr = runtime->stageWidth / 2.0;
    const double ft = runtime->stageHeight / 2.0;
    const double fb = -runtime->stageHeight / 2.0;
    b.left += nx - x; b.right += nx - x;
    b.top += ny - y; b.bottom += ny - y;
    double dx = 0, dy = 0;
    if (b.left < fl) dx += fl - b.left;
    if (b.right > fr) dx += fr - b.right;
    if (b.top > ft) dy += ft - b.top;
    if (b.bottom < fb) dy += fb - b.bottom;
    fx = nx + dx;
    fy = ny + dy;
    return true;
}

// ------------------------------------------------------------- クローン --

std::unordered_map<std::string, Variable> Target::duplicateVariables() {
    std::unordered_map<std::string, Variable> out;
    for (auto& [vid, v] : variables) {
        Variable nv;
        nv.id = v.id;                 // クローンは同じ変数 id を共有
        nv.name = v.name;
        nv.type = v.type;
        nv.isCloud = v.isCloud;
        nv.value = v.value;
        if (v.list) nv.list = v.list->clone();
        out.emplace(nv.id, std::move(nv));
    }
    return out;
}

Target* Target::makeClone() {
    if (!runtime || !runtime->clonesAvailable() || isStage) return nullptr;
    runtime->changeCloneCounter(1);
    Target* c = sprite->createClone();
    c->x = x; c->y = y;
    c->direction = direction;
    c->draggable = draggable;
    c->visible = visible;
    c->size = size;
    c->currentCostume = currentCostume;
    c->rotationStyle = rotationStyle;
    c->effects = effects;
    c->variables = duplicateVariables();
    c->edgeActivatedHatValues = edgeActivatedHatValues;
    c->soundEffects = soundEffects;
    // JS: _onTargetCreated — ペン状態は複製し、penDown 中なら移動フックも継承
    if (penState) {
        c->penState = std::make_unique<PenState>(*penState);
        c->onTargetMoved = onTargetMoved;
    }
    if (runtime->onTargetCreated) runtime->onTargetCreated(c);
    c->updateAllDrawableProperties();
    // JS: initDrawable — クローンはここで start_as_clone ハットを起動
    if (!c->isOriginal)
        runtime->startHats("control_start_as_clone", nullptr, c);
    return c;
}

void Target::dispose() {
    if (!isOriginal && runtime) runtime->changeCloneCounter(-1);
    if (runtime) {
        runtime->requestRemoveMonitorByTargetId(id);
        runtime->stopForTarget(this, nullptr);
        runtime->removeExecutable(this);
        if (runtime->renderer && drawableID >= 0) {
            runtime->renderer->destroyDrawable(drawableID);
            if (visible) {
                emitVisualChange();
                runtime->redrawRequested = true;
            }
        }
    }
    if (sprite) sprite->removeClone(this);
}

// ------------------------------------------------------- edge-activated --

Value Target::updateEdgeActivatedValue(const std::string& blockId, Value newValue) {
    auto it = edgeActivatedHatValues.find(blockId);
    Value old = it == edgeActivatedHatValues.end() ? Value::undefined() : it->second;
    edgeActivatedHatValues[blockId] = newValue;
    return old;
}

bool Target::hasEdgeActivatedValue(const std::string& blockId) const {
    return edgeActivatedHatValues.count(blockId) != 0;
}

void Target::onGreenFlag() { clearEffects(); }

void Target::onStopAll() { clearEffects(); }

} // namespace xyo
