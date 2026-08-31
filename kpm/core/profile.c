#include "stackplz/platform.h"

#include "stackplz/abi.h"
#include "stackplz/profile.h"

#define SPZ_BANNER_CAPACITY_MAX 512U

struct spz_offset_width {
    uint32_t offset;
    uint32_t width;
};

static void spz_copy_reason(char *out, size_t capacity, const char *reason)
{
    size_t length;

    if (out == NULL || capacity == 0U)
        return;
    length = strlen(reason);
    if (length >= capacity)
        length = capacity - 1U;
    memcpy(out, reason, length);
    out[length] = '\0';
}

const char *spz_profile_reason_name(enum spz_profile_reason reason)
{
    switch (reason) {
    case SPZ_PROFILE_REASON_NONE: return "ok";
    case SPZ_PROFILE_REASON_INVALID_ARGUMENT: return "invalid_argument";
    case SPZ_PROFILE_REASON_UNKNOWN_ID: return "unknown_profile_id";
    case SPZ_PROFILE_REASON_GENERATED_BOUNDS: return "generated_profile_bounds";
    case SPZ_PROFILE_REASON_KERNEL_RELEASE: return "kernel_release_mismatch";
    case SPZ_PROFILE_REASON_PAGE_SIZE: return "page_size_mismatch";
    case SPZ_PROFILE_REASON_CPU_COUNT: return "cpu_count_mismatch";
    case SPZ_PROFILE_REASON_DEBUG_ARCH: return "debug_arch_mismatch";
    case SPZ_PROFILE_REASON_BRP_COUNT: return "breakpoint_count_mismatch";
    case SPZ_PROFILE_REASON_WRP_COUNT: return "watchpoint_count_mismatch";
    case SPZ_PROFILE_REASON_CONTEXT_COUNT: return "context_count_mismatch";
    case SPZ_PROFILE_REASON_SYMBOL_LINUX_BANNER: return "missing_symbol_linux_banner";
    case SPZ_PROFILE_REASON_SYMBOL_INIT_TASK: return "missing_symbol_init_task";
    case SPZ_PROFILE_REASON_SYMBOL_FINISH_TASK_SWITCH: return "missing_symbol_finish_task_switch";
    case SPZ_PROFILE_REASON_SYMBOL_DO_EXIT: return "missing_symbol_do_exit";
    case SPZ_PROFILE_REASON_SYMBOL_BREAKPOINT_HANDLER: return "missing_symbol_breakpoint_handler";
    case SPZ_PROFILE_REASON_SYMBOL_WATCHPOINT_HANDLER: return "missing_symbol_watchpoint_handler";
    case SPZ_PROFILE_REASON_SYMBOL_SINGLE_STEP_HANDLER: return "missing_symbol_single_step_handler";
    case SPZ_PROFILE_REASON_SYMBOL_BP_ON_REG: return "missing_symbol_bp_on_reg";
    case SPZ_PROFILE_REASON_SYMBOL_WP_ON_REG: return "missing_symbol_wp_on_reg";
    case SPZ_PROFILE_REASON_SYMBOL_PER_CPU_OFFSET: return "missing_symbol_per_cpu_offset";
    case SPZ_PROFILE_REASON_SYMBOL_NR_CPU_IDS: return "missing_symbol_nr_cpu_ids";
    case SPZ_PROFILE_REASON_SYMBOL_SYSTEM_UNBOUND_WQ: return "missing_symbol_system_unbound_wq";
    case SPZ_PROFILE_REASON_SYMBOL_QUEUE_WORK_ON: return "missing_symbol_queue_work_on";
    case SPZ_PROFILE_REASON_SYMBOL_FLUSH_WORK: return "missing_symbol_flush_work";
    case SPZ_PROFILE_REASON_SYMBOL_SYNCHRONIZE_RCU_TASKS: return "missing_symbol_synchronize_rcu_tasks";
    case SPZ_PROFILE_REASON_SYMBOL_SCHEDULE_ON_EACH_CPU: return "missing_symbol_schedule_on_each_cpu";
    case SPZ_PROFILE_REASON_SYMBOL_KTIME_GET_MONO_FAST_NS: return "missing_symbol_ktime_get_mono_fast_ns";
    case SPZ_PROFILE_REASON_SYMBOL_COPY_FROM_KERNEL_NOFAULT: return "missing_symbol_copy_from_kernel_nofault";
    case SPZ_PROFILE_REASON_INIT_TASK_PID: return "init_task_pid_mismatch";
    case SPZ_PROFILE_REASON_INIT_TASK_TGID: return "init_task_tgid_mismatch";
    case SPZ_PROFILE_REASON_INIT_TASK_COMM: return "init_task_comm_mismatch";
    case SPZ_PROFILE_REASON_INIT_TASK_CRED: return "init_task_cred_mismatch";
    case SPZ_PROFILE_REASON_CPU_TOPOLOGY_CHANGED: return "cpu_topology_changed";
    default: return "unknown_profile_reason";
    }
}

