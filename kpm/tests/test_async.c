#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "mock_debug_regs.h"
#include "../platform/kpatch/compat.h"
#include "test.h"

int spz_kpatch_per_cpu_address(uint64_t base, uint64_t per_cpu_offset,
                               uint64_t stride, uint32_t slot,
                               uint64_t *address);

struct fake_async {
    int in_rcu;
    int queue_result;
    uint32_t queue_calls;
    uint32_t execute_calls;
    uint32_t execute_calls_at_queue;
    enum spz_async_operation last_operation;
    uint32_t last_target;
};

static int fake_queue(void *opaque)
{
    struct fake_async *fake = (struct fake_async *)opaque;

    fake->queue_calls++;
    SPZ_EXPECT(fake->in_rcu != 0);
    fake->execute_calls_at_queue = fake->execute_calls;
    return fake->queue_result;
}

static int fake_execute(void *opaque, enum spz_async_operation operation,
                        uint32_t target_id)
{
    struct fake_async *fake = (struct fake_async *)opaque;

    SPZ_EXPECT_EQ(fake->in_rcu, 0U);
    fake->execute_calls++;
    fake->last_operation = operation;
    fake->last_target = target_id;
    return operation == SPZ_ASYNC_AUDIT ? -EUCLEAN : 0;
}

