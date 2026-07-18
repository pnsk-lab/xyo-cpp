#include "sjit_opcode_effects.h"

static OpcodeEffects effects(
    bool can_yield,
    bool can_allocate,
    bool can_mutate_target,
    bool can_change_value_type,
    bool can_call_unknown,
    bool requires_interpreter) {
    const OpcodeEffects result = {
        can_yield,
        can_allocate,
        can_mutate_target,
        can_change_value_type,
        can_call_unknown,
        requires_interpreter,
    };
    return result;
}

static OpcodeEffects unknown_effects(void) {
    return effects(true, true, true, true, true, true);
}

OpcodeEffects sjit_expr_opcode_effects(int opcode) {
    switch (opcode) {
    case SJIT_EXPR_LITERAL:
        return effects(false, true, false, false, false, false);
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_DAYS_SINCE_2000:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_MOUSE_DOWN:
    case SJIT_EXPR_X_POSITION:
    case SJIT_EXPR_Y_POSITION:
    case SJIT_EXPR_LIST_LENGTH:
        return effects(false, false, false, false, false, false);
    case SJIT_EXPR_LT:
    case SJIT_EXPR_EQ:
    case SJIT_EXPR_GT:
    case SJIT_EXPR_AND:
    case SJIT_EXPR_OR:
    case SJIT_EXPR_NOT:
    case SJIT_EXPR_LIST_CONTAINS:
    case SJIT_EXPR_KEY_PRESSED:
        return effects(false, true, false, false, false, false);
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
    case SJIT_EXPR_ROUND:
    case SJIT_EXPR_RANDOM:
    case SJIT_EXPR_MATHOP:
    case SJIT_EXPR_LENGTH:
    case SJIT_EXPR_LIST_ITEM_NUMBER:
        return effects(false, true, false, false, false, false);
    case SJIT_EXPR_VARIABLE:
    case SJIT_EXPR_ARGUMENT:
    case SJIT_EXPR_LIST_ITEM:
    case SJIT_EXPR_LIST_VARIABLE:
        return effects(false, true, false, true, false, false);
    case SJIT_EXPR_JOIN:
    case SJIT_EXPR_LETTER_OF:
        return effects(false, true, false, false, false, false);
    case SJIT_EXPR_CONTAINS:
        /* Native entries box both operands in owned value slots, then share
           the same case-insensitive Scratch coercion helper as the
           interpreter. */
        return effects(false, true, false, false, false, false);
    case SJIT_EXPR_DIRECTION:
        /* Deliberately interpreter-only for the first explicit reporter
           fallback path. */
        return effects(false, false, false, false, false, true);
    default:
        return unknown_effects();
    }
}

