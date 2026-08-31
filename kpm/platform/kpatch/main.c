#include <compiler.h>
#include <common.h>
#include <hook.h>
#include <kallsyms.h>
#include <kpmodule.h>
#include <kputils.h>
#include <linux/string.h>
#include <log.h>
#include <uapi/asm-generic/errno.h>

#include "compat.h"

KPM_NAME(SPZ_KPM_NAME);
KPM_VERSION(SPZ_KPM_VERSION);
KPM_LICENSE("GPL v2");
KPM_AUTHOR("stackplz contributors");
KPM_DESCRIPTION("Record-only scheduler and ARM64 debug-state observer");

static struct spz_module_state spz_module;
static struct spz_kpatch_runtime_context spz_runtime;
static uint32_t spz_initialized;
static uint32_t spz_exit_started;

static void spz_finish_before(hook_fargs1_t *fargs, void *udata)
{
    (void)fargs;
    spz_module_finish_before((struct spz_module_state *)udata);
}

static void spz_finish_after(hook_fargs1_t *fargs, void *udata)
{
    (void)fargs;
    spz_module_finish_after((struct spz_module_state *)udata);
}

static void spz_exit_before(hook_fargs1_t *fargs, void *udata)
{
    (void)fargs;
    spz_module_exit_before((struct spz_module_state *)udata);
}

static void spz_exception_before(hook_fargs3_t *fargs, void *udata,
                                 enum spz_exception_kind kind)
{
    struct spz_hook_fargs bridge;
    uint32_t index;

    memset(&bridge, 0, sizeof(bridge));
    for (index = 0U; index < 4U; index++)
        bridge.args[index] = fargs->args[index];
    bridge.ret = fargs->ret;
    bridge.skip_origin = fargs->skip_origin;
    spz_module_exception_before((struct spz_module_state *)udata, kind,
                                &bridge);
    fargs->ret = bridge.ret;
    fargs->skip_origin = bridge.skip_origin;
}

static void spz_break_before(hook_fargs3_t *fargs, void *udata)
{
    spz_exception_before(fargs, udata, SPZ_EXCEPTION_BREAKPOINT);
}

static void spz_watch_before(hook_fargs3_t *fargs, void *udata)
{
    spz_exception_before(fargs, udata, SPZ_EXCEPTION_WATCHPOINT);
}

static void spz_step_before(hook_fargs3_t *fargs, void *udata)
{
    spz_exception_before(fargs, udata, SPZ_EXCEPTION_SINGLE_STEP);
}

static void *spz_hook1_address(hook_chain1_callback callback)
{
    union {
        hook_chain1_callback callback;
        void *address;
    } conversion;

    conversion.callback = callback;
    return conversion.address;
}

static void *spz_hook3_address(hook_chain3_callback callback)
{
    union {
        hook_chain3_callback callback;
        void *address;
    } conversion;

    conversion.callback = callback;
    return conversion.address;
}

static int spz_kpatch_hook_wrap(void *context, uint64_t target,
                                uint32_t argument_count, void *before,
                                void *after, void *udata)
{
    union {
        void *address;
        hook_chain1_callback callback;
    } before1, after1;
    union {
        void *address;
        hook_chain3_callback callback;
    } before3, after3;

    (void)context;
    if (target == 0U || before == NULL)
        return -EINVAL;
    if (argument_count == 1U) {
        before1.address = before;
        after1.address = after;
        return (int)hook_wrap1((void *)(uintptr_t)target, before1.callback,
                               after1.callback, udata);
    }
    if (argument_count == 3U) {
        before3.address = before;
        after3.address = after;
        return (int)hook_wrap3((void *)(uintptr_t)target, before3.callback,
                               after3.callback, udata);
    }
    return -EINVAL;
}

static void spz_kpatch_hook_unwrap(void *context, uint64_t target,
                                   void *before, void *after)
{
    (void)context;
    if (target == 0U)
        return;
    /*
     * Keep the now-empty KPatch chain allocated.  d05's final chain free is
     * explicitly marked unsafe.  The task-RCU grace period below drains
     * callback pointers that were fetched before this slot became BUSY.
     */
    hook_unwrap_remove((void *)(uintptr_t)target, before, after, 0);
}

typedef void (*spz_synchronize_rcu_tasks_fn)(void);

static void spz_kpatch_hook_quiesce(void *opaque)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    union {
        uint64_t address;
        spz_synchronize_rcu_tasks_fn function;
    } conversion;

    conversion.address = context->synchronize_rcu_tasks_address;
    conversion.function();
}

