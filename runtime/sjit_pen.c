#include "sjit_pen.h"

#include "sjit_draw.h"
#include "sjit_number.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int ensure_pen_path_capacity(SPenPathBuffer *path, int wanted) {
    if (!path) {
        return 0;
    }
    if (wanted <= path->capacity) {
        return 1;
    }
    int next = path->capacity > 0 ? path->capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        next *= 2;
    }
    SDrawCommand *items = (SDrawCommand *)realloc(path->items, sizeof(SDrawCommand) * (size_t)next);
    if (!items) {
        return 0;
    }
    path->items = items;
    path->capacity = next;
    return 1;
}

void sjit_pen_path_init(SPenPathBuffer *path) {
    if (!path) {
        return;
    }
    path->items = NULL;
    path->length = 0;
    path->capacity = 0;
    path->revision = 0;
}

void sjit_pen_path_destroy(SPenPathBuffer *path) {
    if (!path) {
        return;
    }
    free(path->items);
    sjit_pen_path_init(path);
}

void sjit_pen_path_clear(SPenPathBuffer *path) {
    if (!path) {
        return;
    }
    path->length = 0;
    ++path->revision;
}

int sjit_pen_path_push(
    SPenPathBuffer *path,
    int target_id,
    double x,
    double y,
    double x2,
    double y2,
    double width,
    int r,
    int g,
    int b,
    int a) {
    if (!path || !ensure_pen_path_capacity(path, path->length + 1)) {
        return 0;
    }
    SDrawCommand command = {0};
    command.kind = SJIT_DRAW_PEN_STROKE;
    command.target_id = target_id;
    command.x = x;
    command.y = y;
    command.x2 = x2;
    command.y2 = y2;
    command.pen_width = width;
    command.r = r;
    command.g = g;
    command.b = b;
    command.a = a;
    command.visible = 1;
    path->items[path->length++] = command;
    return 1;
}

int sjit_pen_path_append_draw(SDrawCommandBuffer *draw, const SPenPathBuffer *path) {
    if (!draw || !path) {
        return 0;
    }
    const SDrawCommandBuffer source = {path->items, path->length, path->capacity};
    return sjit_draw_append(draw, &source);
}

static int clamp_channel(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return value;
}

static double clamp_percent(double value) {
    if (!isfinite(value) || value < 0.0) {
        return 0.0;
    }
    if (value > 100.0) {
        return 100.0;
    }
    return value;
}

static double wrap_hue(double hue) {
    if (!isfinite(hue)) {
        return 0.0;
    }
    hue = fmod(hue, 100.0);
    if (hue < 0.0) {
        hue += 100.0;
    }
    return hue;
}

static void rgb_to_hsv_percent(int r, int g, int b, double *h, double *s, double *v);

enum {
    SJIT_PEN_COLOR_CACHE_BITS = 10,
    SJIT_PEN_COLOR_CACHE_SIZE = 1 << SJIT_PEN_COLOR_CACHE_BITS
};

typedef struct {
    uint32_t key;
    uint32_t valid;
    double hue;
    double saturation;
    double brightness;
    double saturation_fraction;
    double x_fraction;
    int hue_sector;
} SPenColorCacheEntry;

typedef struct {
    SPenColorCacheEntry entries[SJIT_PEN_COLOR_CACHE_SIZE];
} SPenColorCache;

static void prepare_cached_hsv_geometry(SPenColorCacheEntry *entry) {
    const double hue = wrap_hue(entry->hue) * 360.0 / 100.0;
    const double hp = hue / 60.0;
    entry->saturation_fraction = clamp_percent(entry->saturation) / 100.0;
    entry->x_fraction = 1.0 - fabs(fmod(hp, 2.0) - 1.0);
    if (hp < 1.0) {
        entry->hue_sector = 0;
    } else if (hp < 2.0) {
        entry->hue_sector = 1;
    } else if (hp < 3.0) {
        entry->hue_sector = 2;
    } else if (hp < 4.0) {
        entry->hue_sector = 3;
    } else if (hp < 5.0) {
        entry->hue_sector = 4;
    } else {
        entry->hue_sector = 5;
    }
}

