#ifndef SJIT_SCRIPT_H
#define SJIT_SCRIPT_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SJIT_EXPR_LITERAL = 0,
    SJIT_EXPR_TIMER = 1,
    SJIT_EXPR_LT = 2,
    SJIT_EXPR_ADD = 3,
    SJIT_EXPR_SUB = 4,
    SJIT_EXPR_MUL = 5,
    SJIT_EXPR_DIV = 6,
    SJIT_EXPR_MATHOP = 7,
    SJIT_EXPR_VARIABLE = 8,
    SJIT_EXPR_EQ = 9,
    SJIT_EXPR_GT = 10,
    SJIT_EXPR_AND = 11,
    SJIT_EXPR_OR = 12,
    SJIT_EXPR_NOT = 13,
    SJIT_EXPR_LIST_ITEM = 14,
    SJIT_EXPR_LIST_ITEM_NUMBER = 15,
    SJIT_EXPR_LIST_LENGTH = 16,
    SJIT_EXPR_LIST_CONTAINS = 17,
    SJIT_EXPR_MOUSE_X = 18,
    SJIT_EXPR_MOUSE_Y = 19,
    SJIT_EXPR_MOUSE_DOWN = 20,
    SJIT_EXPR_ARGUMENT = 21,
    SJIT_EXPR_MOD = 22,
    SJIT_EXPR_ROUND = 23,
    SJIT_EXPR_JOIN = 24,
    SJIT_EXPR_LENGTH = 25,
    SJIT_EXPR_LETTER_OF = 26,
    SJIT_EXPR_RANDOM = 27,
    SJIT_EXPR_KEY_PRESSED = 28,
    SJIT_EXPR_DAYS_SINCE_2000 = 29,
    SJIT_EXPR_X_POSITION = 30,
    SJIT_EXPR_Y_POSITION = 31,
    SJIT_EXPR_CONTAINS = 32,
    SJIT_EXPR_LIST_VARIABLE = 33,
    /* Implemented by the interpreter and intentionally used to exercise the
       explicit whole-script JIT fallback path. */
    SJIT_EXPR_DIRECTION = 34,
    SJIT_EXPR_COSTUME_NUMBER_NAME = 35,
    SJIT_EXPR_SIZE = 36,
    SJIT_EXPR_BACKDROP_NUMBER_NAME = 37,
    SJIT_EXPR_LIST_CONTENTS = 38,
    SJIT_EXPR_TOUCHING_OBJECT = 39,
    SJIT_EXPR_TOUCHING_COLOR = 40,
    SJIT_EXPR_COLOR_TOUCHING_COLOR = 41,
    SJIT_EXPR_DISTANCE_TO = 42,
    SJIT_EXPR_SENSING_OF = 43,
    SJIT_EXPR_CURRENT = 44,
    SJIT_EXPR_ANSWER = 45,
    SJIT_EXPR_LOUDNESS = 46,
    SJIT_EXPR_LOUD = 47,
    SJIT_EXPR_ONLINE = 48,
    SJIT_EXPR_USERNAME = 49,
    SJIT_EXPR_SOUND_VOLUME = 50,
    SJIT_EXPR_COUNTER = 51
} SExprOpcode;

