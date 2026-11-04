#ifndef JZ_INTERVAL_H
#define JZ_INTERVAL_H
#include <stdint.h>

/* Intrusive AVL index of disjoint half-open address ranges. Nodes do not move
 * during rotations/deletion: operations may retain a node outside the lock. */
struct jz_interval {
    uintptr_t start, end;          /* Validated start < end, without wraparound. */
    struct jz_interval *left, *right; /* Borrowed child links, owned by registry. */
    int height;                   /* AVL subtree height; leaf height is one. */
};
struct jz_interval *jz_interval_find(struct jz_interval *, uintptr_t base);
struct jz_interval *jz_interval_overlap(struct jz_interval *, uintptr_t start, uintptr_t end);
struct jz_interval *jz_interval_insert(struct jz_interval *, struct jz_interval *);
struct jz_interval *jz_interval_remove(struct jz_interval *, uintptr_t base);
#endif
