#include "sjit/abi.hpp"
#include "sjit/project_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define SJIT_WEB_API extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define SJIT_WEB_API extern "C"
#endif

namespace {

SRuntime *g_runtime = nullptr;
std::unique_ptr<sjit::ProjectProgram> g_program;
std::string g_last_error;
SHostInputSnapshot g_input{};

void setError(const std::string &message) {
    g_last_error = message;
}

void resetWebState() {
    if (g_runtime) {
        sjit_runtime_destroy(g_runtime);
        g_runtime = nullptr;
    }
    g_program.reset();
    g_input = {};
}

SSprite *targetAt(int index) {
    if (!g_runtime || index < 0 || index >= g_runtime->target_count) {
        return nullptr;
    }
    return g_runtime->targets[index];
}

const sjit::TargetRenderInfo *renderTargetForId(int target_id) {
    if (!g_program) {
        return nullptr;
    }
    for (const sjit::TargetRenderInfo &target : g_program->render_targets) {
        if (target.target_id == target_id) {
            return &target;
        }
    }
    if (g_runtime) {
        SSprite *sprite = sjit_runtime_get_sprite(g_runtime, target_id);
        if (sprite && sprite->sprite_id != target_id) {
            for (const sjit::TargetRenderInfo &target : g_program->render_targets) {
                if (target.target_id == sprite->sprite_id) {
                    return &target;
                }
            }
        }
    }
    return nullptr;
}

const sjit::TargetRenderInfo *renderTargetAt(int index) {
    SSprite *sprite = targetAt(index);
    return sprite ? renderTargetForId(sprite->base.id) : nullptr;
}

const sjit::CostumeRenderInfo *costumeAt(int target_index, int costume_index) {
    const sjit::TargetRenderInfo *target = renderTargetAt(target_index);
    if (!target || costume_index < 0 ||
        costume_index >= static_cast<int>(target->costumes.size())) {
        return nullptr;
    }
    return &target->costumes[static_cast<std::size_t>(costume_index)];
}

int hitTestSprite(double x, double y) {
    if (!g_runtime) {
        return 0;
    }
    int hit_id = 0;
    int hit_layer = std::numeric_limits<int>::min();
    for (int i = 0; i < g_runtime->target_count; ++i) {
        SSprite *sprite = g_runtime->targets[i];
        if (!sprite || sprite->base.is_stage || !sprite->visible ||
            sprite->size <= 0.0 ||
            (std::isfinite(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST]) &&
             sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST] >= 100.0)) {
            continue;
        }

        const sjit::CostumeRenderInfo *costume =
            costumeAt(i, sprite->current_costume);
        bool inside = false;
        if (costume && costume->width > 0.0 && costume->height > 0.0) {
            const double scale = sprite->size / 100.0;
            const double dx = x - sprite->x;
            const double dy = -(y - sprite->y);
            const double angle =
                (90.0 - sprite->direction) * 3.14159265358979323846 / 180.0;
            const double local_x =
                (std::cos(angle) * dx + std::sin(angle) * dy) / scale +
                costume->rotation_center_x;
            const double local_y =
                (-std::sin(angle) * dx + std::cos(angle) * dy) / scale +
                costume->rotation_center_y;
            inside = std::isfinite(local_x) && std::isfinite(local_y) &&
                local_x >= 0.0 && local_y >= 0.0 &&
                local_x < costume->width && local_y < costume->height;
        } else {
            const double radius = std::max(8.0, sprite->size / 100.0 * 18.0);
            const double dx = x - sprite->x;
            const double dy = y - sprite->y;
            inside = dx * dx + dy * dy <= radius * radius;
        }
        if (inside && (hit_id == 0 || sprite->layer_order >= hit_layer)) {
            hit_id = sprite->base.id;
            hit_layer = sprite->layer_order;
        }
    }
    return hit_id;
}

bool writeProjectFile(const void *data, int size) {
    if (!data || size <= 0) {
        setError("project data is empty");
        return false;
    }
    FILE *file = std::fopen("/xyo-project.sb3", "wb");
    if (!file) {
        setError("could not open the Emscripten project file");
        return false;
    }
    const std::size_t written = std::fwrite(data, 1, static_cast<std::size_t>(size), file);
    const bool closed = std::fclose(file) == 0;
    if (written != static_cast<std::size_t>(size) || !closed) {
        setError("could not write the Emscripten project file");
        return false;
    }
    return true;
}

void clearInputEdges() {
    std::fill(std::begin(g_input.key_pressed_edge),
              std::end(g_input.key_pressed_edge), 0);
    g_input.stage_clicked = 0;
    g_input.sprite_clicked_id = 0;
    g_input.mouse_pressed_edge = 0;
}

