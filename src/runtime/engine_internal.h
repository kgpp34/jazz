#ifndef JZ_ENGINE_INTERNAL_H
#define JZ_ENGINE_INTERNAL_H
#include "jz/engine.h"
#include "transport/backend.h"
#include <stddef.h>

/* This header deliberately has no verbs, CUDA, or backend-specific fields. */
struct jz_memory_registry;
struct jz_engine {
    enum jz_engine_state state;   /* Guards configuration and resource transitions. */
    struct jz_backend **backends; /* Owned objects; NULL after successful disposal. */
    struct jz_memory_registry *memory; /* Generic range/state owner, no verbs layouts. */
    size_t backend_count;        /* Number of occupied or already-disposed slots. */
};
/* Private bounded registry, not a dynamic plugin system. Ownership transfers
 * only on successful attach; the caller disposes an unattached object. */
int jz_engine_attach(jz_engine *, struct jz_backend *, struct jz_error *);
struct jz_backend *jz_engine_find(const jz_engine *, unsigned kind);
#endif