typedef enum {
    SJIT_STMT_NOOP = 0,
    SJIT_STMT_RESET_TIMER = 1,
    SJIT_STMT_SET_VARIABLE = 2,
    SJIT_STMT_CHANGE_VARIABLE = 3,
    SJIT_STMT_REPEAT = 4,
    SJIT_STMT_IF = 5,
    SJIT_STMT_SAY = 6,
    SJIT_STMT_PEN_CLEAR = 7,
    SJIT_STMT_PEN_DOWN = 8,
    SJIT_STMT_PEN_UP = 9,
    SJIT_STMT_PEN_SET_SIZE = 10,
    SJIT_STMT_PEN_SET_COLOR = 11,
    SJIT_STMT_PEN_CHANGE_COLOR_PARAM = 12,
    SJIT_STMT_PEN_CHANGE_BRIGHTNESS = 12,
    SJIT_STMT_MOTION_SET_X = 13,
    SJIT_STMT_MOTION_SET_Y = 14,
    SJIT_STMT_MOTION_CHANGE_X = 15,
    SJIT_STMT_MOTION_CHANGE_Y = 16,
    SJIT_STMT_LOOKS_SET_SIZE = 17,
    SJIT_STMT_LIST_ADD = 18,
    SJIT_STMT_LIST_DELETE = 19,
    SJIT_STMT_LIST_DELETE_ALL = 20,
    SJIT_STMT_LIST_INSERT = 21,
    SJIT_STMT_LIST_REPLACE = 22,
    SJIT_STMT_IF_ELSE = 23,
    SJIT_STMT_FOREVER = 24,
    SJIT_STMT_FOR_EACH = 25,
    SJIT_STMT_PROCEDURE_CALL = 26,
    SJIT_STMT_MOTION_GOTO_XY = 27,
    SJIT_STMT_LOOKS_SHOW = 28,
    SJIT_STMT_LOOKS_HIDE = 29,
    SJIT_STMT_REPEAT_UNTIL = 30,
    SJIT_STMT_STOP_THIS_SCRIPT = 31,
    SJIT_STMT_WHILE = 32,
    SJIT_STMT_BROADCAST = 33,
    SJIT_STMT_WAIT = 34,
    SJIT_STMT_WAIT_UNTIL = 35,
    SJIT_STMT_STOP_OTHER_SCRIPTS = 36,
    SJIT_STMT_LOOKS_SAY_FOR_SECS = 37,
    SJIT_STMT_MONITOR_SHOW = 38,
    SJIT_STMT_MONITOR_HIDE = 39,
    SJIT_STMT_STOP_ALL = 40,
    SJIT_STMT_LOOKS_SWITCH_BACKDROP = 41,
    SJIT_STMT_LOOKS_SET_EFFECT = 42,
    SJIT_STMT_LOOKS_CHANGE_EFFECT = 43,
    SJIT_STMT_LOOKS_CLEAR_EFFECTS = 44,
    SJIT_STMT_SENSING_SET_DRAG_MODE = 45,
    SJIT_STMT_LOOKS_SWITCH_COSTUME = 46,
    SJIT_STMT_LOOKS_GO_TO_FRONT_BACK = 47,
    SJIT_STMT_PEN_STAMP = 48,
    SJIT_STMT_BROADCAST_AND_WAIT = 49,
    SJIT_STMT_MOTION_MOVE_STEPS = 50,
    SJIT_STMT_MOTION_GOTO = 51,
    SJIT_STMT_MOTION_TURN_RIGHT = 52,
    SJIT_STMT_MOTION_TURN_LEFT = 53,
    SJIT_STMT_MOTION_POINT_DIRECTION = 54,
    SJIT_STMT_MOTION_POINT_TOWARDS = 55,
    SJIT_STMT_MOTION_GLIDE_XY = 56,
    SJIT_STMT_MOTION_GLIDE_TO = 57,
    SJIT_STMT_MOTION_IF_ON_EDGE_BOUNCE = 58,
    SJIT_STMT_MOTION_SET_ROTATION_STYLE = 59,
    SJIT_STMT_LOOKS_THINK = 60,
    SJIT_STMT_LOOKS_THINK_FOR_SECS = 61,
    SJIT_STMT_LOOKS_SWITCH_BACKDROP_AND_WAIT = 62,
    SJIT_STMT_LOOKS_NEXT_COSTUME = 63,
    SJIT_STMT_LOOKS_NEXT_BACKDROP = 64,
    SJIT_STMT_LOOKS_CHANGE_SIZE = 65,
    SJIT_STMT_LOOKS_HIDE_ALL_SPRITES = 66,
    SJIT_STMT_LOOKS_GO_FORWARD_BACKWARD_LAYERS = 67,
    SJIT_STMT_LOOKS_CHANGE_STRETCH = 68,
    SJIT_STMT_LOOKS_SET_STRETCH = 69,
    SJIT_STMT_CREATE_CLONE = 70,
    SJIT_STMT_DELETE_CLONE = 71,
    SJIT_STMT_CONTROL_GET_COUNTER = 72,
    SJIT_STMT_CONTROL_INCR_COUNTER = 73,
    SJIT_STMT_CONTROL_CLEAR_COUNTER = 74,
    SJIT_STMT_CONTROL_ALL_AT_ONCE = 75,
    SJIT_STMT_SENSING_ASK_AND_WAIT = 76,
    SJIT_STMT_SOUND_PLAY = 77,
    SJIT_STMT_SOUND_PLAY_UNTIL_DONE = 78,
    SJIT_STMT_SOUND_STOP_ALL = 79,
    SJIT_STMT_SOUND_SET_EFFECT = 80,
    SJIT_STMT_SOUND_CHANGE_EFFECT = 81,
    SJIT_STMT_SOUND_CLEAR_EFFECTS = 82,
    SJIT_STMT_SOUND_SET_VOLUME = 83,
    SJIT_STMT_SOUND_CHANGE_VOLUME = 84,
    SJIT_STMT_STOP_OTHER_SCRIPTS_IN_STAGE = 85,
    SJIT_STMT_PEN_SET_COLOR_PARAM = 86,
    SJIT_STMT_PEN_CHANGE_SIZE = 87,
    SJIT_STMT_PEN_SET_SHADE = 88,
    SJIT_STMT_PEN_CHANGE_SHADE = 89,
    SJIT_STMT_PEN_SET_HUE = 90,
    SJIT_STMT_PEN_CHANGE_HUE = 91
} SStatementOpcode;

