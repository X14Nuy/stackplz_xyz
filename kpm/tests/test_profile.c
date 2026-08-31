#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stackplz/profile.h"
#include "test.h"

#define FAKE_TASK_ADDRESS UINT64_C(0x100000)
#define FAKE_CRED_ADDRESS UINT64_C(0x200000)

struct fake_profile_runtime {
    char banner[256];
    uint8_t task[8192];
    uint8_t cred[512];
    const char *missing_symbol;
    uint32_t page_size;
    uint32_t cpu_count;
    uint64_t dfr0;
};

static void put_u32(uint8_t *bytes, uint32_t offset, uint32_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static void put_u64(uint8_t *bytes, uint32_t offset, uint64_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static uint64_t fake_lookup_symbol(void *opaque, const char *name)
{
    struct fake_profile_runtime *fake = (struct fake_profile_runtime *)opaque;
    uint64_t hash = 0U;
    const unsigned char *cursor;

    if (fake->missing_symbol != NULL && strcmp(fake->missing_symbol, name) == 0)
        return 0U;
    if (strcmp(name, "init_task") == 0)
        return FAKE_TASK_ADDRESS;
    for (cursor = (const unsigned char *)name; *cursor != 0U; cursor++)
        hash = hash * UINT64_C(131) + *cursor;
    return UINT64_C(0x400000) + (hash & UINT64_C(0xfffff));
}

static int fake_read_memory(void *opaque, uint64_t address, void *out, size_t length)
{
    struct fake_profile_runtime *fake = (struct fake_profile_runtime *)opaque;

    if (address >= FAKE_TASK_ADDRESS && address - FAKE_TASK_ADDRESS <= sizeof(fake->task) &&
        length <= sizeof(fake->task) - (size_t)(address - FAKE_TASK_ADDRESS)) {
        memcpy(out, fake->task + (size_t)(address - FAKE_TASK_ADDRESS), length);
        return 0;
    }
    if (address >= FAKE_CRED_ADDRESS && address - FAKE_CRED_ADDRESS <= sizeof(fake->cred) &&
        length <= sizeof(fake->cred) - (size_t)(address - FAKE_CRED_ADDRESS)) {
        memcpy(out, fake->cred + (size_t)(address - FAKE_CRED_ADDRESS), length);
        return 0;
    }
    return -EFAULT;
}

static int fake_read_banner(void *opaque, char *out, size_t capacity)
{
    struct fake_profile_runtime *fake = (struct fake_profile_runtime *)opaque;
    size_t length = strlen(fake->banner);

    if (capacity == 0U || length + 1U > capacity)
        return -ENOSPC;
    memcpy(out, fake->banner, length + 1U);
    return 0;
}

static uint32_t fake_page_size(void *opaque)
{
    return ((struct fake_profile_runtime *)opaque)->page_size;
}

static uint32_t fake_cpu_count(void *opaque)
{
    return ((struct fake_profile_runtime *)opaque)->cpu_count;
}

static uint64_t fake_read_dfr0(void *opaque)
{
    return ((struct fake_profile_runtime *)opaque)->dfr0;
}

static int fake_read_owner(void *opaque, uint32_t cpu, uint8_t watchpoint,
                           uint32_t slot, uint64_t *owner)
{
    (void)opaque;
    (void)cpu;
    (void)watchpoint;
    (void)slot;
    *owner = 0U;
    return 0;
}

static void init_fake(struct fake_profile_runtime *fake, const struct spz_device_profile *profile)
{
    memset(fake, 0, sizeof(*fake));
    (void)snprintf(fake->banner, sizeof(fake->banner), "Linux version %s (builder@test) #1 SMP",
                   profile->kernel.release);
    fake->page_size = profile->kernel.page_size;
    fake->cpu_count = profile->debug.max_cpus;
    fake->dfr0 = profile->debug.dfr0;
    put_u32(fake->task, profile->task.pid, 0U);
    put_u32(fake->task, profile->task.tgid, 0U);
    memcpy(fake->task + profile->task.comm, "swapper/0", sizeof("swapper/0"));
    put_u64(fake->task, profile->task.real_cred, FAKE_CRED_ADDRESS);
    put_u64(fake->task, profile->task.cred, FAKE_CRED_ADDRESS);
    put_u32(fake->cred, profile->cred.uid, 0U);
}

static struct spz_profile_runtime_ops fake_ops(struct fake_profile_runtime *fake)
{
    struct spz_profile_runtime_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = fake;
    ops.lookup_symbol = fake_lookup_symbol;
    ops.read_memory = fake_read_memory;
    ops.read_kernel_banner = fake_read_banner;
    ops.page_size = fake_page_size;
    ops.cpu_count = fake_cpu_count;
    ops.read_dfr0 = fake_read_dfr0;
    ops.read_debug_owner = fake_read_owner;
    return ops;
}

static void expect_reason(const struct spz_device_profile *profile,
                          struct fake_profile_runtime *fake,
                          enum spz_profile_reason expected)
{
    struct spz_profile_runtime runtime;
    struct spz_profile_runtime_ops ops = fake_ops(fake);
    char reason[128];
    int result = spz_profile_validate(profile, &ops, &runtime, reason, sizeof(reason));

    SPZ_EXPECT(result < 0);
    SPZ_EXPECT_EQ(runtime.state, SPZ_PROFILE_REJECTED);
    SPZ_EXPECT_EQ(runtime.reason, expected);
    SPZ_EXPECT(runtime.hooks_allowed == 0U);
    SPZ_EXPECT(runtime.profile == NULL);
    SPZ_EXPECT(runtime.symbols[SPZ_SYMBOL_INIT_TASK] == 0U);
    SPZ_EXPECT(strcmp(reason, spz_profile_reason_name(expected)) == 0);
}

static void expect_valid_profile(void)
{
    const struct spz_device_profile *profile = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    struct fake_profile_runtime fake;
    struct spz_profile_runtime runtime;
    struct spz_profile_runtime_ops ops;
    char reason[128];

    SPZ_EXPECT(profile != NULL);
    SPZ_EXPECT(spz_profile_select("oneplus-plk110-a16-b4999618-d05-extra") == NULL);
    init_fake(&fake, profile);
    ops = fake_ops(&fake);
    SPZ_EXPECT_EQ(spz_profile_validate(profile, &ops, &runtime, reason, sizeof(reason)), 0);
    SPZ_EXPECT_EQ(runtime.state, SPZ_PROFILE_READY);
    SPZ_EXPECT_EQ(runtime.reason, SPZ_PROFILE_REASON_NONE);
    SPZ_EXPECT(runtime.hooks_allowed != 0U);
    SPZ_EXPECT(runtime.profile == profile);
    SPZ_EXPECT_EQ(runtime.initial_cpu_count, profile->debug.max_cpus);
    SPZ_EXPECT(runtime.symbols[SPZ_SYMBOL_INIT_TASK] == FAKE_TASK_ADDRESS);
    SPZ_EXPECT(strcmp(reason, "ok") == 0);
    SPZ_EXPECT_EQ(spz_profile_cpu_topology_unchanged(&runtime, &ops, reason, sizeof(reason)), 0);
    fake.cpu_count--;
    SPZ_EXPECT_EQ(spz_profile_cpu_topology_unchanged(&runtime, &ops, reason, sizeof(reason)), -ESTALE);
    SPZ_EXPECT(strcmp(reason, spz_profile_reason_name(SPZ_PROFILE_REASON_CPU_TOPOLOGY_CHANGED)) == 0);
}

static void expect_identity_failures(void)
{
    const struct spz_device_profile *base = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    struct spz_device_profile profile;
    struct fake_profile_runtime fake;

    profile = *base;
    init_fake(&fake, base);
    profile.id = "unknown-profile";
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_UNKNOWN_ID);

    profile = *base;
    init_fake(&fake, base);
    profile.kernel.release = "6.12.23-wrong";
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_KERNEL_RELEASE);

    profile = *base;
    init_fake(&fake, base);
    fake.page_size *= 4U;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_PAGE_SIZE);

    profile = *base;
    init_fake(&fake, base);
    fake.cpu_count = profile.debug.max_cpus + 1U;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_CPU_COUNT);

    profile = *base;
    init_fake(&fake, base);
    fake.dfr0 ^= UINT64_C(1);
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_DEBUG_ARCH);

    profile = *base;
    init_fake(&fake, base);
    fake.dfr0 ^= UINT64_C(1) << 12U;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_BRP_COUNT);

    profile = *base;
    init_fake(&fake, base);
    fake.dfr0 ^= UINT64_C(1) << 20U;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_WRP_COUNT);

    profile = *base;
    init_fake(&fake, base);
    fake.dfr0 ^= UINT64_C(1) << 28U;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_CONTEXT_COUNT);

    profile = *base;
    init_fake(&fake, base);
    memcpy(fake.banner, "not a Linux banner", sizeof("not a Linux banner"));
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_KERNEL_RELEASE);
}

