#ifndef JZ_MEMORY_H
#define JZ_MEMORY_H
#include "jz/engine.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { JZ_MEMORY_LOCATION_MAX = 64 };
struct jz_memory_buffer {
    void *addr;    /* Caller-owned HOST mapping; retain it until cleanup succeeds. */
    size_t length; /* Exact byte range; zero length is invalid. */
};
struct jz_memory_options {
    const char *location;   /* Copied label; "*" requests automatic HOST discovery. */
    bool remote_accessible; /* Default true: permit both remote READ and WRITE. */
};
enum jz_registration_result_state {
    JZ_REGISTRATION_NONE,   /* This request left no registration behind. */
    JZ_REGISTRATION_ACTIVE, /* Register succeeded; or unregister has not confirmed cleanup. */
    JZ_REGISTRATION_CLEANUP_REQUIRED /* Retain buffer; retry unregister by base. */
};
struct jz_registration_result {
    enum jz_registration_result_state state; /* Outcome of this request only. */
};
enum jz_local_memory_state {
    JZ_MEMORY_REGISTERING,
    JZ_MEMORY_ACTIVE,
    JZ_MEMORY_UNREGISTERING,
    JZ_MEMORY_CLEANUP_REQUIRED
};
struct jz_local_memory_info {
    struct jz_memory_buffer buffer;        /* Value snapshot; does not retain memory. */
    enum jz_local_memory_state state;      /* Queries do not acquire a use reference. */
    bool remote_accessible;                /* Registration policy, not data readiness. */
    char location[JZ_MEMORY_LOCATION_MAX]; /* Nominal HOST location; "*" is unknown.
                                            * Automatic discovery samples residency,
                                            * not a guarantee for every page. */
    struct jz_error registration_error;    /* Original registration failure, if any. */
    struct jz_error cleanup_error;         /* First error of the latest cleanup pass. */
};

void jz_memory_options_init(struct jz_memory_options *options);
/* Registration requires READY. Results are mandatory, with at least count
 * entries for a batch; buffers and results must not alias. NULL options selects
 * defaults. A failed batch rolls back every new entry, never existing entries.
 * Check results even on error: CLEANUP_REQUIRED means the buffer must stay live.
 * NONE does not release any pre-existing registration of the same buffer.
 * Disjoint register/unregister/query calls may run concurrently. Lifecycle calls
 * (configure/prepare/destroy) require external exclusion from every other API. */
int jz_register_local_memory(jz_engine *, const struct jz_memory_buffer *,
                             const struct jz_memory_options *, struct jz_registration_result *,
                             struct jz_error *);
int jz_register_local_memory_batch(jz_engine *, const struct jz_memory_buffer *, size_t count,
                                   const struct jz_memory_options *,
                                   struct jz_registration_result *, struct jz_error *);
/* Unregister accepts the exact registration base, never an interior address.
 * Missing bases return -ENOENT. Failure retains only resources still live;
 * retry by the same base. A nonempty batch requires distinct, non-NULL bases.
 * Entries that are absent or busy do not prevent other entries being cleaned.
 * If preflight allocation fails, results conservatively remain ACTIVE. Only
 * NONE confirms absence or completed cleanup for valid input bases. */
int jz_unregister_local_memory(jz_engine *, void *base, struct jz_error *);
int jz_unregister_local_memory_batch(jz_engine *, void *const *bases, size_t count,
                                     struct jz_registration_result *, struct jz_error *);
/* Queries remain available during retryable Engine teardown. */
int jz_query_local_memory(const jz_engine *, void *base, struct jz_local_memory_info *,
                          struct jz_error *);

#ifdef __cplusplus
}
#endif
#endif
