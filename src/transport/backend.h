#ifndef JZ_BACKEND_H
#define JZ_BACKEND_H
#include "jz/error.h"

struct jz_backend;
struct jz_memory_backend_ops;
struct jz_backend_ops {
    /* May fail after partial acquisition; release must handle that state. */
    int (*prepare)(struct jz_backend *, struct jz_error *);
    /* Idempotent and retryable. Retain configuration and any live parent
     * references on failure; success permits another prepare or dispose. */
    int (*release)(struct jz_backend *, struct jz_error *);
    /* Software memory only; called after successful release or before prepare.
     * Never hide a fallible hardware cleanup operation in this callback. */
    void (*dispose)(struct jz_backend *);
};
struct jz_backend {
    unsigned kind;                    /* Opaque lookup key; no runtime type switch. */
    const struct jz_backend_ops *ops;  /* Static lifecycle strategy, outlives owner. */
    const struct jz_memory_backend_ops *memory_ops; /* Optional registration strategy. */
};
#endif
