#ifndef SJIT_CLONE_H
#define SJIT_CLONE_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

SSprite *sjit_clone_create(SRuntime *runtime, SSprite *source);
void sjit_clone_delete(SRuntime *runtime, SSprite *clone);

#ifdef __cplusplus
}
#endif

#endif

