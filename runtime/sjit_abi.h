#ifndef SJIT_ABI_H
#define SJIT_ABI_H

#include "sjit_config.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SRuntime SRuntime;
typedef struct SProject SProject;
typedef struct STarget STarget;
typedef struct SSprite SSprite;
typedef struct SThread SThread;
typedef struct SFrame SFrame;
typedef struct SList SList;
typedef struct SString SString;
typedef struct SCompiledScript SCompiledScript;

typedef enum {
    SJIT_STATUS_OK = 0,
    SJIT_STATUS_YIELDED = 1,
    SJIT_STATUS_YIELD_TICK = 2,
    SJIT_STATUS_WAITING = 3,
    SJIT_STATUS_DONE = 4,
    SJIT_STATUS_ERROR = 5
} SRuntimeStatus;

typedef enum {
    SJIT_VALUE_NUMBER = 0,
    SJIT_VALUE_BOOL = 1,
    SJIT_VALUE_STRING = 2,
    SJIT_VALUE_LIST = 3,
    SJIT_VALUE_NULL = 4
} SValueTag;

typedef struct {
    int tag;
    double number;
    void *ptr;
} SValue;

typedef struct {
    const void *key;
    int scope_depth;
    double counter;
    int branch_active;
    int sub_pc;
} SLoopState;

typedef enum {
    SJIT_THREAD_RUNNING = 0,
    SJIT_THREAD_PROMISE_WAIT = 1,
    SJIT_THREAD_YIELD = 2,
    SJIT_THREAD_YIELD_TICK = 3,
    SJIT_THREAD_DONE = 4,
    SJIT_THREAD_KILLED = 5
} SThreadStatus;

typedef struct SFrame {
    int pc;
    int return_pc;
    int is_loop;
    int warp_mode;
    int finished;
    double wake_time_ms;
    int loop_counter;
    SValue locals[SJIT_MAX_LOCALS];
    SValue stack[SJIT_MAX_STACK];
    int stack_top;
    SValue params[SJIT_MAX_PARAMS];
    int param_count;
    SLoopState loop_states[SJIT_MAX_LOOP_STATES];
    int loop_state_count;
    int loop_state_cache_index;
    int waiting_child_count;
    int started_thread_begin;
    int started_thread_count;
} SFrame;

typedef SRuntimeStatus (*SScriptEntryFn)(SRuntime *runtime, SThread *thread, SFrame *frame);
typedef SRuntimeStatus (*SProcedureEntryFn)(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame,
    SCompiledScript *script,
    int depth,
    double *numeric_arguments,
    SValue *arguments);

typedef struct {
    double now_ms;
    double delta_ms;
    double mouse_x;
    double mouse_y;
    int mouse_down;
    int key_down[256];
    int key_pressed_edge[256];
    int stage_clicked;
    int sprite_clicked_id;
    /* Latched for one host frame so a down/up pair in the same SDL poll is observable. */
    int mouse_pressed_edge;
} SHostInputSnapshot;

typedef enum {
    SJIT_DRAW_CLEAR = 0,
    SJIT_DRAW_SPRITE = 1,
    SJIT_DRAW_TEXT_BUBBLE = 2,
    SJIT_DRAW_PEN_STROKE = 3,
    SJIT_DRAW_LAYER_CHANGE = 4,
    SJIT_DRAW_PEN_RASTER_TILE = 5
} SDrawCommandKind;

typedef struct {
    int kind;
    int target_id;
    int drawable_id;
    int costume_id;
    double x;
    double y;
    double x2;
    double y2;
    double direction;
    double size;
    double pen_width;
    int r;
    int g;
    int b;
    int a;
    int visible;
    int layer;
} SDrawCommand;

typedef struct {
    SDrawCommand *items;
    int length;
    int capacity;
} SDrawCommandBuffer;

enum {
    SJIT_PEN_RASTER_TILE_WIDTH = 480,
    SJIT_PEN_RASTER_TILE_HEIGHT = 360,
    SJIT_PEN_RASTER_TILE_PIXEL_BYTES =
        SJIT_PEN_RASTER_TILE_WIDTH * SJIT_PEN_RASTER_TILE_HEIGHT * 4,
    SJIT_PEN_RASTER_TILE_MASK_BYTES =
        (SJIT_PEN_RASTER_TILE_WIDTH * SJIT_PEN_RASTER_TILE_HEIGHT + 7) / 8
};

typedef struct {
    unsigned char *pixels;
    unsigned char *active_bits;
    int width;
    int height;
    int stride;
    int rows_filled;
    int command_count;
    int target_id;
    int revision;
    int active;
    double origin_x;
    double origin_y;
    double step;
    double pen_width;
} SPenRasterTile;

typedef struct {
    SDrawCommand *items;
    int length;
    int capacity;
    int revision;
} SPenPathBuffer;

enum {
    SJIT_HAT_EVENT_WHENFLAGCLICKED = 1,
    SJIT_HAT_EVENT_WHENKEYPRESSED = 2,
    SJIT_HAT_EVENT_WHENTHISSPRITECLICKED = 3,
    SJIT_HAT_EVENT_WHENSTAGECLICKED = 4,
    SJIT_HAT_EVENT_WHENBROADCASTRECEIVED = 5,
    SJIT_HAT_EVENT_WHENGREATERTHAN = 6,
    SJIT_HAT_EVENT_WHENTOUCHINGOBJECT = 7,
    SJIT_HAT_CONTROL_START_AS_CLONE = 8,
    SJIT_HAT_EVENT_WHENBACKDROPSWITCHESTO = 9
};

enum {
    SJIT_KEY_UP_ARROW = 128,
    SJIT_KEY_DOWN_ARROW = 129,
    SJIT_KEY_RIGHT_ARROW = 130,
    SJIT_KEY_LEFT_ARROW = 131,
    /* TurboWarp exposes these DOM keyboard names as Scratch keys.  Keep them
       in the normalized input range instead of using browser keyCode values,
       which collide with printable punctuation (Delete is keyCode 46). */
    SJIT_KEY_ENTER = 132,
    SJIT_KEY_BACKSPACE = 133,
    SJIT_KEY_DELETE = 134,
    SJIT_KEY_SHIFT = 135,
    SJIT_KEY_CAPS_LOCK = 136,
    SJIT_KEY_SCROLL_LOCK = 137,
    SJIT_KEY_CONTROL = 138,
    SJIT_KEY_ESCAPE = 139,
    SJIT_KEY_INSERT = 140,
    SJIT_KEY_HOME = 141,
    SJIT_KEY_END = 142,
    SJIT_KEY_PAGE_UP = 143,
    SJIT_KEY_PAGE_DOWN = 144
};

#ifdef __cplusplus
}
#endif

#endif
