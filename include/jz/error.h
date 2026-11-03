#ifndef JZ_ERROR_H
#define JZ_ERROR_H
#include <stdint.h>

/* code is a negative errno. stage points to a static string that remains valid after return.
 * system_errno preserves the underlying error; wc_status is set only for CQ errors. */
struct jz_error {
    int code;
    const char *stage;
    int system_errno;
    uint32_t wc_status;
};
#endif
