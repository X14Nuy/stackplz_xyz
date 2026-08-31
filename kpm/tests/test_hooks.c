#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "mock_debug_regs.h"
#include "../platform/kpatch/compat.h"
#include "test.h"

struct fake_hook_api {
    uint64_t wrapped[SPZ_HOOK_COUNT];
    uint64_t unwrapped[SPZ_HOOK_COUNT];
    uint32_t wrap_count;
    uint32_t unwrap_count;
    uint32_t expected_unwraps_before_quiesce;
    uint32_t quiesce_count;
    uint32_t quiesce_order_error;
    int fail_at;
};

struct hook_task_fixture {
    uint8_t task[8192];
    uint8_t cred[512];
};

struct hook_platform {
    uint32_t cpu;
    const void *task;
    uint64_t timestamp;
};

static int fake_wrap(void *opaque, uint64_t target, uint32_t argument_count,
                     void *before, void *after, void *udata)
{
    struct fake_hook_api *fake = (struct fake_hook_api *)opaque;
    uint32_t index = fake->wrap_count++;

    (void)before;
    (void)after;
    (void)udata;
    SPZ_EXPECT(argument_count == 1U || argument_count == 3U);
    fake->wrapped[index] = target;
    return fake->fail_at == (int)index ? -EIO : 0;
}

static void fake_unwrap(void *opaque, uint64_t target, void *before, void *after)
{
    struct fake_hook_api *fake = (struct fake_hook_api *)opaque;

    (void)before;
    (void)after;
    fake->unwrapped[fake->unwrap_count++] = target;
}

static void fake_quiesce(void *opaque)
{
    struct fake_hook_api *fake = (struct fake_hook_api *)opaque;

    if (fake->unwrap_count != fake->expected_unwraps_before_quiesce)
        fake->quiesce_order_error = 1U;
    fake->quiesce_count++;
}

static struct spz_profile_runtime hook_runtime(void)
{
    struct spz_profile_runtime runtime;

    memset(&runtime, 0, sizeof(runtime));
    runtime.profile = spz_profile_select(SPZ_DEFAULT_DEVICE_PROFILE_ID);
    runtime.state = SPZ_PROFILE_READY;
    runtime.hooks_allowed = 1U;
    runtime.symbols[SPZ_SYMBOL_FINISH_TASK_SWITCH] = UINT64_C(0x1000);
    runtime.symbols[SPZ_SYMBOL_DO_EXIT] = UINT64_C(0x2000);
    runtime.symbols[SPZ_SYMBOL_BREAKPOINT_HANDLER] = UINT64_C(0x3000);
    runtime.symbols[SPZ_SYMBOL_WATCHPOINT_HANDLER] = UINT64_C(0x4000);
    runtime.symbols[SPZ_SYMBOL_SINGLE_STEP_HANDLER] = UINT64_C(0x5000);
    return runtime;
}

static struct spz_hook_callbacks hook_callbacks(void)
{
    struct spz_hook_callbacks callbacks;

    callbacks.finish_before = (void *)(uintptr_t)UINT64_C(0x11);
    callbacks.finish_after = (void *)(uintptr_t)UINT64_C(0x12);
    callbacks.exit_before = (void *)(uintptr_t)UINT64_C(0x21);
    callbacks.break_before = (void *)(uintptr_t)UINT64_C(0x31);
    callbacks.watch_before = (void *)(uintptr_t)UINT64_C(0x41);
    callbacks.step_before = (void *)(uintptr_t)UINT64_C(0x51);
    return callbacks;
}

