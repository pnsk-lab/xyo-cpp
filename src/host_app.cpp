#include "sjit/host_app.hpp"

#include "sjit/abi.hpp"
#include "sjit/jit.hpp"
#include "sjit/project_loader.hpp"
#include "sjit/skia_pen_layer.hpp"

#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"

#include <SDL2/SDL.h>
#include <cairo/cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <librsvg/rsvg.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

namespace {

SRuntimeStatus demoFlagScript(SRuntime *runtime, SThread *thread, SFrame *frame) {
    (void)thread;
    SSprite *sprite = sjit_runtime_get_sprite(runtime, 1);
    if (!sprite) {
        return SJIT_STATUS_ERROR;
    }
    switch (frame->pc) {
    case 0:
        sjit_motion_goto_xy(runtime, sprite, sjit_make_number(10.0), sjit_make_number(20.0));
        frame->pc = 1;
        return SJIT_STATUS_YIELDED;
    case 1: {
        SRuntimeStatus wait = sjit_control_wait(runtime, frame, sjit_make_number(0.01), 1);
        if (wait != SJIT_STATUS_OK) {
            return wait;
        }
        frame->pc = 2;
        return SJIT_STATUS_YIELDED;
    }
    case 2:
        sjit_motion_point_in_direction(runtime, sprite, sjit_make_number(181.0));
        frame->finished = 1;
        return SJIT_STATUS_DONE;
    default:
        return SJIT_STATUS_DONE;
    }
}

} // namespace

namespace sjit {

namespace {

constexpr int kStageWidth = 480;
constexpr int kStageHeight = 360;
constexpr double kDefaultStageScale = 2.0;
constexpr double kMinimumStageScale = 0.25;
// Keep the visible stage size independent from the pen buffer.  The default
// window is 2x, so a 2x logical pen buffer gives the pen one pixel per visible
// display pixel instead of scaling a 480x360 buffer with nearest filtering.
constexpr double kMinimumPenResolutionScale = 2.0;
constexpr double kDefaultFps = 60.0;
constexpr int kDensePenCommandThreshold = 8192;
// Graphic effect inputs are script-controlled. Bound the number of derived
// textures so a continuously-changing pixelate value cannot grow host memory
// without limit. 3D2 uses only 25 non-zero levels.
constexpr std::size_t kMaxPixelatedSpriteTextures = 64;

struct StageViewport {
    double scale = kDefaultStageScale;
    int offset_x = 0;
    int offset_y = 0;
    int width = static_cast<int>(kStageWidth * kDefaultStageScale);
    int height = static_cast<int>(kStageHeight * kDefaultStageScale);
};

// The host has one active stage window, so keeping the current viewport here
// lets input, hit testing, and all render paths use exactly the same mapping.
StageViewport g_stage_viewport;

void setStageViewportForScale(double scale) {
    g_stage_viewport.scale = std::max(kMinimumStageScale, scale);
    g_stage_viewport.offset_x = 0;
    g_stage_viewport.offset_y = 0;
    g_stage_viewport.width = std::max(
        1,
        static_cast<int>(std::lround(kStageWidth * g_stage_viewport.scale)));
    g_stage_viewport.height = std::max(
        1,
        static_cast<int>(std::lround(kStageHeight * g_stage_viewport.scale)));
}

void updateStageViewportForWindow(int window_width, int window_height) {
    if (window_width <= 0 || window_height <= 0) {
        return;
    }
    const double scale = std::max(
        kMinimumStageScale,
        std::min(
            static_cast<double>(window_width) / kStageWidth,
            static_cast<double>(window_height) / kStageHeight));
    g_stage_viewport.scale = scale;
    g_stage_viewport.width = std::max(
        1,
        static_cast<int>(std::lround(kStageWidth * scale)));
    g_stage_viewport.height = std::max(
        1,
        static_cast<int>(std::lround(kStageHeight * scale)));
    g_stage_viewport.offset_x = (window_width - g_stage_viewport.width) / 2;
    g_stage_viewport.offset_y = (window_height - g_stage_viewport.height) / 2;
}

struct RuntimeExecution {
    SRuntimeTickFn tick = sjit_runtime_tick;
    SRuntimeVoidFn green_flag = sjit_runtime_green_flag;
    SRuntimeVoidFn stop_all = sjit_runtime_stop_all;
    SRuntimeThreadQueryFn has_threads = sjit_runtime_has_threads;
    SRuntimeThreadQueryFn thread_count = sjit_runtime_thread_count;
    SRuntimePenPathDataFn pen_path_data = sjit_runtime_pen_path_data;
    SRuntimeThreadQueryFn pen_path_count = sjit_runtime_pen_path_count;
    SRuntimeThreadQueryFn pen_path_revision = sjit_runtime_pen_path_revision;
    bool uses_llvm_runtime = false;
};

struct PenPathView {
    const SDrawCommand *items = nullptr;
    int count = 0;
    int revision = 0;
};

PenPathView readPenPath(const RuntimeExecution &execution, const SRuntime *runtime) {
    if (!runtime) {
        return {};
    }
    return {
        execution.pen_path_data(runtime),
        execution.pen_path_count(runtime),
        execution.pen_path_revision(runtime)};
}

RuntimeExecution runtimeExecutionFor(ProjectLoadResult &loaded) {
    RuntimeExecution execution;
    JitEngine *jit = loaded.program.jit.get();
    if (!jit || !jit->hasRuntimeBitcode()) {
        return execution;
    }

    try {
        SRuntimeTickFn tick = jit->runtimeTick();
        SRuntimeVoidFn green_flag = jit->runtimeGreenFlag();
        SRuntimeVoidFn stop_all = jit->runtimeStopAll();
        SRuntimeThreadQueryFn has_threads = jit->runtimeHasThreads();
        SRuntimeThreadQueryFn thread_count = jit->runtimeThreadCount();
        SRuntimePenPathDataFn pen_path_data = jit->runtimePenPathData();
        SRuntimeThreadQueryFn pen_path_count = jit->runtimePenPathCount();
        SRuntimeThreadQueryFn pen_path_revision = jit->runtimePenPathRevision();
        if (tick && green_flag && stop_all && has_threads && thread_count &&
            pen_path_data && pen_path_count && pen_path_revision) {
            execution.tick = tick;
            execution.green_flag = green_flag;
            execution.stop_all = stop_all;
            execution.has_threads = has_threads;
            execution.thread_count = thread_count;
            execution.pen_path_data = pen_path_data;
            execution.pen_path_count = pen_path_count;
            execution.pen_path_revision = pen_path_revision;
            execution.uses_llvm_runtime = true;
        }
    } catch (const std::exception &error) {
        std::cerr << "LLVM runtime execution unavailable: " << error.what() << "\n";
    }
    return execution;
}

double frameMsForFps(double fps) {
    return 1000.0 / (fps > 0.0 ? fps : kDefaultFps);
}

bool configureRuntimeCompatibility(SRuntime *runtime, const ProjectRunOptions &options) {
    if (!runtime || !sjit_runtime_set_compatibility_mode(runtime, options.compatibility_mode)) {
        return false;
    }
    if (options.list_item_limit > 0 &&
        !sjit_runtime_set_list_item_limit(runtime, options.list_item_limit)) {
        return false;
    }
    return true;
}

int scratchToScreenX(double x) {
    return g_stage_viewport.offset_x +
        static_cast<int>(std::lround((x + 240.0) * g_stage_viewport.scale));
}

int scratchToScreenY(double y) {
    return g_stage_viewport.offset_y +
        static_cast<int>(std::lround((180.0 - y) * g_stage_viewport.scale));
}

double penResolutionScale() {
    return std::max(kMinimumPenResolutionScale, g_stage_viewport.scale);
}

int penCanvasWidth() {
    return std::max(
        1,
        static_cast<int>(std::lround(kStageWidth * penResolutionScale())));
}

int penCanvasHeight() {
    return std::max(
        1,
        static_cast<int>(std::lround(kStageHeight * penResolutionScale())));
}

double penCanvasScaleForWidth(int width) {
    return static_cast<double>(width) / static_cast<double>(kStageWidth);
}

int scratchToPenCanvasX(double x, int width) {
    return static_cast<int>(std::floor(
        (x + (kStageWidth * 0.5)) * penCanvasScaleForWidth(width)));
}

int scratchToPenCanvasY(double y, int height) {
    return static_cast<int>(std::floor(
        (kStageHeight * 0.5 - y) *
        (static_cast<double>(height) / static_cast<double>(kStageHeight))));
}

double screenToScratchX(int x) {
    return (static_cast<double>(x - g_stage_viewport.offset_x) /
            g_stage_viewport.scale) - 240.0;
}

double screenToScratchY(int y) {
    return 180.0 -
        (static_cast<double>(y - g_stage_viewport.offset_y) /
         g_stage_viewport.scale);
}

struct HitMask {
    int width = 0;
    int height = 0;
    std::vector<Uint8> alpha;

    bool valid() const {
        return width > 0 && height > 0 &&
            alpha.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
};

struct HitMaskCache {
    std::unordered_map<std::string, HitMask> masks;
};

const TargetRenderInfo *findTargetRenderInfo(
    const std::vector<TargetRenderInfo> &render_targets,
    int target_id);
const CostumeRenderInfo *findCostumeRenderInfo(
    const TargetRenderInfo *target,
    int costume_id);
std::string spriteTextureKey(const TargetRenderInfo *target, int costume_id);

bool loadHitMask(const CostumeRenderInfo &costume, HitMask &mask) {
    mask = {};
    if (costume.source_data.empty()) {
        return false;
    }

    if (costume.data_format == "png") {
        GError *error = nullptr;
        GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
        GdkPixbuf *pixbuf = nullptr;
        if (loader && gdk_pixbuf_loader_write(
                loader,
                reinterpret_cast<const guchar *>(costume.source_data.data()),
                costume.source_data.size(),
                &error) &&
            gdk_pixbuf_loader_close(loader, &error)) {
            pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
            if (pixbuf) {
                g_object_ref(pixbuf);
            }
        }
        if (loader) {
            g_object_unref(loader);
        }
        if (pixbuf) {
            const int width = gdk_pixbuf_get_width(pixbuf);
            const int height = gdk_pixbuf_get_height(pixbuf);
            const int channels = gdk_pixbuf_get_n_channels(pixbuf);
            const int stride = gdk_pixbuf_get_rowstride(pixbuf);
            const guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
            if (width > 0 && height > 0 && (channels == 3 || channels == 4)) {
                mask.width = width;
                mask.height = height;
                mask.alpha.resize(
                    static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        const guchar *source = pixels + y * stride + x * channels;
                        mask.alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)] = channels == 4 ? source[3] : 255;
                    }
                }
            }
            g_object_unref(pixbuf);
        }
        if (error) {
            g_error_free(error);
        }
        return mask.valid();
    }

    if (costume.data_format != "svg" ||
        !std::isfinite(costume.width) || !std::isfinite(costume.height) ||
        costume.width <= 0.0 || costume.height <= 0.0 ||
        costume.width > static_cast<double>(std::numeric_limits<int>::max()) ||
        costume.height > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }

    const int width = std::max(1, static_cast<int>(std::ceil(costume.width)));
    const int height = std::max(1, static_cast<int>(std::ceil(costume.height)));
    GError *error = nullptr;
    RsvgHandle *handle = rsvg_handle_new_from_data(
        reinterpret_cast<const guint8 *>(costume.source_data.data()),
        costume.source_data.size(),
        &error);
    if (!handle) {
        if (error) {
            g_error_free(error);
        }
        return false;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *context = cairo_create(surface);
    RsvgRectangle viewport{0.0, 0.0, costume.width, costume.height};
    const bool rendered = rsvg_handle_render_document(handle, context, &viewport, &error) &&
        cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(context);
    g_object_unref(handle);
    if (!rendered) {
        cairo_surface_destroy(surface);
        if (error) {
            g_error_free(error);
        }
        return false;
    }

    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char *pixels = cairo_image_surface_get_data(surface);
    mask.width = width;
    mask.height = height;
    mask.alpha.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const unsigned char *source_row = pixels + y * stride;
        for (int x = 0; x < width; ++x) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, source_row + x * 4, sizeof(pixel));
            mask.alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)] =
                static_cast<Uint8>((pixel >> 24) & 0xffu);
        }
    }
    cairo_surface_destroy(surface);
    if (error) {
        g_error_free(error);
    }
    return mask.valid();
}

const HitMask *hitMaskFor(
    HitMaskCache &cache,
    const TargetRenderInfo *target,
    int costume_id) {
    const CostumeRenderInfo *costume = findCostumeRenderInfo(target, costume_id);
    if (!target || !costume) {
        return nullptr;
    }
    const std::string key = spriteTextureKey(target, costume_id);
    auto [entry, inserted] = cache.masks.try_emplace(key);
    if (inserted) {
        loadHitMask(*costume, entry->second);
    }
    return entry->second.valid() ? &entry->second : nullptr;
}

int hitTestSpriteAtScreenPoint(
    SRuntime *runtime,
    int screen_x,
    int screen_y,
    const std::vector<TargetRenderInfo> &render_targets,
    HitMaskCache &hit_masks) {
    if (!runtime) {
        return 0;
    }
    int hit_id = 0;
    int hit_layer = -1;
    for (int i = 0; i < runtime->target_count; ++i) {
        SSprite *target = runtime->targets[i];
        if (!target || target->base.is_stage || !target->visible || target->size <= 0.0 ||
            (std::isfinite(target->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST]) &&
             target->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST] >= 100.0)) {
            continue;
        }
        const TargetRenderInfo *info = findTargetRenderInfo(render_targets, target->base.id);
        const CostumeRenderInfo *costume = nullptr;
        if (info && target->current_costume >= 0 &&
            target->current_costume < static_cast<int>(info->costumes.size())) {
            costume = &info->costumes[static_cast<size_t>(target->current_costume)];
        }
        const HitMask *hit_mask = hitMaskFor(hit_masks, info, target->current_costume);

        bool inside = false;
        if (costume && costume->width > 0.0 && costume->height > 0.0) {
            // Convert the mouse point into the unrotated costume's local
            // coordinates around Scratch's rotation center.
            const double scale = target->size / 100.0;
            const double pivot_x = scratchToScreenX(target->x);
            const double pivot_y = scratchToScreenY(target->y);
            const double dx = screen_x - pivot_x;
            const double dy = screen_y - pivot_y;
            const double angle = (90.0 - target->direction) * 3.14159265358979323846 / 180.0;
            const double local_x = (std::cos(angle) * dx + std::sin(angle) * dy) /
                (scale * g_stage_viewport.scale) + costume->rotation_center_x;
            const double local_y = (-std::sin(angle) * dx + std::cos(angle) * dy) /
                (scale * g_stage_viewport.scale) + costume->rotation_center_y;
            const bool inside_costume = std::isfinite(local_x) && std::isfinite(local_y) &&
                local_x >= 0.0 && local_y >= 0.0 &&
                local_x < costume->width && local_y < costume->height;
            if (inside_costume && hit_mask) {
                const int mask_x = std::clamp(
                    static_cast<int>(std::floor(local_x / costume->width * hit_mask->width)),
                    0,
                    hit_mask->width - 1);
                const int mask_y = std::clamp(
                    static_cast<int>(std::floor(local_y / costume->height * hit_mask->height)),
                    0,
                    hit_mask->height - 1);
                inside = hit_mask->alpha[
                    static_cast<std::size_t>(mask_y) * static_cast<std::size_t>(hit_mask->width) +
                    static_cast<std::size_t>(mask_x)] > 0;
            } else {
                // If the asset cannot be decoded for picking, retain the
                // conservative costume bounds rather than making the sprite
                // impossible to click.
                inside = inside_costume;
            }
        } else {
            const int cx = scratchToScreenX(target->x);
            const int cy = scratchToScreenY(target->y);
            const int radius = std::max(8, static_cast<int>(std::lround(
                (target->size / 100.0) * 18.0 * g_stage_viewport.scale)));
            const int dx = screen_x - cx;
            const int dy = screen_y - cy;
            inside = (dx * dx) + (dy * dy) <= radius * radius;
        }
        if (inside && (hit_id == 0 || target->layer_order >= hit_layer)) {
            hit_id = target->base.id;
            hit_layer = target->layer_order;
        }
    }
    return hit_id;
}

struct MouseInteractionState {
    bool left_button_down = false;
    int pressed_target_id = 0;
    bool pressed_target_draggable = false;
    bool dragged = false;
    double press_x = 0.0;
    double press_y = 0.0;
    double drag_offset_x = 0.0;
    double drag_offset_y = 0.0;
};

SSprite *spriteForTargetId(SRuntime *runtime, int target_id) {
    if (!runtime || target_id <= 0) {
        return nullptr;
    }
    for (int i = 0; i < runtime->target_count; ++i) {
        SSprite *target = runtime->targets[i];
        if (target && target->base.id == target_id) {
            return target;
        }
    }
    return nullptr;
}

bool insideStage(int screen_x, int screen_y) {
    return screen_x >= g_stage_viewport.offset_x &&
        screen_x < g_stage_viewport.offset_x + g_stage_viewport.width &&
        screen_y >= g_stage_viewport.offset_y &&
        screen_y < g_stage_viewport.offset_y + g_stage_viewport.height;
}

void setColor(SDL_Renderer *renderer, int r, int g, int b, int a = 255) {
    SDL_SetRenderDrawColor(renderer, static_cast<Uint8>(r), static_cast<Uint8>(g), static_cast<Uint8>(b), static_cast<Uint8>(a));
}

void fillCircle(SDL_Renderer *renderer, int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        const int dx = static_cast<int>(std::sqrt(static_cast<double>((radius * radius) - (dy * dy))));
        SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

const TargetRenderInfo *findTargetRenderInfo(
    const std::vector<TargetRenderInfo> &render_targets,
    int target_id) {
    for (const TargetRenderInfo &render_target : render_targets) {
        if (render_target.target_id == target_id) {
            return &render_target;
        }
    }
    return nullptr;
}

const CostumeRenderInfo *findCostumeRenderInfo(const TargetRenderInfo *target, int costume_id) {
    if (!target || costume_id < 0 || costume_id >= static_cast<int>(target->costumes.size())) {
        return nullptr;
    }
    return &target->costumes[static_cast<size_t>(costume_id)];
}

void drawSpritePlaceholder(
    SDL_Renderer *renderer,
    const SDrawCommand &command,
    const CostumeRenderInfo *costume) {
    if (costume && costume->width > 0.0 && costume->height > 0.0) {
        const double scale = command.size / 100.0;
        const double max_screen_dimension = 96.0;
        double preview_scale = scale;
        const double source_max = std::max(costume->width, costume->height);
        if (source_max > 0.0) {
            const double projected = source_max * scale * g_stage_viewport.scale;
            if (projected > max_screen_dimension) {
                preview_scale *= max_screen_dimension / projected;
            }
        }

        const int width = std::max(12, static_cast<int>(std::lround(
            costume->width * preview_scale * g_stage_viewport.scale)));
        const int height = std::max(12, static_cast<int>(std::lround(
            costume->height * preview_scale * g_stage_viewport.scale)));
        const int left = scratchToScreenX(command.x - (costume->rotation_center_x * preview_scale));
        const int top = scratchToScreenY(command.y + (costume->rotation_center_y * preview_scale));

        SDL_Rect rect{left, top, width, height};
        setColor(renderer, costume->stroke_r, costume->stroke_g, costume->stroke_b, costume->stroke_a);
        SDL_RenderDrawRect(renderer, &rect);

        const int cx = scratchToScreenX(command.x);
        const int cy = scratchToScreenY(command.y);
        setColor(renderer, costume->fill_r, costume->fill_g, costume->fill_b, 255);
        fillCircle(renderer, cx, cy, std::max(2, static_cast<int>(std::lround(
            3.0 * scale * g_stage_viewport.scale))));
        return;
    }

    const int cx = scratchToScreenX(command.x);
    const int cy = scratchToScreenY(command.y);
    const int radius = std::max(8, static_cast<int>(std::lround(
        (command.size / 100.0) * 18.0 * g_stage_viewport.scale)));

    setColor(renderer, 247, 154, 43);
    fillCircle(renderer, cx, cy, radius);
    setColor(renderer, 90, 55, 32);
    const double radians = (90.0 - command.direction) * 3.14159265358979323846 / 180.0;
    const int ex = cx + static_cast<int>(std::lround(std::cos(radians) * radius));
    const int ey = cy - static_cast<int>(std::lround(std::sin(radians) * radius));
    SDL_RenderDrawLine(renderer, cx, cy, ex, ey);
    setColor(renderer, 255, 255, 255);
    fillCircle(renderer, cx - radius / 3, cy - radius / 4, std::max(2, radius / 6));
    fillCircle(renderer, cx + radius / 3, cy - radius / 4, std::max(2, radius / 6));
}

struct SpriteTextureCache {
    std::unordered_map<std::string, SDL_Texture *> textures;
    std::unordered_map<std::string, SDL_Texture *> pixelated_textures;

    ~SpriteTextureCache() {
        for (const auto &entry : textures) {
            SDL_DestroyTexture(entry.second);
        }
        for (const auto &entry : pixelated_textures) {
            SDL_DestroyTexture(entry.second);
        }
    }
};

std::string spriteTextureKey(const TargetRenderInfo *target, int costume_id) {
    return std::to_string(target->target_id) + ":" + std::to_string(costume_id);
}

SDL_Texture *spriteTexture(
    SDL_Renderer *renderer,
    SpriteTextureCache &cache,
    const TargetRenderInfo *target,
    int costume_id) {
    const CostumeRenderInfo *costume = findCostumeRenderInfo(target, costume_id);
    if (!costume || costume->source_data.empty()) {
        return nullptr;
    }
    const std::string key = spriteTextureKey(target, costume_id);
    auto found = cache.textures.find(key);
    if (found != cache.textures.end()) {
        return found->second;
    }

    if (costume->data_format == "png") {
        GError *error = nullptr;
        GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
        GdkPixbuf *pixbuf = nullptr;
        if (loader && gdk_pixbuf_loader_write(
                loader,
                reinterpret_cast<const guchar *>(costume->source_data.data()),
                costume->source_data.size(), &error) &&
            gdk_pixbuf_loader_close(loader, &error)) {
            pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
            if (pixbuf) g_object_ref(pixbuf);
        }
        if (loader) g_object_unref(loader);
        if (pixbuf) {
            const int width = gdk_pixbuf_get_width(pixbuf);
            const int height = gdk_pixbuf_get_height(pixbuf);
            const int channels = gdk_pixbuf_get_n_channels(pixbuf);
            const int stride = gdk_pixbuf_get_rowstride(pixbuf);
            const guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
            std::vector<guchar> rgba(static_cast<size_t>(width * height * 4));
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const guchar *src = pixels + y * stride + x * channels;
                    guchar *dst = rgba.data() + (y * width + x) * 4;
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                    dst[3] = channels == 4 ? src[3] : 255;
                }
            }
            SDL_Texture *texture = SDL_CreateTexture(
                renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
            if (texture && SDL_UpdateTexture(texture, nullptr, rgba.data(), width * 4) == 0) {
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                cache.textures.emplace(key, texture);
                g_object_unref(pixbuf);
                if (error) g_error_free(error);
                return texture;
            }
            if (texture) SDL_DestroyTexture(texture);
            g_object_unref(pixbuf);
        }
        if (error) g_error_free(error);
        return nullptr;
    }

    cairo_surface_t *surface = nullptr;
    if (costume->data_format == "svg") {
        GError *error = nullptr;
        RsvgHandle *handle = rsvg_handle_new_from_data(
            reinterpret_cast<const guint8 *>(costume->source_data.data()),
            costume->source_data.size(), &error);
        if (handle) {
            RsvgRectangle viewport{0.0, 0.0, costume->width, costume->height};
            surface = cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32,
                std::max(1, static_cast<int>(std::ceil(costume->width))),
                std::max(1, static_cast<int>(std::ceil(costume->height))));
            cairo_t *context = cairo_create(surface);
            if (!rsvg_handle_render_document(handle, context, &viewport, &error)) {
                cairo_surface_destroy(surface);
                surface = nullptr;
            }
            cairo_destroy(context);
            g_object_unref(handle);
        }
        if (error) {
            g_error_free(error);
        }
    }
    if (!surface) {
        return nullptr;
    }
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    // Cairo's ARGB32 image surfaces use premultiplied alpha. SDL's regular
    // blend mode expects straight-alpha RGBA, so passing the surface bytes
    // through as ARGB8888 makes translucent SVG pixels get multiplied by
    // their alpha a second time. Flush before reading the image surface and
    // unpremultiply into the same byte layout used by the PNG path.
    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char *source_pixels = cairo_image_surface_get_data(surface);
    std::vector<Uint8> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    for (int y = 0; y < height; ++y) {
        const unsigned char *source_row = source_pixels + y * stride;
        for (int x = 0; x < width; ++x) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, source_row + x * 4, sizeof(pixel));
            Uint8 red = 0;
            Uint8 green = 0;
            Uint8 blue = 0;
            const Uint8 alpha = static_cast<Uint8>((pixel >> 24) & 0xffu);
            if (alpha != 0) {
                red = static_cast<Uint8>(std::min(
                    255,
                    (static_cast<int>((pixel >> 16) & 0xffu) * 255 + alpha / 2) / alpha));
                green = static_cast<Uint8>(std::min(
                    255,
                    (static_cast<int>((pixel >> 8) & 0xffu) * 255 + alpha / 2) / alpha));
                blue = static_cast<Uint8>(std::min(
                    255,
                    (static_cast<int>(pixel & 0xffu) * 255 + alpha / 2) / alpha));
            }
            Uint8 *destination = rgba.data() +
                (static_cast<size_t>(y) * static_cast<size_t>(width) +
                 static_cast<size_t>(x)) * 4u;
            destination[0] = red;
            destination[1] = green;
            destination[2] = blue;
            destination[3] = alpha;
        }
    }
    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture || SDL_UpdateTexture(texture, nullptr, rgba.data(), width * 4) != 0) {
        if (texture) SDL_DestroyTexture(texture);
        cairo_surface_destroy(surface);
        return nullptr;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    cairo_surface_destroy(surface);
    cache.textures.emplace(key, texture);
    return texture;
}

