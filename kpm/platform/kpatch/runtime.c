#include "stackplz/platform.h"

#include "compat.h"

int spz_kpatch_per_cpu_address(uint64_t base, uint64_t per_cpu_offset,
                               uint64_t stride, uint32_t slot,
                               uint64_t *address)
{
    uint64_t per_cpu_base;

    if (address == NULL)
        return -EINVAL;
    if (base == 0U || stride == 0U)
        return -ERANGE;
    per_cpu_base = base + per_cpu_offset;
    if (per_cpu_base == 0U ||
        (uint64_t)slot > (UINT64_MAX - per_cpu_base) / stride)
        return -ERANGE;
    *address = per_cpu_base + (uint64_t)slot * stride;
    return *address == 0U ? -ERANGE : 0;
}

static uint64_t spz_module_timestamp(struct spz_module_state *module)
{
    if (module->platform.timestamp_ns == NULL)
        return 0U;
    return module->platform.timestamp_ns(module->platform.context);
}

static void spz_module_publish_integrity(struct spz_module_state *module,
                                         uint32_t cpu, int status,
                                         uint16_t flags)
{
    struct spz_event event;

    if (cpu >= module->runtime.initial_cpu_count || cpu >= SPZ_MAX_CPUS)
        return;
    memset(&event, 0, sizeof(event));
    event.type = SPZ_EVENT_INTEGRITY;
    event.flags = flags;
    event.timestamp = spz_module_timestamp(module);
    event.value = (uint64_t)(int64_t)status;
    (void)spz_ring_push(&module->ring, cpu, &event);
}

int spz_module_core_init(struct spz_module_state *module,
                         const struct spz_device_profile *profile,
                         const struct spz_profile_runtime *runtime,
                         const struct spz_profile_runtime_ops *profile_ops,
                         const struct spz_platform_ops *platform,
                         const struct spz_debug_ops *debug_ops,
                         const struct spz_async_backend *async_backend,
                         const struct spz_maps_backend *maps_backend)
{
    struct spz_async_backend effective_async;
    int result;

    if (module == NULL || profile == NULL || runtime == NULL ||
        profile_ops == NULL || platform == NULL || debug_ops == NULL ||
        async_backend == NULL || async_backend->queue == NULL ||
        runtime->profile != profile || runtime->state != SPZ_PROFILE_READY ||
        runtime->hooks_allowed == 0U || runtime->initial_cpu_count == 0U ||
        runtime->initial_cpu_count > SPZ_MAX_CPUS ||
        platform->current_cpu == NULL || platform->current_task == NULL ||
        platform->timestamp_ns == NULL || platform->run_each_cpu == NULL)
        return -EINVAL;
    memset(module, 0, sizeof(*module));
    module->profile = profile;
    module->runtime = *runtime;
    module->profile_ops = *profile_ops;
    module->platform = *platform;
    spz_binding_init(&module->binding);
    spz_ring_init(&module->ring);
    result = spz_maps_init(&module->maps, maps_backend);
    if (result != 0) {
        memset(module, 0, sizeof(*module));
        return result;
    }
    result = spz_debug_controller_init(&module->debug, debug_ops,
                                       runtime->initial_cpu_count);
    if (result != 0) {
        memset(module, 0, sizeof(*module));
        return result;
    }
    effective_async = *async_backend;
    effective_async.execute_context = module;
    effective_async.execute = spz_module_async_execute;
    spz_async_init(&module->async, &effective_async);
    module->accepting_commands = 1U;
    module->ready = 1U;
    return 0;
}

static int spz_module_current_cpu(struct spz_module_state *module, uint32_t *cpu)
{
    int result;

    result = module->platform.current_cpu(module->platform.context, cpu);
    if (result != 0)
        return result;
    if (*cpu >= module->runtime.initial_cpu_count || *cpu >= module->debug.cpu_count)
        return -ERANGE;
    return 0;
}

static int spz_module_current_target(struct spz_module_state *module,
                                     struct spz_debug_target *target)
{
    struct spz_binding_snapshot snapshot;
    const void *current;
    int result;

    if (target == NULL)
        return -EINVAL;
    result = spz_binding_snapshot(&module->binding, &snapshot);
    if (result != 0)
        return result;
    if (snapshot.state != SPZ_BINDING_BOUND)
        return 0;
    current = module->platform.current_task(module->platform.context);
    if (current == NULL)
        return -EFAULT;
    memset(target, 0, sizeof(*target));
    result = spz_binding_matches_current(&module->binding, module->profile,
                                         current,
                                         module->profile->kernel.task_struct_size,
                                         snapshot.generation,
                                         &target->identity);
    if (result != 1)
        return result;
    target->binding_id = snapshot.request.binding_id;
    return 1;
}

