#ifndef SJIT_JIT_BRIDGE_H
#define SJIT_JIT_BRIDGE_H

#include "sjit_abi.h"
#include "sjit_script.h"

#ifdef __cplusplus
extern "C" {
#endif

void *sjit_jit_thread_script_data(SThread *thread);
int sjit_jit_frame_pc(SFrame *frame);
void sjit_jit_frame_set_pc(SFrame *frame, int pc);
void sjit_jit_frame_mark_finished(SFrame *frame);
double sjit_jit_sprite_x(SSprite *sprite);
double sjit_jit_sprite_y(SSprite *sprite);
void sjit_jit_pen_set_size_number(SRuntime *runtime, SSprite *sprite, double size);
void sjit_jit_sprite_set_size(SRuntime *runtime, SSprite *sprite, double size);
void sjit_jit_reset_timer(SRuntime *runtime);
SVariable *sjit_jit_statement_scalar_variable(SRuntime *runtime, SCompiledScript *script, int statement_index);
SVariable *sjit_jit_statement_scalar_variable_ptr(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
SVariable *sjit_jit_statement_list_variable_ptr(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
SVariable *sjit_jit_expr_scalar_variable(SRuntime *runtime, int target_id, SExpr *expr);
SVariable *sjit_jit_expr_list_variable(SRuntime *runtime, int target_id, SExpr *expr);
int sjit_jit_variable_is_number(SVariable *variable);
double sjit_jit_variable_number(SRuntime *runtime, SVariable *variable);
void sjit_jit_variable_value(SVariable *variable, SValue *out);
int sjit_jit_variable_truthy(SRuntime *runtime, SVariable *variable);
void sjit_jit_variable_set_number(SVariable *variable, double number);
void sjit_jit_variable_set_string_borrowed(
    SRuntime *runtime,
    SVariable *destination,
    SString *source);
void sjit_jit_variable_change_by_number(SRuntime *runtime, SVariable *variable, double delta);
double sjit_jit_expr_variable_number(SRuntime *runtime, int target_id, SExpr *expr);
double sjit_jit_expr_list_length_number(SRuntime *runtime, int target_id, SExpr *expr);
double sjit_jit_expr_list_item_number_at(SRuntime *runtime, int target_id, SExpr *expr, double index);
int sjit_jit_expr_list_contains_literal(SRuntime *runtime, int target_id, SExpr *expr);
double sjit_jit_list_variable_length_number(SVariable *list_variable);
double sjit_jit_list_variable_item_number_at(
    SRuntime *runtime,
    SVariable *list_variable,
    double index);
double sjit_jit_list_variable_item_number_at_variable(
    SRuntime *runtime,
    SVariable *list_variable,
    SVariable *index_variable);
double sjit_jit_list_variable_item_number_at_argument(
    SRuntime *runtime,
    SVariable *list_variable,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index);
void sjit_jit_list_variable_item_value(
    SRuntime *runtime,
    SVariable *list_variable,
    SValue *index,
    SValue *out);
void sjit_jit_list_variable_item_value_at_number(
    SRuntime *runtime,
    SVariable *list_variable,
    double index,
    SValue *out);
double sjit_jit_list_variable_item_number_value(
    SRuntime *runtime,
    SVariable *list_variable,
    SValue *item);
int sjit_jit_list_variable_contains_value(
    SRuntime *runtime,
    SVariable *list_variable,
    SValue *item);
double sjit_jit_days_since_2000(void);
double sjit_jit_round_number(double value);
double sjit_jit_random_number(double from, double to);
double sjit_jit_mathop_number(SRuntime *runtime, SExpr *expr, double value);
void sjit_jit_value_make_number(double value, SValue *out);
void sjit_jit_value_make_bool(int value, SValue *out);
void sjit_jit_expr_literal_value(SExpr *expr, SValue *out);
double sjit_jit_variable_argument_value(SRuntime *runtime, SVariable *variable, SValue *out);
double sjit_jit_procedure_argument_copy(double *numeric_args, SValue *value_args, int index, SValue *out);
double sjit_jit_expr_list_item_argument_literal(
    SRuntime *runtime,
    int target_id,
    SExpr *expr,
    SValue *out);
double sjit_jit_expr_list_item_argument_at_variable(
    SRuntime *runtime,
    int target_id,
    SExpr *expr,
    SVariable *index_variable,
    SValue *out);
void sjit_jit_procedure_argument_value_at(double *numeric_args, SValue *value_args, int index, SValue *out);
void sjit_jit_value_join_ptr(SRuntime *runtime, SValue *left, SValue *right, SValue *out);
void sjit_jit_expr_variable_value(SRuntime *runtime, int target_id, SExpr *expr, SValue *out);
void sjit_jit_expr_list_item_value(SRuntime *runtime, int target_id, SExpr *expr, SValue *index, SValue *out);
double sjit_jit_expr_list_item_number_value(SRuntime *runtime, int target_id, SExpr *expr, SValue *item);
int sjit_jit_expr_list_contains_value(SRuntime *runtime, int target_id, SExpr *expr, SValue *item);
double sjit_jit_value_length_number(SRuntime *runtime, SValue *value);
void sjit_jit_value_letter_of(SRuntime *runtime, SValue *index, SValue *text, SValue *out);
int sjit_jit_key_pressed_value(SRuntime *runtime, SValue *key);
int sjit_jit_value_truthy(SRuntime *runtime, SValue *value);
int sjit_jit_value_compare(
    SRuntime *runtime,
    SValue *left,
    SValue *right,
    int opcode);
void sjit_jit_pen_set_color_value_and_change_param_number_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SValue *color,
    int param_id,
    double delta);
double sjit_jit_expr_list_item_number_at_variable(
    SRuntime *runtime,
    int target_id,
    SExpr *list_expr,
    SVariable *index_variable);
double sjit_jit_expr_list_item_number_at_argument(
    SRuntime *runtime,
    int target_id,
    SExpr *list_expr,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index);
int sjit_jit_expr_list_item_compare_literal_at_variable(
    SRuntime *runtime,
    int target_id,
    SExpr *list_expr,
    SVariable *index_variable,
    SExpr *literal_expr,
    int opcode);
void sjit_jit_pen_set_color_list_item_and_change_param_number_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SExpr *list_expr,
    SVariable *index_variable,
    int param_id,
    double delta);
void sjit_jit_statement_list_replace_literal_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *index_variable);
void sjit_jit_statement_list_replace_number_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *index_variable,
    double value);