static void expect_hook_order_and_rollback(void)
{
    const uint64_t expected[SPZ_HOOK_COUNT] = {
        UINT64_C(0x1000), UINT64_C(0x2000), UINT64_C(0x3000),
        UINT64_C(0x4000), UINT64_C(0x5000),
    };
    struct spz_profile_runtime runtime = hook_runtime();
    struct spz_hook_callbacks callbacks = hook_callbacks();
    int fail_at;

    for (fail_at = -1; fail_at < (int)SPZ_HOOK_COUNT; fail_at++) {
        struct fake_hook_api fake;
        struct spz_hook_backend backend;
        struct spz_hook_set hooks;
        uint32_t index;
        int result;

        memset(&fake, 0, sizeof(fake));
        fake.fail_at = fail_at;
        backend.context = &fake;
        backend.wrap = fake_wrap;
        backend.unwrap = fake_unwrap;
        backend.quiesce = fake_quiesce;
        fake.expected_unwraps_before_quiesce =
            fail_at < 0 ? SPZ_HOOK_COUNT : (uint32_t)fail_at;
        result = spz_hooks_install(&hooks, &runtime, &backend, &callbacks,
                                   (void *)(uintptr_t)UINT64_C(0xaa));
        if (fail_at < 0) {
            SPZ_EXPECT_EQ(result, 0);
            SPZ_EXPECT_EQ(fake.wrap_count, SPZ_HOOK_COUNT);
            for (index = 0U; index < SPZ_HOOK_COUNT; index++)
                SPZ_EXPECT_EQ(fake.wrapped[index], expected[index]);
            spz_hooks_remove(&hooks);
            SPZ_EXPECT_EQ(fake.unwrap_count, SPZ_HOOK_COUNT);
            SPZ_EXPECT_EQ(fake.quiesce_count, 1U);
            SPZ_EXPECT_EQ(fake.quiesce_order_error, 0U);
            for (index = 0U; index < SPZ_HOOK_COUNT; index++)
                SPZ_EXPECT_EQ(fake.unwrapped[index], expected[SPZ_HOOK_COUNT - 1U - index]);
        } else {
            SPZ_EXPECT(result < 0);
            SPZ_EXPECT_EQ(fake.wrap_count, (uint32_t)fail_at + 1U);
            SPZ_EXPECT_EQ(fake.unwrap_count, (uint32_t)fail_at);
            for (index = 0U; index < (uint32_t)fail_at; index++)
                SPZ_EXPECT_EQ(fake.unwrapped[index], expected[(uint32_t)fail_at - 1U - index]);
            SPZ_EXPECT_EQ(fake.quiesce_count, fail_at == 0 ? 0U : 1U);
            SPZ_EXPECT_EQ(fake.quiesce_order_error, 0U);
            for (index = 0U; index < SPZ_HOOK_COUNT; index++)
                SPZ_EXPECT_EQ(hooks.installed[index], 0U);
        }
    }
}