static int spz_module_arm_live_current(struct spz_module_state *module,
                                       uint32_t cpu)
{
    struct spz_debug_target target;
    int result;

    if (__atomic_load_n(&module->breakpoint_enabled, __ATOMIC_ACQUIRE) == 0U ||
        __atomic_load_n(&module->breakpoint_configured, __ATOMIC_ACQUIRE) == 0U)
        return 0;
    result = spz_module_current_target(module, &target);
    if (result != 1)
        return result < 0 ? result : 0;
    return spz_debug_arm_current(&module->debug, cpu, &module->breakpoint,
                                 &target);
}

static int spz_handler_enter(struct spz_module_state *module)
{
    uint32_t observed = __atomic_load_n(&module->active_handlers,
                                        __ATOMIC_ACQUIRE);

    for (;;) {
        uint32_t desired;

        if ((observed & SPZ_GATE_CLOSED) != 0U ||
            (observed & SPZ_GATE_COUNT_MASK) == SPZ_GATE_COUNT_MASK)
            return 0;
        desired = observed + 1U;
        if (__atomic_compare_exchange_n(&module->active_handlers, &observed,
                                        desired, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return 1;
    }
}

static void spz_handler_leave(struct spz_module_state *module)
{
    (void)__atomic_sub_fetch(&module->active_handlers, 1U, __ATOMIC_ACQ_REL);
}

void spz_module_finish_before(struct spz_module_state *module)
{
    uint32_t cpu;
    int result;

    if (module == NULL || __atomic_load_n(&module->ready, __ATOMIC_ACQUIRE) == 0U)
        return;
    if (!spz_handler_enter(module))
        return;
    result = spz_module_current_cpu(module, &cpu);
    if (result == 0) {
        result = spz_debug_restore_cpu(&module->debug, cpu);
        if (result != 0)
            spz_module_publish_integrity(module, cpu, result,
                                         SPZ_EVENT_FLAG_INTERFERENCE);
    }
    spz_handler_leave(module);
}

void spz_module_finish_after(struct spz_module_state *module)
{
    struct spz_task_identity identity;
    const void *current;
    uint32_t cpu;
    int observed;
    int result;

    if (module == NULL || __atomic_load_n(&module->ready, __ATOMIC_ACQUIRE) == 0U)
        return;
    if (!spz_handler_enter(module))
        return;
    result = spz_module_current_cpu(module, &cpu);
    current = module->platform.current_task(module->platform.context);
    if (result != 0 || current == NULL)
        goto out;
    observed = spz_binding_observe_current(
        &module->binding, module->profile, current,
        module->profile->kernel.task_struct_size, &identity);
    if (observed < 0) {
        spz_module_publish_integrity(module, cpu, observed, 0U);
        goto out;
    }
    if (observed == 1 && !spz_maps_has_task(&module->maps)) {
        result = spz_maps_capture_task(&module->maps, current,
                                       identity.generation);
        if (result != 0 && result != -EOPNOTSUPP && result != -EBUSY)
            spz_module_publish_integrity(module, cpu, result, 0U);
    }
    result = spz_module_arm_live_current(module, cpu);
    if (result != 0 && result != -EBUSY)
        spz_module_publish_integrity(module, cpu, result,
                                     SPZ_EVENT_FLAG_INTERFERENCE);
out:
    spz_handler_leave(module);
}

void spz_module_exit_before(struct spz_module_state *module)
{
    const void *current;

    if (module == NULL || __atomic_load_n(&module->ready, __ATOMIC_ACQUIRE) == 0U)
        return;
    if (!spz_handler_enter(module))
        return;
    current = module->platform.current_task(module->platform.context);
    if (current != NULL)
        (void)spz_binding_mark_exit(&module->binding, module->profile, current,
                                    module->profile->kernel.task_struct_size);
    spz_handler_leave(module);
}

static int spz_frame_from_regs(const struct spz_device_profile *profile,
                               uint64_t regs_address, uint64_t far, uint64_t esr,
                               struct spz_debug_frame *frame)
{
    const uint8_t *regs;
    uint32_t index;

    if (profile == NULL || frame == NULL || regs_address == 0U)
        return -EINVAL;
    regs = (const uint8_t *)(uintptr_t)regs_address;
    memset(frame, 0, sizeof(*frame));
    for (index = 0U; index < 31U; index++)
        memcpy(&frame->x[index], regs + profile->layout.pt_regs_regs + index * 8U,
               sizeof(frame->x[index]));
    memcpy(&frame->sp, regs + profile->layout.pt_regs_sp, sizeof(frame->sp));
    memcpy(&frame->pc, regs + profile->layout.pt_regs_pc, sizeof(frame->pc));
    memcpy(&frame->pstate, regs + profile->layout.pt_regs_pstate,
           sizeof(frame->pstate));
    frame->far = far;
    frame->esr = esr;
    frame->kernel_mode = (frame->pstate & UINT64_C(0xf)) != 0U ? 1U : 0U;
    return 0;
}

static void spz_frame_writeback(const struct spz_device_profile *profile,
                                uint64_t regs_address,
                                const struct spz_debug_frame *frame)
{
    uint8_t *regs = (uint8_t *)(uintptr_t)regs_address;

    memcpy(regs + profile->layout.pt_regs_pstate, &frame->pstate,
           sizeof(frame->pstate));
}

void spz_module_exception_before(struct spz_module_state *module,
                                 enum spz_exception_kind kind,
                                 struct spz_hook_fargs *fargs)
{
    struct spz_debug_target target;
    struct spz_debug_frame frame;
    struct spz_event event;
    uint32_t cpu;
    int result;

    if (module == NULL || fargs == NULL ||
        __atomic_load_n(&module->ready, __ATOMIC_ACQUIRE) == 0U ||
        __atomic_load_n(&module->breakpoint_enabled, __ATOMIC_ACQUIRE) == 0U)
        return;
    if (!spz_handler_enter(module))
        return;
    result = spz_module_current_cpu(module, &cpu);
    if (result != 0)
        goto out;
    result = spz_module_current_target(module, &target);
    if (result != 1)
        goto out;
    result = spz_frame_from_regs(module->profile, fargs->args[2], fargs->args[0],
                                 fargs->args[1], &frame);
    if (result != 0)
        goto out;

    memset(&event, 0, sizeof(event));
    if (kind == SPZ_EXCEPTION_BREAKPOINT)
        result = spz_debug_handle_break(&module->debug, cpu, &target, &frame,
                                        &event);
    else if (kind == SPZ_EXCEPTION_WATCHPOINT)
        result = spz_debug_handle_watch(&module->debug, cpu, &target, &frame,
                                        &event);
    else if (kind == SPZ_EXCEPTION_SINGLE_STEP)
        result = spz_debug_handle_step(&module->debug, cpu, &target, &frame);
    else
        result = -EINVAL;

    if (result == SPZ_DEBUG_CONSUMED || result == SPZ_DEBUG_FORWARD_ORIGINAL)
        spz_frame_writeback(module->profile, fargs->args[2], &frame);
    if ((kind == SPZ_EXCEPTION_BREAKPOINT || kind == SPZ_EXCEPTION_WATCHPOINT) &&
        result == SPZ_DEBUG_CONSUMED) {
        event.timestamp = spz_module_timestamp(module);
        (void)spz_ring_push(&module->ring, cpu, &event);
        if (module->breakpoint.mode == SPZ_BREAK_ONCE)
            __atomic_store_n(&module->breakpoint_enabled, 0U,
                             __ATOMIC_RELEASE);
    }
    if (result == SPZ_DEBUG_CONSUMED) {
        fargs->skip_origin = 1;
        fargs->ret = 0U;
    } else if (result < 0) {
        spz_module_publish_integrity(module, cpu, result,
                                     SPZ_EVENT_FLAG_INTERFERENCE);
    }
out:
    spz_handler_leave(module);
}

static int spz_restore_cpu_callback(void *opaque, uint32_t cpu)
{
    return spz_debug_restore_cpu(&((struct spz_module_state *)opaque)->debug,
                                 cpu);
}

static int spz_enable_cpu_callback(void *opaque, uint32_t cpu)
{
    struct spz_module_state *module = (struct spz_module_state *)opaque;
    uint32_t current_cpu;
    int result = spz_module_current_cpu(module, &current_cpu);

    if (result != 0)
        return result;
    if (current_cpu != cpu)
        return -EXDEV;
    return spz_module_arm_live_current(module, cpu);
}

static int spz_audit_cpu_callback(void *opaque, uint32_t cpu)
{
    return spz_module_audit_cpu((struct spz_module_state *)opaque, cpu);
}

static int spz_run_each_cpu(struct spz_module_state *module,
                            int (*callback)(void *, uint32_t))
{
    return module->platform.run_each_cpu(module->platform.context, callback,
                                         module);
}

static int spz_all_debug_empty(const struct spz_module_state *module)
{
    uint32_t cpu;

    for (cpu = 0U; cpu < module->debug.cpu_count; cpu++) {
        const struct spz_debug_cpu_state *state =
            spz_debug_cpu_state(&module->debug, cpu);

        if (state == NULL || state->state != SPZ_DEBUG_EMPTY)
            return 0;
    }
    return 1;
}

int spz_module_async_execute(void *context, enum spz_async_operation operation,
                             uint32_t target_id)
{
    struct spz_module_state *module = (struct spz_module_state *)context;
    struct spz_binding_snapshot binding;
    unsigned int attempt;
    int result;

    if (module == NULL || module->ready == 0U)
        return -EINVAL;
    if (operation == SPZ_ASYNC_MAPS) {
        result = spz_binding_snapshot(&module->binding, &binding);
        if (result != 0)
            return result;
        if (binding.state != SPZ_BINDING_BOUND)
            return -EAGAIN;
        return spz_maps_snapshot(&module->maps, binding.generation,
                                 module->async.request_id);
    }
    if (operation == SPZ_ASYNC_ENABLE) {
        if (module->breakpoint_configured == 0U ||
            module->breakpoint.id != target_id)
            return -ENOENT;
        result = spz_binding_snapshot(&module->binding, &binding);
        if (result != 0)
            return result;
        if (binding.state != SPZ_BINDING_BOUND)
            return -EAGAIN;
        result = spz_profile_cpu_topology_unchanged(
            &module->runtime, &module->profile_ops, NULL, 0U);
        if (result != 0)
            return result;
        __atomic_store_n(&module->breakpoint_enabled, 1U, __ATOMIC_RELEASE);
        result = spz_run_each_cpu(module, spz_enable_cpu_callback);
        if (result != 0) {
            __atomic_store_n(&module->breakpoint_enabled, 0U, __ATOMIC_RELEASE);
            (void)spz_run_each_cpu(module, spz_restore_cpu_callback);
        }
        return result;
    }
    if (operation == SPZ_ASYNC_DISABLE || operation == SPZ_ASYNC_CLEAR) {
        if (operation == SPZ_ASYNC_DISABLE &&
            module->breakpoint_configured != 0U &&
            module->breakpoint.id != target_id)
            return -ENOENT;
        __atomic_store_n(&module->breakpoint_enabled, 0U, __ATOMIC_RELEASE);
        result = spz_run_each_cpu(module, spz_restore_cpu_callback);
        if (result != 0)
            return result;
        if (operation == SPZ_ASYNC_DISABLE)
            return 0;
        for (attempt = 0U; attempt < 100000U; attempt++) {
            if (__atomic_load_n(&module->active_handlers, __ATOMIC_ACQUIRE) == 0U)
                break;
            __asm__ volatile("" ::: "memory");
        }
        if (__atomic_load_n(&module->active_handlers, __ATOMIC_ACQUIRE) != 0U)
            return -EBUSY;
        if (!spz_all_debug_empty(module))
            return -EUCLEAN;
        result = spz_maps_clear(&module->maps);
        if (result != 0)
            return result;
        result = spz_binding_clear(&module->binding);
        if (result != 0)
            return result;
        memset(&module->breakpoint, 0, sizeof(module->breakpoint));
        __atomic_store_n(&module->breakpoint_configured, 0U, __ATOMIC_RELEASE);
        /*
         * A new client starts polling at sequence zero.  CLEAR runs only
         * after every debug slot is restored and every handler has left, so
         * it is also the safe session boundary at which to discard old hits
         * and reset the consumer cursor.
         */
        spz_ring_init(&module->ring);
        return 0;
    }
    if (operation == SPZ_ASYNC_AUDIT) {
        module->audit_events = 0U;
        return spz_run_each_cpu(module, spz_audit_cpu_callback);
    }
    return -EINVAL;
}

int spz_module_audit_cpu(struct spz_module_state *module, uint32_t cpu)
{
    enum spz_debug_slot_kind kind;

    if (module == NULL || cpu >= module->debug.cpu_count)
        return -EINVAL;
    for (kind = SPZ_DEBUG_SLOT_BREAKPOINT;
         kind <= SPZ_DEBUG_SLOT_WATCHPOINT; kind++) {
        uint32_t count = kind == SPZ_DEBUG_SLOT_BREAKPOINT ?
            module->debug.ops.brp_count : module->debug.ops.wrp_count;
        uint32_t slot;

        for (slot = 0U; slot < count; slot++) {
            struct spz_event event;
            uint64_t owner;
            uint64_t value;
            uint32_t control;
            int result;

            result = module->debug.ops.read_owner(module->debug.ops.context, cpu,
                                                  kind, slot, &owner);
            if (result == 0)
                result = module->debug.ops.read_value(module->debug.ops.context,
                                                      cpu, kind, slot, &value);
            if (result == 0)
                result = module->debug.ops.read_control(module->debug.ops.context,
                                                        cpu, kind, slot,
                                                        &control);
            if (result != 0)
                return result;
            if (owner == 0U && (control & SPZ_DEBUG_CTRL_ENABLE) == 0U)
                continue;
            memset(&event, 0, sizeof(event));
            event.type = SPZ_EVENT_INTEGRITY;
            event.timestamp = spz_module_timestamp(module);
            event.slot_kind = (uint16_t)kind;
            event.slot_index = (uint16_t)slot;
            event.value = value;
            event.observed_control = control;
            event.control = owner != 0U ? 1U : 0U;
            if ((owner == 0U) != ((control & SPZ_DEBUG_CTRL_ENABLE) == 0U))
                event.flags |= SPZ_EVENT_FLAG_INTERFERENCE;
            if (owner != 0U && module->platform.audit_owner != NULL &&
                module->platform.audit_owner(module->platform.context, owner,
                                             &event) != 0)
                event.flags |= SPZ_EVENT_FLAG_INTERFERENCE;
            if (spz_ring_push(&module->ring, cpu, &event) == 0)
                module->audit_events++;
        }
    }
    return 0;
}

int spz_module_can_exit(const struct spz_module_state *module)
{
    struct spz_binding_snapshot binding;
    struct spz_maps_info maps;
    uint32_t active_handlers;
    uint32_t control_busy;

    if (module == NULL)
        return -EINVAL;
    active_handlers = __atomic_load_n(&module->active_handlers,
                                      __ATOMIC_ACQUIRE);
    control_busy = __atomic_load_n(&module->control_busy, __ATOMIC_ACQUIRE);
    if (spz_async_busy(&module->async) ||
        (active_handlers & SPZ_GATE_COUNT_MASK) != 0U ||
        (control_busy & SPZ_GATE_COUNT_MASK) != 0U ||
        __atomic_load_n(&module->breakpoint_enabled, __ATOMIC_ACQUIRE) != 0U ||
        __atomic_load_n(&module->breakpoint_configured, __ATOMIC_ACQUIRE) != 0U ||
        !spz_all_debug_empty(module))
        return -EBUSY;
    if (spz_binding_snapshot(&module->binding, &binding) != 0)
        return -EBUSY;
    if (spz_maps_info(&module->maps, &maps) != 0)
        return -EBUSY;
    if (maps.state != SPZ_MAPS_UNSUPPORTED && maps.state != SPZ_MAPS_EMPTY)
        return -EBUSY;
    return binding.state == SPZ_BINDING_EMPTY ? 0 : -EBUSY;
}

void spz_module_abort_exit(struct spz_module_state *module)
{
    if (module == NULL)
        return;
    __atomic_store_n(&module->active_handlers, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&module->control_busy, 0U, __ATOMIC_RELEASE);
    if (__atomic_load_n(&module->ready, __ATOMIC_ACQUIRE) != 0U)
        __atomic_store_n(&module->accepting_commands, 1U, __ATOMIC_RELEASE);
}

int spz_module_begin_exit(struct spz_module_state *module)
{
    uint32_t expected;
    int result;

    if (module == NULL)
        return -EINVAL;
    __atomic_store_n(&module->accepting_commands, 0U, __ATOMIC_RELEASE);
    expected = 0U;
    if (!__atomic_compare_exchange_n(&module->control_busy, &expected,
                                     SPZ_GATE_CLOSED, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&module->accepting_commands, 1U, __ATOMIC_RELEASE);
        return -EBUSY;
    }
    expected = 0U;
    if (!__atomic_compare_exchange_n(&module->active_handlers, &expected,
                                     SPZ_GATE_CLOSED, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        spz_module_abort_exit(module);
        return -EBUSY;
    }
    result = spz_module_can_exit(module);
    if (result == 0 && module->async.backend.quiesce != NULL)
        result = module->async.backend.quiesce(
            module->async.backend.quiesce_context);
    if (result != 0)
        spz_module_abort_exit(module);
    return result;
}

#if defined(SPZ_KPATCH_BUILD)

#include <kallsyms.h>

#include "../arm64/debug_regs.h"

static uint64_t spz_kpatch_lookup(void *opaque, const char *name)
{
    (void)opaque;
    if (kallsyms_lookup_name == NULL)
        return 0U;
    return (uint64_t)kallsyms_lookup_name(name);
}

typedef long (*spz_nofault_fn)(void *destination, const void *source,
                               size_t length);

static spz_nofault_fn spz_nofault_from_address(uint64_t address)
{
    union {
        uint64_t address;
        spz_nofault_fn function;
    } conversion;

    conversion.address = address;
    return conversion.function;
}

int spz_kpatch_nofault_read(struct spz_kpatch_runtime_context *context,
                            uint64_t address, void *out, size_t length)
{
    spz_nofault_fn function;

    if (context == NULL || out == NULL || address == 0U)
        return -EINVAL;
    function = spz_nofault_from_address(context->nofault_address);
    if (function == NULL)
        return -ENOENT;
    return function(out, (const void *)(uintptr_t)address, length) == 0 ? 0 :
                                                                         -EFAULT;
}

static int spz_kpatch_read_memory(void *opaque, uint64_t address, void *out,
                                  size_t length)
{
    return spz_kpatch_nofault_read(
        (struct spz_kpatch_runtime_context *)opaque, address, out, length);
}

static int spz_kpatch_read_banner(void *opaque, char *out, size_t capacity)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    const char *name = context->module != NULL &&
        context->module->profile != NULL ?
        context->module->profile->symbols.linux_banner : NULL;
    uint64_t address = name == NULL ? 0U : spz_kpatch_lookup(context, name);
    size_t index;

    if (out == NULL || capacity == 0U || address == 0U)
        return -EINVAL;
    for (index = 0U; index + 1U < capacity; index++) {
        int result;

        if ((uint64_t)index > UINT64_MAX - address)
            return -ERANGE;
        result = spz_kpatch_nofault_read(context, address + (uint64_t)index,
                                         &out[index], sizeof(out[index]));
        if (result != 0)
            return result;
        if (out[index] == '\0')
            return 0;
    }
    out[capacity - 1U] = '\0';
    return -ENOSPC;
}

static uint32_t spz_kpatch_page_size(void *opaque)
{
    (void)opaque;
    return spz_arm64_page_size();
}

static uint32_t spz_kpatch_cpu_count(void *opaque)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    const char *name = context->module != NULL &&
        context->module->profile != NULL ?
        context->module->profile->symbols.nr_cpu_ids : NULL;
    uint64_t address = name == NULL ? 0U : spz_kpatch_lookup(context, name);
    uint32_t count = 0U;

    if (address != 0U &&
        spz_kpatch_nofault_read(context, address, &count, sizeof(count)) != 0)
        count = 0U;
    return count;
}

static uint64_t spz_kpatch_dfr0(void *opaque)
{
    (void)opaque;
    return spz_arm64_read_dfr0();
}

static int spz_kpatch_current_cpu(void *opaque, uint32_t *cpu)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    uint64_t offset;
    uint32_t index;

    if (context == NULL || cpu == NULL)
        return -EINVAL;
    offset = spz_arm64_current_cpu_offset();
    for (index = 0U; index < context->cpu_count; index++) {
        if (context->per_cpu_offsets[index] == offset) {
            *cpu = index;
            return 0;
        }
    }
    return -ERANGE;
}