void sjit_jit_statement_list_replace_list_item_at_variables(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *index_variable,
    SExpr *source_expr,
    SVariable *source_index_variable);
void sjit_jit_statement_list_replace_list_item_at_variable_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *index_variable,
    SExpr *source_expr,
    double source_index);
void sjit_jit_pen_render_list_pixel_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SSprite *sprite,
    SExpr *color_expr,
    SExpr *brightness_expr,
    SVariable *index_variable,
    SStatement *replace_statement,
    int param_id);
void sjit_jit_pen_render_list_pixel_from_variables(
    SRuntime *runtime,
    SSprite *sprite,
    SVariable *color_list_variable,
    SVariable *brightness_list_variable,
    SVariable *index_variable,
    SExpr *replacement_literal,
    int param_id);
void sjit_jit_set_col_from_numbers(
    SRuntime *runtime,
    SSprite *sprite,
    SVariable *color_list_variable,
    SVariable *clamp_variable,
    double red,
    double green,
    double blue);
int sjit_jit_pen_render_row_from_variables(
    SRuntime *runtime,
    SSprite *sprite,
    SVariable *color_list_variable,
    SVariable *brightness_list_variable,
    SVariable *row_variable,
    SVariable *column_variable,
    SVariable *index_variable,
    double column_count,
    double x_step,
    SExpr *replacement_literal,
    int param_id);
