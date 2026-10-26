#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Keep this header usable by headless builds without SDL headers.
struct SDL_Renderer;

namespace xyo::ui {

struct Vec2 { float x = 0, y = 0; };
// Packed RGBA: R in bits 0..7, G 8..15, B 16..23, A 24..31.
// Thus a little-endian machine stores the bytes in RGBA order.
using PackedColor = std::uint32_t;
constexpr PackedColor rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                           std::uint8_t a = 255) {
    return PackedColor(r) | (PackedColor(g) << 8) | (PackedColor(b) << 16) |
           (PackedColor(a) << 24);
}
using TextureID = std::uintptr_t; // reinterpret_cast<TextureID>(SDL_Texture*)
enum DrawFlags { DrawFlags_None = 0, DrawFlags_Closed = 1 };
class Font;

// Counts of recorded geometry and merged draw commands for the current frame.
// begin() clears sizes, not capacities; render() does not consume the frame.
struct DrawStats { std::size_t vertices = 0, indices = 0, batches = 0; };

class DrawList {
public:
    void begin(float w, float h);
    // Requires SDL >= 2.0.18. Returns false on an SDL error, or without SDL.
    // Restores the caller's clip and blend state; does not clear or present.
    bool render(SDL_Renderer* renderer);

    void AddRectFilled(Vec2 min, Vec2 max, PackedColor color, float rounding = 0);
    void AddRect(Vec2 min, Vec2 max, PackedColor color, float rounding = 0,
                 int flags = 0, float thickness = 1);
    // Simple polygons (either winding); holes/self-intersections are unsupported.
    // Consecutive duplicate vertices, a repeated closing vertex and collinear
    // boundary points are accepted. Invalid/degenerate polygons are ignored.
    void AddConcavePolyFilled(const Vec2* points, int count, PackedColor color);
    void AddConvexPolyFilled(const Vec2* points, int count, PackedColor color);
    void AddPolyline(const Vec2* points, int count, PackedColor color,
                     int flags, float thickness);
    void AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, PackedColor color);
    void AddLine(Vec2 a, Vec2 b, PackedColor color, float thickness = 1);
    void AddCircleFilled(Vec2 center, float radius, PackedColor color, int segments = 0);
    void AddCircle(Vec2 center, float radius, PackedColor color, int segments = 0,
                   float thickness = 1);
    void AddImage(TextureID texture, Vec2 min, Vec2 max, Vec2 uv0 = {0, 0},
                  Vec2 uv1 = {1, 1}, PackedColor color = 0xffffffffu);
    void AddText(Font*, float size, Vec2 pos, PackedColor color, const char* text);

    // Each push is additionally restricted to the frame bounds. intersect=true
    // intersects with the previous clip; false replaces it until the next pop.
    // Popping the root clip is harmless.
    void PushClipRect(Vec2 min, Vec2 max, bool intersect = false);
    void PopClipRect();

    DrawStats stats() const { return {vertices_.size(), indices_.size(), commands_.size()}; }
    // Retained backing-store capacities (same fields as stats), useful for profiling.
    DrawStats capacities() const { return {vertices_.capacity(), indices_.capacity(), commands_.capacity()}; }
    std::size_t getVertexCount() const { return vertices_.size(); }
    std::size_t getIndexCount() const { return indices_.size(); }
    std::size_t getBatchCount() const { return commands_.size(); }

private:
    struct Color { std::uint8_t r, g, b, a; };
    struct Vertex { Vec2 pos; Color color; Vec2 uv; };
    struct Clip {
        float x0, y0, x1, y1;
        bool operator==(const Clip&) const = default;
    };
    struct Command {
        TextureID texture;
        Clip clip;
        std::size_t first, count;
        // Each command owns a contiguous, nonoverlapping vertex slice. Indices
        // in its index slice are relative to vertexFirst, not the whole frame.
        std::size_t vertexFirst, vertexCount;
    };
    std::vector<Vertex> vertices_;
    std::vector<int> indices_;
    std::vector<Command> commands_;
    std::vector<Clip> clips_;
    // Reusable tessellation scratch, never shrunk between frames.
    std::vector<Vec2> path_, clean_, offsets_;
    std::vector<int> remaining_, triangles_;
    Clip bounds_{0, 0, 0, 0};

    bool drawable(PackedColor color) const;
    bool reserveGeometry(std::size_t vertices, std::size_t indices) const;
    void record(TextureID texture, std::size_t first, std::size_t vertexFirst);
    void vertex(Vec2 pos, PackedColor color, Vec2 uv = {0, 0});
    bool cleanPolygon(const Vec2* points, int count);
    void polygonFringe(PackedColor color);
    void roundedRectPath(Vec2 min, Vec2 max, float rounding);
    void circlePath(Vec2 center, float radius, int segments);
};

} // namespace xyo::ui