const SDrawCommand *drawCommandAt(int index) {
    if (!g_runtime) {
        return nullptr;
    }
    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(g_runtime);
    return draw && index >= 0 && index < draw->length ? &draw->items[index] : nullptr;
}

const SDrawCommand *penCommandAt(int index) {
    if (!g_runtime || index < 0 || index >= sjit_runtime_pen_path_count(g_runtime)) {
        return nullptr;
    }
    const SDrawCommand *path = sjit_runtime_pen_path_data(g_runtime);
    return path ? &path[index] : nullptr;
}

} // namespace

SJIT_WEB_API int sjit_web_load_project_bytes(const void *data, int size) {
    resetWebState();
    g_last_error.clear();
    if (!writeProjectFile(data, size)) {
        return 0;
    }

    SRuntime *runtime = sjit_runtime_create();
    if (!runtime) {
        setError("failed to create the runtime");
        return 0;
    }
    sjit_runtime_set_compatibility_mode(runtime, SJIT_COMPATIBILITY_MODE_TURBOWARP);
    sjit_runtime_set_ask_input_enabled(runtime, 1);
    sjit_runtime_set_current_step_time(runtime, 1000.0 / 60.0);

    sjit::ProjectLoadResult loaded = sjit::loadProjectIntoRuntime(
        runtime,
        "/xyo-project.sb3");
    if (!loaded.ok) {
        setError(loaded.message.empty() ? "failed to load project" : loaded.message);
        sjit_runtime_destroy(runtime);
        return 0;
    }

    try {
        g_program = std::make_unique<sjit::ProjectProgram>(std::move(loaded.program));
    } catch (...) {
        setError("failed to allocate the loaded project");
        sjit_runtime_destroy(runtime);
        return 0;
    }
    g_runtime = runtime;
    sjit_runtime_green_flag(g_runtime);
    return 1;
}

SJIT_WEB_API const char *sjit_web_last_error() {
    return g_last_error.c_str();
}

SJIT_WEB_API void sjit_web_start() {
    if (g_runtime) {
        sjit_runtime_green_flag(g_runtime);
    }
}

SJIT_WEB_API void sjit_web_stop() {
    if (g_runtime) {
        sjit_runtime_stop_all(g_runtime);
    }
}

SJIT_WEB_API int sjit_web_tick(double now_ms, double delta_ms) {
    if (!g_runtime) {
        return SJIT_STATUS_ERROR;
    }
    g_input.now_ms = now_ms;
    g_input.delta_ms = delta_ms;
    sjit_runtime_set_input(g_runtime, &g_input);
    sjit_runtime_set_time(g_runtime, now_ms, delta_ms);
    const int status = static_cast<int>(sjit_runtime_tick(g_runtime));
    clearInputEdges();
    return status;
}

SJIT_WEB_API void sjit_web_set_mouse(
    double x,
    double y,
    int button_down,
    int pressed_edge) {
    g_input.mouse_x = x;
    g_input.mouse_y = y;
    g_input.mouse_down = button_down ? 1 : 0;
    if (!pressed_edge) {
        return;
    }
    g_input.mouse_pressed_edge = 1;
    const int target_id = hitTestSprite(x, y);
    if (target_id > 0) {
        g_input.sprite_clicked_id = target_id;
    } else {
        g_input.stage_clicked = 1;
    }
}

SJIT_WEB_API void sjit_web_set_key(int key, int down) {
    if (key < 0 || key >= 256) {
        return;
    }
    const int next = down ? 1 : 0;
    if (next && !g_input.key_down[key]) {
        g_input.key_pressed_edge[key] = 1;
    }
    g_input.key_down[key] = next;
}

SJIT_WEB_API void sjit_web_blur() {
    std::fill(std::begin(g_input.key_down), std::end(g_input.key_down), 0);
    g_input.mouse_down = 0;
    clearInputEdges();
}

SJIT_WEB_API int sjit_web_draw_count() {
    if (!g_runtime) {
        return 0;
    }
    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(g_runtime);
    return draw ? draw->length : 0;
}

SJIT_WEB_API int sjit_web_draw_command(int index, double *out) {
    const SDrawCommand *command = drawCommandAt(index);
    if (!command || !out) {
        return 0;
    }
    out[0] = static_cast<double>(command->kind);
    out[1] = static_cast<double>(command->target_id);
    out[2] = static_cast<double>(command->costume_id);
    out[3] = command->x;
    out[4] = command->y;
    out[5] = command->x2;
    out[6] = command->y2;
    out[7] = command->direction;
    out[8] = command->size;
    out[9] = command->pen_width;
    out[10] = static_cast<double>(command->r);
    out[11] = static_cast<double>(command->g);
    out[12] = static_cast<double>(command->b);
    out[13] = static_cast<double>(command->a);
    out[14] = static_cast<double>(command->visible);
    out[15] = static_cast<double>(command->layer);
    return 1;
}

