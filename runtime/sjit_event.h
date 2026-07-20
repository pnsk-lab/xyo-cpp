#ifndef SJIT_EVENT_H
#define SJIT_EVENT_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int sjit_event_broadcast(SRuntime *runtime, const char *message);
SRuntimeStatus sjit_event_broadcast_and_wait(SRuntime *runtime, SFrame *frame, const char *message, int resume_pc);
void sjit_event_poll_edge_hats(SRuntime *runtime);

#ifdef __cplusplus
}
#endif

#endif
