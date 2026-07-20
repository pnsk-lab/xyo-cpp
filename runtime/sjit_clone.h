#ifndef SJIT_CLONE_H
#define SJIT_CLONE_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

SSprite *sjit_clone_create(SRuntime *runtime, SSprite *source);
SSprite *sjit_clone_create_requested(SRuntime *runtime, SSprite *current, SValue requested);
void sjit_clone_delete(SRuntime *runtime, SSprite *clone);

#ifdef __cplusplus
}
#endif

#endif
