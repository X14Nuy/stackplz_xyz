#include "stackplz/platform.h"

#include "debug_regs.h"

uint32_t spz_arm64_num_brps_from_dfr0(uint64_t dfr0)
{
    return (uint32_t)((dfr0 >> 12U) & UINT64_C(0xf)) + 1U;
}

uint32_t spz_arm64_num_wrps_from_dfr0(uint64_t dfr0)
{
    return (uint32_t)((dfr0 >> 20U) & UINT64_C(0xf)) + 1U;
}

uint32_t spz_arm64_page_size_from_tcr(uint64_t tcr)
{
    switch ((uint32_t)((tcr >> 30U) & UINT64_C(3))) {
    case 1U: return 16384U;
    case 2U: return 4096U;
    case 3U: return 65536U;
    default: return 0U;
    }
}

#if defined(__aarch64__)

#define SPZ_FOR_EACH_DEBUG_SLOT(action) \
    action(0)  action(1)  action(2)  action(3)  \
    action(4)  action(5)  action(6)  action(7)  \
    action(8)  action(9)  action(10) action(11) \
    action(12) action(13) action(14) action(15)

#define SPZ_READ_BVR_CASE(index) \
    case index: __asm__ volatile("mrs %0, dbgbvr" #index "_el1" : "=r"(result)); break;
#define SPZ_WRITE_BVR_CASE(index) \
    case index: __asm__ volatile("msr dbgbvr" #index "_el1, %0" :: "r"(value)); break;
#define SPZ_READ_BCR_CASE(index) \
    case index: __asm__ volatile("mrs %0, dbgbcr" #index "_el1" : "=r"(result)); break;
#define SPZ_WRITE_BCR_CASE(index) \
    case index: __asm__ volatile("msr dbgbcr" #index "_el1, %0" :: "r"(wide)); break;
#define SPZ_READ_WVR_CASE(index) \
    case index: __asm__ volatile("mrs %0, dbgwvr" #index "_el1" : "=r"(result)); break;
#define SPZ_WRITE_WVR_CASE(index) \
    case index: __asm__ volatile("msr dbgwvr" #index "_el1, %0" :: "r"(value)); break;
#define SPZ_READ_WCR_CASE(index) \
    case index: __asm__ volatile("mrs %0, dbgwcr" #index "_el1" : "=r"(result)); break;
#define SPZ_WRITE_WCR_CASE(index) \
    case index: __asm__ volatile("msr dbgwcr" #index "_el1, %0" :: "r"(wide)); break;

int spz_arm64_read_bvr(uint32_t index, uint64_t *value)
{
    uint64_t result;

    if (value == NULL)
        return -EINVAL;
    switch (index) {
    SPZ_FOR_EACH_DEBUG_SLOT(SPZ_READ_BVR_CASE)
    default: return -ERANGE;
    }
    *value = result;
    return 0;
}

int spz_arm64_write_bvr(uint32_t index, uint64_t value)
{
    switch (index) {
    SPZ_FOR_EACH_DEBUG_SLOT(SPZ_WRITE_BVR_CASE)
    default: return -ERANGE;
    }
    return 0;
}

int spz_arm64_read_bcr(uint32_t index, uint32_t *value)
{
    uint64_t result;

    if (value == NULL)
        return -EINVAL;
    switch (index) {
    SPZ_FOR_EACH_DEBUG_SLOT(SPZ_READ_BCR_CASE)
    default: return -ERANGE;
    }
    *value = (uint32_t)result;
    return 0;
}

int spz_arm64_write_bcr(uint32_t index, uint32_t value)
{
    uint64_t wide = value;

    switch (index) {
    SPZ_FOR_EACH_DEBUG_SLOT(SPZ_WRITE_BCR_CASE)
    default: return -ERANGE;
    }
    return 0;
}

int spz_arm64_read_wvr(uint32_t index, uint64_t *value)
{
    uint64_t result;

    if (value == NULL)
        return -EINVAL;
    switch (index) {
    SPZ_FOR_EACH_DEBUG_SLOT(SPZ_READ_WVR_CASE)
    default: return -ERANGE;
    }
    *value = result;
    return 0;
}

int spz_arm64_write_wvr(uint32_t index, uint64_t value)
{
    switch (index) {
    SPZ_FOR_EACH_DEBUG_SLOT(SPZ_WRITE_WVR_CASE)
    default: return -ERANGE;
    }
    return 0;
}

int spz_arm64_read_wcr(uint32_t index, uint32_t *value)
{
    uint64_t result;

    if (value == NULL)
        return -EINVAL;
    switch (index) {
    SPZ_FOR_EACH_DEBUG_SLOT(SPZ_READ_WCR_CASE)
    default: return -ERANGE;
    }
    *value = (uint32_t)result;
    return 0;
}

int spz_arm64_write_wcr(uint32_t index, uint32_t value)
{
    uint64_t wide = value;

    switch (index) {
    SPZ_FOR_EACH_DEBUG_SLOT(SPZ_WRITE_WCR_CASE)
    default: return -ERANGE;
    }
    return 0;
}

uint64_t spz_arm64_read_dfr0(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, id_aa64dfr0_el1" : "=r"(value));
    return value;
}

uint32_t spz_arm64_page_size(void)
{
    uint64_t tcr;

    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    return spz_arm64_page_size_from_tcr(tcr);
}

uint32_t spz_arm64_num_brps(void)
{
    return spz_arm64_num_brps_from_dfr0(spz_arm64_read_dfr0());
}

uint32_t spz_arm64_num_wrps(void)
{
    return spz_arm64_num_wrps_from_dfr0(spz_arm64_read_dfr0());
}

int spz_arm64_read_mdscr(uint64_t *value)
{
    if (value == NULL)
        return -EINVAL;
    __asm__ volatile("mrs %0, mdscr_el1" : "=r"(*value));
    return 0;
}

int spz_arm64_write_mdscr(uint64_t value)
{
    __asm__ volatile("msr mdscr_el1, %0" :: "r"(value));
    return 0;
}

uintptr_t spz_arm64_current_task(void)
{
    uintptr_t value;

    __asm__ volatile("mrs %0, sp_el0" : "=r"(value));
    return value;
}

uint64_t spz_arm64_current_cpu_offset(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(value));
    return value;
}

void spz_arm64_barrier(void)
{
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

#undef SPZ_READ_BVR_CASE
#undef SPZ_WRITE_BVR_CASE
#undef SPZ_READ_BCR_CASE
#undef SPZ_WRITE_BCR_CASE
#undef SPZ_READ_WVR_CASE
#undef SPZ_WRITE_WVR_CASE
#undef SPZ_READ_WCR_CASE
#undef SPZ_WRITE_WCR_CASE
#undef SPZ_FOR_EACH_DEBUG_SLOT

#else

int spz_arm64_read_bvr(uint32_t index, uint64_t *value)
{
    if (value == NULL)
        return -EINVAL;
    if (index >= SPZ_DEBUG_MAX_SLOTS)
        return -ERANGE;
    return -EOPNOTSUPP;
}

int spz_arm64_write_bvr(uint32_t index, uint64_t value)
{
    (void)value;
    if (index >= SPZ_DEBUG_MAX_SLOTS)
        return -ERANGE;
    return -EOPNOTSUPP;
}

int spz_arm64_read_bcr(uint32_t index, uint32_t *value)
{
    if (value == NULL)
        return -EINVAL;
    if (index >= SPZ_DEBUG_MAX_SLOTS)
        return -ERANGE;
    return -EOPNOTSUPP;
}

int spz_arm64_write_bcr(uint32_t index, uint32_t value)
{
    (void)value;
    if (index >= SPZ_DEBUG_MAX_SLOTS)
        return -ERANGE;
    return -EOPNOTSUPP;
}

int spz_arm64_read_wvr(uint32_t index, uint64_t *value)
{
    if (value == NULL)
        return -EINVAL;
    if (index >= SPZ_DEBUG_MAX_SLOTS)
        return -ERANGE;
    return -EOPNOTSUPP;
}

int spz_arm64_write_wvr(uint32_t index, uint64_t value)
{
    (void)value;
    if (index >= SPZ_DEBUG_MAX_SLOTS)
        return -ERANGE;
    return -EOPNOTSUPP;
}

int spz_arm64_read_wcr(uint32_t index, uint32_t *value)
{
    if (value == NULL)
        return -EINVAL;
    if (index >= SPZ_DEBUG_MAX_SLOTS)
        return -ERANGE;
    return -EOPNOTSUPP;
}

int spz_arm64_write_wcr(uint32_t index, uint32_t value)
{
    (void)value;
    if (index >= SPZ_DEBUG_MAX_SLOTS)
        return -ERANGE;
    return -EOPNOTSUPP;
}

uint64_t spz_arm64_read_dfr0(void)
{
    return 0U;
}

uint32_t spz_arm64_page_size(void)
{
    return 0U;
}

uint32_t spz_arm64_num_brps(void)
{
    return 0U;
}

uint32_t spz_arm64_num_wrps(void)
{
    return 0U;
}

int spz_arm64_read_mdscr(uint64_t *value)
{
    (void)value;
    return -EOPNOTSUPP;
}

int spz_arm64_write_mdscr(uint64_t value)
{
    (void)value;
    return -EOPNOTSUPP;
}

uintptr_t spz_arm64_current_task(void)
{
    return (uintptr_t)0U;
}

uint64_t spz_arm64_current_cpu_offset(void)
{
    return 0U;
}

void spz_arm64_barrier(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#endif

static int spz_arm64_check_cpu(struct spz_arm64_owner_ops *owners, uint32_t cpu)
{
    uint32_t current;
    int result;

    if (owners == NULL || owners->current_cpu == NULL)
        return -EINVAL;
    result = owners->current_cpu(owners->context, &current);
    if (result != 0)
        return result;
    return current == cpu ? 0 : -EXDEV;
}

static int spz_arm64_ops_read_value(void *opaque, uint32_t cpu,
                                    enum spz_debug_slot_kind kind,
                                    uint32_t index, uint64_t *value)
{
    struct spz_arm64_owner_ops *owners = (struct spz_arm64_owner_ops *)opaque;
    int result = spz_arm64_check_cpu(owners, cpu);

    if (result != 0)
        return result;
    if (kind == SPZ_DEBUG_SLOT_BREAKPOINT)
        return spz_arm64_read_bvr(index, value);
    if (kind == SPZ_DEBUG_SLOT_WATCHPOINT)
        return spz_arm64_read_wvr(index, value);
    return -EINVAL;
}

static int spz_arm64_ops_write_value(void *opaque, uint32_t cpu,
                                     enum spz_debug_slot_kind kind,
                                     uint32_t index, uint64_t value)
{
    struct spz_arm64_owner_ops *owners = (struct spz_arm64_owner_ops *)opaque;
    int result = spz_arm64_check_cpu(owners, cpu);

    if (result != 0)
        return result;
    if (kind == SPZ_DEBUG_SLOT_BREAKPOINT)
        return spz_arm64_write_bvr(index, value);
    if (kind == SPZ_DEBUG_SLOT_WATCHPOINT)
        return spz_arm64_write_wvr(index, value);
    return -EINVAL;
}

static int spz_arm64_ops_read_control(void *opaque, uint32_t cpu,
                                      enum spz_debug_slot_kind kind,
                                      uint32_t index, uint32_t *control)
{
    struct spz_arm64_owner_ops *owners = (struct spz_arm64_owner_ops *)opaque;
    int result = spz_arm64_check_cpu(owners, cpu);

    if (result != 0)
        return result;
    if (kind == SPZ_DEBUG_SLOT_BREAKPOINT)
        return spz_arm64_read_bcr(index, control);
    if (kind == SPZ_DEBUG_SLOT_WATCHPOINT)
        return spz_arm64_read_wcr(index, control);
    return -EINVAL;
}

static int spz_arm64_ops_write_control(void *opaque, uint32_t cpu,
                                       enum spz_debug_slot_kind kind,
                                       uint32_t index, uint32_t control)
{
    struct spz_arm64_owner_ops *owners = (struct spz_arm64_owner_ops *)opaque;
    int result = spz_arm64_check_cpu(owners, cpu);

    if (result != 0)
        return result;
    if (kind == SPZ_DEBUG_SLOT_BREAKPOINT)
        return spz_arm64_write_bcr(index, control);
    if (kind == SPZ_DEBUG_SLOT_WATCHPOINT)
        return spz_arm64_write_wcr(index, control);
    return -EINVAL;
}

static int spz_arm64_ops_read_mdscr(void *opaque, uint32_t cpu, uint64_t *value)
{
    int result = spz_arm64_check_cpu((struct spz_arm64_owner_ops *)opaque, cpu);

    return result == 0 ? spz_arm64_read_mdscr(value) : result;
}

static int spz_arm64_ops_write_mdscr(void *opaque, uint32_t cpu, uint64_t value)
{
    int result = spz_arm64_check_cpu((struct spz_arm64_owner_ops *)opaque, cpu);

    return result == 0 ? spz_arm64_write_mdscr(value) : result;
}

static int spz_arm64_ops_read_owner(void *opaque, uint32_t cpu,
                                    enum spz_debug_slot_kind kind,
                                    uint32_t index, uint64_t *owner)
{
    struct spz_arm64_owner_ops *owners = (struct spz_arm64_owner_ops *)opaque;
    int result = spz_arm64_check_cpu(owners, cpu);

    if (result != 0)
        return result;
    if (owners->read_owner == NULL)
        return -EINVAL;
    return owners->read_owner(owners->context, cpu, kind, index, owner);
}

static void spz_arm64_ops_barrier(void *opaque, uint32_t cpu)
{
    (void)opaque;
    (void)cpu;
    spz_arm64_barrier();
}

int spz_arm64_make_debug_ops(struct spz_arm64_owner_ops *owners,
                             struct spz_debug_ops *out)
{
    if (owners == NULL || out == NULL || owners->current_cpu == NULL ||
        owners->read_owner == NULL || owners->brp_count == 0U ||
        owners->brp_count > SPZ_DEBUG_MAX_SLOTS || owners->wrp_count == 0U ||
        owners->wrp_count > SPZ_DEBUG_MAX_SLOTS)
        return -EINVAL;
    out->context = owners;
    out->brp_count = owners->brp_count;
    out->wrp_count = owners->wrp_count;
    out->read_value = spz_arm64_ops_read_value;
    out->write_value = spz_arm64_ops_write_value;
    out->read_control = spz_arm64_ops_read_control;
    out->write_control = spz_arm64_ops_write_control;
    out->read_mdscr = spz_arm64_ops_read_mdscr;
    out->write_mdscr = spz_arm64_ops_write_mdscr;
    out->read_owner = spz_arm64_ops_read_owner;
    out->barrier = spz_arm64_ops_barrier;
    return 0;
}