/*
 * SDL_Renderer has no programmable sampler corresponding to scratch-render's
 * pixelate shader.  Approximate that shader in texture space: shrink the
 * costume to one sample per abs(effect) / 10 source texels, then magnify that
 * intermediate texture with nearest-neighbour sampling.  The intermediate is
 * independent of sprite position, scale, and rotation, so it is safe to cache
 * by costume and reduced dimensions.
 */
SDL_Texture *pixelatedSpriteTexture(
    SDL_Renderer *renderer,
    SpriteTextureCache &cache,
    SDL_Texture *source,
    const TargetRenderInfo *target,
    int costume_id,
    double pixelate_effect) {
    if (!renderer || !source || !target || !SDL_RenderTargetSupported(renderer)) {
        return source;
    }

    const double source_texels_per_sample = std::abs(pixelate_effect) / 10.0;
    if (!(source_texels_per_sample > 1.0)) {
        // A sub-texel grid is visually equivalent to the unmodified costume
        // with this SDL approximation, and avoids an unnecessary upsample.
        return source;
    }

    Uint32 source_format = SDL_PIXELFORMAT_UNKNOWN;
    int source_width = 0;
    int source_height = 0;
    if (SDL_QueryTexture(
            source,
            &source_format,
            nullptr,
            &source_width,
            &source_height) != 0 ||
        source_width <= 0 || source_height <= 0) {
        return source;
    }

    const int reduced_width = std::isfinite(source_texels_per_sample)
        ? std::max(1, static_cast<int>(std::ceil(
              static_cast<double>(source_width) / source_texels_per_sample)))
        : 1;
    const int reduced_height = std::isfinite(source_texels_per_sample)
        ? std::max(1, static_cast<int>(std::ceil(
              static_cast<double>(source_height) / source_texels_per_sample)))
        : 1;
    if (reduced_width >= source_width && reduced_height >= source_height) {
        return source;
    }

    const std::string key = spriteTextureKey(target, costume_id) +
        ":pixelate:" + std::to_string(reduced_width) + "x" +
        std::to_string(reduced_height);
    const auto found = cache.pixelated_textures.find(key);
    if (found != cache.pixelated_textures.end()) {
        return found->second;
    }
    if (cache.pixelated_textures.size() >= kMaxPixelatedSpriteTextures) {
        return source;
    }

    SDL_Texture *pixelated = SDL_CreateTexture(
        renderer,
        source_format,
        SDL_TEXTUREACCESS_TARGET,
        reduced_width,
        reduced_height);
    if (!pixelated) {
        // Some renderers cannot use the source format as a target even though
        // they support render targets in general. RGBA32 is the portable
        // fallback used by the PNG costume path as well.
        pixelated = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_TARGET,
            reduced_width,
            reduced_height);
    }
    if (!pixelated) {
        return source;
    }

    SDL_Texture *previous_target = SDL_GetRenderTarget(renderer);
    SDL_Rect previous_viewport{};
    SDL_RenderGetViewport(renderer, &previous_viewport);
    SDL_Rect previous_clip{};
    SDL_RenderGetClipRect(renderer, &previous_clip);
    const SDL_bool previous_clip_enabled = SDL_RenderIsClipEnabled(renderer);
    float previous_scale_x = 1.0f;
    float previous_scale_y = 1.0f;
    SDL_RenderGetScale(renderer, &previous_scale_x, &previous_scale_y);
    Uint8 previous_draw_r = 0;
    Uint8 previous_draw_g = 0;
    Uint8 previous_draw_b = 0;
    Uint8 previous_draw_a = 0;
    SDL_GetRenderDrawColor(
        renderer,
        &previous_draw_r,
        &previous_draw_g,
        &previous_draw_b,
        &previous_draw_a);
    SDL_BlendMode previous_draw_blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &previous_draw_blend);

    SDL_BlendMode previous_source_blend = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(source, &previous_source_blend);
    SDL_ScaleMode previous_source_scale = SDL_ScaleModeNearest;
    SDL_GetTextureScaleMode(source, &previous_source_scale);
    Uint8 previous_source_r = 255;
    Uint8 previous_source_g = 255;
    Uint8 previous_source_b = 255;
    Uint8 previous_source_alpha = 255;
    SDL_GetTextureColorMod(
        source,
        &previous_source_r,
        &previous_source_g,
        &previous_source_b);
    SDL_GetTextureAlphaMod(source, &previous_source_alpha);

    bool generated = SDL_SetRenderTarget(renderer, pixelated) == 0;
    if (generated) {
        SDL_RenderSetScale(renderer, 1.0f, 1.0f);
        SDL_RenderSetViewport(renderer, nullptr);
        SDL_RenderSetClipRect(renderer, nullptr);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
        generated = SDL_RenderClear(renderer) == 0;
    }
    if (generated) {
        generated = SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE) == 0 &&
            SDL_SetTextureScaleMode(source, SDL_ScaleModeNearest) == 0 &&
            SDL_SetTextureColorMod(source, 255, 255, 255) == 0 &&
            SDL_SetTextureAlphaMod(source, 255) == 0;
    }
    if (generated) {
        const SDL_Rect reduced_destination{0, 0, reduced_width, reduced_height};
        generated = SDL_RenderCopy(
            renderer,
            source,
            nullptr,
            &reduced_destination) == 0;
    }

    SDL_SetTextureBlendMode(source, previous_source_blend);
    SDL_SetTextureScaleMode(source, previous_source_scale);
    SDL_SetTextureColorMod(
        source,
        previous_source_r,
        previous_source_g,
        previous_source_b);
    SDL_SetTextureAlphaMod(source, previous_source_alpha);
    if (SDL_SetRenderTarget(renderer, previous_target) == 0) {
        SDL_RenderSetScale(renderer, previous_scale_x, previous_scale_y);
        SDL_RenderSetViewport(renderer, &previous_viewport);
        SDL_RenderSetClipRect(
            renderer,
            previous_clip_enabled == SDL_TRUE ? &previous_clip : nullptr);
        SDL_SetRenderDrawBlendMode(renderer, previous_draw_blend);
        SDL_SetRenderDrawColor(
            renderer,
            previous_draw_r,
            previous_draw_g,
            previous_draw_b,
            previous_draw_a);
    } else {
        generated = false;
    }

    if (!generated ||
        SDL_SetTextureBlendMode(pixelated, SDL_BLENDMODE_BLEND) != 0 ||
        SDL_SetTextureScaleMode(pixelated, SDL_ScaleModeNearest) != 0) {
        SDL_DestroyTexture(pixelated);
        return source;
    }

    cache.pixelated_textures.emplace(key, pixelated);
    return pixelated;
}