void sjit_jit_script_set_variable(SRuntime *runtime, SCompiledScript *script, int statement_index);
void sjit_jit_script_change_variable(SRuntime *runtime, SCompiledScript *script, int statement_index);
void sjit_jit_script_list_add(SRuntime *runtime, SCompiledScript *script, int statement_index);
void sjit_jit_script_list_delete(SRuntime *runtime, SCompiledScript *script, int statement_index);
void sjit_jit_script_list_delete_all(SRuntime *runtime, SCompiledScript *script, int statement_index);
void sjit_jit_script_list_insert(SRuntime *runtime, SCompiledScript *script, int statement_index);
void sjit_jit_script_list_replace(SRuntime *runtime, SCompiledScript *script, int statement_index);
void sjit_jit_script_say(SRuntime *runtime, SCompiledScript *script, int statement_index);
void sjit_jit_script_pen_set_color(SRuntime *runtime, SCompiledScript *script, int statement_index);
void sjit_jit_script_pen_change_color_param(SRuntime *runtime, SCompiledScript *script, int statement_index);
double sjit_jit_statement_number(SRuntime *runtime, SCompiledScript *script, SStatement *statement, int expr_slot);
int sjit_jit_statement_bool(SRuntime *runtime, SCompiledScript *script, SStatement *statement, int expr_slot);
void sjit_jit_statement_set_monitor_visibility(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement);
void sjit_jit_statement_set_variable(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_change_variable(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_set_variable_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double value);
void sjit_jit_variable_set_from_variable(
    SRuntime *runtime,
    SVariable *destination,
    SVariable *source);
void sjit_jit_variable_set_from_argument(
    SRuntime *runtime,
    SVariable *destination,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index);
void sjit_jit_variable_set_from_literal(
    SRuntime *runtime,
    SVariable *destination,
    SExpr *literal_expr);
void sjit_jit_variable_set_from_list_item_literal(
    SRuntime *runtime,
    int target_id,
    SVariable *destination,
    SExpr *list_expr);
void sjit_jit_variable_set_from_list_item_at_number(
    SRuntime *runtime,
    int target_id,
    SVariable *destination,
    SExpr *list_expr,
    double index);
void sjit_jit_variable_set_from_list_item_at_variable(
    SRuntime *runtime,
    int target_id,
    SVariable *destination,
    SExpr *list_expr,
    SVariable *index_variable);
void sjit_jit_variable_set_from_list_item_at_argument(
    SRuntime *runtime,
    int target_id,
    SVariable *destination,
    SExpr *list_expr,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index);
void sjit_jit_statement_change_variable_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double value);
void sjit_jit_statement_list_add(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_list_delete(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_list_delete_all(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_list_insert(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_list_replace(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_list_add_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double value);
void sjit_jit_statement_list_add_from_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *source);
void sjit_jit_statement_list_add_from_argument(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index);
void sjit_jit_statement_list_add_from_literal(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement);
void sjit_jit_statement_list_add_list_item_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SExpr *list_expr,
    SVariable *index_variable);
int sjit_jit_statement_list_add_literal_repeated(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double count);
void sjit_jit_statement_list_delete_at_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double index);
void sjit_jit_statement_list_insert_number_at(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double index,
    double value);
void sjit_jit_statement_list_replace_number_at(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double index,
    double value);
void sjit_jit_list_variable_add_number(SVariable *list_variable, double value);
void sjit_jit_list_variable_clear(SVariable *list_variable);
void sjit_jit_list_variable_replace_number_at(
    SRuntime *runtime,
    SVariable *list_variable,
    double index,
    double value);
void sjit_jit_list_variable_replace_number_at_variable(
    SRuntime *runtime,
    SVariable *list_variable,
    SVariable *index_variable,
    double value);
void sjit_jit_list_variable_replace_from_variables(
    SRuntime *runtime,
    SVariable *list_variable,
    SVariable *index_variable,
    SVariable *value_variable);
void sjit_jit_list_variable_replace_list_item_at_variables(
    SRuntime *runtime,
    SVariable *list_variable,
    SVariable *index_variable,
    SVariable *source_list_variable,
    SVariable *source_index_variable);
void sjit_jit_statement_say(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_pen_set_color(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_pen_change_color_param(SRuntime *runtime, SCompiledScript *script, SStatement *statement);
void sjit_jit_statement_set_variable_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *value);
void sjit_jit_statement_list_add_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *value);
void sjit_jit_statement_list_delete_index_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *index);
void sjit_jit_statement_list_insert_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *index,
    SValue *value);
void sjit_jit_statement_list_replace_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *index,
    SValue *value);
void sjit_jit_statement_say_value_ptr(SRuntime *runtime, SValue *value);
void sjit_jit_statement_pen_set_color_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SValue *value);
void sjit_jit_statement_pen_change_color_param_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SValue *param,
    SValue *value);
SLoopState *sjit_jit_control_loop_state(SFrame *frame, SStatement *statement, int create);
void sjit_jit_control_loop_reset(SFrame *frame, SStatement *statement);
SLoopState *sjit_jit_procedure_control_loop_state(
    SFrame *frame,
    SStatement *statement,
    int depth,
    int create);
void sjit_jit_procedure_control_loop_reset(SFrame *frame, SStatement *statement, int depth);
SLoopState *sjit_jit_procedure_activation_state(
    SFrame *frame,
    const void *key,
    int depth,
    int create);
void sjit_jit_procedure_activation_reset(SFrame *frame, const void *key, int depth);
int sjit_jit_repeat_should_enter_number(SFrame *frame, SStatement *statement, double times);
int sjit_jit_procedure_repeat_should_enter_number(
    SFrame *frame,
    SStatement *statement,
    int depth,
    double times);
double sjit_jit_repeat_remaining_number(SFrame *frame, SStatement *statement, double times);
double sjit_jit_repeat_take_all_number(SFrame *frame, SStatement *statement, double times);
double sjit_jit_round_repeat_count(double value);
SRuntimeStatus sjit_jit_finish_control_branch(SFrame *frame, SLoopState *state);
SRuntimeStatus sjit_jit_finish_batched_loop_branch(
    SRuntime *runtime,
    SFrame *frame,
    SLoopState *state,
    int branch_count);

#ifdef __cplusplus
}
#endif

#endif
