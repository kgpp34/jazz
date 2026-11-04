#ifndef JZ_ERROR_H
#define JZ_ERROR_H

/* All functions return zero on success or a negative errno on failure. */
struct jz_error {
    int code;             /* Negative errno, matching the function result. */
    const char *stage;     /* Static diagnostic label; survives object destruction. */
    int system_errno;     /* Positive underlying cause; zero on success. */
};
#endif