bool drawSpriteTexture(
    SDL_Renderer *renderer,
    SRuntime *runtime,
    SpriteTextureCache &cache,
    const SDrawCommand &command,
    const TargetRenderInfo *target) {
    const CostumeRenderInfo *costume = findCostumeRenderInfo(target, command.costume_id);
    SDL_Texture *texture = spriteTexture(renderer, cache, target, command.costume_id);
    if (!texture || !costume) return false;
    const SSprite *sprite = runtime
        ? sjit_runtime_get_sprite(runtime, command.target_id)
        : nullptr;
    double ghost_effect = 0.0;
    double pixelate_effect = 0.0;
    if (sprite) {
        ghost_effect = sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST];
        pixelate_effect = sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_PIXELATE];
    }
    texture = pixelatedSpriteTexture(
        renderer,
        cache,
        texture,
        target,
        command.costume_id,
        pixelate_effect);

    // Cast.toNumber maps NaN to zero before effect storage. Keep the renderer
    // defensive in case it observes externally-constructed runtime state.
    if (std::isnan(ghost_effect)) {
        ghost_effect = 0.0;
    }
    const double clamped_ghost = std::max(0.0, std::min(ghost_effect, 100.0));
    const Uint8 ghost_alpha = static_cast<Uint8>(std::clamp(
        static_cast<int>(std::lround(255.0 * (1.0 - clamped_ghost / 100.0))),
        0,
        255));
    Uint8 previous_alpha = 255;
    SDL_GetTextureAlphaMod(texture, &previous_alpha);
    if (SDL_SetTextureAlphaMod(texture, ghost_alpha) != 0) {
        return false;
    }
    const double scale = std::max(0.0, command.size) / 100.0;
    SDL_Rect destination{
        scratchToScreenX(command.x - costume->rotation_center_x * scale),
        scratchToScreenY(command.y + costume->rotation_center_y * scale),
        std::max(1, static_cast<int>(std::lround(
            costume->width * scale * g_stage_viewport.scale))),
        std::max(1, static_cast<int>(std::lround(
            costume->height * scale * g_stage_viewport.scale)))};
    SDL_Point center{
        static_cast<int>(std::lround(
            costume->rotation_center_x * scale * g_stage_viewport.scale)),
        static_cast<int>(std::lround(
            costume->rotation_center_y * scale * g_stage_viewport.scale))};
    // Scratch direction 90 points right and 0 points up. SDL's positive
    // RenderCopyEx angle rotates clockwise in screen coordinates, so the
    // equivalent angle is direction - 90 rather than 90 - direction.
    const double angle = command.direction - 90.0;
    const bool rendered = SDL_RenderCopyEx(
        renderer,
        texture,
        nullptr,
        &destination,
        angle,
        &center,
        SDL_FLIP_NONE) == 0;
    const bool restored = SDL_SetTextureAlphaMod(texture, previous_alpha) == 0;
    return rendered && restored;
}

bool drawStageBackdrop(
    SDL_Renderer *renderer,
    SRuntime *runtime,
    SpriteTextureCache &cache,
    const std::vector<TargetRenderInfo> *render_targets) {
    if (!runtime || !render_targets) {
        return false;
    }
    for (int i = 0; i < runtime->target_count; ++i) {
        const SSprite *target = runtime->targets[i];
        if (!target || !target->base.is_stage) {
            continue;
        }
        const TargetRenderInfo *render_info = findTargetRenderInfo(*render_targets, target->base.id);
        const CostumeRenderInfo *costume = findCostumeRenderInfo(
            render_info,
            target->current_costume);
        SDL_Texture *texture = spriteTexture(renderer, cache, render_info, target->current_costume);
        if (!texture || !costume ||
            !std::isfinite(costume->width) || !std::isfinite(costume->height) ||
            costume->width <= 0.0 || costume->height <= 0.0) {
            return false;
        }

        // A backdrop is a stage-layer drawable, not a texture that is always
        // stretched to the stage bounds. Keep its logical costume size and
        // place its rotation center at the stage origin, matching Scratch's
        // renderer. Bitmap backdrops with a 2x bitmap resolution already have
        // logical dimensions in CostumeRenderInfo, so this also handles them.
        const double scale = 1.0;
        SDL_Rect destination{
            scratchToScreenX(-costume->rotation_center_x * scale),
            scratchToScreenY(costume->rotation_center_y * scale),
            std::max(1, static_cast<int>(std::lround(
                costume->width * scale * g_stage_viewport.scale))),
            std::max(1, static_cast<int>(std::lround(
                costume->height * scale * g_stage_viewport.scale)))};
        return SDL_RenderCopy(renderer, texture, nullptr, &destination) == 0;
    }
    return false;
}

struct PenLayerCache {
    SDL_Texture *texture = nullptr;
    std::unique_ptr<SkiaPenLayer> layer;
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    int rendered_pen_count = 0;
    int pen_revision = -1;
    bool texture_failed = false;
    bool failure_reported = false;
    bool uses_gpu = false;
    bool dense_pixels = false;
};

struct SkiaGpuState {
    SDL_Window *window = nullptr;
    SDL_GLContext context = nullptr;
    SDL_Window *main_window = nullptr;
    SDL_GLContext main_context = nullptr;
    sk_sp<GrDirectContext> context_owner;
};

GrGLFuncPtr getSdlGlProc(void *, const char name[]) {
    return reinterpret_cast<GrGLFuncPtr>(SDL_GL_GetProcAddress(name));
}

void reportPenLayerFailure(PenLayerCache &cache, const std::string &message) {
    if (!cache.failure_reported) {
        std::cerr << "Skia pen layer unavailable: " << message << "\n";
        cache.failure_reported = true;
    }
}

void destroyPenLayerCache(PenLayerCache &cache) {
    if (cache.texture) {
        SDL_DestroyTexture(cache.texture);
        cache.texture = nullptr;
    }
    cache.layer.reset();
    cache.pixels.clear();
    cache.width = 0;
    cache.height = 0;
    cache.rendered_pen_count = 0;
    cache.pen_revision = -1;
    cache.texture_failed = false;
    cache.failure_reported = false;
    cache.uses_gpu = false;
    cache.dense_pixels = false;
}

void discardPenLayerResources(PenLayerCache &cache) {
    if (cache.texture) {
        SDL_DestroyTexture(cache.texture);
        cache.texture = nullptr;
    }
    cache.layer.reset();
    cache.pixels.clear();
    cache.width = 0;
    cache.height = 0;
    cache.rendered_pen_count = 0;
    cache.pen_revision = -1;
    cache.uses_gpu = false;
}

bool ensurePenLayerSurface(
    GrDirectContext *gpu_context,
    bool use_gpu,
    PenLayerCache &cache) {
    const int width = penCanvasWidth();
    const int height = penCanvasHeight();
    if (cache.layer && cache.width == width && cache.height == height &&
        cache.uses_gpu == use_gpu) {
        return true;
    }
    if (cache.texture_failed) {
        return false;
    }
    discardPenLayerResources(cache);
    cache.layer = std::make_unique<SkiaPenLayer>(width, height, use_gpu ? gpu_context : nullptr);
    if (!cache.layer->valid()) {
        reportPenLayerFailure(cache, "could not create the Skia raster surface");
        cache.layer.reset();
        cache.texture_failed = true;
        return false;
    }
    cache.uses_gpu = use_gpu;
    cache.width = width;
    cache.height = height;
    return true;
}

bool ensurePenLayerTexture(SDL_Renderer *renderer, PenLayerCache &cache) {
    const int width = penCanvasWidth();
    const int height = penCanvasHeight();
    const std::size_t pixel_bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (cache.texture && cache.width == width && cache.height == height &&
        cache.pixels.size() == pixel_bytes) {
        return true;
    }
    if (cache.width != 0 &&
        (cache.width != width || cache.height != height)) {
        discardPenLayerResources(cache);
    } else if (cache.texture) {
        SDL_DestroyTexture(cache.texture);
        cache.texture = nullptr;
        cache.pixels.clear();
        cache.rendered_pen_count = 0;
        cache.pen_revision = -1;
    }
    cache.texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height);
    if (!cache.texture) {
        reportPenLayerFailure(cache, SDL_GetError());
        cache.layer.reset();
        cache.texture_failed = true;
        return false;
    }
    if (SDL_SetTextureBlendMode(cache.texture, SDL_BLENDMODE_BLEND) != 0) {
        reportPenLayerFailure(cache, SDL_GetError());
        SDL_DestroyTexture(cache.texture);
        cache.texture = nullptr;
        cache.layer.reset();
        cache.texture_failed = true;
        return false;
    }
    if (SDL_SetTextureScaleMode(cache.texture, SDL_ScaleModeNearest) != 0) {
        reportPenLayerFailure(cache, SDL_GetError());
        SDL_DestroyTexture(cache.texture);
        cache.texture = nullptr;
        cache.layer.reset();
        cache.texture_failed = true;
        return false;
    }
    cache.pixels.assign(
        pixel_bytes,
        std::uint8_t{0});
    cache.width = width;
    cache.height = height;
    cache.pen_revision = -1;
    cache.rendered_pen_count = 0;
    return true;
}

void resetPenLayerStorage(PenLayerCache &cache, bool dense_pixels) {
    discardPenLayerResources(cache);
    cache.dense_pixels = dense_pixels;
}

