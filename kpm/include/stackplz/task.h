#ifndef STACKPLZ_TASK_H
#define STACKPLZ_TASK_H

#include <stddef.h>
#include <stdint.h>

#include "stackplz/core.h"
#include "stackplz/profile.h"

enum spz_binding_state {
    SPZ_BINDING_EMPTY = 0,
    SPZ_BINDING_PENDING,
    SPZ_BINDING_BOUND,
    SPZ_BINDING_STALE,
    SPZ_BINDING_EXITED,
};

struct spz_binding_request {
    uint32_t binding_id;
    uint32_t pid;
    enum spz_bind_mode mode;
    uint32_t uid;
    uint64_t start_boot_time;
    char comm[SPZ_COMM_LEN];
    uint8_t has_uid;
    uint8_t has_comm;
    uint8_t has_start_boot_time;
};

struct spz_task_identity {
    uint64_t generation;
    uint64_t task_cookie;
    uint32_t pid;
    uint32_t tgid;
    uint32_t uid;
    uint64_t start_time;
    uint64_t start_boot_time;
    char comm[SPZ_COMM_LEN];
};

struct spz_binding_snapshot {
    enum spz_binding_state state;
    uint64_t generation;
    struct spz_binding_request request;
    struct spz_task_identity identity;
};

struct spz_binding {
    uint64_t sequence;
    uint64_t generation;
    uint32_t state;
    struct spz_binding_request request;
    struct spz_task_identity identity;
};

void spz_binding_init(struct spz_binding *binding);
int spz_binding_set(struct spz_binding *binding, const struct spz_binding_request *request,
                    uint64_t *generation);
int spz_binding_observe_current(struct spz_binding *binding,
                                const struct spz_device_profile *profile,
                                const void *current_task, size_t available,
                                struct spz_task_identity *identity);
int spz_binding_matches_current(struct spz_binding *binding,
                                const struct spz_device_profile *profile,
                                const void *current_task, size_t available,
                                uint64_t expected_generation,
                                struct spz_task_identity *identity);
int spz_binding_mark_exit(struct spz_binding *binding,
                          const struct spz_device_profile *profile,
                          const void *current_task, size_t available);
int spz_binding_clear(struct spz_binding *binding);
int spz_binding_snapshot(const struct spz_binding *binding,
                         struct spz_binding_snapshot *snapshot);

#endif
