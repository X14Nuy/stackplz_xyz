#ifndef STACKPLZ_DEBUG_H
#define STACKPLZ_DEBUG_H

#include <stddef.h>
#include <stdint.h>

#include "stackplz/abi.h"
#include "stackplz/core.h"
#include "stackplz/task.h"

#define SPZ_DEBUG_MAX_SLOTS 16U
#define SPZ_DEBUG_CTRL_ENABLE UINT32_C(1)
#define SPZ_MDSCR_SS (UINT64_C(1) << 0U)
#define SPZ_MDSCR_MDE (UINT64_C(1) << 15U)
#define SPZ_PSTATE_SS (UINT64_C(1) << 21U)
#define SPZ_ESR_WNR (UINT64_C(1) << 6U)

#define SPZ_EVENT_FLAG_KERNEL_UACCESS UINT16_C(0x0001)
#define SPZ_EVENT_FLAG_INTERFERENCE UINT16_C(0x0002)

enum spz_debug_slot_kind {
    SPZ_DEBUG_SLOT_BREAKPOINT = 1,
    SPZ_DEBUG_SLOT_WATCHPOINT = 2,
};

enum spz_debug_state {
    SPZ_DEBUG_EMPTY = 0,
    SPZ_DEBUG_SNAPSHOTTED,
    SPZ_DEBUG_ARMED,
    SPZ_DEBUG_HIT_DISABLED,
    SPZ_DEBUG_STEP_PENDING,
    SPZ_DEBUG_ONE_SHOT_DONE,
    SPZ_DEBUG_RESTORING,
    SPZ_DEBUG_QUARANTINED,
};

enum spz_debug_result {
    SPZ_DEBUG_NOT_OWNED = 0,
    SPZ_DEBUG_CONSUMED = 1,
    SPZ_DEBUG_FORWARD_ORIGINAL = 2,
};

struct spz_debug_request {
    uint32_t id;
    enum spz_break_kind kind;
    uint64_t address;
    uint8_t length;
    enum spz_break_mode mode;
};

struct spz_debug_target {
    uint32_t binding_id;
    struct spz_task_identity identity;
};

struct spz_debug_frame {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t esr;
    uint64_t far;
    uint8_t kernel_mode;
};

struct spz_debug_ops {
    void *context;
    uint32_t brp_count;
    uint32_t wrp_count;
    int (*read_value)(void *context, uint32_t cpu, enum spz_debug_slot_kind kind,
                      uint32_t index, uint64_t *value);
    int (*write_value)(void *context, uint32_t cpu, enum spz_debug_slot_kind kind,
                       uint32_t index, uint64_t value);
    int (*read_control)(void *context, uint32_t cpu, enum spz_debug_slot_kind kind,
                        uint32_t index, uint32_t *control);
    int (*write_control)(void *context, uint32_t cpu, enum spz_debug_slot_kind kind,
                         uint32_t index, uint32_t control);
    int (*read_mdscr)(void *context, uint32_t cpu, uint64_t *value);
    int (*write_mdscr)(void *context, uint32_t cpu, uint64_t value);
    int (*read_owner)(void *context, uint32_t cpu, enum spz_debug_slot_kind kind,
                      uint32_t index, uint64_t *owner);
    void (*barrier)(void *context, uint32_t cpu);
};

struct spz_debug_cpu_state {
    enum spz_debug_state state;
    enum spz_debug_slot_kind kind;
    uint32_t slot;
    uint64_t saved_value;
    uint32_t saved_control;
    uint64_t saved_mdscr;
    uint64_t programmed_value;
    uint32_t programmed_control;
    uint32_t disabled_control;
    uint8_t added_mde;
    uint8_t added_mdscr_ss;
    uint8_t added_pstate_ss;
    uint8_t forward_single_step;
    uint64_t transition;
    struct spz_debug_request request;
    struct spz_debug_target target;
};

struct spz_debug_controller {
    struct spz_debug_ops ops;
    uint32_t cpu_count;
    struct spz_debug_cpu_state cpu[SPZ_MAX_CPUS];
};

int spz_debug_validate_request(const struct spz_debug_request *request,
                               uint64_t *value, uint32_t *control, uint8_t *bas);
int spz_debug_controller_init(struct spz_debug_controller *controller,
                              const struct spz_debug_ops *ops, uint32_t cpu_count);
int spz_debug_arm_current(struct spz_debug_controller *controller, uint32_t cpu,
                          const struct spz_debug_request *request,
                          const struct spz_debug_target *target);
int spz_debug_restore_cpu(struct spz_debug_controller *controller, uint32_t cpu);
int spz_debug_handle_break(struct spz_debug_controller *controller, uint32_t cpu,
                           const struct spz_debug_target *current,
                           struct spz_debug_frame *frame, struct spz_event *event);
int spz_debug_handle_watch(struct spz_debug_controller *controller, uint32_t cpu,
                           const struct spz_debug_target *current,
                           struct spz_debug_frame *frame, struct spz_event *event);
int spz_debug_handle_step(struct spz_debug_controller *controller, uint32_t cpu,
                          const struct spz_debug_target *current,
                          struct spz_debug_frame *frame);
const struct spz_debug_cpu_state *spz_debug_cpu_state(const struct spz_debug_controller *controller,
                                                       uint32_t cpu);

#endif