static __attribute__((noinline, cold)) SPenColorCacheEntry *
pen_color_cache_entry_slow(
    SRuntime *runtime,
    int r,
    int g,
    int b,
    SPenColorCacheEntry *fallback) {
    SPenColorCache *cache = runtime ? (SPenColorCache *)runtime->pen_color_cache : NULL;
    if (!cache && runtime) {
        cache = (SPenColorCache *)calloc(1, sizeof(SPenColorCache));
        runtime->pen_color_cache = cache;
    }
    const uint32_t key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    SPenColorCacheEntry *entry = cache ? &cache->entries[
        (key * UINT32_C(2654435761)) >> (32 - SJIT_PEN_COLOR_CACHE_BITS)] :
        fallback;
    rgb_to_hsv_percent(r, g, b, &entry->hue, &entry->saturation, &entry->brightness);
    prepare_cached_hsv_geometry(entry);
    entry->key = key;
    entry->valid = 1;
    return entry;
}

static inline __attribute__((always_inline)) SPenColorCacheEntry *
pen_color_cache_entry(
    SRuntime *runtime,
    int r,
    int g,
    int b,
    SPenColorCacheEntry *fallback) {
    SPenColorCache *cache = runtime ? (SPenColorCache *)runtime->pen_color_cache : NULL;
    if (!cache) {
        return pen_color_cache_entry_slow(runtime, r, g, b, fallback);
    }

    const uint32_t key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    SPenColorCacheEntry *entry = &cache->entries[
        (key * UINT32_C(2654435761)) >> (32 - SJIT_PEN_COLOR_CACHE_BITS)];
    if (!entry->valid || entry->key != key) {
        return pen_color_cache_entry_slow(runtime, r, g, b, fallback);
    }
    return entry;
}

static inline __attribute__((always_inline)) void rgb_to_hsv_percent_cached(
    SRuntime *runtime,
    int r,
    int g,
    int b,
    double *h,
    double *s,
    double *v) {
    SPenColorCacheEntry fallback = {0};
    const SPenColorCacheEntry *entry = pen_color_cache_entry(
        runtime, r, g, b, &fallback);
    *h = entry->hue;
    *s = entry->saturation;
    *v = entry->brightness;
}

static int hex_digit_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

static int parse_hex_color(const char *text, int *r, int *g, int *b) {
    if (!text || text[0] != '#') {
        return 0;
    }
    if (text[1] && text[2] && text[3] && text[4] == '\0') {
        const int r1 = hex_digit_value(text[1]);
        const int g1 = hex_digit_value(text[2]);
        const int b1 = hex_digit_value(text[3]);
        if (r1 < 0 || g1 < 0 || b1 < 0) {
            return 0;
        }
        *r = (r1 << 4) | r1;
        *g = (g1 << 4) | g1;
        *b = (b1 << 4) | b1;
        return 1;
    }
    if (text[1] && text[2] && text[3] && text[4] && text[5] && text[6] && text[7] == '\0') {
        const int r1 = hex_digit_value(text[1]);
        const int r2 = hex_digit_value(text[2]);
        const int g1 = hex_digit_value(text[3]);
        const int g2 = hex_digit_value(text[4]);
        const int b1 = hex_digit_value(text[5]);
        const int b2 = hex_digit_value(text[6]);
        if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) {
            return 0;
        }
        *r = (r1 << 4) | r2;
        *g = (g1 << 4) | g2;
        *b = (b1 << 4) | b2;
        return 1;
    }
    return 0;
}

