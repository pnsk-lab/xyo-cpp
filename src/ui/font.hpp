#pragma once

#include "draw.hpp"

struct SDL_Renderer;

namespace xyo::ui {

// The caller retains the TTF bytes for the entire initialized lifetime.
// Render-thread only. Destroy/reset before destroying the SDL renderer, and
// only after any recorded draw lists referencing this font have been consumed.
class Font {
public:
    Font() noexcept;
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    Font(Font&&) = delete;
    Font& operator=(Font&&) = delete;

    bool init(SDL_Renderer* renderer, const void* ttf, int size,
              float bakeSize = 32.0f);
    void reset() noexcept;
    bool IsLoaded() const noexcept;
    // Sticky status for failures during lazy rasterization / texture uploads.
    bool HasError() const noexcept;
    Vec2 CalcTextSizeA(float size, float maxWidth, float wrapWidth,
                       const char* text);

    float FontSize = 0.0f;
    float Ascent = 0.0f;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    friend class DrawList;
};

} // namespace xyo::ui
