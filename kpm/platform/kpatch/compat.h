#ifndef STACKPLZ_KPATCH_COMPAT_H
#define STACKPLZ_KPATCH_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#include "stackplz/core.h"
#include "stackplz/debug.h"
#include "stackplz/maps.h"
#include "stackplz/profile.h"
#include "stackplz/task.h"
#include "../arm64/debug_regs.h"

#define SPZ_KPM_NAME SPZ_DEFAULT_KPM_MODULE_NAME
#define SPZ_KPM_VERSION "0.1.0"
#define SPZ_CONTROL_RESPONSE_MAX 4096U
#define SPZ_WORK_STORAGE_MAX 256U
#define SPZ_HOOK_COUNT 5U
#define SPZ_GATE_CLOSED UINT32_C(0x80000000)
#define SPZ_GATE_COUNT_MASK UINT32_C(0x7fffffff)

enum spz_hook_id {
    SPZ_HOOK_FINISH_TASK_SWITCH = 0,
    SPZ_HOOK_DO_EXIT,
    SPZ_HOOK_BREAKPOINT,
    SPZ_HOOK_WATCHPOINT,
    SPZ_HOOK_SINGLE_STEP,
};

enum spz_exception_kind {
    SPZ_EXCEPTION_BREAKPOINT = 1,
    SPZ_EXCEPTION_WATCHPOINT,
    SPZ_EXCEPTION_SINGLE_STEP,
};

enum spz_async_state {
    SPZ_ASYNC_FREE = 0,
    SPZ_ASYNC_PENDING,
    SPZ_ASYNC_RUNNING,
    SPZ_ASYNC_DONE,
};

enum spz_async_operation {
    SPZ_ASYNC_NONE = 0,
    SPZ_ASYNC_ENABLE,
    SPZ_ASYNC_DISABLE,
    SPZ_ASYNC_CLEAR,
    SPZ_ASYNC_AUDIT,
    SPZ_ASYNC_MAPS,
};

struct spz_hook_backend {
    void *context;
    int (*wrap)(void *context, uint64_t target, uint32_t argument_count,
                void *before, void *after, void *udata);
    void (*unwrap)(void *context, uint64_t target, void *before, void *after);
    void (*quiesce)(void *context);
};

struct spz_hook_callbacks {
    void *finish_before;
    void *finish_after;
    void *exit_before;
    void *break_before;
    void *watch_before;
    void *step_before;
};

struct spz_hook_set {
    struct spz_hook_backend backend;
    struct spz_hook_callbacks callbacks;
    uint64_t targets[SPZ_HOOK_COUNT];
    uint8_t installed[SPZ_HOOK_COUNT];
    void *udata;
};

struct spz_hook_fargs {
    uint64_t args[4];
    uint64_t ret;
    int skip_origin;
};

struct spz_platform_ops {
    void *context;
    int (*current_cpu)(void *context, uint32_t *cpu);
    const void *(*current_task)(void *context);
    uint64_t (*timestamp_ns)(void *context);
    int (*run_each_cpu)(void *context,
                        int (*callback)(void *callback_context, uint32_t cpu),
                        void *callback_context);
    int (*audit_owner)(void *context, uint64_t owner,
                       struct spz_event *event);
};

struct spz_async_backend {
    void *queue_context;
    int (*queue)(void *context);
    void *execute_context;
    int (*execute)(void *context, enum spz_async_operation operation,
                   uint32_t target_id);
    void *quiesce_context;
    int (*quiesce)(void *context);
};

struct spz_async_snapshot {
    enum spz_async_state state;
    enum spz_async_operation operation;
    uint64_t request_id;
    uint32_t target_id;
    int status;
};

struct spz_async_request {
    uint32_t state;
    uint32_t operation;
    uint64_t request_id;
    uint64_t next_request_id;
    uint32_t target_id;
    int status;
    struct spz_async_backend backend;
};

struct spz_module_state {
    const struct spz_device_profile *profile;
    struct spz_profile_runtime runtime;
    struct spz_profile_runtime_ops profile_ops;
    struct spz_platform_ops platform;
    struct spz_binding binding;
    struct spz_debug_controller debug;
    struct spz_ring ring;
    struct spz_maps maps;
    struct spz_debug_request breakpoint;
    struct spz_async_request async;
    struct spz_hook_set hooks;
    uint32_t breakpoint_configured;
    uint32_t breakpoint_enabled;
    uint32_t accepting_commands;
    uint32_t ready;
    uint32_t active_handlers;
    uint32_t binding_counter;
    uint32_t control_busy;
    uint32_t audit_events;
    char control_scratch[SPZ_CONTROL_RESPONSE_MAX];
};

