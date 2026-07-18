#ifndef SJIT_OWNERSHIP_INTERNAL_H
#define SJIT_OWNERSHIP_INTERNAL_H

#include "sjit_ownership.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal ownership proof result.  The dependency array contains the stable
   indices of every owner-local scalar reached from the script and its called
   procedures.  The caller owns the array and must destroy or transfer it. */
typedef struct {
    SOwnershipAnalysis analysis;
    int *owned_variable_indices;
    int owned_variable_count;
} SOwnershipManifest;

SOwnershipManifest sjit_analyze_script_ownership_manifest(
    SRuntime *runtime,
    const SCompiledScript *script);
void sjit_ownership_manifest_destroy(SOwnershipManifest *manifest);

/* JitEngine calls this only after its private entry registry has matched the
   top-level and every procedure pointer to the exact compiled script. */
int sjit_runtime_record_attested_script_ownership(
    SRuntime *runtime,
    int script_id,
    const SCompiledScript *script,
    SScriptEntryFn entry);

#ifdef __cplusplus
}
#endif

#endif