static int spz_reject(struct spz_profile_runtime *runtime, enum spz_profile_reason reason,
                      char *reason_text, size_t reason_capacity, int error)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->state = SPZ_PROFILE_REJECTED;
    runtime->reason = reason;
    spz_copy_reason(reason_text, reason_capacity, spz_profile_reason_name(reason));
    return error;
}

const struct spz_device_profile *spz_profile_select(const char *id)
{
    size_t index;

    if (id == NULL || id[0] == '\0')
        return NULL;
    for (index = 0U; index < SPZ_DEVICE_PROFILE_COUNT; index++) {
        if (strcmp(SPZ_DEVICE_PROFILES[index].id, id) == 0)
            return &SPZ_DEVICE_PROFILES[index];
    }
    return NULL;
}

static int spz_field_fits(uint32_t total, struct spz_offset_width field)
{
    return field.offset <= total && field.width <= total - field.offset;
}

static int spz_profile_bounds_valid(const struct spz_device_profile *profile)
{
    const struct spz_offset_width task_fields[] = {
        {profile->task.thread_info_flags, 8U}, {profile->task.cpu, 4U},
        {profile->task.state, 8U}, {profile->task.usage, 4U},
        {profile->task.tasks, 16U}, {profile->task.mm, 8U},
        {profile->task.active_mm, 8U}, {profile->task.exit_state, 4U},
        {profile->task.pid, 4U}, {profile->task.tgid, 4U},
        {profile->task.real_parent, 8U}, {profile->task.parent, 8U},
        {profile->task.thread_pid, 8U}, {profile->task.pid_links, 16U},
        {profile->task.thread_node, 16U}, {profile->task.start_time, 8U},
        {profile->task.start_boottime, 8U}, {profile->task.real_cred, 8U},
        {profile->task.cred, 8U}, {profile->task.comm, profile->kernel.task_comm_len},
        {profile->task.signal, 8U}, {profile->task.perf_event_ctxp, 8U},
        {profile->task.perf_event_list, 16U}, {profile->task.thread, 1U},
    };
    const struct spz_offset_width perf_fields[] = {
        {profile->perf_event.state, 4U}, {profile->perf_event.attr, 1U},
        {profile->perf_event.attr_bp_type, 4U}, {profile->perf_event.attr_bp_addr, 8U},
        {profile->perf_event.attr_bp_len, 8U}, {profile->perf_event.hw, 1U},
        {profile->perf_event.arch_address, 8U}, {profile->perf_event.arch_trigger, 8U},
        {profile->perf_event.arch_ctrl, 4U}, {profile->perf_event.hw_target, 8U},
        {profile->perf_event.ctx, 8U}, {profile->perf_event.oncpu, 4U},
        {profile->perf_event.cpu, 4U}, {profile->perf_event.owner, 8U},
        {profile->perf_event.context_task, 8U},
    };
    size_t index;
    uint64_t dfr0;

    if (profile->kernel.task_struct_size == 0U || profile->kernel.cred_size == 0U ||
        profile->kernel.perf_event_size == 0U)
        return 0;
    if (profile->kernel.task_comm_len != SPZ_COMM_LEN)
        return 0;
    if (profile->kernel.linux_banner_capacity < 64U ||
        profile->kernel.linux_banner_capacity > SPZ_BANNER_CAPACITY_MAX)
        return 0;
    if (profile->kernel.page_size != 4096U && profile->kernel.page_size != 16384U &&
        profile->kernel.page_size != 65536U)
        return 0;
    if (profile->kernel.va_bits < 32U || profile->kernel.va_bits > 52U)
        return 0;
    for (index = 0U; index < sizeof(task_fields) / sizeof(task_fields[0]); index++) {
        if (!spz_field_fits(profile->kernel.task_struct_size, task_fields[index]))
            return 0;
    }
    if (!spz_field_fits(profile->kernel.cred_size,
                        (struct spz_offset_width){profile->cred.uid, 4U}))
        return 0;
    for (index = 0U; index < sizeof(perf_fields) / sizeof(perf_fields[0]); index++) {
        if (!spz_field_fits(profile->kernel.perf_event_size, perf_fields[index]))
            return 0;
    }
    if (profile->task.signal_thread_head > 4096U - 16U)
        return 0;
    if (profile->layout.work_struct_size < 32U ||
        profile->layout.work_struct_size > 256U ||
        !spz_field_fits(profile->layout.work_struct_size,
                        (struct spz_offset_width){profile->layout.work_data, 8U}) ||
        !spz_field_fits(profile->layout.work_struct_size,
                        (struct spz_offset_width){profile->layout.work_entry, 16U}) ||
        !spz_field_fits(profile->layout.work_struct_size,
                        (struct spz_offset_width){profile->layout.work_func, 8U}) ||
        profile->layout.pt_regs_size < 272U || profile->layout.pt_regs_size > 1024U ||
        !spz_field_fits(profile->layout.pt_regs_size,
                        (struct spz_offset_width){profile->layout.pt_regs_regs, 31U * 8U}) ||
        !spz_field_fits(profile->layout.pt_regs_size,
                        (struct spz_offset_width){profile->layout.pt_regs_sp, 8U}) ||
        !spz_field_fits(profile->layout.pt_regs_size,
                        (struct spz_offset_width){profile->layout.pt_regs_pc, 8U}) ||
        !spz_field_fits(profile->layout.pt_regs_size,
                        (struct spz_offset_width){profile->layout.pt_regs_pstate, 8U}) ||
        profile->layout.per_cpu_pointer_stride != 8U)
        return 0;
    if (profile->debug.debug_arch < 6U || profile->debug.debug_arch > 15U ||
        profile->debug.brps == 0U || profile->debug.brps > 16U ||
        profile->debug.wrps == 0U || profile->debug.wrps > 16U ||
        profile->debug.ctx_cmps > 16U || profile->debug.max_cpus == 0U ||
        profile->debug.max_cpus > SPZ_MAX_CPUS)
        return 0;
    dfr0 = profile->debug.dfr0;
    if ((uint32_t)(dfr0 & 0xfU) != profile->debug.debug_arch ||
        (uint32_t)((dfr0 >> 12U) & 0xfU) + 1U != profile->debug.brps ||
        (uint32_t)((dfr0 >> 20U) & 0xfU) + 1U != profile->debug.wrps ||
        (uint32_t)((dfr0 >> 28U) & 0xfU) + 1U != profile->debug.ctx_cmps)
        return 0;
    if ((profile->hooks.finish_task_switch_args != 1U &&
         profile->hooks.finish_task_switch_args != 3U) ||
        (profile->hooks.do_exit_args != 1U && profile->hooks.do_exit_args != 3U) ||
        (profile->hooks.breakpoint_handler_args != 1U &&
         profile->hooks.breakpoint_handler_args != 3U) ||
        (profile->hooks.watchpoint_handler_args != 1U &&
         profile->hooks.watchpoint_handler_args != 3U) ||
        (profile->hooks.single_step_handler_args != 1U &&
         profile->hooks.single_step_handler_args != 3U))
        return 0;
    return 1;
}

