#include "sjit_variable.h"

#include "sjit_list.h"
#include "sjit_number.h"
#include "sjit_value.h"

void sjit_variable_init(SVariable *variable, int id, const char *name, int type) {
    if (!variable) {
        return;
    }
    variable->id = id;
    variable->scratch_id = NULL;
    variable->name = sjit_string_new(name ? name : "");
    variable->type = type;
    variable->is_cloud = 0;
    variable->scalar_kind = type == SJIT_VAR_SCALAR ? SJIT_SCALAR_NUMBER : SJIT_SCALAR_DYNAMIC;
    if (type == SJIT_VAR_LIST) {
        variable->value = sjit_make_list(sjit_list_create());
    } else if (type == SJIT_VAR_BROADCAST) {
        variable->value = sjit_make_string(name ? name : "");
    } else {
        variable->value = sjit_make_number(0.0);
    }
}

void sjit_variable_init_with_scratch_id(
    SVariable *variable,
    const char *scratch_id,
    const char *name,
    int type) {
    sjit_variable_init(variable, 0, name, type);
    if (variable && scratch_id && scratch_id[0] != '\0') {
        variable->scratch_id = sjit_string_new(scratch_id);
    }
}

void sjit_variable_destroy(SVariable *variable) {
    if (!variable) {
        return;
    }
    sjit_string_destroy(variable->name);
    sjit_string_destroy(variable->scratch_id);
    sjit_value_destroy(variable->value);
    variable->name = 0;
    variable->scratch_id = 0;
    variable->value = sjit_make_null();
}

void sjit_variable_set_scalar_kind(SVariable *variable, int scalar_kind) {
    if (!variable || variable->type != SJIT_VAR_SCALAR) {
        return;
    }
    switch (scalar_kind) {
    case SJIT_SCALAR_NUMBER:
    case SJIT_SCALAR_STRING:
    case SJIT_SCALAR_BOOL:
        variable->scalar_kind = scalar_kind;
        break;
    default:
        variable->scalar_kind = SJIT_SCALAR_DYNAMIC;
        break;
    }
}

void sjit_variable_set(SVariable *variable, SValue value) {
    sjit_variable_set_move(variable, sjit_value_clone(value));
}

void sjit_variable_set_move(SVariable *variable, SValue value) {
    if (!variable) {
        sjit_value_destroy(value);
        return;
    }
    sjit_value_destroy(variable->value);
    variable->value = value;
}

void sjit_variable_set_number(SVariable *variable, double number) {
    if (!variable) {
        return;
    }
    if (variable->value.tag != SJIT_VALUE_NUMBER) {
        sjit_value_destroy(variable->value);
        variable->value = sjit_make_number(number);
        return;
    }
    variable->value.number = number;
    variable->value.ptr = NULL;
}

void sjit_variable_set_bool(SVariable *variable, int value) {
    if (!variable) {
        return;
    }
    if (variable->value.tag != SJIT_VALUE_BOOL) {
        sjit_value_destroy(variable->value);
        variable->value = sjit_make_bool(value);
        return;
    }
    variable->value.number = value ? 1.0 : 0.0;
    variable->value.ptr = NULL;
}

void sjit_variable_set_list_item_limit(SVariable *variable, int item_limit) {
    if (!variable || variable->value.tag != SJIT_VALUE_LIST || !variable->value.ptr) {
        return;
    }
    sjit_list_set_item_limit((SList *)variable->value.ptr, item_limit);
}

double sjit_variable_number(SRuntime *runtime, const SVariable *variable) {
    if (!variable) {
        return 0.0;
    }
    if (variable->value.tag == SJIT_VALUE_NUMBER) {
        return variable->value.number != variable->value.number ? 0.0 : variable->value.number;
    }
    return sjit_to_number_fast(runtime, variable->value);
}

void sjit_variable_change_by(SRuntime *runtime, SVariable *variable, SValue delta) {
    if (!variable) {
        return;
    }
    const double next = sjit_to_number_fast(runtime, variable->value) + sjit_to_number_fast(runtime, delta);
    sjit_variable_set(variable, sjit_make_number(next));
}

void sjit_variable_change_by_number(SRuntime *runtime, SVariable *variable, double delta) {
    if (!variable) {
        return;
    }
    sjit_variable_set_number_fast(variable, sjit_variable_number(runtime, variable) + delta);
}

SVariable sjit_variable_clone(const SVariable *variable) {
    SVariable copy;
    copy.id = variable ? variable->id : 0;
    copy.name = sjit_string_clone(variable ? variable->name : NULL);
    copy.type = variable ? variable->type : SJIT_VAR_SCALAR;
    copy.is_cloud = variable ? variable->is_cloud : 0;
    copy.scalar_kind = variable ? variable->scalar_kind : SJIT_SCALAR_NUMBER;
    copy.value = variable ? sjit_value_clone(variable->value) : sjit_make_number(0.0);
    copy.scratch_id = sjit_string_clone(variable ? variable->scratch_id : NULL);
    return copy;
}
