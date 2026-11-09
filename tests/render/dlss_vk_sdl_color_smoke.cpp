// Native NVIDIA NGX DLSS -> SDL2 OpenGL end-to-end integration test.
// Requires a real RTX GPU, NVIDIA's DLSS SDK and XYO_DLSS_FEATURE_DIR.
// The output is checked for vertical orientation and major color shifts.
#include "../../src/render/dlss_gl_pipeline.hpp"

#include <SDL.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using xyo::render::DlssGlPipeline;

namespace {
void fill(SDL_Renderer* renderer, const SDL_Rect& rectangle,
          SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rectangle);
}
}

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_Window* window = SDL_CreateWindow(
        "DLSS SDL spatial parity test",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 960, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 2;
    }
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(renderer, &info);
    if (!info.name || std::string(info.name) != "opengl") {
        std::fprintf(stderr, "SDL renderer is not OpenGL\n");
        return 2;
    }
    auto* source = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_TARGET, 480, 360);
    auto* destination = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_TARGET, 960, 720);
    if (!source || !destination) {
        std::fprintf(stderr, "SDL target allocation: %s\n", SDL_GetError());
        return 2;
    }
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
    SDL_SetTextureBlendMode(destination, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(renderer, source);
    fill(renderer, SDL_Rect{0, 0, 240, 180}, SDL_Color{220, 25, 35, 255});
    fill(renderer, SDL_Rect{240, 0, 240, 180}, SDL_Color{25, 220, 55, 255});
    fill(renderer, SDL_Rect{0, 180, 240, 180}, SDL_Color{35, 45, 225, 255});
    fill(renderer, SDL_Rect{240, 180, 240, 180}, SDL_Color{230, 205, 35, 255});
    SDL_SetRenderTarget(renderer, nullptr);
    SDL_RenderFlush(renderer);

    DlssGlPipeline pipeline;
    auto* output = pipeline.apply(renderer, source, 960, 720);
    if (!output) {
        std::fprintf(stderr, "NGX DLSS pipeline failed (set "
                             "XYO_DLSS_REAL_DEBUG=1)\n");
        return 3;
    }
    if (SDL_SetRenderTarget(renderer, destination) != 0 ||
        SDL_RenderCopy(renderer, output, nullptr, nullptr) != 0) {
        std::fprintf(stderr, "SDL output texture copy failed\n");
        return 3;
    }
    std::vector<uint8_t> pixels(960u * 720u * 4u);
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32,
                             pixels.data(), 960 * 4) != 0) {
        std::fprintf(stderr, "SDL output readback failed: %s\n",
                     SDL_GetError());
        return 3;
    }
    const std::array<std::array<int, 4>, 4> expected{{
        {220, 25, 35, 255}, {25, 220, 55, 255},
        {35, 45, 225, 255}, {230, 205, 35, 255}
    }};
    bool pass = true;
    for (int i = 0; i < 4; ++i) {
        const int x = i % 2 ? 720 : 240;
        const int y = i / 2 ? 540 : 180;
        const auto* px = &pixels[(y * 960 + x) * 4];
        std::printf("quadrant %d: RGBA %u %u %u %u (expected ~%d %d %d %d)\n",
                    i, px[0], px[1], px[2], px[3],
                    expected[i][0], expected[i][1],
                    expected[i][2], expected[i][3]);
        for (int component = 0; component < 3; ++component)
            if (std::abs(int(px[component])-expected[i][component]) > 55)
                pass = false;
    }
    // Second frame changes the actual GL/SDL input colors: reused Vulkan
    // semaphores, command buffer and image memory must not show the old data.
    SDL_SetRenderTarget(renderer, source);
    fill(renderer, SDL_Rect{0, 0, 240, 180}, SDL_Color{30, 45, 210, 255});
    fill(renderer, SDL_Rect{240, 0, 240, 180}, SDL_Color{215, 45, 30, 255});
    fill(renderer, SDL_Rect{0, 180, 240, 180}, SDL_Color{215, 205, 30, 255});
    fill(renderer, SDL_Rect{240, 180, 240, 180}, SDL_Color{30, 210, 50, 255});
    SDL_SetRenderTarget(renderer, nullptr);
    output = pipeline.apply(renderer, source, 960, 720);
    if (!output) {
        std::fprintf(stderr, "Repeated DLSS evaluate failed\n");
        return 3;
    }
    SDL_SetRenderTarget(renderer, destination);
    SDL_RenderCopy(renderer, output, nullptr, nullptr);
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32,
                             pixels.data(), 960 * 4) != 0)
        return 3;
    const std::array<std::array<int, 4>, 4> changed{{
        {30, 45, 210, 255}, {215, 45, 30, 255},
        {215, 205, 30, 255}, {30, 210, 50, 255}
    }};
    for (int i = 0; i < 4; ++i) {
        const int x = i % 2 ? 720 : 240;
        const int y = i / 2 ? 540 : 180;
        const auto* px = &pixels[(y * 960 + x) * 4];
        std::printf("frame 2 quadrant %d: RGBA %u %u %u %u\n",
                    i, px[0], px[1], px[2], px[3]);
        for (int component = 0; component < 3; ++component)
            if (std::abs(int(px[component])-changed[i][component]) > 55)
                pass = false;
    }
    // On resize NGX must recreate its feature and import new Vulkan image
    // storage; the SDL destination size is checked before leaving the test.
    SDL_SetRenderTarget(renderer, nullptr);
    output = pipeline.apply(renderer, source, 1440, 1080);
    int resizedW = 0, resizedH = 0;
    if (!output || SDL_QueryTexture(output, nullptr, nullptr,
                                    &resizedW, &resizedH) ||
        resizedW != 1440 || resizedH != 1080) {
        std::fprintf(stderr, "DLSS dynamic resize failed\n");
        return 3;
    }
    std::puts("DLSS dynamic resize 480x360 -> 1440x1080: PASS");

    // Optional long-running stress test: alternate source frames while
    // reusing Vulkan binary semaphores, texture memory and command buffers.
    // Periodically read the actual DLSS output to catch stale-frame bugs.
    const char* countEnv = std::getenv("XYO_DLSS_STRESS_FRAMES");
    const int count = countEnv ? std::clamp(std::atoi(countEnv), 0, 2000) : 0;
    const bool resizeStress =
        std::getenv("XYO_DLSS_STRESS_RESIZE") != nullptr;
    const bool modeSwitch =
        std::getenv("XYO_DLSS_STRESS_MODE_SWITCH") != nullptr;
    const bool presetSwitch =
        std::getenv("XYO_DLSS_STRESS_PRESET_SWITCH") != nullptr;
    const bool noiseCut =
        std::getenv("XYO_DLSS_STRESS_NOISE_CUT") != nullptr;
    const bool noiseMotion =
        std::getenv("XYO_DLSS_STRESS_NOISE_MOVE") != nullptr;
    const bool noiseEnabled = noiseCut || noiseMotion;
    // A moving opaque rectangle exposes trails behind its old position.
    // Unlike uniform noise, its previous/current silhouettes are known.
    const bool squareMotion =
        std::getenv("XYO_DLSS_STRESS_SQUARE_MOVE") != nullptr;
    // Semi-transparent object moving behind a fixed opaque occluder.
    const bool alphaOcclusion =
        std::getenv("XYO_DLSS_STRESS_ALPHA_OCCLUSION") != nullptr;
    const bool measuredMotion = squareMotion || alphaOcclusion;
    SDL_Texture* noise = nullptr;
    std::vector<uint8_t> noisePixels;
    int lowConfidenceFrames = 0;
    if (noiseEnabled) {
        noise = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                  SDL_TEXTUREACCESS_STREAMING, 480, 360);
        if (!noise) return 4;
        noisePixels.resize(480u * 360u * 4);
    }
    if (count > 0) {
        std::vector<uint8_t> framePixels(1440u * 1080u * 4);
        double trailingRedSum = 0.0;
        uint64_t trailingSamples = 0;
        const auto began = std::chrono::steady_clock::now();
        for (int f = 0; f < count; ++f) {
            const bool red = (f & 1) == 0;
            const SDL_Color color = red
                ? SDL_Color{230, 20, 35, 255}
                : SDL_Color{20, 45, 230, 255};
            if (SDL_SetRenderTarget(renderer, source) != 0) return 4;
            if (measuredMotion) {
                // Known opaque or semi-transparent moving rectangle.
                // In alpha mode, draw an opaque foreground occluder AFTER it
                // so the test covers disocclusions and partial transparency.
                constexpr SDL_Color bg{24, 36, 50, 255};
                const SDL_Color fg{230, 30, 36,
                                   static_cast<uint8_t>(alphaOcclusion ? 128 : 255)};
                SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
                SDL_RenderClear(renderer);
                SDL_SetRenderDrawBlendMode(renderer,
                    alphaOcclusion ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
                fill(renderer, SDL_Rect{20 + (f % 45) * 6, 100, 100, 80}, fg);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                if (alphaOcclusion) {
                    // Fixed opaque geometry in front of the moving object.
                    fill(renderer, SDL_Rect{200, 110, 40, 60},
                         SDL_Color{44, 70, 80, 255});
                }
            } else if (noiseEnabled) {
                // Alternating unrelated frames or a textured image shifted
                // 6px per frame. The latter must NOT trigger scene reset.
                for (int y = 0; y < 360; ++y)
                    for (int x = 0; x < 480; ++x) {
                        const int sx = noiseMotion ? (x - 6 * f + 1403) :
                                                x + (f & 1 ? 143 : 1403);
                        const int sy = noiseMotion ? y + 803 :
                                                y + (f & 1 ? 307 : 803);
                        uint32_t value =
                            uint32_t(sx) * 741103597u ^
                            uint32_t(sy) * 1597334677u;
                        value ^= value >> 13;
                        value *= 3266489917u;
                        value ^= value >> 16;
                        const auto offset = (size_t(y) * 480 + x) * 4;
                        noisePixels[offset] = uint8_t(value);
                        noisePixels[offset + 1] = uint8_t(value >> 8);
                        noisePixels[offset + 2] = uint8_t(value >> 16);
                        noisePixels[offset + 3] = 255;
                    }
                if (SDL_UpdateTexture(noise, nullptr, noisePixels.data(),
                                      480 * 4) != 0 ||
                    SDL_RenderCopy(renderer, noise, nullptr, nullptr) != 0)
                    return 4;
            } else {
                SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
                SDL_RenderClear(renderer);
            }
            SDL_SetRenderTarget(renderer, nullptr);
            if (modeSwitch && f % 60 == 0) {
                const char* mode = (f / 60) % 3 == 0 ? "quality" :
                    (f / 60) % 3 == 1 ? "balanced" : "performance";
                if (setenv("XYO_DLSS_MODE", mode, 1) != 0) return 4;
            }
            if (presetSwitch && f % 80 == 0) {
                const char* preset = (f / 80) % 3 == 0 ? "K" :
                    (f / 80) % 3 == 1 ? "J" : "auto";
                if (setenv("XYO_DLSS_PRESET", preset, 1) != 0) return 4;
            }
            const bool small = resizeStress && ((f / 40) & 1);
            const int frameW = measuredMotion || small ? 960 : 1440;
            const int frameH = measuredMotion || small ? 720 : 1080;
            const char* alphaGuard =
                std::getenv("XYO_DLSS_STRESS_ALPHA_SAFE");
            const bool forceHistoryReset =
                alphaOcclusion && alphaGuard && alphaGuard[0] == '1';
            output = pipeline.apply(renderer, source, frameW, frameH,
                                    forceHistoryReset);
            if (!output) {
                std::fprintf(stderr, "DLSS stress frame %d failed\n", f);
                return 4;
            }
            // The first frame starts after a color-quadrant diagnostic,
            // so judge only steady-state motion after the first two frames.
            if (noiseEnabled && f >= 2 && pipeline.motionLowConfidence())
                ++lowConfidenceFrames;
            if (measuredMotion || f == 0 ||
                (f + 1) % 40 == 0 || f + 1 == count) {
                if (SDL_SetRenderTarget(renderer, output) != 0 ||
                    SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32,
                        framePixels.data(), frameW * 4) != 0) {
                    std::fprintf(stderr, "DLSS stress readback %d failed\n", f);
                    return 4;
                }
                const auto* px = &framePixels[
                    ((frameH / 2) * frameW + (frameW / 2)) * 4];
                if (!noiseEnabled && !measuredMotion)
                    for (int channel = 0; channel < 3; ++channel)
                        if (std::abs(int(px[channel]) - int(
                                channel == 0 ? color.r :
                                channel == 1 ? color.g : color.b)) > 55)
                            pass = false;
                if (measuredMotion && f >= 3 && (f % 45) != 0) {
                    // The 6px strip immediately left of the new left edge
                    // was colored red in the preceding frame, but must now
                    // be the background color. Measure residual red, ignoring
                    // the 1px nearest-edge filter region.
                    const int currentX = (20 + (f % 45) * 6) * 2;
                    const int minX = currentX - 12;
                    for (int y = 224; y < 336; y += 4)
                        for (int x = minX; x < currentX - 2; ++x) {
                            const auto* sample =
                                &framePixels[(y * frameW + x) * 4];
                            // Exclude the fixed foreground occluder.
                            if (alphaOcclusion && x >= 400 && x < 480)
                                continue;
                            trailingRedSum +=
                                std::max(0, int(sample[0]) - 24);
                            ++trailingSamples;
                        }
                    // y=105 is inside the moving rectangle but above the
                    // foreground occluder, giving an unoccluded control.
                    const auto* center =
                        &framePixels[(105 * 2 * frameW +
                                      (20 + (f % 45) * 6 + 50) * 2) * 4];
                    if (center[0] < (alphaOcclusion ? 65 : 120))
                        pass = false;
                }
                SDL_SetRenderTarget(renderer, nullptr);
            }
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - began).count();
        std::printf("DLSS stress: %d frames, %.1f ms total, %.3f ms/frame "
                    "(includes SDL input updates and periodic GPU readback)\n",
                    count, elapsed, elapsed / count);
        if (measuredMotion) {
            std::printf("DLSS moving %s: trailing red %.3f ",
                        alphaOcclusion ? "alpha+occlusion" : "square",
                        trailingSamples ? trailingRedSum / trailingSamples : 0);
            std::printf("(average excess red channel above background, "
                        "%llu samples)\n",
                        static_cast<unsigned long long>(trailingSamples));
        }
        if (noiseEnabled) {
            std::printf("Optical confidence: %d low-confidence frames "
                        "after warmup (%s)\n", lowConfidenceFrames,
                        noiseCut ? "scene cuts" : "6px motion");
            if ((noiseCut && lowConfidenceFrames == 0) ||
                (noiseMotion && lowConfidenceFrames != 0))
                pass = false;
        }
    }
    if (noise) SDL_DestroyTexture(noise);
    SDL_SetRenderTarget(renderer, nullptr);
    pipeline.reset();

    // Destroy and recreate every Vulkan/GL object against the same SDL
    // context; this catches lifetime bugs hidden by process exit.
    if (!pipeline.apply(renderer, source, 960, 720)) {
        std::fprintf(stderr, "DLSS reinitialize after reset failed\n");
        return 4;
    }
    pipeline.reset();
    std::puts("DLSS teardown and NGX/Vulkan reinitialize: PASS");
    SDL_DestroyTexture(destination);
    SDL_DestroyTexture(source);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (pass) std::puts("PASS: DLSS SDK + OpenGL color/orientation parity");
    else std::puts("FAIL: DLSS SDK + OpenGL color/orientation parity");
    return pass ? 0 : 1;
}
