#ifndef STACKPLZ_PROFILE_H
#define STACKPLZ_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "device_profiles.h"

enum spz_profile_state {
    SPZ_PROFILE_UNINITIALIZED = 0,
    SPZ_PROFILE_REJECTED,
    SPZ_PROFILE_READY,
};

enum spz_profile_reason {
    SPZ_PROFILE_REASON_NONE = 0,
    SPZ_PROFILE_REASON_INVALID_ARGUMENT,
    SPZ_PROFILE_REASON_UNKNOWN_ID,
    SPZ_PROFILE_REASON_GENERATED_BOUNDS,
    SPZ_PROFILE_REASON_KERNEL_RELEASE,
    SPZ_PROFILE_REASON_PAGE_SIZE,
    SPZ_PROFILE_REASON_CPU_COUNT,
    SPZ_PROFILE_REASON_DEBUG_ARCH,
    SPZ_PROFILE_REASON_BRP_COUNT,
    SPZ_PROFILE_REASON_WRP_COUNT,
    SPZ_PROFILE_REASON_CONTEXT_COUNT,
    SPZ_PROFILE_REASON_SYMBOL_LINUX_BANNER,
    SPZ_PROFILE_REASON_SYMBOL_INIT_TASK,
    SPZ_PROFILE_REASON_SYMBOL_FINISH_TASK_SWITCH,
    SPZ_PROFILE_REASON_SYMBOL_DO_EXIT,
    SPZ_PROFILE_REASON_SYMBOL_BREAKPOINT_HANDLER,
    SPZ_PROFILE_REASON_SYMBOL_WATCHPOINT_HANDLER,
    SPZ_PROFILE_REASON_SYMBOL_SINGLE_STEP_HANDLER,
    SPZ_PROFILE_REASON_SYMBOL_BP_ON_REG,
    SPZ_PROFILE_REASON_SYMBOL_WP_ON_REG,
    SPZ_PROFILE_REASON_SYMBOL_PER_CPU_OFFSET,
    SPZ_PROFILE_REASON_SYMBOL_NR_CPU_IDS,
    SPZ_PROFILE_REASON_SYMBOL_SYSTEM_UNBOUND_WQ,
    SPZ_PROFILE_REASON_SYMBOL_QUEUE_WORK_ON,
    SPZ_PROFILE_REASON_SYMBOL_FLUSH_WORK,
    SPZ_PROFILE_REASON_SYMBOL_SYNCHRONIZE_RCU_TASKS,
    SPZ_PROFILE_REASON_SYMBOL_SCHEDULE_ON_EACH_CPU,
    SPZ_PROFILE_REASON_SYMBOL_KTIME_GET_MONO_FAST_NS,
    SPZ_PROFILE_REASON_SYMBOL_COPY_FROM_KERNEL_NOFAULT,
    SPZ_PROFILE_REASON_INIT_TASK_PID,
    SPZ_PROFILE_REASON_INIT_TASK_TGID,
    SPZ_PROFILE_REASON_INIT_TASK_COMM,
    SPZ_PROFILE_REASON_INIT_TASK_CRED,
    SPZ_PROFILE_REASON_CPU_TOPOLOGY_CHANGED,
};

enum spz_runtime_symbol {
    SPZ_SYMBOL_LINUX_BANNER = 0,
    SPZ_SYMBOL_INIT_TASK,
    SPZ_SYMBOL_FINISH_TASK_SWITCH,
    SPZ_SYMBOL_DO_EXIT,
    SPZ_SYMBOL_BREAKPOINT_HANDLER,
    SPZ_SYMBOL_WATCHPOINT_HANDLER,
    SPZ_SYMBOL_SINGLE_STEP_HANDLER,
    SPZ_SYMBOL_BP_ON_REG,
    SPZ_SYMBOL_WP_ON_REG,
    SPZ_SYMBOL_PER_CPU_OFFSET,
    SPZ_SYMBOL_NR_CPU_IDS,
    SPZ_SYMBOL_SYSTEM_UNBOUND_WQ,
    SPZ_SYMBOL_QUEUE_WORK_ON,
    SPZ_SYMBOL_FLUSH_WORK,
    SPZ_SYMBOL_SYNCHRONIZE_RCU_TASKS,
    SPZ_SYMBOL_SCHEDULE_ON_EACH_CPU,
    SPZ_SYMBOL_KTIME_GET_MONO_FAST_NS,
    SPZ_SYMBOL_COPY_FROM_KERNEL_NOFAULT,
    SPZ_SYMBOL_COUNT,
};

struct spz_profile_runtime_ops {
    void *context;
    uint64_t (*lookup_symbol)(void *context, const char *name);
    int (*read_memory)(void *context, uint64_t address, void *out, size_t length);
    int (*read_kernel_banner)(void *context, char *out, size_t capacity);
    uint32_t (*page_size)(void *context);
    uint32_t (*cpu_count)(void *context);
    uint64_t (*read_dfr0)(void *context);
    int (*read_debug_owner)(void *context, uint32_t cpu, uint8_t watchpoint,
                            uint32_t slot, uint64_t *owner);
};

struct spz_profile_runtime {
    const struct spz_device_profile *profile;
    uint64_t symbols[SPZ_SYMBOL_COUNT];
    uint64_t dfr0;
    uint32_t initial_cpu_count;
    enum spz_profile_state state;
    enum spz_profile_reason reason;
    uint8_t hooks_allowed;
};

const struct spz_device_profile *spz_profile_select(const char *id);
const char *spz_profile_reason_name(enum spz_profile_reason reason);
int spz_profile_validate(const struct spz_device_profile *profile,
                         const struct spz_profile_runtime_ops *ops,
                         struct spz_profile_runtime *runtime, char *reason,
                         size_t reason_capacity);
int spz_profile_cpu_topology_unchanged(const struct spz_profile_runtime *runtime,
                                       const struct spz_profile_runtime_ops *ops,
                                       char *reason, size_t reason_capacity);

#endif