static const void *spz_kpatch_current_task(void *opaque)
{
    (void)opaque;
    return (const void *)spz_arm64_current_task();
}

typedef uint64_t (*spz_time_fn)(void);
static spz_time_fn spz_time_from_address(uint64_t address)
{
    union {
        uint64_t address;
        spz_time_fn function;
    } conversion;

    conversion.address = address;
    return conversion.function;
}

static uint64_t spz_kpatch_timestamp(void *opaque)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    spz_time_fn function = spz_time_from_address(context->time_address);

    return function == NULL ? 0U : function();
}

static int spz_kpatch_owner_address(struct spz_kpatch_runtime_context *context,
                                    uint32_t cpu,
                                    enum spz_debug_slot_kind kind,
                                    uint32_t slot, uint64_t *address)
{
    uint64_t base;
    uint64_t stride;

    if (context == NULL || context->module == NULL || address == NULL ||
        cpu >= context->cpu_count)
        return -EINVAL;
    base = kind == SPZ_DEBUG_SLOT_BREAKPOINT ? context->bp_on_reg :
                                               context->wp_on_reg;
    stride = context->module->profile->layout.per_cpu_pointer_stride;
    return spz_kpatch_per_cpu_address(base, context->per_cpu_offsets[cpu],
                                      stride, slot, address);
}