static uint32_t to_js_uint32(double number) {
    if (!isfinite(number) || number == 0.0) {
        return 0u;
    }
    double bits = fmod(trunc(number), 4294967296.0);
    if (bits < 0.0) {
        bits += 4294967296.0;
    }
    return (uint32_t)bits;
}

static void set_color_rgba(
    SRuntime *runtime,
    SSprite *sprite,
    int r,
    int g,
    int b,
    int a) {
    if (!sprite) {
        return;
    }
    sprite->pen_r = clamp_channel(r);
    sprite->pen_g = clamp_channel(g);
    sprite->pen_b = clamp_channel(b);
    sprite->pen_a = clamp_channel(a);
    sprite->pen_transparency = 100.0 - ((double)sprite->pen_a * 100.0 / 255.0);
    rgb_to_hsv_percent_cached(
        runtime,
        sprite->pen_r,
        sprite->pen_g,
        sprite->pen_b,
        &sprite->pen_hue,
        &sprite->pen_saturation,
        &sprite->pen_brightness);
}

static void rgb_to_hsv_percent(int r, int g, int b, double *h, double *s, double *v) {
    const double rd = (double)clamp_channel(r) / 255.0;
    const double gd = (double)clamp_channel(g) / 255.0;
    const double bd = (double)clamp_channel(b) / 255.0;
    const double max = fmax(rd, fmax(gd, bd));
    const double min = fmin(rd, fmin(gd, bd));
    const double delta = max - min;
    double hue = 0.0;
    if (delta > 0.0) {
        if (max == rd) {
            hue = fmod(((gd - bd) / delta), 6.0);
        } else if (max == gd) {
            hue = ((bd - rd) / delta) + 2.0;
        } else {
            hue = ((rd - gd) / delta) + 4.0;
        }
        hue *= 60.0;
        if (hue < 0.0) {
            hue += 360.0;
        }
    }
    if (h) {
        *h = hue * 100.0 / 360.0;
    }
    if (s) {
        *s = max <= 0.0 ? 0.0 : (delta / max) * 100.0;
    }
    if (v) {
        *v = max * 100.0;
    }
}

static void apply_pen_color_state(SSprite *sprite) {
    if (!sprite) {
        return;
    }
    const double hue = wrap_hue(sprite->pen_hue) * 360.0 / 100.0;
    const double saturation = clamp_percent(sprite->pen_saturation) / 100.0;
    const double value = clamp_percent(sprite->pen_brightness) / 100.0;
    const double chroma = value * saturation;
    const double hp = hue / 60.0;
    const double x = chroma * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double r1 = 0.0;
    double g1 = 0.0;
    double b1 = 0.0;
    if (hp < 1.0) {
        r1 = chroma;
        g1 = x;
    } else if (hp < 2.0) {
        r1 = x;
        g1 = chroma;
    } else if (hp < 3.0) {
        g1 = chroma;
        b1 = x;
    } else if (hp < 4.0) {
        g1 = x;
        b1 = chroma;
    } else if (hp < 5.0) {
        r1 = x;
        b1 = chroma;
    } else {
        r1 = chroma;
        b1 = x;
    }
    const double m = value - chroma;
    /* Scratch's hsvToRgb implementation uses Math.floor for each channel,
       rather than rounding to nearest. This matters for pen brightness
       changes, where a channel can differ by one between the two rules. */
    sprite->pen_r = clamp_channel((int)floor((r1 + m) * 255.0));
    sprite->pen_g = clamp_channel((int)floor((g1 + m) * 255.0));
    sprite->pen_b = clamp_channel((int)floor((b1 + m) * 255.0));
    sprite->pen_a = clamp_channel((int)lround((100.0 - clamp_percent(sprite->pen_transparency)) * 2.55));
}

