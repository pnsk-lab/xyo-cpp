#ifndef SJIT_WEB_JIT_BRIDGE_H
#define SJIT_WEB_JIT_BRIDGE_H

#include "sjit_abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int sjit_web_jit_register_module(
    const void *bytes,
    int size,
    const char *entry_name,
    int stack_base);
void sjit_web_jit_unregister_module(int handle);
int sjit_web_jit_invoke(int handle, int runtime, int thread, int frame);
SRuntimeStatus sjit_web_wasm_entry(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