static void expect_symbol_failures(void)
{
    static const struct {
        const char *name;
        enum spz_profile_reason reason;
    } symbols[] = {
        {"linux_banner", SPZ_PROFILE_REASON_SYMBOL_LINUX_BANNER},
        {"init_task", SPZ_PROFILE_REASON_SYMBOL_INIT_TASK},
        {"finish_task_switch", SPZ_PROFILE_REASON_SYMBOL_FINISH_TASK_SWITCH},
        {"do_exit", SPZ_PROFILE_REASON_SYMBOL_DO_EXIT},
        {"breakpoint_handler", SPZ_PROFILE_REASON_SYMBOL_BREAKPOINT_HANDLER},
        {"watchpoint_handler", SPZ_PROFILE_REASON_SYMBOL_WATCHPOINT_HANDLER},
        {"single_step_handler", SPZ_PROFILE_REASON_SYMBOL_SINGLE_STEP_HANDLER},
        {"bp_on_reg", SPZ_PROFILE_REASON_SYMBOL_BP_ON_REG},
        {"wp_on_reg", SPZ_PROFILE_REASON_SYMBOL_WP_ON_REG},
        {"__per_cpu_offset", SPZ_PROFILE_REASON_SYMBOL_PER_CPU_OFFSET},
        {"nr_cpu_ids", SPZ_PROFILE_REASON_SYMBOL_NR_CPU_IDS},
        {"system_unbound_wq", SPZ_PROFILE_REASON_SYMBOL_SYSTEM_UNBOUND_WQ},
        {"queue_work_on", SPZ_PROFILE_REASON_SYMBOL_QUEUE_WORK_ON},
        {"synchronize_rcu_tasks", SPZ_PROFILE_REASON_SYMBOL_SYNCHRONIZE_RCU_TASKS},
        {"schedule_on_each_cpu", SPZ_PROFILE_REASON_SYMBOL_SCHEDULE_ON_EACH_CPU},
        {"ktime_get_mono_fast_ns", SPZ_PROFILE_REASON_SYMBOL_KTIME_GET_MONO_FAST_NS},
        {"copy_from_kernel_nofault", SPZ_PROFILE_REASON_SYMBOL_COPY_FROM_KERNEL_NOFAULT},
    };
    const struct spz_device_profile *profile = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    size_t index;

    for (index = 0U; index < sizeof(symbols) / sizeof(symbols[0]); index++) {
        struct fake_profile_runtime fake;

        init_fake(&fake, profile);
        fake.missing_symbol = symbols[index].name;
        expect_reason(profile, &fake, symbols[index].reason);
    }
}