void blendDensePixel(
    std::vector<std::uint8_t> &pixels,
    int width,
    int height,
    int x,
    int y,
    int r,
    int g,
    int b,
    int a) {
    if (x < 0 || y < 0 || x >= width || y >= height || a <= 0) {
        return;
    }
    std::uint8_t *destination = pixels.data() +
        (static_cast<std::size_t>(y * width + x) * 4u);
    const int source_alpha = std::clamp(a, 0, 255);
    if (source_alpha == 255) {
        destination[0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
        destination[1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
        destination[2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
        destination[3] = 255;
        return;
    }
    const int destination_alpha = destination[3];
    const int inverse_alpha = 255 - source_alpha;
    const int output_alpha = source_alpha + ((destination_alpha * inverse_alpha + 127) / 255);
    if (output_alpha == 0) {
        return;
    }
    const auto channel = [&](int source, int destination_channel) {
        const int premultiplied = source * source_alpha +
            ((destination_channel * destination_alpha * inverse_alpha + 127) / 255);
        return static_cast<std::uint8_t>(std::clamp(
            (premultiplied + output_alpha / 2) / output_alpha, 0, 255));
    };
    destination[0] = channel(std::clamp(r, 0, 255), destination[0]);
    destination[1] = channel(std::clamp(g, 0, 255), destination[1]);
    destination[2] = channel(std::clamp(b, 0, 255), destination[2]);
    destination[3] = static_cast<std::uint8_t>(output_alpha);
}

void rasterizeDenseStroke(
    std::vector<std::uint8_t> &pixels,
    int width,
    int height,
    const SDrawCommand &command) {
    int x0 = scratchToPenCanvasX(command.x, width);
    int y0 = scratchToPenCanvasY(command.y, height);
    const int x1 = scratchToPenCanvasX(command.x2, width);
    const int y1 = scratchToPenCanvasY(command.y2, height);
    if (command.a <= 0) {
        return;
    }
    const double scale = penCanvasScaleForWidth(width);
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    const int radius = std::max(0, static_cast<int>(std::lround(
        std::max(1.0, command.pen_width) * scale)) / 2);
    if (x0 == x1 && y0 == y1 && radius == 0) {
        if (command.a >= 255 && x0 >= 0 && y0 >= 0 &&
            x0 < width && y0 < height) {
            std::uint8_t *destination = pixels.data() +
                (static_cast<std::size_t>(y0 * width + x0) * 4u);
            destination[0] = static_cast<std::uint8_t>(std::clamp(command.r, 0, 255));
            destination[1] = static_cast<std::uint8_t>(std::clamp(command.g, 0, 255));
            destination[2] = static_cast<std::uint8_t>(std::clamp(command.b, 0, 255));
            destination[3] = 255;
        } else {
            blendDensePixel(
                pixels, width, height, x0, y0,
                command.r, command.g, command.b, command.a);
        }
        return;
    }
    if (x0 == x1 && y0 == y1 && radius == 1 && command.a >= 255) {
        const std::uint8_t red = static_cast<std::uint8_t>(std::clamp(command.r, 0, 255));
        const std::uint8_t green = static_cast<std::uint8_t>(std::clamp(command.g, 0, 255));
        const std::uint8_t blue = static_cast<std::uint8_t>(std::clamp(command.b, 0, 255));
        if (x0 > 0 && y0 > 0 && x0 + 1 < width && y0 + 1 < height) {
            const auto writeOpaque = [&](std::uint8_t *destination) {
                destination[0] = red;
                destination[1] = green;
                destination[2] = blue;
                destination[3] = 255;
            };
            const std::size_t center =
                static_cast<std::size_t>(y0 * width + x0) * 4u;
            std::uint8_t *destination = pixels.data() + center;
            writeOpaque(destination);
            writeOpaque(destination - 4);
            writeOpaque(destination + 4);
            writeOpaque(destination - (width * 4));
            writeOpaque(destination + (width * 4));
            return;
        }
        const auto putOpaque = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= width || y >= height) {
                return;
            }
            std::uint8_t *destination = pixels.data() +
                (static_cast<std::size_t>(y * width + x) * 4u);
            destination[0] = red;
            destination[1] = green;
            destination[2] = blue;
            destination[3] = 255;
        };
        putOpaque(x0, y0);
        putOpaque(x0 - 1, y0);
        putOpaque(x0 + 1, y0);
        putOpaque(x0, y0 - 1);
        putOpaque(x0, y0 + 1);
        return;
    }
    for (;;) {
        for (int oy = -radius; oy <= radius; ++oy) {
            for (int ox = -radius; ox <= radius; ++ox) {
                if (ox * ox + oy * oy <= radius * radius || radius == 0) {
                    blendDensePixel(
                        pixels, width, height, x0 + ox, y0 + oy,
                        command.r, command.g, command.b, command.a);
                }
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twice_error = error * 2;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

bool renderDensePenLayer(
    SDL_Renderer *renderer,
    const PenPathView &pen,
    PenLayerCache &cache) {
    if (!cache.dense_pixels) {
        resetPenLayerStorage(cache, true);
    }
    if (!ensurePenLayerTexture(renderer, cache)) {
        return false;
    }
    if (cache.pen_revision != pen.revision || pen.count < cache.rendered_pen_count) {
        std::fill(cache.pixels.begin(), cache.pixels.end(), std::uint8_t{0});
        cache.rendered_pen_count = 0;
        cache.pen_revision = pen.revision;
    }
    for (int i = cache.rendered_pen_count; i < pen.count; ++i) {
        const SDrawCommand &command = pen.items[i];
        if (command.kind == SJIT_DRAW_PEN_STROKE && command.visible) {
            rasterizeDenseStroke(cache.pixels, cache.width, cache.height, command);
        }
    }
    cache.rendered_pen_count = pen.count;
    const int row_bytes = cache.width * 4;
    const SDL_Rect destination{
        g_stage_viewport.offset_x,
        g_stage_viewport.offset_y,
        g_stage_viewport.width,
        g_stage_viewport.height};
    if (SDL_UpdateTexture(cache.texture, nullptr, cache.pixels.data(), row_bytes) != 0 ||
        SDL_RenderCopy(renderer, cache.texture, nullptr, &destination) != 0) {
        reportPenLayerFailure(cache, SDL_GetError());
        return false;
    }
    return true;
}

bool renderCompactPenRasterTile(
    SDL_Renderer *renderer,
    const SRuntime *runtime,
    PenLayerCache &cache) {
    if (!runtime) {
        return false;
    }
    const SPenRasterTile &tile = runtime->pen_raster_tile;
    if (!tile.active || !tile.pixels || !tile.active_bits ||
        tile.width != SJIT_PEN_RASTER_TILE_WIDTH ||
        tile.height != SJIT_PEN_RASTER_TILE_HEIGHT ||
        tile.stride != SJIT_PEN_RASTER_TILE_WIDTH * 4 ||
        tile.rows_filled < 0 || tile.rows_filled > tile.height ||
        tile.command_count < 0 ||
        tile.command_count > tile.rows_filled * tile.width ||
        tile.revision != runtime->pen.revision ||
        runtime->pen.length < 0 || runtime->pen.length > runtime->pen.capacity ||
        (runtime->pen.length > 0 && !runtime->pen.items)) {
        return false;
    }
    if (!cache.dense_pixels) {
        resetPenLayerStorage(cache, true);
    }
    if (!ensurePenLayerTexture(renderer, cache)) {
        return false;
    }

    std::fill(cache.pixels.begin(), cache.pixels.end(), std::uint8_t{0});
    // The runtime ABI tile intentionally stays at Scratch's logical
    // 480x360 resolution. Expand it into the independent high-resolution
    // host buffer before compositing any commands that follow the tile.
    for (int y = 0; y < cache.height; ++y) {
        const int source_y = std::min(
            tile.height - 1,
            (y * tile.height) / std::max(1, cache.height));
        const std::uint8_t *source_row = tile.pixels +
            static_cast<std::size_t>(source_y) * static_cast<std::size_t>(tile.stride);
        std::uint8_t *destination_row = cache.pixels.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(cache.width) * 4u;
        for (int x = 0; x < cache.width; ++x) {
            const int source_x = std::min(
                tile.width - 1,
                (x * tile.width) / std::max(1, cache.width));
            std::memcpy(
                destination_row + static_cast<std::size_t>(x) * 4u,
                source_row + static_cast<std::size_t>(source_x) * 4u,
                4u);
        }
    }
    for (int i = 0; i < runtime->pen.length; ++i) {
        const SDrawCommand &command = runtime->pen.items[i];
        if (command.kind == SJIT_DRAW_PEN_STROKE && command.visible) {
            rasterizeDenseStroke(cache.pixels, cache.width, cache.height, command);
        }
    }
    cache.pen_revision = tile.revision;
    cache.rendered_pen_count = tile.command_count + runtime->pen.length;

    const int row_bytes = cache.width * 4;
    const SDL_Rect destination{
        g_stage_viewport.offset_x,
        g_stage_viewport.offset_y,
        g_stage_viewport.width,
        g_stage_viewport.height};
    if (SDL_UpdateTexture(cache.texture, nullptr, cache.pixels.data(), row_bytes) != 0 ||
        SDL_RenderCopy(renderer, cache.texture, nullptr, &destination) != 0) {
        reportPenLayerFailure(cache, SDL_GetError());
        return false;
    }
    return true;
}

void clearPenLayerTexture(PenLayerCache &cache) {
    if (cache.layer) {
        cache.layer->clear();
    }
    cache.rendered_pen_count = 0;
}

bool drawPenStrokeToLayer(PenLayerCache &cache, const SDrawCommand &command) {
    if (!cache.layer) {
        return false;
    }
    const double scale = penCanvasScaleForWidth(cache.width);
    return cache.layer->drawStroke(
        (command.x + (kStageWidth * 0.5)) * scale,
        (kStageHeight * 0.5 - command.y) * scale,
        (command.x2 + (kStageWidth * 0.5)) * scale,
        (kStageHeight * 0.5 - command.y2) * scale,
        std::max(1.0, command.pen_width) * scale,
        command.r,
        command.g,
        command.b,
        command.a);
}

bool renderPenLayer(
    SDL_Renderer *renderer,
    SRuntime *runtime,
    const RuntimeExecution &execution,
    SkiaGpuState *gpu,
    PenLayerCache &cache) {
    if (!runtime) {
        return false;
    }
    if (cache.dense_pixels) {
        resetPenLayerStorage(cache, false);
    }
    const PenPathView pen = readPenPath(execution, runtime);
    const int pen_count = pen.count;
    if (pen_count < 0 || (pen_count > 0 && !pen.items)) {
        reportPenLayerFailure(cache, "runtime returned an invalid pen path");
        return false;
    }
    const bool use_gpu = gpu && gpu->context_owner;
    const auto restoreMainContext = [&]() {
        if (gpu && gpu->main_context) {
            SDL_GL_MakeCurrent(gpu->main_window, gpu->main_context);
        }
    };
    if (use_gpu) {
        if (SDL_RenderFlush(renderer) != 0) {
            reportPenLayerFailure(cache, SDL_GetError());
            return false;
        }
        if (SDL_GL_MakeCurrent(gpu->window, gpu->context) != 0) {
            reportPenLayerFailure(cache, SDL_GetError());
            return false;
        }
    }
    if (!ensurePenLayerSurface(
            use_gpu ? gpu->context_owner.get() : nullptr, use_gpu, cache)) {
        restoreMainContext();
        return false;
    }
    restoreMainContext();
    if (!ensurePenLayerTexture(renderer, cache)) {
        return false;
    }
    if (use_gpu &&
        SDL_GL_MakeCurrent(gpu->window, gpu->context) != 0) {
        reportPenLayerFailure(cache, SDL_GetError());
        return false;
    }

    bool dirty = false;
    if (cache.pen_revision != pen.revision || pen_count < cache.rendered_pen_count) {
        cache.pen_revision = pen.revision;
        clearPenLayerTexture(cache);
        dirty = true;
    }

    std::vector<SkiaPenPoint> opaque_points;
    const auto flushOpaquePoints = [&]() {
        if (opaque_points.empty()) {
            return true;
        }
        const bool drawn = cache.layer->drawOpaquePoints(opaque_points.data(), opaque_points.size());
        opaque_points.clear();
        return drawn;
    };
    for (int i = cache.rendered_pen_count; i < pen_count; ++i) {
        const SDrawCommand &command = pen.items[i];
        if (command.kind == SJIT_DRAW_PEN_STROKE && command.visible) {
            if (cache.uses_gpu && command.a >= 255 &&
                command.x == command.x2 && command.y == command.y2) {
                const double scale = penCanvasScaleForWidth(cache.width);
                opaque_points.push_back({
                    (command.x + (kStageWidth * 0.5)) * scale,
                    (kStageHeight * 0.5 - command.y) * scale,
                    std::max(1.0, command.pen_width) * scale,
                    command.r,
                    command.g,
                    command.b});
                dirty = true;
                continue;
            }
            if (!flushOpaquePoints()) {
                reportPenLayerFailure(cache, "rejected a non-finite pen point");
            }
            if (drawPenStrokeToLayer(cache, command)) {
                dirty = true;
            } else {
                reportPenLayerFailure(cache, "rejected a non-finite pen stroke");
            }
        }
    }
    if (!flushOpaquePoints()) {
        reportPenLayerFailure(cache, "rejected a non-finite pen point");
    }
    cache.rendered_pen_count = pen_count;

    if (dirty) {
        const std::size_t row_bytes =
            static_cast<std::size_t>(cache.width) * 4u;
        if (!cache.layer->readRgbaPixels(cache.pixels.data(), row_bytes)) {
            restoreMainContext();
            reportPenLayerFailure(cache, "could not transfer pixels to SDL");
            cache.rendered_pen_count = 0;
            cache.pen_revision = -1;
            return false;
        }
        restoreMainContext();
        if (SDL_UpdateTexture(
                cache.texture,
                nullptr,
                cache.pixels.data(),
                static_cast<int>(row_bytes)) != 0) {
            reportPenLayerFailure(cache, "could not transfer pixels to SDL");
            cache.rendered_pen_count = 0;
            cache.pen_revision = -1;
            return false;
        }
    } else {
        restoreMainContext();
    }
    const SDL_Rect destination{
        g_stage_viewport.offset_x,
        g_stage_viewport.offset_y,
        g_stage_viewport.width,
        g_stage_viewport.height};
    if (SDL_RenderCopy(renderer, cache.texture, nullptr, &destination) != 0) {
        reportPenLayerFailure(cache, SDL_GetError());
        return false;
    }
    return true;
}

const char *glyphRows(char raw) {
    const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
    switch (ch) {
    case '0': return "111101101101101101111";
    case '1': return "010110010010010010111";
    case '2': return "111001001111100100111";
    case '3': return "111001001111001001111";
    case '4': return "101101101111001001001";
    case '5': return "111100100111001001111";
    case '6': return "111100100111101101111";
    case '7': return "111001001010010010010";
    case '8': return "111101101111101101111";
    case '9': return "111101101111001001111";
    case 'A': return "010101101111101101101";
    case 'B': return "110101101110101101110";
    case 'C': return "111100100100100100111";
    case 'D': return "110101101101101101110";
    case 'E': return "111100100111100100111";
    case 'F': return "111100100111100100100";
    case 'G': return "111100100101101101111";
    case 'H': return "101101101111101101101";
    case 'I': return "111010010010010010111";
    case 'J': return "001001001001101101111";
    case 'K': return "101101110100110101101";
    case 'L': return "100100100100100100111";
    case 'M': return "101111111101101101101";
    case 'N': return "101111111111111111101";
    case 'O': return "111101101101101101111";
    case 'P': return "111101101111100100100";
    case 'Q': return "111101101101101111001";
    case 'R': return "111101101111110101101";
    case 'S': return "111100100111001001111";
    case 'T': return "111010010010010010010";
    case 'U': return "101101101101101101111";
    case 'V': return "101101101101101101010";
    case 'W': return "101101101101111111101";
    case 'X': return "101101101010101101101";
    case 'Y': return "101101101010010010010";
    case 'Z': return "111001001010100100111";
    case '.': return "000000000000000010010";
    case ':': return "000010010000010010000";
    case '-': return "000000000111000000000";
    case '+': return "000010010111010010000";
    case '/': return "001001010010010100100";
    case '_': return "000000000000000000111";
    case '=': return "000000111000111000000";
    case '@': return "111101111101111100111";
    case ' ': return "000000000000000000000";
    default: return "111101001001001101111";
    }
}

void drawChar(SDL_Renderer *renderer, int x, int y, char ch, int scale) {
    const char *rows = glyphRows(ch);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (rows[(row * 3) + col] == '1') {
                SDL_Rect pixel{x + (col * scale), y + (row * scale), scale, scale};
                SDL_RenderFillRect(renderer, &pixel);
            }
        }
    }
}

void drawText(SDL_Renderer *renderer, int x, int y, const std::string &text, int scale = 2) {
    int cursor = x;
    for (char ch : text) {
        drawChar(renderer, cursor, y, ch, scale);
        cursor += 4 * scale;
    }
}

std::string valueToDisplay(SRuntime *runtime, SValue value) {
    if (value.tag == SJIT_VALUE_LIST) {
        return "[" + std::to_string(sjit_list_length(static_cast<SList *>(value.ptr))) + "]";
    }
    SValue text = sjit_to_string(runtime, value);
    std::string out = sjit_string_cstr(static_cast<SString *>(text.ptr));
    sjit_value_destroy(text);
    if (out.size() > 18) {
        out.resize(18);
    }
    return out;
}

void drawVariableMonitors(SDL_Renderer *renderer, SRuntime *runtime) {
    if (!runtime) {
        return;
    }
    const int stage_width = g_stage_viewport.width;
    const int stage_height = g_stage_viewport.height;
    for (int index = 0; index < sjit_runtime_variable_monitor_count(runtime); ++index) {
        const SVariableMonitor *monitor = sjit_runtime_variable_monitor_at(runtime, index);
        if (!monitor || !monitor->visible) {
            continue;
        }
        SVariable *variable = sjit_runtime_variable_for_monitor(runtime, monitor);
        if (!variable) {
            continue;
        }

        const std::string label = sjit_string_cstr(monitor->label);
        const std::string value = valueToDisplay(runtime, variable->value);
        const bool large = monitor->mode == SJIT_MONITOR_MODE_LARGE;
        const std::string text = large ? value : label + ":" + value;
        const int text_scale = std::max(1, static_cast<int>(std::lround(
            (large ? 3.0 : 2.0) * g_stage_viewport.scale / kDefaultStageScale)));
        const int stage_x = std::clamp(
            static_cast<int>(std::lround(monitor->x * g_stage_viewport.scale)),
            0,
            stage_width - 1);
        const int stage_y = std::clamp(
            static_cast<int>(std::lround(monitor->y * g_stage_viewport.scale)),
            0,
            stage_height - 1);
        const int x = g_stage_viewport.offset_x + stage_x;
        const int y = g_stage_viewport.offset_y + stage_y;
        const double base_text_scale = large ? 3.0 : 2.0;
        const double base_auto_width =
            16.0 + static_cast<double>(text.size()) * 4.0 * base_text_scale;
        const int auto_width = std::max(
            1,
            static_cast<int>(std::lround(
                base_auto_width * g_stage_viewport.scale / kDefaultStageScale)));
        const int requested_width = monitor->width > 0.0 ?
            static_cast<int>(std::lround(monitor->width * g_stage_viewport.scale)) : auto_width;
        const int requested_height = monitor->height > 0.0 ?
            static_cast<int>(std::lround(monitor->height * g_stage_viewport.scale)) :
            std::max(1, static_cast<int>(std::lround(
                (large ? 30.0 : 24.0) * g_stage_viewport.scale / kDefaultStageScale)));
        SDL_Rect box{
            x,
            y,
            std::clamp(requested_width, 1, stage_width - stage_x),
            std::clamp(requested_height, 1, stage_height - stage_y)};
        setColor(renderer, 255, 248, 215);
        SDL_RenderFillRect(renderer, &box);
        setColor(renderer, 108, 91, 40);
        SDL_RenderDrawRect(renderer, &box);
        setColor(renderer, 54, 50, 42);
        drawText(renderer, x + 8, y + std::max(3, (box.h - (5 * text_scale)) / 2), text, text_scale);
    }
}

int keyIndexForSdl(SDL_Keycode key, bool turbo_warp_compatibility) {
    switch (key) {
    case SDLK_UP:
        return SJIT_KEY_UP_ARROW;
    case SDLK_DOWN:
        return SJIT_KEY_DOWN_ARROW;
    case SDLK_RIGHT:
        return SJIT_KEY_RIGHT_ARROW;
    case SDLK_LEFT:
        return SJIT_KEY_LEFT_ARROW;
    default:
        break;
    }
    if (turbo_warp_compatibility) {
        switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return SJIT_KEY_ENTER;
        case SDLK_BACKSPACE:
            return SJIT_KEY_BACKSPACE;
        case SDLK_DELETE:
            return SJIT_KEY_DELETE;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:
            return SJIT_KEY_SHIFT;
        case SDLK_CAPSLOCK:
            return SJIT_KEY_CAPS_LOCK;
        case SDLK_SCROLLLOCK:
            return SJIT_KEY_SCROLL_LOCK;
        case SDLK_LCTRL:
        case SDLK_RCTRL:
            return SJIT_KEY_CONTROL;
        case SDLK_ESCAPE:
            return SJIT_KEY_ESCAPE;
        case SDLK_INSERT:
            return SJIT_KEY_INSERT;
        case SDLK_HOME:
            return SJIT_KEY_HOME;
        case SDLK_END:
            return SJIT_KEY_END;
        case SDLK_PAGEUP:
            return SJIT_KEY_PAGE_UP;
        case SDLK_PAGEDOWN:
            return SJIT_KEY_PAGE_DOWN;
        case SDLK_KP_0:
            return '0';
        case SDLK_KP_1:
            return '1';
        case SDLK_KP_2:
            return '2';
        case SDLK_KP_3:
            return '3';
        case SDLK_KP_4:
            return '4';
        case SDLK_KP_5:
            return '5';
        case SDLK_KP_6:
            return '6';
        case SDLK_KP_7:
            return '7';
        case SDLK_KP_8:
            return '8';
        case SDLK_KP_9:
            return '9';
        case SDLK_KP_PERIOD:
            return '.';
        case SDLK_KP_DIVIDE:
            return '/';
        case SDLK_KP_MULTIPLY:
            return '*';
        case SDLK_KP_MINUS:
            return '-';
        case SDLK_KP_PLUS:
            return '+';
        case SDLK_KP_EQUALS:
            return '=';
        default:
            break;
        }
    }
    if (key >= 0 && key < 256) {
        const int raw = static_cast<int>(key);
        return raw >= 'a' && raw <= 'z' ? raw - ('a' - 'A') : raw;
    }
    return -1;
}

void updateInputFromEvent(
    SHostInputSnapshot &input,
    SRuntime *runtime,
    MouseInteractionState &mouse,
    HitMaskCache &hit_masks,
    const std::vector<TargetRenderInfo> &render_targets,
    bool turbo_warp_compatibility,
    const SDL_Event &event) {
    if (event.type == SDL_MOUSEMOTION) {
        input.mouse_x = screenToScratchX(event.motion.x);
        input.mouse_y = screenToScratchY(event.motion.y);
        if (mouse.left_button_down && mouse.pressed_target_draggable) {
            const double x = input.mouse_x;
            const double y = input.mouse_y;
            const double dx = x - mouse.press_x;
            const double dy = y - mouse.press_y;
            // Ignore sub-pixel motion so a stationary draggable sprite still
            // behaves as a click. Once crossed, keep the interaction in drag
            // mode until the button is released.
            if (!mouse.dragged && dx * dx + dy * dy > 0.25) {
                mouse.dragged = true;
            }
            if (mouse.dragged) {
                if (SSprite *target = spriteForTargetId(runtime, mouse.pressed_target_id)) {
                    sjit_sprite_set_xy(
                        runtime,
                        target,
                        x + mouse.drag_offset_x,
                        y + mouse.drag_offset_y,
                        0);
                }
            }
        }
    } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
        // Scratch's ordinary mouse input is the primary (left) button. Do
        // not let a right-button event clear a left-button drag or start a
        // second click hat.
        if (event.button.button != SDL_BUTTON_LEFT) {
            return;
        }
        input.mouse_x = screenToScratchX(event.button.x);
        input.mouse_y = screenToScratchY(event.button.y);
        if (event.type == SDL_MOUSEBUTTONDOWN) {
            input.mouse_down = 1;
            input.mouse_pressed_edge = 1;
            mouse.left_button_down = true;
            mouse.pressed_target_id = 0;
            mouse.pressed_target_draggable = false;
            mouse.dragged = false;
            mouse.press_x = input.mouse_x;
            mouse.press_y = input.mouse_y;

            if (!insideStage(event.button.x, event.button.y)) {
                return;
            }
            const int clicked_target = hitTestSpriteAtScreenPoint(
                runtime,
                event.button.x,
                event.button.y,
                render_targets,
                hit_masks);
            if (SSprite *target = spriteForTargetId(runtime, clicked_target)) {
                mouse.pressed_target_id = clicked_target;
                mouse.pressed_target_draggable = target->draggable != 0;
                mouse.drag_offset_x = target->x - input.mouse_x;
                mouse.drag_offset_y = target->y - input.mouse_y;
                if (!mouse.pressed_target_draggable) {
                    input.sprite_clicked_id = clicked_target;
                }
            } else {
                input.stage_clicked = 1;
            }
        } else {
            input.mouse_down = 0;
            if (mouse.left_button_down && mouse.pressed_target_draggable &&
                !mouse.dragged && insideStage(event.button.x, event.button.y)) {
                const int released_target = hitTestSpriteAtScreenPoint(
                    runtime,
                    event.button.x,
                    event.button.y,
                    render_targets,
                    hit_masks);
                if (released_target == mouse.pressed_target_id) {
                    input.sprite_clicked_id = released_target;
                }
            }
            mouse.left_button_down = false;
            mouse.pressed_target_id = 0;
            mouse.pressed_target_draggable = false;
            mouse.dragged = false;
            mouse.drag_offset_x = 0.0;
            mouse.drag_offset_y = 0.0;
        }
    } else if (event.type == SDL_MOUSEWHEEL) {
        // TurboWarp's mouseWheel IO starts the corresponding arrow-key hats,
        // but does not mutate the keyboard's pressed state. Keep this as a
        // separate event path so scrolling cannot make sensing_keypressed
        // report an arrow key as held. SDL's flipped direction stores the
        // opposite sign, so normalize it before applying the DOM-like
        // deltaY < 0 (up) / deltaY > 0 (down) mapping.
        if (!turbo_warp_compatibility || !runtime) {
            return;
        }
        const double vertical = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ?
            -static_cast<double>(event.wheel.y) : static_cast<double>(event.wheel.y);
        const char *key = vertical > 0.0 ? "up arrow" :
            (vertical < 0.0 ? "down arrow" : nullptr);
        if (key) {
            sjit_runtime_start_hats(runtime, SJIT_HAT_EVENT_WHENKEYPRESSED, key);
        }
    } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        const int key = keyIndexForSdl(event.key.keysym.sym, turbo_warp_compatibility);
        if (key >= 0 && key < 256) {
            input.key_down[key] = event.type == SDL_KEYDOWN ? 1 : 0;
            input.key_pressed_edge[key] = event.type == SDL_KEYDOWN && event.key.repeat == 0 ? 1 : 0;
        }
    } else if (event.type == SDL_WINDOWEVENT &&
               event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        input.mouse_down = 0;
        std::fill(std::begin(input.key_down), std::end(input.key_down), 0);
        std::fill(std::begin(input.key_pressed_edge), std::end(input.key_pressed_edge), 0);
        mouse = {};
    }
}

