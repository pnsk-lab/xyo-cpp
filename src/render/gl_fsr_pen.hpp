// AMD FidelityFX FSR 1 (EASU + RCAS) for the accumulated Scratch pen layer.
// Visual-only: the logical pen raster and collision/hot-start data stay unchanged.
#pragma once

struct SDL_Renderer;
struct SDL_Texture;

namespace xyo {

class GlFsrPen {
public:
    GlFsrPen() = default;
    ~GlFsrPen();
    GlFsrPen(const GlFsrPen&) = delete;
    GlFsrPen& operator=(const GlFsrPen&) = delete;

    // Requires SDL's OpenGL 4.2+ renderer for textureGather.
    bool available(SDL_Renderer* renderer);

    // Returns a straight-alpha SDL display texture, preserving source orientation.
    // Source can be straight or premultiplied RGBA8; output never changes pen data.
    SDL_Texture* apply(SDL_Renderer* renderer, SDL_Texture* source,
                       int outputWidth, int outputHeight,
                       bool sourcePremultiplied, float rcasSharpness,
                       bool sourceChanged, bool enableRcas);

    void reset();

private:
    bool initialize(SDL_Renderer* renderer);
    bool ensureTargets(SDL_Renderer* renderer, int width, int height);

    unsigned programs_[2] = {0, 0};
    unsigned vao_ = 0;
    unsigned fbo_ = 0;
    SDL_Texture* easuTarget_ = nullptr;
    SDL_Texture* outputTarget_ = nullptr;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    bool initialized_ = false;
    bool supported_ = false;
    bool cached_ = false;
    SDL_Texture* cachedSource_ = nullptr;
    SDL_Texture* cachedOutput_ = nullptr;
    bool cachedPremult_ = false;
    bool cachedRcas_ = false;
    float cachedSharpness_ = 0.0f;
};

} // namespace xyo
