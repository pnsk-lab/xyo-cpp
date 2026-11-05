#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental, IR inspection only; no pointer or entry may be executed. */
typedef struct XyoBrowserIrResult XyoBrowserIrResult;
XyoBrowserIrResult* xyo_browser_ir_compile(const uint8_t* sb3, size_t size);
/* Returned UTF-8 NUL-terminated strings belong to result until destroy. */
int xyo_browser_ir_ok(const XyoBrowserIrResult* result);
const char* xyo_browser_ir_text(const XyoBrowserIrResult* result);
/* UTF-8 JSON v1: DataLayout, partial ABI fingerprint, hex-encoded slot keys. */
const char* xyo_browser_ir_metadata(const XyoBrowserIrResult* result);
const char* xyo_browser_ir_error(const XyoBrowserIrResult* result);
void xyo_browser_ir_destroy(XyoBrowserIrResult* result);
#ifdef __cplusplus
}
#endif
