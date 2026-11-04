#ifndef JZ_INTERNAL_ERROR_H
#define JZ_INTERNAL_ERROR_H
#include "jz/error.h"

static inline void jz_error_clear(struct jz_error *error)
{
    if (error) *error = (struct jz_error){0};
}
static inline int jz_fail(struct jz_error *error, int cause, const char *stage)
{
    if (error) *error = (struct jz_error){
        .code = -cause, .stage = stage, .system_errno = cause };
    return -cause;
}
#endif
