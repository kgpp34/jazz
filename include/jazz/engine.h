#ifndef JAZZ_ENGINE_H
#define JAZZ_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#include "jazz/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jazz_engine jazz_engine;
enum { JAZZ_DEVICE_NAME_MAX = 64, JAZZ_ENGINE_DEVICE_LIMIT = 64 };
enum jazz_setup { JAZZ_SETUP_CM = 1, JAZZ_SETUP_OOB = 2 };
enum jazz_engine_state {
    JAZZ_ENGINE_CREATED,
    JAZZ_ENGINE_PREPARING,
    JAZZ_ENGINE_READY,
    JAZZ_ENGINE_CLEANUP_REQUIRED
};

struct jazz_engine_config {
    unsigned setup_modes; /* 默认 CM | OOB；OOB-only 不使用 CM context provider。 */
    const char *const *devices;
    size_t device_count; /* 显式设备集合，1..DEVICE_LIMIT；重复名字被拒绝。 */
};

/* 自有值快照：不把 ibv_context/PD 指针暴露给调用方。
 * READY 只表示设备资源准备好，不表示已经与任何 peer 建连。 */
struct jazz_device_info {
    char name[JAZZ_DEVICE_NAME_MAX];
    uint64_t guid;
    uint64_t max_mr_size;
    uint32_t max_qp_wr;
    uint32_t max_cqe;
    uint32_t max_sge;
    uint32_t max_read_outstanding;
    uint32_t max_read_responder;
    uint8_t port_count;
};

struct jazz_port_info {
    uint8_t port;
    uint8_t active;
    uint8_t ethernet;
    uint32_t active_mtu; /* 字节，不是 ibv_mtu 枚举。 */
    uint32_t max_message_size;
    uint32_t roce_v2_gid_count;
};

/* 本阶段所有 API 与生命周期操作必须由调用方串行使用。
 * 无后台线程，没有 MR/QP；不承诺 ABI 稳定。
 * create 要求输出句柄先初始化为 NULL；复制配置和名字，不打开设备。
 * prepare 不能重复或静默降级。
 * prepare 失败：error 保留首个准备错误，state 表示是否还需要清理。
 * destroy 失败保留句柄及资源，允许重试；成功才置 *engine=NULL。
 * 调用者不能通过复制裸指针延长句柄寿命。 */
void jazz_engine_config_init(struct jazz_engine_config *config);
int jazz_engine_create(const struct jazz_engine_config *config,
                       jazz_engine **out, struct jazz_error *error);
int jazz_engine_prepare(jazz_engine *engine, struct jazz_error *error);
int jazz_engine_get_state(const jazz_engine *engine, enum jazz_engine_state *out);
int jazz_engine_query_device(const jazz_engine *engine, size_t index,
                             struct jazz_device_info *out, struct jazz_error *error);
int jazz_engine_query_port(const jazz_engine *engine, size_t device_index,
                           uint8_t port, struct jazz_port_info *out,
                           struct jazz_error *error);
int jazz_engine_destroy(jazz_engine **engine, struct jazz_error *error);

#ifdef __cplusplus
}
#endif
#endif