typedef enum {
    SJIT_STMT_EXPR_VALUE = 0,
    SJIT_STMT_EXPR_INDEX = 1,
    SJIT_STMT_EXPR_CONDITION = 2,
    SJIT_STMT_EXPR_TIMES = 3
} SStatementExprSlot;

typedef struct SExpr {
    int opcode;
    SValue literal;
    struct SExpr *left;
    struct SExpr *right;
    int number_cache_valid;
    double number_cache;
    SSprite *variable_cache_owner;
    int variable_cache_target_id;
    int variable_cache_owner_target_id;
    int variable_cache_index;
    int variable_cache_type;
    int mathop_cache_valid;
    int mathop_cache;
    int variable_cache_owner_is_original;
    SRuntime *variable_cache_runtime;
    /* Paired with variable_cache_runtime so allocator address reuse cannot
       turn a stale cross-runtime cache entry into a hit.  Zero is invalid. */
    uint64_t variable_cache_runtime_instance_id;
    /* Serialized SB3 variable/list identity.  Kept at the end so the JIT's
       existing field offsets remain stable. */
    SString *variable_id;
    /* The resolved variable's immutable name/ID object.  This makes cache
       validation pointer-based after the first lookup. */
    const SString *variable_cache_identity;
} SExpr;

typedef struct {
    SString *name;
    SExpr *value;
} SArgumentExpr;

typedef struct SStatement {
    int opcode;
    SString *variable_name;
    SString *procedure_name;
    SExpr *value;
    SExpr *index;
    SExpr *condition;
    SExpr *times;
    SArgumentExpr *arguments;
    int argument_count;
    struct SStatement *substack;
    int substack_count;
    struct SStatement *substack2;
    int substack2_count;
    SSprite *variable_cache_owner;
    int variable_cache_target_id;
    int variable_cache_owner_target_id;
    int variable_cache_index;
    int variable_cache_type;
    int procedure_cache_index;
    int procedure_cache_valid;
    int loop_state_cache_index;
    int loop_state_cache_scope_depth;
    int pen_color_param_cache_valid;
    int pen_color_param_cache;
    int substack_sync_cache;
    int substack2_sync_cache;
    int variable_cache_owner_is_original;
    SRuntime *variable_cache_runtime;
    SString *variable_id;
    /* See SExpr::variable_cache_runtime_instance_id. */
    uint64_t variable_cache_runtime_instance_id;
    int looks_effect_cache_valid;
    int looks_effect;
    int drag_mode;
    /* The resolved variable's immutable name/ID object. */
    const SString *variable_cache_identity;
    int layer_front;
} SStatement;

typedef struct {
    SString *name;
    int warp_mode;
    SString **argument_names;
    int argument_count;
    SStatement *statements;
    int statement_count;
    SProcedureEntryFn jit_entry;
} SCompiledProcedure;

struct SCompiledScript {
    /*
     * JIT entries may embed addresses of this immutable script and its owned
     * statement/expression/procedure tree.  The tree must not be moved or
     * destroyed while an entry can still be called.  A runtime-specialized
     * entry may also borrow an original SSprite owner after its runtime
     * instance guard; original target objects stay at stable addresses until
     * that runtime is destroyed.  Reallocatable SVariable arrays are reloaded
     * from the guarded owner on every native invocation and addressed by their
     * append-only index; SVariable/SList addresses are not borrowed by normal
     * code generation.  Destroy/invalidate the owning JitEngine before
     * replacing a project, and keep it alive while registered entries can run.
     * Once ownership analysis/certification succeeds, all semantic fields in
     * this tree (opcodes, operands, names, control-flow arrays, procedure
     * metadata) must likewise remain unchanged while its registration can
     * run.  Guarded interpreter cache fields may still be refreshed without
     * changing those semantics.
     */
    int target_id;
    SStatement *statements;
    int statement_count;
    SCompiledProcedure *procedures;
    int procedure_count;
    SSprite *bound_target;
    /* Zero means the JIT was built without runtime specialization.  Otherwise
       native entries validate this identity before dereferencing stable owner
       pointers or using invocation-local variable handles; a different
       runtime stays on the interpreter path. */
    uint64_t jit_runtime_instance_id;
    /* Edge-activated hat inputs are kept on the immutable script arena so the
       runtime can re-evaluate reporter-valued hat arguments each tick. */
    SExpr *hat_edge_value;
};