static int spz_release_matches_banner(const char *banner, const char *release)
{
    static const char prefix[] = "Linux version ";
    size_t prefix_length = sizeof(prefix) - 1U;
    size_t release_length;

    if (banner == NULL || release == NULL)
        return 0;
    release_length = strlen(release);
    if (strncmp(banner, prefix, prefix_length) != 0)
        return 0;
    if (strncmp(banner + prefix_length, release, release_length) != 0)
        return 0;
    return banner[prefix_length + release_length] == ' ' ||
           banner[prefix_length + release_length] == '\0';
}

static int spz_read_init_task(const struct spz_device_profile *profile,
                              const struct spz_profile_runtime_ops *ops,
                              uint64_t init_task,
                              enum spz_profile_reason *reason)
{
    uint32_t pid;
    uint32_t tgid;
    uint32_t uid;
    uint64_t cred;
    uint64_t real_cred;
    char comm[SPZ_COMM_LEN];

#define SPZ_READ_MEMBER(base, offset, destination)                                      \
    ((base) > UINT64_MAX - (uint64_t)(offset) ? -EOVERFLOW :                            \
     ops->read_memory(ops->context, (base) + (uint64_t)(offset),                        \
                      (destination), sizeof(*(destination))))

    if (SPZ_READ_MEMBER(init_task, profile->task.pid, &pid) != 0 ||
        pid != 0U) {
        *reason = SPZ_PROFILE_REASON_INIT_TASK_PID;
        return -EINVAL;
    }
    if (SPZ_READ_MEMBER(init_task, profile->task.tgid, &tgid) != 0 ||
        tgid != 0U) {
        *reason = SPZ_PROFILE_REASON_INIT_TASK_TGID;
        return -EINVAL;
    }
    memset(comm, 0, sizeof(comm));
    if (init_task > UINT64_MAX - profile->task.comm ||
        ops->read_memory(ops->context, init_task + profile->task.comm, comm, sizeof(comm)) != 0 ||
        memcmp(comm, "swapper", sizeof("swapper") - 1U) != 0) {
        *reason = SPZ_PROFILE_REASON_INIT_TASK_COMM;
        return -EINVAL;
    }
    if (SPZ_READ_MEMBER(init_task, profile->task.cred, &cred) != 0 ||
        SPZ_READ_MEMBER(init_task, profile->task.real_cred, &real_cred) != 0 ||
        cred == 0U || cred != real_cred || cred > UINT64_MAX - profile->cred.uid ||
        ops->read_memory(ops->context, cred + profile->cred.uid, &uid, sizeof(uid)) != 0 ||
        uid != 0U) {
        *reason = SPZ_PROFILE_REASON_INIT_TASK_CRED;
        return -EINVAL;
    }
#undef SPZ_READ_MEMBER
    return 0;
}