void clearKeyEdges(SHostInputSnapshot &input) {
    std::fill(std::begin(input.key_pressed_edge), std::end(input.key_pressed_edge), 0);
    input.stage_clicked = 0;
    input.sprite_clicked_id = 0;
    input.mouse_pressed_edge = 0;
}

void renderRuntime(
    SDL_Renderer *renderer,
    SRuntime *runtime,
    const RuntimeExecution &execution,
    SkiaGpuState *gpu,
    PenLayerCache &pen_cache,
    SpriteTextureCache &sprite_cache,
    const std::vector<TargetRenderInfo> *render_targets) {
    setColor(renderer, 245, 247, 250);
    SDL_RenderClear(renderer);

    SDL_Rect stage{
        g_stage_viewport.offset_x,
        g_stage_viewport.offset_y,
        g_stage_viewport.width,
        g_stage_viewport.height};
    setColor(renderer, 255, 255, 255);
    SDL_RenderFillRect(renderer, &stage);
    setColor(renderer, 212, 218, 227);
    SDL_RenderDrawRect(renderer, &stage);

    // The stage is not a visible sprite and therefore does not appear in the
    // runtime draw-command buffer. Render its selected backdrop explicitly
    // before the persistent pen layer and sprites.
    drawStageBackdrop(renderer, runtime, sprite_cache, render_targets);

    bool pen_layer_rendered =
        renderCompactPenRasterTile(renderer, runtime, pen_cache);
    if (!pen_layer_rendered) {
        const PenPathView pen = readPenPath(execution, runtime);
        const bool use_dense_pen_path =
            pen.count >= kDensePenCommandThreshold && pen.items;
        pen_layer_rendered = use_dense_pen_path
            ? renderDensePenLayer(renderer, pen, pen_cache)
            : renderPenLayer(renderer, runtime, execution, gpu, pen_cache);
    }
    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(runtime);
    if (draw) {
        const int first_draw_command = pen_layer_rendered ?
            std::clamp(runtime->draw_pen_length, 0, draw->length) : 0;
        for (int i = first_draw_command; i < draw->length; ++i) {
            const SDrawCommand &command = draw->items[i];
            if (command.kind == SJIT_DRAW_SPRITE && command.visible && command.size > 0.0) {
                const TargetRenderInfo *target_render = render_targets
                    ? findTargetRenderInfo(*render_targets, command.target_id)
                    : nullptr;
                const CostumeRenderInfo *costume = findCostumeRenderInfo(target_render, command.costume_id);
                if (!drawSpriteTexture(
                        renderer,
                        runtime,
                        sprite_cache,
                        command,
                        target_render)) {
                    drawSpritePlaceholder(renderer, command, costume);
                }
            }
        }
    }

    drawVariableMonitors(renderer, runtime);
    SDL_RenderPresent(renderer);

}

