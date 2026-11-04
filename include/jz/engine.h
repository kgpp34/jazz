#ifndef JZ_ENGINE_H
#define JZ_ENGINE_H
#include "jz/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque owner of configured backends. Copying a handle does not retain it. */
typedef struct jz_engine jz_engine;
enum jz_engine_state {
    JZ_ENGINE_CREATED,          /* Software configuration; no prepared resources. */
    JZ_ENGINE_PREPARING,        /* Internal transition during synchronous prepare. */
    JZ_ENGINE_READY,            /* Backends prepared; no peer connectivity implied. */
    JZ_ENGINE_CLEANUP_REQUIRED  /* Failed release retained resources; retry destroy. */
};

/* Serialize all lifecycle, configuration and query calls in this stage.
 * No threads, MRs or QPs exist yet. The ABI remains experimental.
 * Initialize *out to NULL before create; it remains unchanged on failure. */
int jz_engine_create(jz_engine **out, struct jz_error *error);
/* Configure at least one backend before prepare. On failure, the original prepare
 * error is preserved. Successful rollback returns to CREATED with configuration
 * retained; failed rollback requires destroy. READY cannot be prepared again. */
int jz_engine_prepare(jz_engine *engine, struct jz_error *error);
int jz_engine_get_state(const jz_engine *engine, enum jz_engine_state *out);
/* NULL handles are already destroyed. Failure preserves *engine for retry;
 * successful destruction sets it to NULL. */
int jz_engine_destroy(jz_engine **engine, struct jz_error *error);

#ifdef __cplusplus
}
#endif
#endif