static int spz_kpatch_read_owner(void *opaque, uint32_t cpu,
                                 enum spz_debug_slot_kind kind, uint32_t slot,
                                 uint64_t *owner)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    uint64_t address;
    uint32_t current;
    int result;

    if (owner == NULL)
        return -EINVAL;
    result = spz_kpatch_current_cpu(context, &current);
    if (result != 0 || current != cpu)
        return result != 0 ? result : -EXDEV;
    result = spz_kpatch_owner_address(context, cpu, kind, slot, &address);
    if (result != 0)
        return result;
    return spz_kpatch_nofault_read(context, address, owner, sizeof(*owner));
}

static int spz_kpatch_profile_read_owner(void *opaque, uint32_t cpu,
                                         uint8_t watchpoint, uint32_t slot,
                                         uint64_t *owner)
{
    return spz_kpatch_read_owner(
        opaque, cpu, watchpoint != 0U ? SPZ_DEBUG_SLOT_WATCHPOINT :
                                       SPZ_DEBUG_SLOT_BREAKPOINT,
        slot, owner);
}

static int spz_kpatch_kernel_pointer(const struct spz_device_profile *profile,
                                     uint64_t pointer)
{
    uint32_t va_bits = profile->kernel.va_bits;
    uint64_t upper_mask;

    if ((pointer & UINT64_C(7)) != 0U || va_bits == 0U || va_bits >= 64U)
        return 0;
    upper_mask = UINT64_MAX << va_bits;
    return (pointer & upper_mask) == upper_mask;
}

