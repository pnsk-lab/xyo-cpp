// OpenGL effect pass for SDL's OpenGL renderer.
// All seven Scratch effects use their original texture; only pixel-accurate
// collision/stamp operations need the CPU effect bitmap.
#pragma once

#include "../model/graphic_effects.hpp"

struct SDL_Renderer;
struct SDL_Texture;

namespace xyo {

class GlEffects {
public:
    GlEffects() = default;
    ~GlEffects();

    GlEffects(const GlEffects&) = delete;
    GlEffects& operator=(const GlEffects&) = delete;

    // Returns false when the SDL renderer is not OpenGL 3.3+.
    bool available(SDL_Renderer* renderer);

    // Renders straight-alpha RGBA into a persistent SDL texture.
    // source and destination must be distinct textures of equal dimensions.
    bool apply(SDL_Renderer* renderer, SDL_Texture* source,
               SDL_Texture* destination, int width, int height,
               const GraphicEffects& effects);

private:
    bool init(SDL_Renderer* renderer);
    bool initialized_ = false;
    bool supported_ = false;
    unsigned program_ = 0;
    unsigned vao_ = 0;
    unsigned fbo_ = 0;
    int uSource_ = -1;
    int uSize_ = -1;
    int uFxA_ = -1;
    int uFxB_ = -1;
};

} // namespace xyo