struct spz_kpatch_runtime_context {
    struct spz_module_state *module;
    struct spz_arm64_owner_ops owner_ops;
    uint64_t per_cpu_offsets[SPZ_MAX_CPUS];
    uint64_t bp_on_reg;
    uint64_t wp_on_reg;
    uint64_t system_unbound_wq_address;
    uint64_t queue_work_on_address;
    uint64_t flush_work_address;
    uint64_t synchronize_rcu_tasks_address;
    uint64_t schedule_on_each_cpu_address;
    uint64_t time_address;
    uint64_t nofault_address;
    uint64_t show_map_vma_address;
    uint64_t find_vma_address;
    uint64_t mas_walk_address;
    uint64_t get_task_mm_address;
    uint64_t mmput_address;
    uint64_t mmap_read_lock_killable_address;
    uint64_t mmap_read_unlock_address;
    uint64_t get_task_struct_address;
    uint64_t put_task_struct_address;
    uint64_t vmalloc_address;
    uint64_t vfree_address;
    void *system_unbound_wq;
    int (*each_cpu_callback)(void *callback_context, uint32_t cpu);
    void *each_cpu_context;
    int each_cpu_status;
    uint32_t cpu_count;
    uint32_t each_cpu_active;
    uint32_t transport_ready;
    uint8_t work_storage[SPZ_WORK_STORAGE_MAX] __attribute__((aligned(8)));
};

struct spz_maps_kernel_ops {
    void *context;
    void *(*allocate)(void *context, uint32_t size);
    void (*free)(void *context, void *data);
    void *(*get_task_mm)(void *context, const void *task);
    void (*mmput)(void *context, void *mm);
    int (*mmap_read_lock_killable)(void *context, void *mm);
    void (*mmap_read_unlock)(void *context, void *mm);
    void *(*find_vma)(void *context, void *mm, uint64_t address);
    void *(*mas_walk)(void *context, void *iterator);
    void (*show_map_vma)(void *context, void *seq_file, void *vma);
};

int spz_hooks_install(struct spz_hook_set *hooks,
                      const struct spz_profile_runtime *runtime,
                      const struct spz_hook_backend *backend,
                      const struct spz_hook_callbacks *callbacks, void *udata);
void spz_hooks_remove(struct spz_hook_set *hooks);

void spz_async_init(struct spz_async_request *request,
                    const struct spz_async_backend *backend);
int spz_async_submit(struct spz_async_request *request,
                     enum spz_async_operation operation, uint32_t target_id,
                     uint64_t *request_id);
int spz_async_run(struct spz_async_request *request);
int spz_async_snapshot(const struct spz_async_request *request,
                       struct spz_async_snapshot *snapshot);
int spz_async_busy(const struct spz_async_request *request);

int spz_module_core_init(struct spz_module_state *module,
                         const struct spz_device_profile *profile,
                         const struct spz_profile_runtime *runtime,
                         const struct spz_profile_runtime_ops *profile_ops,
                         const struct spz_platform_ops *platform,
                         const struct spz_debug_ops *debug_ops,
                         const struct spz_async_backend *async_backend,
                         const struct spz_maps_backend *maps_backend);
void spz_module_finish_before(struct spz_module_state *module);
void spz_module_finish_after(struct spz_module_state *module);
void spz_module_exit_before(struct spz_module_state *module);
void spz_module_exception_before(struct spz_module_state *module,
                                 enum spz_exception_kind kind,
                                 struct spz_hook_fargs *fargs);
int spz_module_async_execute(void *context, enum spz_async_operation operation,
                             uint32_t target_id);
int spz_module_can_exit(const struct spz_module_state *module);
int spz_module_begin_exit(struct spz_module_state *module);
void spz_module_abort_exit(struct spz_module_state *module);
int spz_module_audit_cpu(struct spz_module_state *module, uint32_t cpu);

int spz_control_execute(struct spz_module_state *module, char *command,
                        size_t command_length, char *response,
                        size_t response_capacity);

int spz_kpatch_runtime_prepare(struct spz_module_state *module,
                              struct spz_kpatch_runtime_context *context,
                              const char *profile_id,
                              char *reason, size_t reason_capacity);
void spz_kpatch_runtime_zero(struct spz_module_state *module,
                            struct spz_kpatch_runtime_context *context);
int spz_kpatch_nofault_read(struct spz_kpatch_runtime_context *context,
                            uint64_t address, void *out, size_t length);
int spz_kpatch_per_cpu_address(uint64_t base, uint64_t per_cpu_offset,
                               uint64_t stride, uint32_t slot,
                               uint64_t *address);
int spz_kpatch_async_transport_init(struct spz_kpatch_runtime_context *context);
void spz_kpatch_async_transport_reset(
    struct spz_kpatch_runtime_context *context);
int spz_kpatch_async_queue(void *context);
int spz_kpatch_async_quiesce(void *context);
int spz_kpatch_run_each_cpu(
    void *context, int (*callback)(void *callback_context, uint32_t cpu),
    void *callback_context);
int spz_maps_render_vmas(const struct spz_device_profile *profile,
                         const struct spz_maps_kernel_ops *ops,
                         const void *task, uint8_t **data, uint32_t *length);
int spz_kpatch_maps_backend_init(struct spz_kpatch_runtime_context *context,
                                struct spz_maps_backend *backend);

#endif