int spz_profile_validate(const struct spz_device_profile *profile,
                         const struct spz_profile_runtime_ops *ops,
                         struct spz_profile_runtime *runtime, char *reason,
                         size_t reason_capacity)
{
    static const enum spz_profile_reason symbol_reasons[SPZ_SYMBOL_COUNT] = {
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
    };
    struct spz_profile_runtime candidate;
    const struct spz_device_profile *selected;
    const char *symbol_names[SPZ_SYMBOL_COUNT];
    char banner[SPZ_BANNER_CAPACITY_MAX];
    enum spz_profile_reason init_reason = SPZ_PROFILE_REASON_NONE;
    uint64_t dfr0;
    uint32_t cpu_count;
    uint32_t banner_capacity;
    int banner_result;
    size_t index;

    if (runtime == NULL) {
        spz_copy_reason(reason, reason_capacity,
                        spz_profile_reason_name(SPZ_PROFILE_REASON_INVALID_ARGUMENT));
        return -EINVAL;
    }
    memset(runtime, 0, sizeof(*runtime));
    if (profile == NULL || ops == NULL || ops->lookup_symbol == NULL ||
        ops->read_memory == NULL || ops->read_kernel_banner == NULL ||
        ops->page_size == NULL || ops->cpu_count == NULL || ops->read_dfr0 == NULL ||
        ops->read_debug_owner == NULL)
        return spz_reject(runtime, SPZ_PROFILE_REASON_INVALID_ARGUMENT, reason,
                          reason_capacity, -EINVAL);
    selected = spz_profile_select(profile->id);
    if (selected == NULL)
        return spz_reject(runtime, SPZ_PROFILE_REASON_UNKNOWN_ID, reason,
                          reason_capacity, -ENOENT);
    if (!spz_profile_bounds_valid(profile))
        return spz_reject(runtime, SPZ_PROFILE_REASON_GENERATED_BOUNDS, reason,
                          reason_capacity, -ERANGE);
    if (ops->page_size(ops->context) != profile->kernel.page_size)
        return spz_reject(runtime, SPZ_PROFILE_REASON_PAGE_SIZE, reason,
                          reason_capacity, -EINVAL);
    cpu_count = ops->cpu_count(ops->context);
    if (cpu_count == 0U || cpu_count > profile->debug.max_cpus || cpu_count > SPZ_MAX_CPUS)
        return spz_reject(runtime, SPZ_PROFILE_REASON_CPU_COUNT, reason,
                          reason_capacity, -ERANGE);
    dfr0 = ops->read_dfr0(ops->context);
    if ((uint32_t)(dfr0 & 0xfU) != profile->debug.debug_arch)
        return spz_reject(runtime, SPZ_PROFILE_REASON_DEBUG_ARCH, reason,
                          reason_capacity, -EINVAL);
    if ((uint32_t)((dfr0 >> 12U) & 0xfU) + 1U != profile->debug.brps)
        return spz_reject(runtime, SPZ_PROFILE_REASON_BRP_COUNT, reason,
                          reason_capacity, -EINVAL);
    if ((uint32_t)((dfr0 >> 20U) & 0xfU) + 1U != profile->debug.wrps)
        return spz_reject(runtime, SPZ_PROFILE_REASON_WRP_COUNT, reason,
                          reason_capacity, -EINVAL);
    if ((uint32_t)((dfr0 >> 28U) & 0xfU) + 1U != profile->debug.ctx_cmps)
        return spz_reject(runtime, SPZ_PROFILE_REASON_CONTEXT_COUNT, reason,
                          reason_capacity, -EINVAL);

    symbol_names[SPZ_SYMBOL_LINUX_BANNER] = profile->symbols.linux_banner;
    symbol_names[SPZ_SYMBOL_INIT_TASK] = profile->symbols.init_task;
    symbol_names[SPZ_SYMBOL_FINISH_TASK_SWITCH] = profile->symbols.finish_task_switch;
    symbol_names[SPZ_SYMBOL_DO_EXIT] = profile->symbols.do_exit;
    symbol_names[SPZ_SYMBOL_BREAKPOINT_HANDLER] = profile->symbols.breakpoint_handler;
    symbol_names[SPZ_SYMBOL_WATCHPOINT_HANDLER] = profile->symbols.watchpoint_handler;
    symbol_names[SPZ_SYMBOL_SINGLE_STEP_HANDLER] = profile->symbols.single_step_handler;
    symbol_names[SPZ_SYMBOL_BP_ON_REG] = profile->symbols.bp_on_reg;
    symbol_names[SPZ_SYMBOL_WP_ON_REG] = profile->symbols.wp_on_reg;
    symbol_names[SPZ_SYMBOL_PER_CPU_OFFSET] = profile->symbols.per_cpu_offset;
    symbol_names[SPZ_SYMBOL_NR_CPU_IDS] = profile->symbols.nr_cpu_ids;
    symbol_names[SPZ_SYMBOL_SYSTEM_UNBOUND_WQ] = profile->symbols.system_unbound_wq;
    symbol_names[SPZ_SYMBOL_QUEUE_WORK_ON] = profile->symbols.queue_work_on;
    symbol_names[SPZ_SYMBOL_FLUSH_WORK] = profile->symbols.flush_work;
    symbol_names[SPZ_SYMBOL_SYNCHRONIZE_RCU_TASKS] =
        profile->symbols.synchronize_rcu_tasks;
    symbol_names[SPZ_SYMBOL_SCHEDULE_ON_EACH_CPU] = profile->symbols.schedule_on_each_cpu;
    symbol_names[SPZ_SYMBOL_KTIME_GET_MONO_FAST_NS] = profile->symbols.ktime_get_mono_fast_ns;
    symbol_names[SPZ_SYMBOL_COPY_FROM_KERNEL_NOFAULT] =
        profile->symbols.copy_from_kernel_nofault;

    memset(&candidate, 0, sizeof(candidate));
    for (index = 0U; index < SPZ_SYMBOL_COUNT; index++) {
        if (symbol_names[index] == NULL || symbol_names[index][0] == '\0')
            return spz_reject(runtime, symbol_reasons[index], reason,
                              reason_capacity, -ENOENT);
        candidate.symbols[index] = ops->lookup_symbol(ops->context, symbol_names[index]);
        if (candidate.symbols[index] == 0U)
            return spz_reject(runtime, symbol_reasons[index], reason,
                              reason_capacity, -ENOENT);
    }

    memset(banner, 0, sizeof(banner));
    banner_capacity = profile->kernel.linux_banner_capacity;
    banner_result = ops->read_kernel_banner(ops->context, banner, banner_capacity);
    if (banner_result == -ENOSPC) {
        if (profile->quirks.linux_banner_prefix_ok == 0U)
            return spz_reject(runtime, SPZ_PROFILE_REASON_KERNEL_RELEASE, reason,
                              reason_capacity, -EINVAL);
    } else if (banner_result != 0) {
        return spz_reject(runtime, SPZ_PROFILE_REASON_KERNEL_RELEASE, reason,
                          reason_capacity, -EINVAL);
    }
    if (memchr(banner, '\0', banner_capacity) == NULL ||
        !spz_release_matches_banner(banner, profile->kernel.release))
        return spz_reject(runtime, SPZ_PROFILE_REASON_KERNEL_RELEASE, reason,
                          reason_capacity, -EINVAL);
    if (spz_read_init_task(profile, ops, candidate.symbols[SPZ_SYMBOL_INIT_TASK],
                           &init_reason) != 0)
        return spz_reject(runtime, init_reason, reason, reason_capacity, -EINVAL);

    candidate.profile = profile;
    candidate.dfr0 = dfr0;
    candidate.initial_cpu_count = cpu_count;
    candidate.state = SPZ_PROFILE_READY;
    candidate.reason = SPZ_PROFILE_REASON_NONE;
    candidate.hooks_allowed = 1U;
    *runtime = candidate;
    spz_copy_reason(reason, reason_capacity, spz_profile_reason_name(SPZ_PROFILE_REASON_NONE));
    return 0;
}

int spz_profile_cpu_topology_unchanged(const struct spz_profile_runtime *runtime,
                                       const struct spz_profile_runtime_ops *ops,
                                       char *reason, size_t reason_capacity)
{
    if (runtime == NULL || ops == NULL || ops->cpu_count == NULL ||
        runtime->state != SPZ_PROFILE_READY || runtime->hooks_allowed == 0U) {
        spz_copy_reason(reason, reason_capacity,
                        spz_profile_reason_name(SPZ_PROFILE_REASON_INVALID_ARGUMENT));
        return -EINVAL;
    }
    if (ops->cpu_count(ops->context) != runtime->initial_cpu_count) {
        spz_copy_reason(reason, reason_capacity,
                        spz_profile_reason_name(SPZ_PROFILE_REASON_CPU_TOPOLOGY_CHANGED));
        return -ESTALE;
    }
    spz_copy_reason(reason, reason_capacity, spz_profile_reason_name(SPZ_PROFILE_REASON_NONE));
    return 0;
}