static inline __attribute__((always_inline)) void apply_cached_pen_color_state(
    SSprite *sprite,
    const SPenColorCacheEntry *entry) {
    const double value = clamp_percent(sprite->pen_brightness) / 100.0;
    const double chroma = value * entry->saturation_fraction;
    const double x = chroma * entry->x_fraction;
    double r1 = 0.0;
    double g1 = 0.0;
    double b1 = 0.0;
    switch (entry->hue_sector) {
        case 0:
            r1 = chroma;
            g1 = x;
            break;
        case 1:
            r1 = x;
            g1 = chroma;
            break;
        case 2:
            g1 = chroma;
            b1 = x;
            break;
        case 3:
            g1 = x;
            b1 = chroma;
            break;
        case 4:
            r1 = x;
            b1 = chroma;
            break;
        default:
            r1 = chroma;
            b1 = x;
            break;
    }
    const double m = value - chroma;
    sprite->pen_r = clamp_channel((int)floor((r1 + m) * 255.0));
    sprite->pen_g = clamp_channel((int)floor((g1 + m) * 255.0));
    sprite->pen_b = clamp_channel((int)floor((b1 + m) * 255.0));
    sprite->pen_a = clamp_channel((int)lround(
        (100.0 - clamp_percent(sprite->pen_transparency)) * 2.55));
}

void sjit_pen_clear(SRuntime *runtime) {
    if (!runtime) {
        return;
    }
    sjit_pen_path_clear(&runtime->pen);
    runtime->pen_raster_tile.width = 0;
    runtime->pen_raster_tile.height = 0;
    runtime->pen_raster_tile.stride = 0;
    runtime->pen_raster_tile.rows_filled = 0;
    runtime->pen_raster_tile.command_count = 0;
    runtime->pen_raster_tile.target_id = 0;
    runtime->pen_raster_tile.active = 0;
    runtime->pen_materialized_valid = 0;
    sjit_runtime_request_redraw(runtime);
}

static void append_pen_stamp(SRuntime *runtime, const SSprite *sprite) {
    if (!runtime || !sprite) {
        return;
    }
    sjit_pen_path_push(
        &runtime->pen,
        sprite->base.id,
        sprite->x,
        sprite->y,
        sprite->x,
        sprite->y,
        sprite->pen_size,
        sprite->pen_r,
        sprite->pen_g,
        sprite->pen_b,
        sprite->pen_a);
    runtime->pen_materialized_valid = 0;
    /* Pen stamps already imply a redraw; avoid an out-of-TU call for every
       rasterized pixel. */
    runtime->redraw_requested = 1;
}

void sjit_pen_down(SRuntime *runtime, SSprite *sprite) {
    if (!sprite) {
        return;
    }
    sprite->pen_down = 1;
    append_pen_stamp(runtime, sprite);
}

void sjit_pen_up(SRuntime *runtime, SSprite *sprite) {
    (void)runtime;
    if (sprite) {
        sprite->pen_down = 0;
    }
}

void sjit_pen_stamp(SRuntime *runtime, SSprite *sprite) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_PEN_STAMP);
    if (!sprite) {
        return;
    }
    append_pen_stamp(runtime, sprite);
    sprite->pen_down = 0;
}

void sjit_pen_set_size(SRuntime *runtime, SSprite *sprite, SValue size) {
    (void)runtime;
    if (!sprite) {
        return;
    }
    double next = sjit_to_number_fast(runtime, size);
    if (next < 1.0) {
        next = 1.0;
    }
    sprite->pen_size = next;
}

void sjit_pen_set_color_rgb(SRuntime *runtime, SSprite *sprite, int r, int g, int b) {
    set_color_rgba(runtime, sprite, r, g, b, 255);
}