SJIT_WEB_API int sjit_web_pen_count() {
    return g_runtime ? sjit_runtime_pen_path_count(g_runtime) : 0;
}

SJIT_WEB_API int sjit_web_pen_command(int index, double *out) {
    const SDrawCommand *command = penCommandAt(index);
    if (!command || !out) {
        return 0;
    }
    out[0] = static_cast<double>(command->kind);
    out[1] = static_cast<double>(command->target_id);
    out[2] = command->x;
    out[3] = command->y;
    out[4] = command->x2;
    out[5] = command->y2;
    out[6] = command->pen_width;
    out[7] = static_cast<double>(command->r);
    out[8] = static_cast<double>(command->g);
    out[9] = static_cast<double>(command->b);
    out[10] = static_cast<double>(command->a);
    return 1;
}

SJIT_WEB_API int sjit_web_target_count() {
    return g_runtime ? g_runtime->target_count : 0;
}

SJIT_WEB_API int sjit_web_target_id(int index) {
    SSprite *target = targetAt(index);
    return target ? target->base.id : 0;
}

SJIT_WEB_API int sjit_web_target_is_stage(int index) {
    SSprite *target = targetAt(index);
    return target && target->base.is_stage ? 1 : 0;
}

SJIT_WEB_API int sjit_web_render_target_id(int target_id) {
    if (!g_runtime) {
        return target_id;
    }
    SSprite *target = sjit_runtime_get_sprite(g_runtime, target_id);
    return target ? target->sprite_id : target_id;
}

SJIT_WEB_API const char *sjit_web_target_name(int index) {
    SSprite *target = targetAt(index);
    return target ? sjit_string_cstr(target->base.name) : "";
}

SJIT_WEB_API int sjit_web_current_costume(int index) {
    SSprite *target = targetAt(index);
    return target ? target->current_costume : 0;
}

SJIT_WEB_API int sjit_web_costume_count(int target_index) {
    const sjit::TargetRenderInfo *target = renderTargetAt(target_index);
    return target ? static_cast<int>(target->costumes.size()) : 0;
}

SJIT_WEB_API const char *sjit_web_costume_format(int target_index, int costume_index) {
    const sjit::CostumeRenderInfo *costume = costumeAt(target_index, costume_index);
    return costume ? costume->data_format.c_str() : "";
}

SJIT_WEB_API const void *sjit_web_costume_data(int target_index, int costume_index) {
    const sjit::CostumeRenderInfo *costume = costumeAt(target_index, costume_index);
    return costume && !costume->source_data.empty() ? costume->source_data.data() : nullptr;
}

SJIT_WEB_API int sjit_web_costume_data_size(int target_index, int costume_index) {
    const sjit::CostumeRenderInfo *costume = costumeAt(target_index, costume_index);
    return costume && costume->source_data.size() <=
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ?
        static_cast<int>(costume->source_data.size()) : 0;
}

SJIT_WEB_API double sjit_web_costume_width(int target_index, int costume_index) {
    const sjit::CostumeRenderInfo *costume = costumeAt(target_index, costume_index);
    return costume ? costume->width : 0.0;
}

SJIT_WEB_API double sjit_web_costume_height(int target_index, int costume_index) {
    const sjit::CostumeRenderInfo *costume = costumeAt(target_index, costume_index);
    return costume ? costume->height : 0.0;
}

SJIT_WEB_API double sjit_web_costume_rotation_center_x(int target_index, int costume_index) {
    const sjit::CostumeRenderInfo *costume = costumeAt(target_index, costume_index);
    return costume ? costume->rotation_center_x : 0.0;
}

SJIT_WEB_API double sjit_web_costume_rotation_center_y(int target_index, int costume_index) {
    const sjit::CostumeRenderInfo *costume = costumeAt(target_index, costume_index);
    return costume ? costume->rotation_center_y : 0.0;
}

SJIT_WEB_API int sjit_web_answer_pending() {
    return g_runtime && g_runtime->question && !g_runtime->answer_ready ? 1 : 0;
}

SJIT_WEB_API const char *sjit_web_question() {
    return g_runtime && g_runtime->question ?
        sjit_string_cstr(g_runtime->question) : "";
}

SJIT_WEB_API void sjit_web_answer(const char *text) {
    if (!g_runtime) {
        return;
    }
    sjit_runtime_set_answer(g_runtime, sjit_make_string(text ? text : ""));
}

SJIT_WEB_API const char *sjit_web_bubble_text(int index) {
    SSprite *target = targetAt(index);
    return target && target->bubble_text ?
        sjit_string_cstr(target->bubble_text) : "";
}

SJIT_WEB_API int sjit_web_bubble_thought(int index) {
    SSprite *target = targetAt(index);
    return target && target->bubble_text && target->bubble_thought ? 1 : 0;
}
