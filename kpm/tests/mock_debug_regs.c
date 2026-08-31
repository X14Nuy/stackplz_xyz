#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "mock_debug_regs.h"

static unsigned int kind_index(enum spz_debug_slot_kind kind)
{
    return kind == SPZ_DEBUG_SLOT_WATCHPOINT ? 1U : 0U;
}

static uint32_t kind_limit(const struct mock_debug_regs *mock,
                           enum spz_debug_slot_kind kind)
{
    return kind == SPZ_DEBUG_SLOT_WATCHPOINT ? mock->wrp_count : mock->brp_count;
}

static int valid_slot(const struct mock_debug_regs *mock, uint32_t cpu,
                      enum spz_debug_slot_kind kind, uint32_t index)
{
    return cpu < SPZ_MAX_CPUS &&
           (kind == SPZ_DEBUG_SLOT_BREAKPOINT || kind == SPZ_DEBUG_SLOT_WATCHPOINT) &&
           index < kind_limit(mock, kind);
}

static void append_log(struct mock_debug_regs *mock, enum mock_debug_write_kind kind,
                       uint32_t cpu, enum spz_debug_slot_kind slot_kind,
                       uint32_t slot, uint64_t value)
{
    mock->total_write_count++;
    if (mock->log_count < MOCK_DEBUG_LOG_CAPACITY) {
        struct mock_debug_write *entry = &mock->log[mock->log_count++];

        entry->kind = kind;
        entry->cpu = cpu;
        entry->slot_kind = slot_kind;
        entry->slot = slot;
        entry->value = value;
    }
}

static int mock_read_value(void *opaque, uint32_t cpu, enum spz_debug_slot_kind kind,
                           uint32_t index, uint64_t *value)
{
    struct mock_debug_regs *mock = (struct mock_debug_regs *)opaque;

    if (value == NULL || !valid_slot(mock, cpu, kind, index))
        return -EINVAL;
    *value = mock->value[cpu][kind_index(kind)][index];
    return 0;
}

static int mock_write_value(void *opaque, uint32_t cpu, enum spz_debug_slot_kind kind,
                            uint32_t index, uint64_t value)
{
    struct mock_debug_regs *mock = (struct mock_debug_regs *)opaque;

    if (!valid_slot(mock, cpu, kind, index))
        return -EINVAL;
    mock->value[cpu][kind_index(kind)][index] = value;
    append_log(mock, MOCK_WRITE_VALUE, cpu, kind, index, value);
    return 0;
}

static int mock_read_control(void *opaque, uint32_t cpu, enum spz_debug_slot_kind kind,
                             uint32_t index, uint32_t *control)
{
    struct mock_debug_regs *mock = (struct mock_debug_regs *)opaque;

    if (control == NULL || !valid_slot(mock, cpu, kind, index))
        return -EINVAL;
    *control = mock->control[cpu][kind_index(kind)][index];
    return 0;
}

static int mock_write_control(void *opaque, uint32_t cpu, enum spz_debug_slot_kind kind,
                              uint32_t index, uint32_t control)
{
    struct mock_debug_regs *mock = (struct mock_debug_regs *)opaque;

    if (!valid_slot(mock, cpu, kind, index))
        return -EINVAL;
    if ((control & SPZ_DEBUG_CTRL_ENABLE) == 0U && mock->copy_probe_event != NULL &&
        mock->copy_probe_event->registers.pc != 0U)
        mock->copied_before_disable = 1U;
    mock->control[cpu][kind_index(kind)][index] = control;
    append_log(mock, MOCK_WRITE_CONTROL, cpu, kind, index, control);
    return 0;
}

static int mock_read_mdscr(void *opaque, uint32_t cpu, uint64_t *value)
{
    struct mock_debug_regs *mock = (struct mock_debug_regs *)opaque;

    if (value == NULL || cpu >= SPZ_MAX_CPUS)
        return -EINVAL;
    *value = mock->mdscr[cpu];
    return 0;
}

static int mock_write_mdscr(void *opaque, uint32_t cpu, uint64_t value)
{
    struct mock_debug_regs *mock = (struct mock_debug_regs *)opaque;

    if (cpu >= SPZ_MAX_CPUS)
        return -EINVAL;
    mock->mdscr[cpu] = value;
    append_log(mock, MOCK_WRITE_MDSCR, cpu, SPZ_DEBUG_SLOT_BREAKPOINT, 0U, value);
    return 0;
}

static int mock_read_owner(void *opaque, uint32_t cpu, enum spz_debug_slot_kind kind,
                           uint32_t index, uint64_t *owner)
{
    struct mock_debug_regs *mock = (struct mock_debug_regs *)opaque;

    if (owner == NULL || !valid_slot(mock, cpu, kind, index))
        return -EINVAL;
    *owner = mock->owner[cpu][kind_index(kind)][index];
    return 0;
}

static void mock_barrier(void *opaque, uint32_t cpu)
{
    struct mock_debug_regs *mock = (struct mock_debug_regs *)opaque;

    append_log(mock, MOCK_WRITE_BARRIER, cpu, SPZ_DEBUG_SLOT_BREAKPOINT, 0U, 0U);
}

void mock_debug_init(struct mock_debug_regs *mock, uint32_t brps, uint32_t wrps)
{
    memset(mock, 0, sizeof(*mock));
    mock->brp_count = brps;
    mock->wrp_count = wrps;
}

struct spz_debug_ops mock_debug_ops(struct mock_debug_regs *mock)
{
    struct spz_debug_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = mock;
    ops.brp_count = mock->brp_count;
    ops.wrp_count = mock->wrp_count;
    ops.read_value = mock_read_value;
    ops.write_value = mock_write_value;
    ops.read_control = mock_read_control;
    ops.write_control = mock_write_control;
    ops.read_mdscr = mock_read_mdscr;
    ops.write_mdscr = mock_write_mdscr;
    ops.read_owner = mock_read_owner;
    ops.barrier = mock_barrier;
    return ops;
}

uint32_t mock_debug_write_count(const struct mock_debug_regs *mock)
{
    return mock->total_write_count;
}
