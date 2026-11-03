#ifndef JAZZ_ERROR_H
#define JAZZ_ERROR_H
#include <stdint.h>

/* code 为负 errno；stage 指向静态字符串，操作返回后仍有效。
 * system_errno 保留底层错误；wc_status 仅用于实际 CQ 完成错误。 */
struct jazz_error {
    int code;
    const char *stage;
    int system_errno;
    uint32_t wc_status;
};
#endif