static int spz_kpatch_audit_owner(void *opaque, uint64_t owner,
                                  struct spz_event *event)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    const struct spz_device_profile *profile = context->module->profile;
    int32_t state;
    int32_t oncpu;
    int32_t cpu;
    uint32_t bp_type;
    uint64_t bp_address;
    uint64_t bp_length;
    uint64_t arch_address;
    uint64_t arch_trigger;
    uint32_t arch_control;

    if (!spz_kpatch_kernel_pointer(profile, owner) ||
        owner > UINT64_MAX - profile->kernel.perf_event_size)
        return -EFAULT;
#define SPZ_AUDIT_READ(offset, destination)                                      \
    spz_kpatch_nofault_read(context, owner + (uint64_t)(offset),                 \
                            (destination), sizeof(*(destination)))
    if (SPZ_AUDIT_READ(profile->perf_event.state, &state) != 0 ||
        SPZ_AUDIT_READ(profile->perf_event.attr_bp_type, &bp_type) != 0 ||
        SPZ_AUDIT_READ(profile->perf_event.attr_bp_addr, &bp_address) != 0 ||
        SPZ_AUDIT_READ(profile->perf_event.attr_bp_len, &bp_length) != 0 ||
        SPZ_AUDIT_READ(profile->perf_event.arch_address, &arch_address) != 0 ||
        SPZ_AUDIT_READ(profile->perf_event.arch_trigger, &arch_trigger) != 0 ||
        SPZ_AUDIT_READ(profile->perf_event.arch_ctrl, &arch_control) != 0 ||
        SPZ_AUDIT_READ(profile->perf_event.oncpu, &oncpu) != 0 ||
        SPZ_AUDIT_READ(profile->perf_event.cpu, &cpu) != 0)
        return -EFAULT;