static inline __attribute__((always_inline)) void pen_set_color_value_impl(
    SRuntime *runtime,
    SSprite *sprite,
    SValue value) {
    if (!sprite) {
        return;
    }
    if (value.tag == SJIT_VALUE_STRING && value.ptr) {
        const char *raw = sjit_string_cstr((const SString *)value.ptr);
        if (raw && raw[0] == '#') {
            int r = 0;
            int g = 0;
            int b = 0;
            if (parse_hex_color(raw, &r, &g, &b)) {
                set_color_rgba(runtime, sprite, r, g, b, 255);
            } else {
                set_color_rgba(runtime, sprite, 0, 0, 0, 255);
            }
            return;
        }
    }

    const uint32_t bits = to_js_uint32(sjit_to_number_fast(runtime, value));
    const int a = (int)((bits >> 24) & 0xffu);
    set_color_rgba(
        runtime,
        sprite,
        (int)((bits >> 16) & 0xffu),
        (int)((bits >> 8) & 0xffu),
        (int)(bits & 0xffu),
        a > 0 ? a : 255);
}

void sjit_pen_set_color_value(SRuntime *runtime, SSprite *sprite, SValue value) {
    pen_set_color_value_impl(runtime, sprite, value);
}

int sjit_pen_color_param_id(const char *param) {
    if (sjit_cstr_equals_ignore_case(param, "color")) {
        return 1;
    }
    if (sjit_cstr_equals_ignore_case(param, "saturation")) {
        return 2;
    }
    if (sjit_cstr_equals_ignore_case(param, "brightness")) {
        return 3;
    }
    if (sjit_cstr_equals_ignore_case(param, "transparency")) {
        return 4;
    }
    return 0;
}

static inline __attribute__((always_inline)) void pen_change_color_param_number_impl(
    SRuntime *runtime,
    SSprite *sprite,
    int param_id,
    double delta) {
    (void)runtime;
    if (!sprite) {
        return;
    }
    if (param_id == 1) {
        sprite->pen_hue = wrap_hue(sprite->pen_hue + delta);
    } else if (param_id == 2) {
        sprite->pen_saturation = clamp_percent(sprite->pen_saturation + delta);
    } else if (param_id == 3) {
        sprite->pen_brightness = clamp_percent(sprite->pen_brightness + delta);
    } else if (param_id == 4) {
        sprite->pen_transparency = clamp_percent(sprite->pen_transparency + delta);
    } else {
        return;
    }
    apply_pen_color_state(sprite);
}

void sjit_pen_change_color_param_number(SRuntime *runtime, SSprite *sprite, int param_id, double delta) {
    pen_change_color_param_number_impl(runtime, sprite, param_id, delta);
}

void sjit_pen_set_color_param_number(
    SRuntime *runtime,
    SSprite *sprite,
    int param_id,
    double value) {
    (void)runtime;
    if (!sprite || !isfinite(value)) {
        return;
    }
    if (param_id == 1) {
        sprite->pen_hue = wrap_hue(value);
    } else if (param_id == 2) {
        sprite->pen_saturation = clamp_percent(value);
    } else if (param_id == 3) {
        sprite->pen_brightness = clamp_percent(value);
    } else if (param_id == 4) {
        sprite->pen_transparency = clamp_percent(value);
    } else {
        return;
    }
    apply_pen_color_state(sprite);
}

void sjit_pen_change_size(SRuntime *runtime, SSprite *sprite, double delta) {
    if (!sprite || !isfinite(delta)) {
        return;
    }
    sjit_pen_set_size(runtime, sprite, sjit_make_number_fast(sprite->pen_size + delta));
}

static void legacy_update_pen_color(SRuntime *runtime, SSprite *sprite) {
    if (!sprite) {
        return;
    }
    const int alpha = sprite->pen_a;
    sprite->pen_saturation = 100.0;
    sprite->pen_brightness = 100.0;
    apply_pen_color_state(sprite);

    double shade = isfinite(sprite->pen_legacy_shade) ?
        fmod(sprite->pen_legacy_shade, 200.0) : 0.0;
    if (shade < 0.0) {
        shade += 200.0;
    }
    double r = (double)sprite->pen_r;
    double g = (double)sprite->pen_g;
    double b = (double)sprite->pen_b;
    if (shade < 50.0) {
        const double fraction = (10.0 + shade) / 60.0;
        r *= fraction;
        g *= fraction;
        b *= fraction;
    } else {
        const double fraction = (shade - 50.0) / 60.0;
        r += (255.0 - r) * fraction;
        g += (255.0 - g) * fraction;
        b += (255.0 - b) * fraction;
    }
    set_color_rgba(
        runtime,
        sprite,
        (int)r,
        (int)g,
        (int)b,
        alpha);
}