static struct spz_hook_callbacks spz_callbacks(void)
{
    struct spz_hook_callbacks callbacks;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.finish_before = spz_hook1_address(spz_finish_before);
    callbacks.finish_after = spz_hook1_address(spz_finish_after);
    callbacks.exit_before = spz_hook1_address(spz_exit_before);
    callbacks.break_before = spz_hook3_address(spz_break_before);
    callbacks.watch_before = spz_hook3_address(spz_watch_before);
    callbacks.step_before = spz_hook3_address(spz_step_before);
    return callbacks;
}

static int spz_profile_argument(const char *args, const char **profile_id)
{
    static const char prefix[] = "profile=";
    size_t length;
    size_t index;

    if (profile_id == NULL)
        return -EINVAL;
    if (args == NULL || args[0] == '\0') {
        *profile_id = SPZ_DEFAULT_DEVICE_PROFILE_ID;
        return 0;
    }
    for (length = 0U; length <= 128U; length++) {
        if (args[length] == '\0')
            break;
    }
    if (length > 128U)
        return -E2BIG;
    if (length <= sizeof(prefix) - 1U ||
        memcmp(args, prefix, sizeof(prefix) - 1U) != 0)
        return -EINVAL;
    for (index = sizeof(prefix) - 1U; index < length; index++) {
        uint8_t byte = (uint8_t)args[index];

        if ((byte >= (uint8_t)'a' && byte <= (uint8_t)'z') ||
            (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') ||
            (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
            byte == (uint8_t)'.' || byte == (uint8_t)'_' ||
            byte == (uint8_t)':' || byte == (uint8_t)'-')
            continue;
        return -EINVAL;
    }
    *profile_id = args + sizeof(prefix) - 1U;
    return 0;
}

static int spz_hex_u32(const char *text, uint32_t *value)
{
    uint32_t parsed = 0U;
    size_t index;

    if (text == NULL || value == NULL || text[0] == '\0')
        return -EINVAL;
    for (index = 0U; index < 8U && text[index] != '\0'; index++) {
        uint8_t byte = (uint8_t)text[index];
        uint32_t digit;

        if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9')
            digit = (uint32_t)(byte - (uint8_t)'0');
        else if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f')
            digit = (uint32_t)(byte - (uint8_t)'a') + 10U;
        else if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F')
            digit = (uint32_t)(byte - (uint8_t)'A') + 10U;
        else
            return -EINVAL;
        parsed = (parsed << 4U) | digit;
    }
    if (index == 0U || text[index] != '\0')
        return -ERANGE;
    *value = parsed;
    return 0;
}

static int spz_cpu_barrier(void *context, uint32_t cpu)
{
    (void)context;
    (void)cpu;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

static int spz_safe_unload_marker_ok(const struct spz_device_profile *profile)
{
    const char *name;
    unsigned long address;
    int value;

    if (profile == NULL)
        return -EINVAL;
    name = profile->kpatch.safe_unload_symbol;
    if (name == NULL || name[0] == '\0')
        return profile->quirks.safe_unload_required != 0U ? -EOPNOTSUPP : 0;
    /*
     * Stock d05 loaders do not export kpm_safe_unload_v1. A hard ELF import
     * is rejected before init, so resolve the marker from kallsyms when it
     * exists. A missing marker is accepted unless this profile requires it.
     */
    if (kallsyms_lookup_name == NULL)
        return profile->quirks.safe_unload_required != 0U ? -EOPNOTSUPP : 0;
    address = kallsyms_lookup_name(name);
    if (address == 0UL)
        return profile->quirks.safe_unload_required != 0U ? -ENOENT : 0;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    if (value != 1)
        return -EOPNOTSUPP;
    return 0;
}

static long spz_kpm_init(const char *args, const char *event, void *reserved)
{
    struct spz_hook_backend backend;
    struct spz_hook_callbacks callbacks;
    const struct spz_device_profile *profile;
    const char *profile_id;
    uint32_t expected_kpver;
    uint32_t expected_kver;
    char reason[96];
    int result;

    (void)event;
    (void)reserved;
    result = spz_profile_argument(args, &profile_id);
    if (result != 0)
        return result;
    profile = spz_profile_select(profile_id);
    if (profile == NULL)
        return -ENOENT;
    result = spz_safe_unload_marker_ok(profile);
    if (result != 0)
        return result;
    if (spz_hex_u32(profile->kpatch.kpver, &expected_kpver) != 0 ||
        spz_hex_u32(profile->kpatch.kver, &expected_kver) != 0 ||
        kpver != expected_kpver || kver != expected_kver)
        return -EPROTO;
    memset(reason, 0, sizeof(reason));
    result = spz_kpatch_runtime_prepare(&spz_module, &spz_runtime, profile_id,
                                        reason, sizeof(reason));
    if (result != 0) {
        logki("stackplz-kpm prepare failed rc=%d reason=%s\n", result, reason);
        spz_kpatch_runtime_zero(&spz_module, &spz_runtime);
        return result;
    }
    __atomic_store_n(&spz_module.accepting_commands, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&spz_module.ready, 0U, __ATOMIC_RELEASE);
    memset(&backend, 0, sizeof(backend));
    backend.context = &spz_runtime;
    backend.wrap = spz_kpatch_hook_wrap;
    backend.unwrap = spz_kpatch_hook_unwrap;
    backend.quiesce = spz_kpatch_hook_quiesce;
    callbacks = spz_callbacks();
    result = spz_hooks_install(&spz_module.hooks, &spz_module.runtime, &backend,
                               &callbacks, &spz_module);
    if (result != 0) {
        int barrier_result = spz_kpatch_run_each_cpu(
            &spz_runtime, spz_cpu_barrier, NULL);

        if (barrier_result != 0) {
            /*
             * Keep an inert image resident if quiescence cannot be proven.
             * The patched loader will also refuse every later unload until
             * the per-CPU barrier succeeds.
             */
            __atomic_store_n(&spz_module.accepting_commands, 0U,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&spz_module.ready, 0U, __ATOMIC_RELEASE);
            __atomic_store_n(&spz_module.active_handlers, SPZ_GATE_CLOSED,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&spz_exit_started, 1U, __ATOMIC_RELEASE);
            __atomic_store_n(&spz_initialized, 1U, __ATOMIC_RELEASE);
            return 0;
        }
        spz_kpatch_runtime_zero(&spz_module, &spz_runtime);
        return result;
    }
    __atomic_store_n(&spz_module.accepting_commands, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&spz_module.ready, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&spz_initialized, 1U, __ATOMIC_RELEASE);
    return 0;
}

static long spz_kpm_control(const char *args, char *__user out_msg, int outlen)
{
    char command[SPZ_MAX_COMMAND + 1U];
    char response[SPZ_CONTROL_RESPONSE_MAX];
    size_t command_length;
    size_t copy_length;
    int response_length;
    int copied;

    if (__atomic_load_n(&spz_initialized, __ATOMIC_ACQUIRE) == 0U ||
        args == NULL || out_msg == NULL || outlen <= 0)
        return -EINVAL;
    for (command_length = 0U; command_length <= SPZ_MAX_COMMAND;
         command_length++) {
        if (args[command_length] == '\0')
            break;
    }
    if (command_length > SPZ_MAX_COMMAND) {
        memcpy(command, args, SPZ_MAX_COMMAND);
        command[SPZ_MAX_COMMAND] = '\0';
    } else {
        memcpy(command, args, command_length + 1U);
    }
    response_length = spz_control_execute(
        &spz_module, command,
        command_length > SPZ_MAX_COMMAND ? SPZ_MAX_COMMAND + 1U :
                                           command_length,
        response, sizeof(response));
    if (response_length < 0)
        return response_length;
    copy_length = (size_t)response_length + 1U;
    if (copy_length > (size_t)outlen || copy_length > sizeof(response))
        return -ENOSPC;
    copied = compat_copy_to_user(out_msg, response, (int)copy_length);
    return copied == (int)copy_length ? 0 : -EFAULT;
}

static long spz_kpm_exit(void *reserved)
{
    int result;

    (void)reserved;
    if (__atomic_load_n(&spz_initialized, __ATOMIC_ACQUIRE) == 0U)
        return 0;
    if (__atomic_load_n(&spz_exit_started, __ATOMIC_ACQUIRE) == 0U) {
        result = spz_module_begin_exit(&spz_module);
        if (result != 0)
            return result;
        spz_hooks_remove(&spz_module.hooks);
        __atomic_store_n(&spz_exit_started, 1U, __ATOMIC_RELEASE);
    }
    result = spz_kpatch_run_each_cpu(&spz_runtime, spz_cpu_barrier, NULL);
    if (result != 0)
        return result;
    __atomic_store_n(&spz_module.ready, 0U, __ATOMIC_RELEASE);
    spz_kpatch_runtime_zero(&spz_module, &spz_runtime);
    __atomic_store_n(&spz_initialized, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&spz_exit_started, 0U, __ATOMIC_RELEASE);
    return 0;
}

KPM_INIT(spz_kpm_init);
KPM_CTL0(spz_kpm_control);
KPM_EXIT(spz_kpm_exit);
