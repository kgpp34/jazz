#ifndef JZ_MEMORY_INTERNAL_H
#define JZ_MEMORY_INTERNAL_H
#include "memory/interval.h"
#include "transport/memory_backend.h"
#include <pthread.h>

struct jz_memory_binding {
    struct jz_backend *backend; /* Borrowed; Engine retains backends until registry empties. */
    void *resource;             /* Owned by backend, reachable until cleanup succeeds. */
};
struct jz_memory_record {
    struct jz_interval index;        /* First member; stable identity outside registry lock. */
    struct jz_local_memory_info info; /* Only read/written while holding registry lock. */
    struct jz_memory_binding *bindings; /* Control operation owns tokens while state is busy. */
    size_t binding_count;           /* Fixed memory-capable backend set for this record. */
};
struct jz_memory_registry {
    pthread_rwlock_t lock;       /* Short index/state critical sections, never provider calls. */
    struct jz_interval *root;    /* Owns all ACTIVE, busy and cleanup-pending records. */
    size_t count;               /* Includes reservations and cleanup failures. */
};
int jz_memory_registry_create(struct jz_memory_registry **, struct jz_error *);
void jz_memory_registry_dispose(struct jz_memory_registry *);
int jz_memory_release_all(jz_engine *, struct jz_error *);
#endif