void sjit_pen_set_hue_number(SRuntime *runtime, SSprite *sprite, double hue) {
    if (!sprite || !isfinite(hue)) {
        return;
    }
    sprite->pen_hue = wrap_hue(hue / 2.0);
    sprite->pen_transparency = 0.0;
    legacy_update_pen_color(runtime, sprite);
}

void sjit_pen_change_hue_number(SRuntime *runtime, SSprite *sprite, double delta) {
    if (!sprite || !isfinite(delta)) {
        return;
    }
    sprite->pen_hue = wrap_hue(sprite->pen_hue + delta / 2.0);
    legacy_update_pen_color(runtime, sprite);
}

void sjit_pen_set_shade_number(SRuntime *runtime, SSprite *sprite, double shade) {
    if (!sprite || !isfinite(shade)) {
        return;
    }
    sprite->pen_legacy_shade = fmod(shade, 200.0);
    if (sprite->pen_legacy_shade < 0.0) {
        sprite->pen_legacy_shade += 200.0;
    }
    legacy_update_pen_color(runtime, sprite);
}

void sjit_pen_change_shade_number(SRuntime *runtime, SSprite *sprite, double delta) {
    if (!sprite || !isfinite(delta)) {
        return;
    }
    sjit_pen_set_shade_number(runtime, sprite, sprite->pen_legacy_shade + delta);
}

void sjit_pen_set_number_color_and_change_brightness(
    SRuntime *runtime,
    SSprite *sprite,
    double color,
    double delta) {
    if (!sprite) {
        return;
    }
    const uint32_t bits = to_js_uint32(color);
    const int r = (int)((bits >> 16) & 0xffu);
    const int g = (int)((bits >> 8) & 0xffu);
    const int b = (int)(bits & 0xffu);
    const int packed_alpha = (int)((bits >> 24) & 0xffu);
    const int a = packed_alpha > 0 ? packed_alpha : 255;
    SPenColorCacheEntry fallback = {0};
    const SPenColorCacheEntry *entry = pen_color_cache_entry(
        runtime, r, g, b, &fallback);

    sprite->pen_hue = entry->hue;
    sprite->pen_saturation = entry->saturation;
    sprite->pen_brightness = clamp_percent(entry->brightness + delta);
    sprite->pen_transparency = 100.0 - ((double)a * 100.0 / 255.0);
    apply_cached_pen_color_state(sprite, entry);
}

void sjit_pen_set_color_value_and_change_param_number(
    SRuntime *runtime,
    SSprite *sprite,
    SValue value,
    int param_id,
    double delta) {
    if (param_id == 3 && value.tag == SJIT_VALUE_NUMBER) {
        sjit_pen_set_number_color_and_change_brightness(
            runtime, sprite, value.number, delta);
        return;
    }
    pen_set_color_value_impl(runtime, sprite, value);
    pen_change_color_param_number_impl(runtime, sprite, param_id, delta);
}

void sjit_pen_change_color_param(SRuntime *runtime, SSprite *sprite, const char *param, SValue delta) {
    sjit_pen_change_color_param_number(
        runtime,
        sprite,
        sjit_pen_color_param_id(param),
        sjit_to_number_fast(runtime, delta));
}

void sjit_pen_change_brightness(SRuntime *runtime, SSprite *sprite, SValue delta) {
    sjit_pen_change_color_param(runtime, sprite, "brightness", delta);
}