OpcodeEffects sjit_statement_opcode_effects(int opcode) {
    switch (opcode) {
    case SJIT_STMT_NOOP:
        return effects(false, false, false, false, false, false);
    case SJIT_STMT_RESET_TIMER:
        return effects(false, false, false, false, false, false);
    case SJIT_STMT_SET_VARIABLE:
    case SJIT_STMT_CHANGE_VARIABLE:
        return effects(false, true, true, true, false, false);
    case SJIT_STMT_REPEAT:
    case SJIT_STMT_REPEAT_UNTIL:
    case SJIT_STMT_WHILE:
    case SJIT_STMT_FOREVER:
    case SJIT_STMT_FOR_EACH:
        return effects(true, false, true, false, false, false);
    case SJIT_STMT_IF:
    case SJIT_STMT_IF_ELSE:
        return effects(false, false, false, false, false, false);
    case SJIT_STMT_SAY:
        return effects(false, true, false, false, false, false);
    case SJIT_STMT_PEN_CLEAR:
    case SJIT_STMT_PEN_DOWN:
    case SJIT_STMT_PEN_UP:
    case SJIT_STMT_PEN_SET_SIZE:
    case SJIT_STMT_PEN_SET_COLOR:
    case SJIT_STMT_PEN_CHANGE_COLOR_PARAM:
    case SJIT_STMT_MOTION_SET_X:
    case SJIT_STMT_MOTION_SET_Y:
    case SJIT_STMT_MOTION_CHANGE_X:
    case SJIT_STMT_MOTION_CHANGE_Y:
    case SJIT_STMT_LOOKS_SET_SIZE:
    case SJIT_STMT_MOTION_GOTO_XY:
    case SJIT_STMT_LOOKS_SHOW:
    case SJIT_STMT_LOOKS_HIDE:
    case SJIT_STMT_LOOKS_SET_EFFECT:
    case SJIT_STMT_LOOKS_CHANGE_EFFECT:
    case SJIT_STMT_LOOKS_CLEAR_EFFECTS:
        return effects(false, true, true, false, false, false);
    case SJIT_STMT_SENSING_SET_DRAG_MODE:
        return effects(false, false, true, false, false, false);
    case SJIT_STMT_LIST_ADD:
    case SJIT_STMT_LIST_DELETE:
    case SJIT_STMT_LIST_DELETE_ALL:
    case SJIT_STMT_LIST_INSERT:
    case SJIT_STMT_LIST_REPLACE:
        return effects(false, true, true, true, false, false);
    case SJIT_STMT_PROCEDURE_CALL:
        return effects(true, true, true, true, true, false);
    case SJIT_STMT_STOP_THIS_SCRIPT:
        return effects(false, false, false, false, false, false);
    case SJIT_STMT_BROADCAST:
        return effects(false, true, false, false, true, false);
    case SJIT_STMT_WAIT:
    case SJIT_STMT_WAIT_UNTIL:
    case SJIT_STMT_LOOKS_SAY_FOR_SECS:
        return effects(true, true, false, false, false, false);
    case SJIT_STMT_STOP_OTHER_SCRIPTS:
    case SJIT_STMT_STOP_ALL:
        return effects(false, false, false, false, true, false);
    case SJIT_STMT_MONITOR_SHOW:
    case SJIT_STMT_MONITOR_HIDE:
        return effects(false, false, true, false, false, false);
    case SJIT_STMT_LOOKS_SWITCH_BACKDROP:
        /* Both native entry shapes call the shared runtime implementation,
           which owns backdrop metadata and starts matching backdrop hats. */
        return effects(false, true, true, false, true, false);
    default:
        return unknown_effects();
    }
}

const char *sjit_expr_opcode_name(int opcode) {
    switch (opcode) {
    case SJIT_EXPR_LITERAL: return "literal";
    case SJIT_EXPR_TIMER: return "timer";
    case SJIT_EXPR_LT: return "less-than";
    case SJIT_EXPR_ADD: return "add";
    case SJIT_EXPR_SUB: return "subtract";
    case SJIT_EXPR_MUL: return "multiply";
    case SJIT_EXPR_DIV: return "divide";
    case SJIT_EXPR_MATHOP: return "mathop";
    case SJIT_EXPR_VARIABLE: return "variable";
    case SJIT_EXPR_EQ: return "equals";
    case SJIT_EXPR_GT: return "greater-than";
    case SJIT_EXPR_AND: return "and";
    case SJIT_EXPR_OR: return "or";
    case SJIT_EXPR_NOT: return "not";
    case SJIT_EXPR_LIST_ITEM: return "list-item";
    case SJIT_EXPR_LIST_ITEM_NUMBER: return "list-item-number";
    case SJIT_EXPR_LIST_LENGTH: return "list-length";
    case SJIT_EXPR_LIST_CONTAINS: return "list-contains";
    case SJIT_EXPR_MOUSE_X: return "mouse-x";
    case SJIT_EXPR_MOUSE_Y: return "mouse-y";
    case SJIT_EXPR_MOUSE_DOWN: return "mouse-down";
    case SJIT_EXPR_ARGUMENT: return "argument";
    case SJIT_EXPR_MOD: return "mod";
    case SJIT_EXPR_ROUND: return "round";
    case SJIT_EXPR_JOIN: return "join";
    case SJIT_EXPR_LENGTH: return "length";
    case SJIT_EXPR_LETTER_OF: return "letter-of";
    case SJIT_EXPR_RANDOM: return "random";
    case SJIT_EXPR_KEY_PRESSED: return "key-pressed";
    case SJIT_EXPR_DAYS_SINCE_2000: return "days-since-2000";
    case SJIT_EXPR_X_POSITION: return "x-position";
    case SJIT_EXPR_Y_POSITION: return "y-position";
    case SJIT_EXPR_CONTAINS: return "contains";
    case SJIT_EXPR_LIST_VARIABLE: return "list-variable";
    case SJIT_EXPR_DIRECTION: return "direction";
    default: return "unknown-expression";
    }
}

