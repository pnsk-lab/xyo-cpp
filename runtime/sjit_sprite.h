#ifndef SJIT_SPRITE_H
#define SJIT_SPRITE_H

#include "sjit_target.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SJIT_GRAPHIC_EFFECT_COLOR = 0,
    SJIT_GRAPHIC_EFFECT_FISHEYE = 1,
    SJIT_GRAPHIC_EFFECT_WHIRL = 2,
    SJIT_GRAPHIC_EFFECT_PIXELATE = 3,
    SJIT_GRAPHIC_EFFECT_MOSAIC = 4,
    SJIT_GRAPHIC_EFFECT_BRIGHTNESS = 5,
    SJIT_GRAPHIC_EFFECT_GHOST = 6,
    SJIT_GRAPHIC_EFFECT_COUNT = 7
} SGraphicEffect;

struct SSprite {
    STarget base;
    int sprite_id;
    int drawable_id;
    double x;
    double y;
    double direction;
    double size;
    int visible;
    int current_costume;
    int rotation_style;
    int draggable;
    double volume;
    int layer_order;
    int pen_down;
    double pen_size;
    int pen_r;
    int pen_g;
    int pen_b;
    int pen_a;
    double pen_hue;
    double pen_saturation;
    double pen_brightness;
    double pen_transparency;
    SString **costume_names;
    int costume_count;
    double graphic_effects[SJIT_GRAPHIC_EFFECT_COUNT];
};

SSprite *sjit_sprite_create(int id, int drawable_id, const char *name, int is_stage);
SSprite *sjit_sprite_clone(const SSprite *source, int id, int drawable_id);
void sjit_sprite_destroy(SSprite *sprite);
void sjit_sprite_set_xy(SRuntime *runtime, SSprite *sprite, double x, double y, int force);
void sjit_sprite_set_direction(SRuntime *runtime, SSprite *sprite, double direction);
void sjit_sprite_set_visible(SRuntime *runtime, SSprite *sprite, int visible);
void sjit_sprite_set_draggable(SSprite *sprite, int draggable);
int sjit_sprite_set_costume_names(
    SSprite *sprite,
    const char *const *costume_names,
    int costume_count);
int sjit_sprite_costume_index_by_name(const SSprite *sprite, const char *name);
const char *sjit_sprite_current_costume_name(const SSprite *sprite);
void sjit_sprite_set_costume(SRuntime *runtime, SSprite *sprite, int costume_index);
void sjit_sprite_set_costume_number(SRuntime *runtime, SSprite *sprite, double costume_index);

#ifdef __cplusplus
}
#endif

#endif
