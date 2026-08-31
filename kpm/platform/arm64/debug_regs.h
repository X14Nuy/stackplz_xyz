#ifndef STACKPLZ_ARM64_DEBUG_REGS_H
#define STACKPLZ_ARM64_DEBUG_REGS_H

#include <stdint.h>

#include "stackplz/debug.h"

struct spz_arm64_owner_ops {
    void *context;
    uint32_t brp_count;
    uint32_t wrp_count;
    int (*current_cpu)(void *context, uint32_t *cpu);
    int (*read_owner)(void *context, uint32_t cpu, enum spz_debug_slot_kind kind,
                      uint32_t index, uint64_t *owner);
};

uint32_t spz_arm64_num_brps_from_dfr0(uint64_t dfr0);
uint32_t spz_arm64_num_wrps_from_dfr0(uint64_t dfr0);
uint32_t spz_arm64_page_size_from_tcr(uint64_t tcr);
uint32_t spz_arm64_num_brps(void);
uint32_t spz_arm64_num_wrps(void);
uint64_t spz_arm64_read_dfr0(void);
uint32_t spz_arm64_page_size(void);

int spz_arm64_read_bvr(uint32_t index, uint64_t *value);
int spz_arm64_write_bvr(uint32_t index, uint64_t value);
int spz_arm64_read_bcr(uint32_t index, uint32_t *value);
int spz_arm64_write_bcr(uint32_t index, uint32_t value);
int spz_arm64_read_wvr(uint32_t index, uint64_t *value);
int spz_arm64_write_wvr(uint32_t index, uint64_t value);
int spz_arm64_read_wcr(uint32_t index, uint32_t *value);
int spz_arm64_write_wcr(uint32_t index, uint32_t value);
int spz_arm64_read_mdscr(uint64_t *value);
int spz_arm64_write_mdscr(uint64_t value);
uintptr_t spz_arm64_current_task(void);
uint64_t spz_arm64_current_cpu_offset(void);
void spz_arm64_barrier(void);

int spz_arm64_make_debug_ops(struct spz_arm64_owner_ops *owners,
                             struct spz_debug_ops *out);

#endif