#undef SPZ_AUDIT_READ
    event->requested_address = bp_address;
    event->observed_address = arch_address;
    event->start_time = arch_trigger;
    event->start_boot_time = bp_length;
    event->control = arch_control;
    event->pid = (uint32_t)state;
    event->tgid = (uint32_t)oncpu;
    event->uid = (uint32_t)cpu;
    event->exception_class = bp_type;
    return 0;
}

int spz_kpatch_runtime_prepare(struct spz_module_state *module,
                              struct spz_kpatch_runtime_context *context,
                              const char *profile_id,
                              char *reason, size_t reason_capacity)
{
    const struct spz_device_profile *profile;
    struct spz_profile_runtime_ops profile_ops;
    struct spz_profile_runtime runtime;
    struct spz_debug_ops debug_ops;
    struct spz_platform_ops platform;
    struct spz_async_backend async_backend;
    struct spz_maps_backend maps_backend;
    uint64_t per_cpu_address;
    uint32_t cpu;
    int result;

    if (module == NULL || context == NULL || profile_id == NULL)
        return -EINVAL;
    memset(module, 0, sizeof(*module));
    memset(context, 0, sizeof(*context));
    context->module = module;
    profile = spz_profile_select(profile_id);
    if (profile == NULL)
        return -ENOENT;
    module->profile = profile;
    context->nofault_address = spz_kpatch_lookup(
        context, profile->symbols.copy_from_kernel_nofault);
    if (context->nofault_address == 0U)
        return -ENOENT;
    memset(&profile_ops, 0, sizeof(profile_ops));
    profile_ops.context = context;
    profile_ops.lookup_symbol = spz_kpatch_lookup;
    profile_ops.read_memory = spz_kpatch_read_memory;
    profile_ops.read_kernel_banner = spz_kpatch_read_banner;
    profile_ops.page_size = spz_kpatch_page_size;
    profile_ops.cpu_count = spz_kpatch_cpu_count;
    profile_ops.read_dfr0 = spz_kpatch_dfr0;
    profile_ops.read_debug_owner = spz_kpatch_profile_read_owner;

    /* Resolve the per-CPU table before validation needs owner-slot reads. */
    context->cpu_count = spz_kpatch_cpu_count(context);
    if (context->cpu_count == 0U || context->cpu_count > SPZ_MAX_CPUS)
        return -ERANGE;
    per_cpu_address = spz_kpatch_lookup(context, profile->symbols.per_cpu_offset);
    context->bp_on_reg = spz_kpatch_lookup(context, profile->symbols.bp_on_reg);
    context->wp_on_reg = spz_kpatch_lookup(context, profile->symbols.wp_on_reg);
    if (per_cpu_address == 0U || context->bp_on_reg == 0U ||
        context->wp_on_reg == 0U)
        return -ENOENT;
    for (cpu = 0U; cpu < context->cpu_count; cpu++) {
        uint64_t offset_address;

        if ((uint64_t)cpu >
            (UINT64_MAX - per_cpu_address) /
                sizeof(context->per_cpu_offsets[cpu]))
            return -ERANGE;
        offset_address = per_cpu_address +
            (uint64_t)cpu * sizeof(context->per_cpu_offsets[cpu]);
        result = spz_kpatch_nofault_read(
            context, offset_address, &context->per_cpu_offsets[cpu],
            sizeof(context->per_cpu_offsets[cpu]));
        if (result != 0)
            return result;
    }

    result = spz_profile_validate(profile, &profile_ops, &runtime, reason,
                                  reason_capacity);
    if (result != 0)
        return result;
    context->system_unbound_wq_address =
        runtime.symbols[SPZ_SYMBOL_SYSTEM_UNBOUND_WQ];
    context->queue_work_on_address = runtime.symbols[SPZ_SYMBOL_QUEUE_WORK_ON];
    context->flush_work_address = runtime.symbols[SPZ_SYMBOL_FLUSH_WORK];
    context->synchronize_rcu_tasks_address =
        runtime.symbols[SPZ_SYMBOL_SYNCHRONIZE_RCU_TASKS];
    context->schedule_on_each_cpu_address =
        runtime.symbols[SPZ_SYMBOL_SCHEDULE_ON_EACH_CPU];
    context->time_address = runtime.symbols[SPZ_SYMBOL_KTIME_GET_MONO_FAST_NS];
    context->nofault_address =
        runtime.symbols[SPZ_SYMBOL_COPY_FROM_KERNEL_NOFAULT];

    memset(&context->owner_ops, 0, sizeof(context->owner_ops));
    context->owner_ops.context = context;
    context->owner_ops.brp_count = profile->debug.brps;
    context->owner_ops.wrp_count = profile->debug.wrps;
    context->owner_ops.current_cpu = spz_kpatch_current_cpu;
    context->owner_ops.read_owner = spz_kpatch_read_owner;
    result = spz_arm64_make_debug_ops(&context->owner_ops, &debug_ops);
    if (result != 0)
        return result;
    result = spz_kpatch_async_transport_init(context);
    if (result != 0)
        return result;
    memset(&platform, 0, sizeof(platform));
    platform.context = context;
    platform.current_cpu = spz_kpatch_current_cpu;
    platform.current_task = spz_kpatch_current_task;
    platform.timestamp_ns = spz_kpatch_timestamp;
    platform.run_each_cpu = spz_kpatch_run_each_cpu;
    platform.audit_owner = spz_kpatch_audit_owner;
    memset(&async_backend, 0, sizeof(async_backend));
    async_backend.queue_context = context;
    async_backend.queue = spz_kpatch_async_queue;
    async_backend.quiesce_context = context;
    async_backend.quiesce = spz_kpatch_async_quiesce;
    memset(&maps_backend, 0, sizeof(maps_backend));
    result = spz_kpatch_maps_backend_init(context, &maps_backend);
    if (result != 0) {
        spz_kpatch_async_transport_reset(context);
        return result;
    }
    result = spz_module_core_init(module, profile, &runtime, &profile_ops,
                                  &platform, &debug_ops, &async_backend,
                                  &maps_backend);
    if (result != 0)
        spz_kpatch_async_transport_reset(context);
    return result;
}

void spz_kpatch_runtime_zero(struct spz_module_state *module,
                            struct spz_kpatch_runtime_context *context)
{
    if (context != NULL)
        spz_kpatch_async_transport_reset(context);
    if (module != NULL)
        memset(module, 0, sizeof(*module));
    if (context != NULL)
        memset(context, 0, sizeof(*context));
}

#else

int spz_kpatch_nofault_read(struct spz_kpatch_runtime_context *context,
                            uint64_t address, void *out, size_t length)
{
    (void)context;
    (void)address;
    (void)out;
    (void)length;
    return -EOPNOTSUPP;
}

int spz_kpatch_runtime_prepare(struct spz_module_state *module,
                              struct spz_kpatch_runtime_context *context,
                              const char *profile_id,
                              char *reason, size_t reason_capacity)
{
    (void)module;
    (void)context;
    (void)profile_id;
    (void)reason;
    (void)reason_capacity;
    return -EOPNOTSUPP;
}

void spz_kpatch_runtime_zero(struct spz_module_state *module,
                            struct spz_kpatch_runtime_context *context)
{
    if (module != NULL)
        memset(module, 0, sizeof(*module));
    if (context != NULL)
        memset(context, 0, sizeof(*context));
}

#endif
