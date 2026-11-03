#ifndef JAZZ_ENGINE_INTERNAL_H
#define JAZZ_ENGINE_INTERNAL_H
#include "jazz/engine.h"
#include <infiniband/verbs.h>
#include <stdbool.h>

/* 窄测试接缝：只替换本阶段真实使用的设备调用，不模拟数据传输。
 * 返回值沿用 verbs/CM 契约；生产入口始终使用 jze_system_ops。 */
struct jze_device_ops {
    struct ibv_context **(*cm_list)(int *count);
    void (*cm_free)(struct ibv_context **list);
    struct ibv_device **(*verbs_list)(int *count);
    void (*verbs_free)(struct ibv_device **list);
    const char *(*name)(struct ibv_device *device);
    struct ibv_context *(*open)(struct ibv_device *device);
    int (*close)(struct ibv_context *context);
    int (*query_device)(struct ibv_context *, struct ibv_device_attr *);
    int (*query_port)(struct ibv_context *, uint8_t, struct ibv_port_attr *);
    int (*query_gid)(struct ibv_context *, uint32_t, uint32_t,
                     struct ibv_gid_entry *, uint32_t);
    struct ibv_pd *(*alloc_pd)(struct ibv_context *);
    int (*dealloc_pd)(struct ibv_pd *);
};

struct jze_device {
    char name[JAZZ_DEVICE_NAME_MAX];
    struct ibv_context *verbs;
    struct ibv_pd *pd; /* Jazz 自有；绝不借用 CM 默认 PD。 */
    bool owned_context;
    struct jazz_device_info info;
    struct jazz_port_info *ports;
};

struct jazz_engine {
    enum jazz_engine_state state;
    unsigned setup_modes;
    size_t device_count;
    struct jze_device *devices;
    const struct jze_device_ops *ops;
    /* CM 列表使借用 context 保活；直到所有自有 PD 释放才归还。 */
    struct ibv_context **cm_contexts;
    struct ibv_device **verbs_devices;
};

extern const struct jze_device_ops jze_system_ops;
int jze_fail(struct jazz_error *error, int errnum, const char *stage);
int jze_engine_create(const struct jazz_engine_config *config,
                      const struct jze_device_ops *ops,
                      jazz_engine **out, struct jazz_error *error);
int jze_devices_prepare(jazz_engine *engine, struct jazz_error *error);
int jze_devices_release(jazz_engine *engine, struct jazz_error *error);
#endif
