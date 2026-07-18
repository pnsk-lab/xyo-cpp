#ifndef SJIT_FRAME_H
#define SJIT_FRAME_H

#include "sjit_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

void sjit_frame_init(SFrame *frame);
void sjit_frame_reset(SFrame *frame);
int sjit_frame_stack_push(SFrame *frame, SValue value);
SValue sjit_frame_stack_pop(SFrame *frame);

#ifdef __cplusplus
}
#endif

#endif