static void put_u32(uint8_t *bytes, uint32_t offset, uint32_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static void put_u64(uint8_t *bytes, uint32_t offset, uint64_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static void init_hook_task(struct hook_task_fixture *fixture,
                           const struct spz_device_profile *profile)
{
    uint64_t cred = (uint64_t)(uintptr_t)fixture->cred;

    memset(fixture, 0, sizeof(*fixture));
    put_u32(fixture->task, profile->task.pid, 31337U);
    put_u32(fixture->task, profile->task.tgid, 31300U);
    put_u64(fixture->task, profile->task.start_time, UINT64_C(100));
    put_u64(fixture->task, profile->task.start_boottime, UINT64_C(101));
    put_u64(fixture->task, profile->task.cred, cred);
    put_u32(fixture->cred, profile->cred.uid, 10234U);
    memcpy(fixture->task + profile->task.comm, "target.proc", sizeof("target.proc"));
}

static int hook_current_cpu(void *opaque, uint32_t *cpu)
{
    *cpu = ((struct hook_platform *)opaque)->cpu;
    return 0;
}

static const void *hook_current_task(void *opaque)
{
    return ((struct hook_platform *)opaque)->task;
}

static uint64_t hook_timestamp(void *opaque)
{
    return ((struct hook_platform *)opaque)->timestamp;
}

static uint32_t hook_cpu_count(void *opaque)
{
    (void)opaque;
    return 2U;
}

static int hook_each_cpu(void *opaque,
                         int (*callback)(void *callback_context, uint32_t cpu),
                         void *callback_context)
{
    struct hook_platform *platform = (struct hook_platform *)opaque;
    uint32_t saved = platform->cpu;
    uint32_t cpu;
    int result = 0;

    for (cpu = 0U; cpu < 2U; cpu++) {
        platform->cpu = cpu;
        result = callback(callback_context, cpu);
        if (result != 0)
            break;
    }
    platform->cpu = saved;
    return result;
}

static int queue_never_used(void *opaque)
{
    (void)opaque;
    return 0;
}

static void expect_finish_and_exception_paths(void)
{
    const struct spz_device_profile *profile =
        spz_profile_select(SPZ_DEFAULT_DEVICE_PROFILE_ID);
    struct spz_profile_runtime runtime;
    struct spz_profile_runtime_ops profile_ops;
    struct spz_platform_ops platform_ops;
    struct spz_async_backend async_backend;
    struct hook_platform platform;
    struct hook_task_fixture fixture;
    struct mock_debug_regs registers;
    struct spz_debug_ops debug_ops;
    struct spz_module_state module;
    struct spz_binding_request binding_request;
    struct spz_debug_request request;
    struct spz_debug_target old_target;
    struct spz_event event;
    struct spz_hook_fargs fargs;
    uint8_t pt_regs[1024];
    uint64_t generation;
    uint64_t value;
    uint32_t index;

    init_hook_task(&fixture, profile);
    memset(&runtime, 0, sizeof(runtime));
    runtime.profile = profile;
    runtime.state = SPZ_PROFILE_READY;
    runtime.hooks_allowed = 1U;
    runtime.initial_cpu_count = 2U;
    memset(&profile_ops, 0, sizeof(profile_ops));
    profile_ops.cpu_count = hook_cpu_count;
    memset(&platform, 0, sizeof(platform));
    platform.task = fixture.task;
    platform.timestamp = UINT64_C(0x123456789);
    memset(&platform_ops, 0, sizeof(platform_ops));
    platform_ops.context = &platform;
    platform_ops.current_cpu = hook_current_cpu;
    platform_ops.current_task = hook_current_task;
    platform_ops.timestamp_ns = hook_timestamp;
    platform_ops.run_each_cpu = hook_each_cpu;
    memset(&async_backend, 0, sizeof(async_backend));
    async_backend.queue = queue_never_used;
    mock_debug_init(&registers, 6U, 4U);
    debug_ops = mock_debug_ops(&registers);
    SPZ_EXPECT_EQ(spz_module_core_init(&module, profile, &runtime, &profile_ops,
                                       &platform_ops, &debug_ops, &async_backend,
                                       NULL), 0);

    memset(&binding_request, 0, sizeof(binding_request));
    binding_request.binding_id = 11U;
    binding_request.pid = 31337U;
    binding_request.mode = SPZ_BIND_PID;
    SPZ_EXPECT_EQ(spz_binding_set(&module.binding, &binding_request, &generation), 0);
    memset(&request, 0, sizeof(request));
    request.id = 7U;
    request.kind = SPZ_BREAK_EXECUTE;
    request.address = UINT64_C(0x1000);
    request.length = 4U;
    request.mode = SPZ_BREAK_ONCE;
    module.breakpoint = request;
    module.breakpoint_configured = 1U;
    module.breakpoint_enabled = 1U;

    memset(&old_target, 0, sizeof(old_target));
    old_target.binding_id = 99U;
    old_target.identity.generation = 88U;
    old_target.identity.pid = 1U;
    old_target.identity.tgid = 1U;
    SPZ_EXPECT_EQ(spz_debug_arm_current(&module.debug, 0U, &request, &old_target), 0);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&module.debug, 0U)->state, SPZ_DEBUG_ARMED);

    spz_module_finish_before(&module);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&module.debug, 0U)->state, SPZ_DEBUG_EMPTY);
    value = UINT64_C(0xfeedface); /* stand-in for the unmodified original call */
    spz_module_finish_after(&module);
    SPZ_EXPECT_EQ(value, UINT64_C(0xfeedface));
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&module.debug, 0U)->state, SPZ_DEBUG_ARMED);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&module.debug, 0U)->target.binding_id, 11U);

    memset(pt_regs, 0, sizeof(pt_regs));
    for (index = 0U; index < 31U; index++)
        put_u64(pt_regs, profile->layout.pt_regs_regs + index * 8U,
                UINT64_C(0x100) + index);
    put_u64(pt_regs, profile->layout.pt_regs_sp, UINT64_C(0x7ffffff000));
    put_u64(pt_regs, profile->layout.pt_regs_pc, request.address);
    put_u64(pt_regs, profile->layout.pt_regs_pstate, UINT64_C(0x600003c0));
    memset(&fargs, 0, sizeof(fargs));
    fargs.args[1] = UINT64_C(0x22) << 26U;
    fargs.args[2] = (uint64_t)(uintptr_t)pt_regs;
    fargs.ret = UINT64_C(0x55);
    spz_module_exception_before(&module, SPZ_EXCEPTION_BREAKPOINT, &fargs);
    SPZ_EXPECT_EQ(fargs.skip_origin, 1U);
    SPZ_EXPECT_EQ(fargs.ret, 0U);
    SPZ_EXPECT_EQ(spz_ring_pop_after(&module.ring, 0U, &event), 1);
    SPZ_EXPECT_EQ(event.type, SPZ_EVENT_BREAKPOINT);
    SPZ_EXPECT_EQ(event.timestamp, platform.timestamp);
    SPZ_EXPECT_EQ(event.registers.x[30], UINT64_C(0x11e));
    SPZ_EXPECT_EQ(module.breakpoint_enabled, 0U);

    spz_module_finish_before(&module);
    spz_module_finish_after(&module);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&module.debug, 0U)->state,
                  SPZ_DEBUG_EMPTY);
    put_u64(pt_regs, profile->layout.pt_regs_pc, request.address + 4U);
    memset(&fargs, 0, sizeof(fargs));
    fargs.args[1] = UINT64_C(0x22) << 26U;
    fargs.args[2] = (uint64_t)(uintptr_t)pt_regs;
    fargs.ret = UINT64_C(0x77);
    spz_module_exception_before(&module, SPZ_EXCEPTION_BREAKPOINT, &fargs);
    SPZ_EXPECT_EQ(fargs.skip_origin, 0U);
    SPZ_EXPECT_EQ(fargs.ret, UINT64_C(0x77));
}

int test_hooks(void)
{
    expect_hook_order_and_rollback();
    expect_finish_and_exception_paths();
    return 0;
}
