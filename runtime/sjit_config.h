#ifndef SJIT_CONFIG_H
#define SJIT_CONFIG_H

#define SJIT_MAX_LOCALS 256
#define SJIT_MAX_STACK 256
#define SJIT_MAX_PARAMS 64
#define SJIT_MAX_PROCEDURE_CALL_DEPTH 16384
#define SJIT_MAX_LOOP_STATES 2048
#define SJIT_MAX_SCHEDULER_PASSES 100000
/* Keep the historical Scratch cap available as an explicit compatibility
   preset, while the default runtime preset is large enough for TurboWarp's
   64 MiB DRAM list used by linux.sb3.  A runtime may override this per list. */
#define SJIT_SCRATCH_LIST_ITEM_LIMIT 200000
#define SJIT_TURBOWARP_LIST_ITEM_LIMIT 67108864
#define SJIT_LIST_ITEM_LIMIT SJIT_TURBOWARP_LIST_ITEM_LIMIT
#define SJIT_INITIAL_CAPACITY 8
#define SJIT_WARP_TIME_MS 500.0

#endif