static void expect_async_defers_outside_rcu(void)
{
    struct fake_async fake;
    struct spz_async_backend backend;
    struct spz_async_request request;
    struct spz_async_snapshot snapshot;
    uint64_t request_id;

    memset(&fake, 0, sizeof(fake));
    memset(&backend, 0, sizeof(backend));
    backend.queue_context = &fake;
    backend.queue = fake_queue;
    backend.execute_context = &fake;
    backend.execute = fake_execute;
    spz_async_init(&request, &backend);

    fake.in_rcu = 1;
    SPZ_EXPECT_EQ(spz_async_submit(&request, SPZ_ASYNC_ENABLE, 7U, &request_id), 0);
    SPZ_EXPECT_EQ(request_id, 1U);
    SPZ_EXPECT_EQ(fake.queue_calls, 1U);
    SPZ_EXPECT_EQ(fake.execute_calls, 0U);
    SPZ_EXPECT_EQ(fake.execute_calls_at_queue, 0U);
    SPZ_EXPECT_EQ(spz_async_snapshot(&request, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_ASYNC_PENDING);
    SPZ_EXPECT_EQ(spz_async_submit(&request, SPZ_ASYNC_CLEAR, 0U, NULL), -EBUSY);

    fake.in_rcu = 0;
    SPZ_EXPECT_EQ(spz_async_run(&request), 0);
    SPZ_EXPECT_EQ(fake.execute_calls, 1U);
    SPZ_EXPECT_EQ(fake.last_operation, SPZ_ASYNC_ENABLE);
    SPZ_EXPECT_EQ(fake.last_target, 7U);
    SPZ_EXPECT_EQ(spz_async_snapshot(&request, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_ASYNC_DONE);
    SPZ_EXPECT_EQ(snapshot.status, 0U);

    fake.in_rcu = 1;
    SPZ_EXPECT_EQ(spz_async_submit(&request, SPZ_ASYNC_AUDIT, 0U, &request_id), 0);
    SPZ_EXPECT_EQ(request_id, 2U);
    SPZ_EXPECT_EQ(fake.execute_calls_at_queue, 1U);
    fake.in_rcu = 0;
    SPZ_EXPECT_EQ(spz_async_run(&request), -EUCLEAN);
    SPZ_EXPECT_EQ(spz_async_snapshot(&request, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.status, -EUCLEAN);

    fake.in_rcu = 1;
    SPZ_EXPECT_EQ(spz_async_submit(&request, SPZ_ASYNC_MAPS, 0U,
                                   &request_id), 0);
    SPZ_EXPECT_EQ(request_id, 3U);
    fake.in_rcu = 0;
    SPZ_EXPECT_EQ(spz_async_run(&request), 0);
    SPZ_EXPECT_EQ(fake.last_operation, SPZ_ASYNC_MAPS);
}

static void expect_per_cpu_address_uses_kernel_modular_arithmetic(void)
{
    uint64_t address = 0U;

    SPZ_EXPECT_EQ(spz_kpatch_per_cpu_address(UINT64_C(0x100000),
                                             UINT64_C(0x2000), 8U, 3U,
                                             &address), 0);
    SPZ_EXPECT_EQ(address, UINT64_C(0x102018));

    address = 0U;
    SPZ_EXPECT_EQ(spz_kpatch_per_cpu_address(UINT64_C(0xfffffffffffff000),
                                             UINT64_C(0x2000), 8U, 3U,
                                             &address), 0);
    SPZ_EXPECT_EQ(address, UINT64_C(0x1018));
}

static void expect_per_cpu_address_rejects_invalid_slot_math(void)
{
    uint64_t address = 0U;

    SPZ_EXPECT_EQ(spz_kpatch_per_cpu_address(UINT64_C(0x1000), 0U, 0U, 0U,
                                             &address), -ERANGE);
    SPZ_EXPECT_EQ(spz_kpatch_per_cpu_address(UINT64_MAX - 7U, 0U, 8U, 1U,
                                             &address), -ERANGE);
    SPZ_EXPECT_EQ(spz_kpatch_per_cpu_address(UINT64_C(0x1000), 0U, 8U, 0U,
                                             NULL), -EINVAL);
}

struct module_platform {
    uint32_t cpu;
    uint32_t cpu_count;
    int quiesce_result;
    uint32_t quiesce_calls;
};

static int module_current_cpu(void *opaque, uint32_t *cpu)
{
    *cpu = ((struct module_platform *)opaque)->cpu;
    return 0;
}

static const void *module_current_task(void *opaque)
{
    (void)opaque;
    return NULL;
}

static uint64_t module_timestamp(void *opaque)
{
    (void)opaque;
    return UINT64_C(123);
}

static uint32_t module_cpu_count(void *opaque)
{
    return ((struct module_platform *)opaque)->cpu_count;
}

static int module_each_cpu(void *opaque,
                           int (*callback)(void *callback_context, uint32_t cpu),
                           void *callback_context)
{
    struct module_platform *platform = (struct module_platform *)opaque;
    uint32_t saved = platform->cpu;
    uint32_t cpu;
    int result = 0;

    for (cpu = 0U; cpu < platform->cpu_count; cpu++) {
        platform->cpu = cpu;
        result = callback(callback_context, cpu);
        if (result != 0)
            break;
    }
    platform->cpu = saved;
    return result;
}

static int module_queue(void *opaque)
{
    (void)opaque;
    return 0;
}

static int module_quiesce(void *opaque)
{
    struct module_platform *platform = (struct module_platform *)opaque;

    platform->quiesce_calls++;
    return platform->quiesce_result;
}

static void expect_exit_requires_completed_clear(void)
{
    const struct spz_device_profile *profile =
        spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    struct spz_profile_runtime runtime;
    struct spz_profile_runtime_ops profile_ops;
    struct spz_platform_ops platform_ops;
    struct spz_async_backend async_backend;
    struct spz_module_state module;
    struct module_platform platform;
    struct mock_debug_regs registers;
    struct spz_debug_ops debug_ops;
    struct spz_binding_request binding;
    struct spz_event input;
    struct spz_event output;
    uint64_t generation;

    memset(&platform, 0, sizeof(platform));
    platform.cpu_count = 2U;
    memset(&runtime, 0, sizeof(runtime));
    runtime.profile = profile;
    runtime.state = SPZ_PROFILE_READY;
    runtime.hooks_allowed = 1U;
    runtime.initial_cpu_count = platform.cpu_count;
    memset(&profile_ops, 0, sizeof(profile_ops));
    profile_ops.context = &platform;
    profile_ops.cpu_count = module_cpu_count;
    memset(&platform_ops, 0, sizeof(platform_ops));
    platform_ops.context = &platform;
    platform_ops.current_cpu = module_current_cpu;
    platform_ops.current_task = module_current_task;
    platform_ops.timestamp_ns = module_timestamp;
    platform_ops.run_each_cpu = module_each_cpu;
    memset(&async_backend, 0, sizeof(async_backend));
    async_backend.queue = module_queue;
    async_backend.quiesce_context = &platform;
    async_backend.quiesce = module_quiesce;
    mock_debug_init(&registers, 6U, 4U);
    debug_ops = mock_debug_ops(&registers);
    SPZ_EXPECT_EQ(spz_module_core_init(&module, profile, &runtime, &profile_ops,
                                       &platform_ops, &debug_ops, &async_backend,
                                       NULL), 0);
    SPZ_EXPECT_EQ(spz_module_can_exit(&module), 0);

    memset(&binding, 0, sizeof(binding));
    binding.binding_id = 1U;
    binding.pid = 31337U;
    binding.mode = SPZ_BIND_PID;
    SPZ_EXPECT_EQ(spz_binding_set(&module.binding, &binding, &generation), 0);
    module.breakpoint_configured = 1U;
    module.breakpoint_enabled = 1U;
    memset(&input, 0, sizeof(input));
    input.type = SPZ_EVENT_BREAKPOINT;
    SPZ_EXPECT_EQ(spz_ring_push(&module.ring, 0U, &input), 0);
    SPZ_EXPECT_EQ(spz_ring_pop_after(&module.ring, 0U, &output), 1);
    SPZ_EXPECT_EQ(module.ring.consumer_after, 1U);
    SPZ_EXPECT_EQ(spz_module_can_exit(&module), -EBUSY);
    SPZ_EXPECT_EQ(spz_module_begin_exit(&module), -EBUSY);
    SPZ_EXPECT_EQ(module.accepting_commands, 1U);
    SPZ_EXPECT_EQ(module.active_handlers, 0U);
    SPZ_EXPECT_EQ(module.control_busy, 0U);

    SPZ_EXPECT_EQ(spz_module_async_execute(&module, SPZ_ASYNC_CLEAR, 0U), 0);
    SPZ_EXPECT_EQ(module.ring.next_sequence, 0U);
    SPZ_EXPECT_EQ(module.ring.consumer_after, 0U);
    SPZ_EXPECT_EQ(spz_module_can_exit(&module), 0);
    platform.quiesce_result = -EAGAIN;
    SPZ_EXPECT_EQ(spz_module_begin_exit(&module), -EAGAIN);
    SPZ_EXPECT_EQ(platform.quiesce_calls, 1U);
    SPZ_EXPECT_EQ(module.accepting_commands, 1U);
    SPZ_EXPECT_EQ(module.active_handlers, 0U);
    SPZ_EXPECT_EQ(module.control_busy, 0U);
    platform.quiesce_result = 0;
    SPZ_EXPECT_EQ(spz_module_begin_exit(&module), 0);
    SPZ_EXPECT_EQ(platform.quiesce_calls, 2U);
    SPZ_EXPECT_EQ(module.accepting_commands, 0U);
    SPZ_EXPECT_EQ(module.active_handlers, SPZ_GATE_CLOSED);
    SPZ_EXPECT_EQ(module.control_busy, SPZ_GATE_CLOSED);
    spz_module_finish_before(&module);
    SPZ_EXPECT_EQ(module.active_handlers, SPZ_GATE_CLOSED);
    spz_module_abort_exit(&module);
    SPZ_EXPECT_EQ(module.accepting_commands, 1U);
    SPZ_EXPECT_EQ(module.active_handlers, 0U);
    SPZ_EXPECT_EQ(module.control_busy, 0U);

    __atomic_store_n(&module.async.state, (uint32_t)SPZ_ASYNC_PENDING,
                     __ATOMIC_RELEASE);
    SPZ_EXPECT_EQ(spz_module_can_exit(&module), -EBUSY);
    __atomic_store_n(&module.async.state, (uint32_t)SPZ_ASYNC_DONE,
                     __ATOMIC_RELEASE);
    SPZ_EXPECT_EQ(spz_module_can_exit(&module), 0);
}

int test_async(void)
{
    expect_async_defers_outside_rcu();
    expect_per_cpu_address_uses_kernel_modular_arithmetic();
    expect_per_cpu_address_rejects_invalid_slot_math();
    expect_exit_requires_completed_clear();
    return 0;
}
