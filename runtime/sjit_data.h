#ifndef SJIT_DATA_H
#define SJIT_DATA_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

SVariable *sjit_data_lookup_or_create_variable(SSprite *sprite, int id, const char *name, int type);
void sjit_data_set_variable(SVariable *variable, SValue value);
void sjit_data_change_variable_by(SRuntime *runtime, SVariable *variable, SValue delta);

#ifdef __cplusplus
}
#endif

#endif

