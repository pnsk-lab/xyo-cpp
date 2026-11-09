// DLSS-equivalent spatial/temporal upscaler for the SDL OpenGL renderer.
//
// The stage is rendered into an offscreen target at a reduced internal
// resolution and then reconstructed at display resolution with a
// Catmull-Rom (bicubic) kernel plus an unsharp mask. When temporal
// accumulation is enabled the previous reconstructed frame is reused with
// a neighborhood clamp (variance clipping), which removes the flicker of
// thin pen strokes and high-frequency costume detail.
//
// Non-GL backends (software SDL, Emscripten) fall back to SDL's linear
// scaling; see SdlRenderer::drawScaledStage().
#pragma once

struct SDL_Renderer;
struct SDL_Texture;

namespace xyo {

class GlUpscale {
public:
    GlUpscale() = default;
    ~GlUpscale();

    GlUpscale(const GlUpscale&) = delete;
    GlUpscale& operator=(const GlUpscale&) = delete;

    /// Returns false when the SDL renderer is not OpenGL 3.3+.
    bool available(SDL_Renderer* renderer);

    /// Reconstruct source (the reduced-resolution stage target) at
    /// outW x outH. Returns an SDL texture owned by this object, or nullptr
    /// on failure. The returned texture is opaque and must not be used as a
    /// render target by the caller.
    SDL_Texture* apply(SDL_Renderer* renderer, SDL_Texture* source,
                       int outW, int outH, float sharpness,
                       float historyWeight);

    /// Drops accumulated history (resize, reload, scene discontinuity).
    void reset();

private:
    bool init(SDL_Renderer* renderer);
    bool ensureTargets(SDL_Renderer* renderer, int w, int h);

    bool initialized_ = false;
    bool supported_ = false;
    unsigned program_ = 0;
    unsigned vao_ = 0;
    unsigned fbo_ = 0;
    int uSource_ = -1;
    int uHistory_ = -1;
    int uSrcSize_ = -1;
    int uSharpness_ = -1;
    int uHistoryWeight_ = -1;
    SDL_Texture* target_[2] = {nullptr, nullptr};
    int targetW_ = 0, targetH_ = 0;
    int history_ = -1;   // index of the last reconstructed frame
};

} // namespace xyo