std::string makeWindowTitle(SRuntime *runtime, int tick) {
    std::string title = "xyo-cpp Scratch window";
    int shown = 0;
    for (int target_index = 0; target_index < runtime->target_count; ++target_index) {
        SSprite *target = runtime->targets[target_index];
        for (int variable_index = 0; variable_index < target->base.variable_count; ++variable_index) {
            SVariable *variable = &target->base.variables[variable_index];
            title += " | ";
            title += sjit_string_cstr(variable->name);
            title += "=";
            title += valueToDisplay(runtime, variable->value);
            if (++shown >= 8) {
                title += " | ...";
                title += " | tick=" + std::to_string(tick);
                return title;
            }
        }
    }
    title += " | tick=" + std::to_string(tick);
    return title;
}

} // namespace

const char *runtimeStatusName(SRuntimeStatus status) {
    switch (status) {
    case SJIT_STATUS_OK:
        return "ok";
    case SJIT_STATUS_YIELDED:
        return "yielded";
    case SJIT_STATUS_YIELD_TICK:
        return "yield_tick";
    case SJIT_STATUS_WAITING:
        return "waiting";
    case SJIT_STATUS_DONE:
        return "done";
    case SJIT_STATUS_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

const char *threadStatusName(int status) {
    switch (status) {
    case SJIT_THREAD_RUNNING:
        return "running";
    case SJIT_THREAD_PROMISE_WAIT:
        return "promise_wait";
    case SJIT_THREAD_YIELD:
        return "yield";
    case SJIT_THREAD_YIELD_TICK:
        return "yield_tick";
    case SJIT_THREAD_DONE:
        return "done";
    case SJIT_THREAD_KILLED:
        return "killed";
    default:
        return "unknown";
    }
}

static void printRuntimeSummary(
    SRuntime *runtime,
    const RuntimeExecution &execution,
    SRuntimeStatus last_status = SJIT_STATUS_DONE) {
    if (!runtime) {
        return;
    }
    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(runtime);
    const PenPathView pen = readPenPath(execution, runtime);
    std::cout << "last status: " << runtimeStatusName(last_status) << "\n";
    std::cout << "threads: " << runtime->thread_count << "\n";
    for (int i = 0; i < runtime->thread_count; ++i) {
        SThread *thread = runtime->threads[i];
        std::cout << "  thread " << thread->id
                  << " target=" << thread->target_id
                  << " script=" << thread->script_id
                  << " status=" << threadStatusName(thread->status)
                  << " pc=" << thread->frame.pc
                  << " loops=" << thread->frame.loop_state_count
                  << "\n";
    }
    std::cout << "pen commands: " << pen.count << "\n";
    std::cout << "draw commands: " << (draw ? draw->length : 0) << "\n";
#ifdef SJIT_PROFILE_RUNTIME
    static const char *profile_names[SJIT_PROFILE_COUNT] = {
        "list_number_at_number",
        "list_number_at_value",
        "list_item_value",
        "list_item_number_search",
        "list_contains",
        "variable_number",
        "variable_value",
        "value_compare",
        "mathop",
        "mathop_abs",
        "mathop_floor",
        "mathop_ceil",
        "mathop_sqrt",
        "mathop_sin",
        "mathop_cos",
        "mathop_tan",
        "mathop_asin",
        "mathop_acos",
        "mathop_atan",
        "mathop_ln",
        "mathop_log10",
        "mathop_exp",
        "mathop_pow10",
        "variable_set_from_value",
        "list_replace_number",
        "pen_render_pixel",
        "pen_color_list",
        "pen_color_value",
        "pen_stamp",
        "value_compare_number_number",
        "list_value_cached_number",
        "variable_set_number_source",
        "list_value_index_number",
        "set_from_variable",
        "set_from_argument",
        "set_from_list_item",
        "value_compare_number_string",
        "value_compare_string_number",
        "value_compare_string_string",
        "value_compare_number_other",
        "value_compare_other_number",
        "value_compare_other_other",
        "value_compare_lt",
        "value_compare_eq",
        "value_compare_gt",
        "variable_set_string_source",
        "variable_set_bool_source",
        "variable_set_null_source",
        "variable_set_list_source",
        "variable_set_same_string",
        "value_compare_string_string_numeric",
        "list_number_numeric_cache",
        "list_number_item_number",
        "list_number_item_string_cache",
        "list_number_item_fallback",
    };
    std::cout << "runtime profile:\n";
    for (int i = 0; i < SJIT_PROFILE_COUNT; ++i) {
        std::cout << "  " << profile_names[i] << "=" << runtime->profile_counts[i] << "\n";
    }
#endif
    for (int i = 0; i < runtime->target_count; ++i) {
        SSprite *target = runtime->targets[i];
        std::cout << "target " << target->base.id << " " << sjit_string_cstr(target->base.name)
                  << " x=" << target->x << " y=" << target->y
                  << " direction=" << target->direction << "\n";
        for (int j = 0; j < target->base.variable_count; ++j) {
            SVariable *variable = &target->base.variables[j];
            std::cout << "  variable " << sjit_string_cstr(variable->name) << " = ";
            if (variable->value.tag == SJIT_VALUE_LIST) {
                SList *list = static_cast<SList *>(variable->value.ptr);
                const int length = sjit_list_length(list);
                std::cout << "[list length=" << length << "]";
                std::cout << "\n";
            } else {
                SValue text = sjit_to_string(runtime, variable->value);
                std::cout << sjit_string_cstr(static_cast<SString *>(text.ptr)) << "\n";
                sjit_value_destroy(text);
            }
        }
    }
}

int runHostDemo() {
    SRuntime *runtime = sjit_runtime_create();
    if (!runtime) {
        std::cerr << "failed to create runtime\n";
        return 1;
    }

    SSprite *sprite = sjit_runtime_create_sprite(runtime, "Sprite1", 0);
    if (!sprite) {
        sjit_runtime_destroy(runtime);
        std::cerr << "failed to create sprite\n";
        return 1;
    }

    if (!sjit_runtime_register_script(
            runtime,
            sprite->base.id,
            100,
            SJIT_HAT_EVENT_WHENFLAGCLICKED,
            "",
            1,
            0,
            demoFlagScript)) {
        sjit_runtime_destroy(runtime);
        std::cerr << "failed to register demo script\n";
        return 1;
    }

    sjit_runtime_green_flag(runtime);
    sjit_runtime_set_time(runtime, 0.0, 0.0);
    sjit_runtime_tick(runtime);
    sjit_runtime_set_time(runtime, 20.0, 20.0);
    sjit_runtime_tick(runtime);
    sjit_runtime_set_time(runtime, 40.0, 20.0);
    sjit_runtime_tick(runtime);
    sjit_runtime_set_time(runtime, 60.0, 20.0);
    sjit_runtime_tick(runtime);

    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(runtime);
    std::cout << "draw commands: " << (draw ? draw->length : 0) << "\n";
    if (draw && draw->length > 0) {
        const SDrawCommand &cmd = draw->items[0];
        std::cout << "sprite " << cmd.target_id << " x=" << cmd.x << " y=" << cmd.y
                  << " direction=" << cmd.direction << "\n";
    }

    try {
        SValue jitValue = runSmokeJit();
        std::cout << "jit smoke number: " << jitValue.number << "\n";
    } catch (const std::exception &error) {
        sjit_runtime_destroy(runtime);
        std::cerr << "jit smoke failed: " << error.what() << "\n";
        return 1;
    }

    sjit_runtime_destroy(runtime);
    return 0;
}

int runProjectFile(const char *path, ProjectRunOptions options) {
    SRuntime *runtime = sjit_runtime_create();
    if (!runtime) {
        std::cerr << "failed to create runtime\n";
        return 1;
    }

    if (!configureRuntimeCompatibility(runtime, options)) {
        std::cerr << "failed to configure runtime compatibility\n";
        sjit_runtime_destroy(runtime);
        return 1;
    }

    ProjectLoadResult loaded = loadProjectIntoRuntime(runtime, path ? path : "");
    std::cout << loaded.message << "\n";
    if (!loaded.ok) {
        sjit_runtime_destroy(runtime);
        return 1;
    }
    RuntimeExecution execution = runtimeExecutionFor(loaded);
    std::cout << "runtime execution: "
              << (execution.uses_llvm_runtime ? "LLVM bitcode" : "host C ABI")
              << "\n";
    std::cout << "compatibility: "
              << (sjit_runtime_compatibility_mode(runtime) ==
                          SJIT_COMPATIBILITY_MODE_TURBOWARP ?
                      "turbowarp" : "scratch")
              << " (list limit " << sjit_runtime_list_item_limit(runtime) << ")\n";

    const double step_ms = frameMsForFps(options.fps);
    sjit_runtime_set_turbo_mode(runtime, options.turbo_mode ? 1 : 0);
    sjit_runtime_set_current_step_time(runtime, step_ms);
    sjit_runtime_set_time(runtime, 0.0, 0.0);
    execution.green_flag(runtime);

    const int max_ticks = options.max_frames > 0 ? options.max_frames : 600;
    int ticks = 0;
    SRuntimeStatus last_status = SJIT_STATUS_DONE;
    int current_pen_commands = execution.pen_path_count(runtime);
    int max_pen_commands = current_pen_commands;
    int first_pen_tick = current_pen_commands > 0 ? 0 : -1;
    for (; ticks < max_ticks && execution.has_threads(runtime); ++ticks) {
        const double now = static_cast<double>(ticks) * step_ms;
        sjit_runtime_set_time(runtime, now, step_ms);
        last_status = execution.tick(runtime);
        current_pen_commands = execution.pen_path_count(runtime);
        if (current_pen_commands > max_pen_commands) {
            max_pen_commands = current_pen_commands;
        }
        if (first_pen_tick < 0 && current_pen_commands > 0) {
            first_pen_tick = ticks + 1;
        }
    }
    const int remaining_threads = execution.thread_count(runtime);
    if (remaining_threads > 0) {
        std::cerr << "project still has " << remaining_threads
                  << " running threads after " << max_ticks << " ticks\n";
    }
    std::cout << "ticks: " << ticks << "\n";
    std::cout << "max pen commands: " << max_pen_commands;
    if (first_pen_tick >= 0) {
        std::cout << " first at tick " << first_pen_tick;
    }
    std::cout << "\n";
    printRuntimeSummary(runtime, execution, last_status);

    sjit_runtime_destroy(runtime);
    return 0;
}

int runProjectWindow(const char *path, ProjectRunOptions options) {
    SRuntime *runtime = sjit_runtime_create();
    if (!runtime) {
        std::cerr << "failed to create runtime\n";
        return 1;
    }

    if (!configureRuntimeCompatibility(runtime, options)) {
        std::cerr << "failed to configure runtime compatibility\n";
        sjit_runtime_destroy(runtime);
        return 1;
    }

    ProjectLoadResult loaded = loadProjectIntoRuntime(runtime, path ? path : "");
    std::cout << loaded.message << "\n";
    if (!loaded.ok) {
        sjit_runtime_destroy(runtime);
        return 1;
    }
    RuntimeExecution execution = runtimeExecutionFor(loaded);
    std::cout << "runtime execution: "
              << (execution.uses_llvm_runtime ? "LLVM bitcode" : "host C ABI")
              << "\n";
    std::cout << "compatibility: "
              << (sjit_runtime_compatibility_mode(runtime) ==
                          SJIT_COMPATIBILITY_MODE_TURBOWARP ?
                      "turbowarp" : "scratch")
              << " (list limit " << sjit_runtime_list_item_limit(runtime) << ")\n";
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        sjit_runtime_destroy(runtime);
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    setStageViewportForScale(options.stage_scale);
    const int initial_width = g_stage_viewport.width;
    const int initial_height = g_stage_viewport.height;
    SDL_Window *window = SDL_CreateWindow(
        "xyo-cpp Scratch window",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        initial_width,
        initial_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        sjit_runtime_destroy(runtime);
        return 1;
    }
    SDL_SetWindowMinimumSize(
        window,
        static_cast<int>(std::lround(kStageWidth * kMinimumStageScale)),
        static_cast<int>(std::lround(kStageHeight * kMinimumStageScale)));

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
    }
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        sjit_runtime_destroy(runtime);
        return 1;
    }
    int initial_window_width = 0;
    int initial_window_height = 0;
    SDL_GetWindowSize(window, &initial_window_width, &initial_window_height);
    updateStageViewportForWindow(initial_window_width, initial_window_height);

    SkiaGpuState gpu;
    gpu.main_window = window;
    gpu.main_context = SDL_GL_GetCurrentContext();
    gpu.window = SDL_CreateWindow(
        "xyo-cpp Skia GPU context",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        1,
        1,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (gpu.window) {
        gpu.context = SDL_GL_CreateContext(gpu.window);
    }
    if (gpu.context && SDL_GL_MakeCurrent(gpu.window, gpu.context) == 0) {
        sk_sp<const GrGLInterface> gl_interface =
            GrGLMakeAssembledGLInterface(nullptr, getSdlGlProc);
        if (gl_interface) {
            gpu.context_owner = GrDirectContexts::MakeGL(std::move(gl_interface));
        }
    }
    if (gpu.main_context) {
        SDL_GL_MakeCurrent(gpu.main_window, gpu.main_context);
    }
    if (gpu.context_owner) {
        std::cout << "Skia pen backend: OpenGL GPU\n";
    } else {
        std::cerr << "Skia OpenGL context unavailable; using raster fallback\n";
    }

    SHostInputSnapshot input{};
    const double target_frame_ms = frameMsForFps(options.fps);
    sjit_runtime_set_turbo_mode(runtime, options.turbo_mode ? 1 : 0);
    sjit_runtime_set_current_step_time(runtime, target_frame_ms);
    sjit_runtime_set_time(runtime, 0.0, 0.0);
    execution.green_flag(runtime);

    bool running = true;
    int tick = 0;
    SRuntimeStatus last_status = SJIT_STATUS_DONE;
    PenLayerCache pen_cache;
    SpriteTextureCache sprite_cache;
    HitMaskCache hit_masks;
    MouseInteractionState mouse;
    Uint64 last = SDL_GetPerformanceCounter();
    Uint64 last_title_update = 0;
    while (running && (options.max_frames <= 0 || tick < options.max_frames)) {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window, &window_width, &window_height);
        updateStageViewportForWindow(window_width, window_height);
        SDL_Event event;
        clearKeyEdges(input);
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT &&
                       (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        event.window.event == SDL_WINDOWEVENT_RESIZED)) {
                updateStageViewportForWindow(
                    event.window.data1,
                    event.window.data2);
            } else if (event.type == SDL_KEYDOWN &&
                       event.key.keysym.sym == SDLK_ESCAPE &&
                       sjit_runtime_compatibility_mode(runtime) !=
                           SJIT_COMPATIBILITY_MODE_TURBOWARP) {
                running = false;
            }
            updateInputFromEvent(
                input,
                runtime,
                mouse,
                hit_masks,
                loaded.program.render_targets,
                sjit_runtime_compatibility_mode(runtime) ==
                    SJIT_COMPATIBILITY_MODE_TURBOWARP,
                event);
        }
        const Uint64 now_counter = SDL_GetPerformanceCounter();
        const double delta_ms = static_cast<double>(now_counter - last) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
        last = now_counter;
        const double now_ms = static_cast<double>(SDL_GetTicks64());
        input.now_ms = now_ms;
        input.delta_ms = delta_ms;
        sjit_runtime_set_input(runtime, &input);
        sjit_runtime_set_time(runtime, now_ms, delta_ms);
        last_status = execution.tick(runtime);
        renderRuntime(
            renderer,
            runtime,
            execution,
            &gpu,
            pen_cache,
            sprite_cache,
            &loaded.program.render_targets);
        const Uint64 now_ticks = SDL_GetTicks64();
        if (now_ticks - last_title_update >= 1000) {
            SDL_SetWindowTitle(window, makeWindowTitle(runtime, tick).c_str());
            last_title_update = now_ticks;
        }
        ++tick;

        const Uint64 frame_end = SDL_GetPerformanceCounter();
        const double frame_ms =
            static_cast<double>(frame_end - now_counter) * 1000.0 /
            static_cast<double>(SDL_GetPerformanceFrequency());
        if (frame_ms < target_frame_ms) {
            SDL_Delay(static_cast<Uint32>(std::floor(target_frame_ms - frame_ms)));
        }
    }

    printRuntimeSummary(runtime, execution, last_status);
    destroyPenLayerCache(pen_cache);
    gpu.context_owner.reset();
    if (gpu.context) {
        SDL_GL_DeleteContext(gpu.context);
    }
    if (gpu.window) {
        SDL_DestroyWindow(gpu.window);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    sjit_runtime_destroy(runtime);
    return 0;
}

}