SCompiledScript *sjit_compiled_script_create(int target_id, int statement_count);
void sjit_compiled_script_destroy(SCompiledScript *script);
SExpr *sjit_expr_create_literal(SValue value);
SExpr *sjit_expr_create_binary(int opcode, SExpr *left, SExpr *right);
SExpr *sjit_expr_create_unary(int opcode, SExpr *operand);
SExpr *sjit_expr_create_mathop(const char *operator_name, SExpr *operand);
SExpr *sjit_expr_create_variable(const char *variable_name);
SExpr *sjit_expr_create_variable_with_id(
    const char *variable_id,
    const char *variable_name);
SExpr *sjit_expr_create_list_variable(const char *list_name);
SExpr *sjit_expr_create_list_variable_with_id(
    const char *list_id,
    const char *list_name);
SExpr *sjit_expr_create_argument(const char *argument_name);
SExpr *sjit_expr_create_list_item(const char *list_name, SExpr *index);
SExpr *sjit_expr_create_list_item_with_id(
    const char *list_id,
    const char *list_name,
    SExpr *index);
SExpr *sjit_expr_create_list_item_number(const char *list_name, SExpr *item);
SExpr *sjit_expr_create_list_item_number_with_id(
    const char *list_id,
    const char *list_name,
    SExpr *item);
SExpr *sjit_expr_create_list_length(const char *list_name);
SExpr *sjit_expr_create_list_length_with_id(
    const char *list_id,
    const char *list_name);
SExpr *sjit_expr_create_list_contains(const char *list_name, SExpr *item);
SExpr *sjit_expr_create_list_contains_with_id(
    const char *list_id,
    const char *list_name,
    SExpr *item);
SExpr *sjit_expr_create_timer(void);
SExpr *sjit_expr_create_days_since_2000(void);
SExpr *sjit_expr_create_mouse_x(void);
SExpr *sjit_expr_create_mouse_y(void);
SExpr *sjit_expr_create_x_position(void);
SExpr *sjit_expr_create_y_position(void);
SExpr *sjit_expr_create_direction(void);
SExpr *sjit_expr_create_costume_number_name(int number_name);
SExpr *sjit_expr_create_size(void);
SExpr *sjit_expr_create_backdrop_number_name(int number_name);
SExpr *sjit_expr_create_list_contents(const char *list_name);
SExpr *sjit_expr_create_list_contents_with_id(const char *list_id, const char *list_name);
SExpr *sjit_expr_create_touching_object(SExpr *object);
SExpr *sjit_expr_create_touching_color(SExpr *color);
SExpr *sjit_expr_create_color_touching_color(SExpr *color, SExpr *color2);
SExpr *sjit_expr_create_distance_to(SExpr *target);
SExpr *sjit_expr_create_sensing_of(SExpr *attribute, SExpr *object);
SExpr *sjit_expr_create_current(SExpr *menu);
SExpr *sjit_expr_create_answer(void);
SExpr *sjit_expr_create_loudness(void);
SExpr *sjit_expr_create_loud(void);
SExpr *sjit_expr_create_online(void);
SExpr *sjit_expr_create_username(void);
SExpr *sjit_expr_create_sound_volume(void);
SExpr *sjit_expr_create_counter(void);
SExpr *sjit_expr_create_mouse_down(void);
SExpr *sjit_expr_create_key_pressed(SExpr *key_name);
void sjit_expr_destroy(SExpr *expr);
void sjit_statement_destroy(SStatement *statement);
void sjit_compiled_procedure_destroy(SCompiledProcedure *procedure);
SValue sjit_script_eval_statement_expr(
    SRuntime *runtime,
    SCompiledScript *script,
    int statement_index,
    int expr_slot);
SValue sjit_script_eval_statement_expr_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    int expr_slot);
SValue sjit_script_eval_expr(
    SRuntime *runtime,
    SCompiledScript *script,
    SExpr *expr);
double sjit_script_eval_statement_number(
    SRuntime *runtime,
    SCompiledScript *script,
    int statement_index,
    int expr_slot);
double sjit_script_eval_statement_number_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    int expr_slot);
int sjit_script_eval_statement_bool_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    int expr_slot);
double sjit_script_value_ptr_to_number(SRuntime *runtime, SValue *value);
void sjit_script_destroy_value_ptr(SValue *value);
SRuntimeStatus sjit_script_execute_statement(SRuntime *runtime, SCompiledScript *script, int statement_index);
SRuntimeStatus sjit_script_execute_statement_with_frame(
    SRuntime *runtime,
    SCompiledScript *script,
    int statement_index,
    SFrame *frame);
SRuntimeStatus sjit_script_execute_statement_ptr_with_thread(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame,
    SCompiledScript *script,
    SStatement *statement);
SRuntimeStatus sjit_script_execute_procedure_statement(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame,
    SCompiledScript *script,
    SStatement *statement);
SRuntimeStatus sjit_script_interpreter_entry(SRuntime *runtime, SThread *thread, SFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
