#ifndef STACKPLZ_MOCK_DEBUG_REGS_H
#define STACKPLZ_MOCK_DEBUG_REGS_H

#include <stdint.h>

#include "stackplz/debug.h"

#define MOCK_DEBUG_LOG_CAPACITY 4096U

enum mock_debug_write_kind {
    MOCK_WRITE_VALUE = 1,
    MOCK_WRITE_CONTROL,
    MOCK_WRITE_MDSCR,
    MOCK_WRITE_BARRIER,
};

struct mock_debug_write {
    enum mock_debug_write_kind kind;
    uint32_t cpu;
    enum spz_debug_slot_kind slot_kind;
    uint32_t slot;
    uint64_t value;
};

struct mock_debug_regs {
    uint64_t value[SPZ_MAX_CPUS][2][SPZ_DEBUG_MAX_SLOTS];
    uint32_t control[SPZ_MAX_CPUS][2][SPZ_DEBUG_MAX_SLOTS];
    uint64_t owner[SPZ_MAX_CPUS][2][SPZ_DEBUG_MAX_SLOTS];
    uint64_t mdscr[SPZ_MAX_CPUS];
    struct mock_debug_write log[MOCK_DEBUG_LOG_CAPACITY];
    uint32_t log_count;
    uint32_t total_write_count;
    uint32_t brp_count;
    uint32_t wrp_count;
    struct spz_event *copy_probe_event;
    uint8_t copied_before_disable;
};

void mock_debug_init(struct mock_debug_regs *mock, uint32_t brps, uint32_t wrps);
struct spz_debug_ops mock_debug_ops(struct mock_debug_regs *mock);
uint32_t mock_debug_write_count(const struct mock_debug_regs *mock);

#endif