const char *sjit_statement_opcode_name(int opcode) {
    switch (opcode) {
    case SJIT_STMT_NOOP: return "noop";
    case SJIT_STMT_RESET_TIMER: return "reset-timer";
    case SJIT_STMT_SET_VARIABLE: return "set-variable";
    case SJIT_STMT_CHANGE_VARIABLE: return "change-variable";
    case SJIT_STMT_REPEAT: return "repeat";
    case SJIT_STMT_IF: return "if";
    case SJIT_STMT_SAY: return "say";
    case SJIT_STMT_PEN_CLEAR: return "pen-clear";
    case SJIT_STMT_PEN_DOWN: return "pen-down";
    case SJIT_STMT_PEN_UP: return "pen-up";
    case SJIT_STMT_PEN_SET_SIZE: return "pen-set-size";
    case SJIT_STMT_PEN_SET_COLOR: return "pen-set-color";
    case SJIT_STMT_PEN_CHANGE_COLOR_PARAM: return "pen-change-color-param";
    case SJIT_STMT_MOTION_SET_X: return "set-x";
    case SJIT_STMT_MOTION_SET_Y: return "set-y";
    case SJIT_STMT_MOTION_CHANGE_X: return "change-x";
    case SJIT_STMT_MOTION_CHANGE_Y: return "change-y";
    case SJIT_STMT_LOOKS_SET_SIZE: return "set-size";
    case SJIT_STMT_LIST_ADD: return "list-add";
    case SJIT_STMT_LIST_DELETE: return "list-delete";
    case SJIT_STMT_LIST_DELETE_ALL: return "list-delete-all";
    case SJIT_STMT_LIST_INSERT: return "list-insert";
    case SJIT_STMT_LIST_REPLACE: return "list-replace";
    case SJIT_STMT_IF_ELSE: return "if-else";
    case SJIT_STMT_FOREVER: return "forever";
    case SJIT_STMT_FOR_EACH: return "for-each";
    case SJIT_STMT_PROCEDURE_CALL: return "procedure-call";
    case SJIT_STMT_MOTION_GOTO_XY: return "go-to-xy";
    case SJIT_STMT_LOOKS_SHOW: return "show";
    case SJIT_STMT_LOOKS_HIDE: return "hide";
    case SJIT_STMT_REPEAT_UNTIL: return "repeat-until";
    case SJIT_STMT_STOP_THIS_SCRIPT: return "stop-this-script";
    case SJIT_STMT_WHILE: return "while";
    case SJIT_STMT_BROADCAST: return "broadcast";
    case SJIT_STMT_WAIT: return "wait";
    case SJIT_STMT_WAIT_UNTIL: return "wait-until";
    case SJIT_STMT_STOP_OTHER_SCRIPTS: return "stop-other-scripts";
    case SJIT_STMT_STOP_ALL: return "stop-all";
    case SJIT_STMT_LOOKS_SAY_FOR_SECS: return "say-for-seconds";
    case SJIT_STMT_MONITOR_SHOW: return "monitor-show";
    case SJIT_STMT_MONITOR_HIDE: return "monitor-hide";
    case SJIT_STMT_LOOKS_SWITCH_BACKDROP: return "switch-backdrop";
    case SJIT_STMT_LOOKS_SET_EFFECT: return "set-graphic-effect";
    case SJIT_STMT_LOOKS_CHANGE_EFFECT: return "change-graphic-effect";
    case SJIT_STMT_LOOKS_CLEAR_EFFECTS: return "clear-graphic-effects";
    case SJIT_STMT_SENSING_SET_DRAG_MODE: return "set-drag-mode";
    default: return "unknown-statement";
    }
}
