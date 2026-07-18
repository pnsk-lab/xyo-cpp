#include "sjit_data.h"

SVariable *sjit_data_lookup_or_create_variable(SSprite *sprite, int id, const char *name, int type) {
    if (!sprite) {
        return NULL;
    }
    return sjit_target_lookup_or_create_variable(&sprite->base, id, name, type);
}

void sjit_data_set_variable(SVariable *variable, SValue value) {
    sjit_variable_set(variable, value);
}

void sjit_data_change_variable_by(SRuntime *runtime, SVariable *variable, SValue delta) {
    sjit_variable_change_by(runtime, variable, delta);
}