static void expect_structure_failures(void)
{
    const struct spz_device_profile *base = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    struct spz_device_profile profile;
    struct fake_profile_runtime fake;

    profile = *base;
    init_fake(&fake, base);
    profile.task.comm = profile.kernel.task_struct_size - 8U;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_GENERATED_BOUNDS);

    profile = *base;
    init_fake(&fake, base);
    profile.cred.uid = profile.kernel.cred_size - 2U;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_GENERATED_BOUNDS);

    profile = *base;
    init_fake(&fake, base);
    profile.layout.work_func = profile.layout.work_struct_size;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_GENERATED_BOUNDS);

    profile = *base;
    init_fake(&fake, base);
    profile.layout.pt_regs_pstate = profile.layout.pt_regs_size;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_GENERATED_BOUNDS);

    profile = *base;
    init_fake(&fake, base);
    profile.hooks.finish_task_switch_args = 2U;
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_GENERATED_BOUNDS);

    profile = *base;
    init_fake(&fake, base);
    put_u32(fake.task, profile.task.pid, 1U);
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_INIT_TASK_PID);

    profile = *base;
    init_fake(&fake, base);
    put_u32(fake.task, profile.task.tgid, 1U);
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_INIT_TASK_TGID);

    profile = *base;
    init_fake(&fake, base);
    memcpy(fake.task + profile.task.comm, "not-init-task", sizeof("not-init-task"));
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_INIT_TASK_COMM);

    profile = *base;
    init_fake(&fake, base);
    put_u64(fake.task, profile.task.cred, 0U);
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_INIT_TASK_CRED);

    profile = *base;
    init_fake(&fake, base);
    put_u64(fake.task, profile.task.real_cred, FAKE_CRED_ADDRESS + 16U);
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_INIT_TASK_CRED);

    profile = *base;
    init_fake(&fake, base);
    put_u32(fake.cred, profile.cred.uid, 1000U);
    expect_reason(&profile, &fake, SPZ_PROFILE_REASON_INIT_TASK_CRED);
}

int test_profile(void)
{
    expect_valid_profile();
    expect_identity_failures();
    expect_symbol_failures();
    expect_structure_failures();
    return 0;
}
