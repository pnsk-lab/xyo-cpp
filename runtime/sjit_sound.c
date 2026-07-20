#include "sjit_sound.h"

#include "sjit_number.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int ensure_audio_capacity(SRuntime *runtime, int wanted) {
    if (!runtime) {
        return 0;
    }
    if (wanted <= runtime->audio.capacity) {
        return 1;
    }
    int next = runtime->audio.capacity > 0 ? runtime->audio.capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        if (next > INT_MAX / 2) {
            next = wanted;
            break;
        }
        next *= 2;
    }
    SAudioCommand *items = (SAudioCommand *)realloc(
        runtime->audio.items,
        sizeof(SAudioCommand) * (size_t)next);
    if (!items) {
        return 0;
    }
    runtime->audio.items = items;
    runtime->audio.capacity = next;
    return 1;
}

static int append_audio_command(
    SRuntime *runtime,
    int kind,
    SSprite *sprite,
    int effect,
    double value,
    const char *sound_name) {
    if (!runtime || !ensure_audio_capacity(runtime, runtime->audio.length + 1)) {
        return 0;
    }
    SAudioCommand *command = &runtime->audio.items[runtime->audio.length++];
    command->kind = kind;
    command->target_id = sprite ? sprite->base.id : 0;
    command->effect = effect;
    command->value = value;
    command->sound_name = sjit_string_new(sound_name ? sound_name : "");
    if (!command->sound_name) {
        --runtime->audio.length;
        return 0;
    }
    ++runtime->audio.revision;
    return 1;
}

static int ascii_equal(const char *left, const char *right) {
    if (!left || !right) {
        return 0;
    }
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static int sound_effect_id(SValue effect, SRuntime *runtime) {
    SValue text = sjit_to_string(runtime, effect);
    const char *name = sjit_string_cstr((const SString *)text.ptr);
    int id = 0;
    if (ascii_equal(name, "pitch")) {
        id = 1;
    } else if (ascii_equal(name, "pan left/right") || ascii_equal(name, "pan")) {
        id = 2;
    }
    sjit_value_destroy_fast(text);
    return id;
}

static double clamp_sound_effect(int effect, double value) {
    if (!isfinite(value)) {
        return 0.0;
    }
    if (effect == 1) {
        return fmin(360.0, fmax(-360.0, value));
    }
    if (effect == 2) {
        return fmin(100.0, fmax(-100.0, value));
    }
    return value;
}

void sjit_sound_play(SRuntime *runtime, SSprite *sprite, SValue sound, int wait) {
    (void)wait;
    if (!runtime) {
        return;
    }
    SValue text = sjit_to_string(runtime, sound);
    append_audio_command(
        runtime,
        SJIT_AUDIO_PLAY,
        sprite,
        0,
        0.0,
        sjit_string_cstr((const SString *)text.ptr));
    sjit_value_destroy_fast(text);
}

void sjit_sound_stop_all(SRuntime *runtime) {
    append_audio_command(runtime, SJIT_AUDIO_STOP_ALL, NULL, 0, 0.0, "");
}

void sjit_sound_set_effect(
    SRuntime *runtime,
    SSprite *sprite,
    SValue effect,
    double value,
    int change) {
    const int id = sound_effect_id(effect, runtime);
    const double requested = isfinite(value) ? value : 0.0;
    double next = clamp_sound_effect(id, requested);
    if (sprite) {
        if (id == 1) {
            next = clamp_sound_effect(
                id,
                change ? sprite->sound_pitch + requested : requested);
            sprite->sound_pitch = next;
        } else if (id == 2) {
            next = clamp_sound_effect(
                id,
                change ? sprite->sound_pan + requested : requested);
            sprite->sound_pan = next;
        }
    }
    append_audio_command(
        runtime,
        change ? SJIT_AUDIO_CHANGE_EFFECT : SJIT_AUDIO_SET_EFFECT,
        sprite,
        id,
        change ? requested : next,
        "");
}

void sjit_sound_clear_effects(SRuntime *runtime, SSprite *sprite) {
    if (sprite) {
        sprite->sound_pitch = 0.0;
        sprite->sound_pan = 0.0;
    }
    append_audio_command(runtime, SJIT_AUDIO_CLEAR_EFFECTS, sprite, 0, 0.0, "");
}

void sjit_sound_set_volume(
    SRuntime *runtime,
    SSprite *sprite,
    double value,
    int change) {
    if (!sprite) {
        return;
    }
    if (!isfinite(value)) {
        value = 0.0;
    }
    const double next = fmin(100.0, fmax(0.0, change ? sprite->volume + value : value));
    sprite->volume = next;
    append_audio_command(
        runtime,
        change ? SJIT_AUDIO_CHANGE_VOLUME : SJIT_AUDIO_SET_VOLUME,
        sprite,
        0,
        change ? value : next,
        "");
}

double sjit_sound_volume(const SSprite *sprite) {
    return sprite ? sprite->volume : 0.0;
}
