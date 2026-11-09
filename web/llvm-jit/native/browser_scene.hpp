#pragma once
// Minimal synchronous browser renderer adapter for xyo::Runtime.
// Actual pixels are composed by the browser Canvas2D host.
#include "engine/runtime.hpp"
#include "model/target.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace xyo::browser {
class SceneRenderer final : public RendererAPI {
public:
    explicit SceneRenderer(Runtime& runtime) : rt_(runtime) {}

    void sync() {
        // Clones are added during a step, so refresh before any snapshot or input.
        for (Target* target : rt_.targets) {
            if (!target) continue;
            if (target->drawableID < 0) target->drawableID = nextId_++;
            owned_[target->drawableID] = target;
        }
        for (auto it = owned_.begin(); it != owned_.end();) {
            if (std::find(rt_.targets.begin(), rt_.targets.end(), it->second) == rt_.targets.end())
                it = owned_.erase(it);
            else ++it;
        }
    }
    int drawableOfTarget(Target* target) const override { return target ? target->drawableID : -1; }
    bool hasDrawable(int id) const override { return owned_.count(id) != 0; }
    void destroyDrawable(int id) override { owned_.erase(id); }
    void draw() override {}
    void emitVisualChange(Target*) override {}

    std::vector<int> drawableOrder() const override {
        std::vector<Target*> sorted;
        sorted.reserve(owned_.size());
        for (auto [id, target] : owned_) if (target) sorted.push_back(target);
        std::sort(sorted.begin(), sorted.end(), [](Target* a, Target* b) {
            if (a->layerOrder != b->layerOrder) return a->layerOrder < b->layerOrder;
            return a->drawableID < b->drawableID;
        });
        std::vector<int> result;
        for (Target* target : sorted) result.push_back(target->drawableID);
        return result;
    }
    double setDrawableOrder(int id, double order, const char*, bool relative) override {
        auto it = owned_.find(id);
        if (it == owned_.end() || !std::isfinite(order)) return 0;
        Target* target = it->second;
        int old = target->layerOrder;
        target->layerOrder = static_cast<int>(std::clamp(relative ? old + order : order, -100000.0, 100000.0));
        return target->layerOrder;
    }
    void getCurrentSkinSize(int id, double& w, double& h) const override {
        w = h = 100;
        auto it = owned_.find(id);
        if (it == owned_.end()) return;
        Target* target = it->second;
        const Costume* c = target->getCurrentCostume();
        if (!c || c->dataFormat != "png" || c->data.size() < 24) return;
        const auto* p = c->data.data();
        if (p[0] != 137 || p[1] != 80 || p[2] != 78 || p[3] != 71) return;
        auto be32 = [](const uint8_t* x) -> uint32_t {
            return uint32_t(x[0]) << 24 | uint32_t(x[1]) << 16 | uint32_t(x[2]) << 8 | x[3];
        };
        const double resolution = c->bitmapResolution > 0 ? c->bitmapResolution : 1;
        w = std::min(4096.0, be32(p + 16) / resolution);
        h = std::min(4096.0, be32(p + 20) / resolution);
    }
    void getDrawableBounds(int id, double b[4]) const override {
        auto it = owned_.find(id);
        if (it == owned_.end()) { std::fill(b, b + 4, 0.0); return; }
        Target* t = it->second;
        double w, h;
        getCurrentSkinSize(id, w, h);
        const double scale = std::max(0.0, t->size / 100.0);
        w *= scale;
        h *= scale;
        // Conservative axis-aligned box. Rotated costume coverage is approximate.
        b[0] = t->x - w / 2; b[1] = t->x + w / 2;
        b[2] = t->y + h / 2; b[3] = t->y - h / 2;
    }
    bool drawableTouching(int id, double x, double y) override {
        double b[4]; getDrawableBounds(id, b);
        return hasDrawable(id) && x >= b[0] && x <= b[1] && y >= b[3] && y <= b[2];
    }
    int pick(double x, double y) override {
        // Mouse::postData supplies canvas coordinates.
        const double sx = x - rt_.stageWidth / 2.0;
        const double sy = rt_.stageHeight / 2.0 - y;
        auto sorted = drawableOrder();
        for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
            auto t = owned_.find(*it);
            if (t != owned_.end() && !t->second->isStage && t->second->visible &&
                drawableTouching(*it, sx, sy)) return *it;
        }
        return -1;
    }
    bool isTouchingDrawables(int id, const int* ids, size_t count) override {
        if (!ids || !hasDrawable(id)) return false;
        double a[4]; getDrawableBounds(id, a);
        for (size_t i = 0; i < count; i++) {
            if (!hasDrawable(ids[i])) continue;
            double b[4]; getDrawableBounds(ids[i], b);
            if (a[0] <= b[1] && a[1] >= b[0] && a[3] <= b[2] && a[2] >= b[3]) return true;
        }
        return false;
    }
    void getFencedPosition(int, double& x, double& y, bool fence) const override {
        if (!fence) return;
        x = std::clamp(x, -rt_.stageWidth / 2.0, rt_.stageWidth / 2.0);
        y = std::clamp(y, -rt_.stageHeight / 2.0, rt_.stageHeight / 2.0);
    }
    void getFenceRect(int, double out[4]) const override {
        out[0] = -rt_.stageWidth / 2.0; out[1] = rt_.stageWidth / 2.0;
        out[2] = -rt_.stageHeight / 2.0; out[3] = rt_.stageHeight / 2.0;
    }

    int createPenSkin() override { return 1; }
    void penLine(int, const float rgba[4], double size, double x0, double y0,
                 double x1, double y1) override {
        record("{\"type\":\"line\",\"rgba\":[" + number(rgba[0]) + "," +
            number(rgba[1]) + "," + number(rgba[2]) + "," + number(rgba[3]) +
            "],\"size\":" + number(size) + ",\"x0\":" + number(x0) +
            ",\"y0\":" + number(y0) + ",\"x1\":" + number(x1) +
            ",\"y1\":" + number(y1) + "}");
    }
    void penPoint(int skin, const float rgba[4], double diameter, double x, double y) override {
        penLine(skin, rgba, diameter, x, y, x, y);
    }
    void penClear(int) override { record("{\"type\":\"clear\"}"); }
    void penStamp(int, int id) override {
        record("{\"type\":\"stamp\",\"drawableId\":" + std::to_string(id) + "}");
    }
    std::string consumePenJson() {
        std::string result = "[";
        for (size_t i = 0; i < penEvents_.size(); i++) {
            if (i) result += ',';
            result += penEvents_[i];
        }
        result += ']';
        penEvents_.clear();
        return result;
    }
    void clear() { penEvents_.clear(); owned_.clear(); nextId_ = 1; }
private:
    static std::string number(double x) { return std::isfinite(x) ? std::to_string(x) : "0"; }
    void record(std::string value) {
        if (penEvents_.size() < 4096) penEvents_.push_back(std::move(value));
    }
    Runtime& rt_;
    int nextId_ = 1;
    std::unordered_map<int, Target*> owned_;
    std::vector<std::string> penEvents_;
};
} // namespace xyo::browser
