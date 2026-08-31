#include "stackplz/platform.h"

#include "stackplz/debug.h"

#define SPZ_DEBUG_PRIVILEGE_EL0 (UINT32_C(2) << 1U)
#define SPZ_DEBUG_LSC_LOAD (UINT32_C(1) << 3U)
#define SPZ_DEBUG_LSC_STORE (UINT32_C(2) << 3U)

static void spz_debug_transition(struct spz_debug_cpu_state *state,
                                 enum spz_debug_state next)
{
    state->transition++;
    if (state->transition == 0U)
        state->transition = 1U;
    state->state = next;
}

static void spz_debug_reset_state(struct spz_debug_cpu_state *state)
{
    uint64_t transition = state->transition + 1U;

    if (transition == 0U)
        transition = 1U;
    memset(state, 0, sizeof(*state));
    state->transition = transition;
    state->state = SPZ_DEBUG_EMPTY;
}

static enum spz_debug_slot_kind spz_request_slot_kind(const struct spz_debug_request *request)
{
    return request->kind == SPZ_BREAK_EXECUTE ? SPZ_DEBUG_SLOT_BREAKPOINT :
                                                SPZ_DEBUG_SLOT_WATCHPOINT;
}

int spz_debug_validate_request(const struct spz_debug_request *request,
                               uint64_t *value, uint32_t *control, uint8_t *bas)
{
    uint64_t end;
    uint64_t register_value;
    uint32_t lsc = 0U;
    uint8_t byte_mask;

    if (request == NULL || value == NULL || control == NULL || bas == NULL ||
        request->id == 0U)
        return -EINVAL;
    if (request->mode != SPZ_BREAK_ONCE && request->mode != SPZ_BREAK_REPEAT)
        return -EINVAL;
    if (request->address == 0U || (request->address >> 56U) != 0U)
        return -EINVAL;
    if (request->kind == SPZ_BREAK_EXECUTE) {
        if (request->length != 4U || (request->address & 3U) != 0U)
            return -EINVAL;
        register_value = request->address;
        byte_mask = UINT8_C(0x0f);
    } else {
        if (request->kind != SPZ_BREAK_READ && request->kind != SPZ_BREAK_WRITE &&
            request->kind != SPZ_BREAK_READ_WRITE)
            return -EINVAL;
        if (request->length != 1U && request->length != 2U && request->length != 4U &&
            request->length != 8U)
            return -EINVAL;
        end = request->address + (uint64_t)request->length - 1U;
        if (end < request->address || (request->address >> 3U) != (end >> 3U))
            return -EINVAL;
        register_value = request->address & ~UINT64_C(7);
        byte_mask = (uint8_t)(((UINT32_C(1) << request->length) - 1U)
                              << (request->address & 7U));
        if (request->kind == SPZ_BREAK_READ)
            lsc = SPZ_DEBUG_LSC_LOAD;
        else if (request->kind == SPZ_BREAK_WRITE)
            lsc = SPZ_DEBUG_LSC_STORE;
        else
            lsc = SPZ_DEBUG_LSC_LOAD | SPZ_DEBUG_LSC_STORE;
    }
    *value = register_value;
    *bas = byte_mask;
    *control = ((uint32_t)byte_mask << 5U) | lsc | SPZ_DEBUG_PRIVILEGE_EL0 |
               SPZ_DEBUG_CTRL_ENABLE;
    return 0;
}

int spz_debug_controller_init(struct spz_debug_controller *controller,
                              const struct spz_debug_ops *ops, uint32_t cpu_count)
{
    if (controller == NULL || ops == NULL || cpu_count == 0U || cpu_count > SPZ_MAX_CPUS ||
        ops->brp_count == 0U || ops->brp_count > SPZ_DEBUG_MAX_SLOTS ||
        ops->wrp_count == 0U || ops->wrp_count > SPZ_DEBUG_MAX_SLOTS ||
        ops->read_value == NULL || ops->write_value == NULL ||
        ops->read_control == NULL || ops->write_control == NULL ||
        ops->read_mdscr == NULL || ops->write_mdscr == NULL ||
        ops->read_owner == NULL || ops->barrier == NULL)
        return -EINVAL;
    memset(controller, 0, sizeof(*controller));
    controller->ops = *ops;
    controller->cpu_count = cpu_count;
    return 0;
}

static int spz_debug_valid_target(const struct spz_debug_target *target)
{
    return target != NULL && target->binding_id != 0U &&
           target->identity.generation != 0U &&
           (target->identity.pid != 0U || target->identity.tgid != 0U);
}

static int spz_debug_target_matches(const struct spz_debug_target *expected,
                                    const struct spz_debug_target *current)
{
    return current != NULL && expected->binding_id == current->binding_id &&
           expected->identity.generation == current->identity.generation &&
           expected->identity.pid == current->identity.pid &&
           expected->identity.tgid == current->identity.tgid &&
           expected->identity.start_time == current->identity.start_time &&
           expected->identity.start_boot_time == current->identity.start_boot_time;
}

static uint32_t spz_debug_slot_count(const struct spz_debug_controller *controller,
                                     enum spz_debug_slot_kind kind)
{
    return kind == SPZ_DEBUG_SLOT_BREAKPOINT ? controller->ops.brp_count :
                                               controller->ops.wrp_count;
}

static int spz_debug_find_slot(struct spz_debug_controller *controller, uint32_t cpu,
                               enum spz_debug_slot_kind kind, uint32_t *selected,
                               uint64_t *saved_value, uint32_t *saved_control)
{
    uint32_t count = spz_debug_slot_count(controller, kind);
    uint32_t index = count;

    while (index != 0U) {
        uint64_t owner;
        uint64_t value;
        uint32_t control;
        int result;

        index--;
        result = controller->ops.read_owner(controller->ops.context, cpu, kind, index, &owner);
        if (result != 0)
            return result;
        result = controller->ops.read_control(controller->ops.context, cpu, kind, index,
                                              &control);
        if (result != 0)
            return result;
        if ((owner == 0U) != ((control & SPZ_DEBUG_CTRL_ENABLE) == 0U))
            return -EBUSY;
        if (owner != 0U)
            continue;
        result = controller->ops.read_value(controller->ops.context, cpu, kind, index, &value);
        if (result != 0)
            return result;
        *selected = index;
        *saved_value = value;
        *saved_control = control;
        return 0;
    }
    return -EBUSY;
}

static int spz_debug_live_image(struct spz_debug_controller *controller, uint32_t cpu,
                                struct spz_debug_cpu_state *state, uint64_t *value,
                                uint32_t *control, uint64_t *owner)
{
    int result;

    result = controller->ops.read_owner(controller->ops.context, cpu, state->kind,
                                        state->slot, owner);
    if (result != 0)
        return result;
    result = controller->ops.read_value(controller->ops.context, cpu, state->kind,
                                        state->slot, value);
    if (result != 0)
        return result;
    return controller->ops.read_control(controller->ops.context, cpu, state->kind,
                                        state->slot, control);
}

static int spz_debug_rollback_arm(struct spz_debug_controller *controller, uint32_t cpu,
                                  struct spz_debug_cpu_state *state)
{
    uint64_t live_value;
    uint64_t owner;
    uint32_t live_control;

    if (spz_debug_live_image(controller, cpu, state, &live_value, &live_control, &owner) != 0 ||
        owner != 0U || live_value != state->programmed_value ||
        (live_control != state->programmed_control && live_control != state->saved_control)) {
        spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
        return -EUCLEAN;
    }
    if (live_control == state->programmed_control &&
        controller->ops.write_control(controller->ops.context, cpu, state->kind,
                                      state->slot, state->saved_control) != 0) {
        spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
        return -EIO;
    }
    if (controller->ops.write_value(controller->ops.context, cpu, state->kind,
                                    state->slot, state->saved_value) != 0) {
        spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
        return -EIO;
    }
    controller->ops.barrier(controller->ops.context, cpu);
    spz_debug_reset_state(state);
    return 0;
}

int spz_debug_arm_current(struct spz_debug_controller *controller, uint32_t cpu,
                          const struct spz_debug_request *request,
                          const struct spz_debug_target *target)
{
    struct spz_debug_cpu_state *state;
    enum spz_debug_slot_kind kind;
    uint64_t programmed_value;
    uint64_t saved_value;
    uint64_t saved_mdscr;
    uint32_t programmed_control;
    uint32_t saved_control;
    uint32_t slot;
    uint8_t bas;
    uint64_t previous_transition;
    int result;

    if (controller == NULL || cpu >= controller->cpu_count || !spz_debug_valid_target(target))
        return -EINVAL;
    state = &controller->cpu[cpu];
    if (state->state != SPZ_DEBUG_EMPTY)
        return -EBUSY;
    result = spz_debug_validate_request(request, &programmed_value, &programmed_control, &bas);
    if (result != 0)
        return result;
    (void)bas;
    kind = spz_request_slot_kind(request);
    result = spz_debug_find_slot(controller, cpu, kind, &slot, &saved_value, &saved_control);
    if (result != 0)
        return result;
    result = controller->ops.read_mdscr(controller->ops.context, cpu, &saved_mdscr);
    if (result != 0)
        return result;

    previous_transition = state->transition;
    memset(state, 0, sizeof(*state));
    state->transition = previous_transition;
    state->kind = kind;
    state->slot = slot;
    state->saved_value = saved_value;
    state->saved_control = saved_control;
    state->saved_mdscr = saved_mdscr;
    state->programmed_value = programmed_value;
    state->programmed_control = programmed_control;
    state->disabled_control = programmed_control & ~SPZ_DEBUG_CTRL_ENABLE;
    state->added_mde = (saved_mdscr & SPZ_MDSCR_MDE) == 0U ? 1U : 0U;
    state->request = *request;
    state->target = *target;
    spz_debug_transition(state, SPZ_DEBUG_SNAPSHOTTED);

    result = controller->ops.write_value(controller->ops.context, cpu, kind, slot,
                                         programmed_value);
    if (result != 0) {
        spz_debug_reset_state(state);
        return result;
    }
    result = controller->ops.write_control(controller->ops.context, cpu, kind, slot,
                                           programmed_control);
    if (result != 0) {
        (void)spz_debug_rollback_arm(controller, cpu, state);
        return result;
    }
    if (state->added_mde != 0U) {
        result = controller->ops.write_mdscr(controller->ops.context, cpu,
                                             saved_mdscr | SPZ_MDSCR_MDE);
        if (result != 0) {
            (void)spz_debug_rollback_arm(controller, cpu, state);
            return result;
        }
    }
    controller->ops.barrier(controller->ops.context, cpu);
    spz_debug_transition(state, SPZ_DEBUG_ARMED);
    return 0;
}

static int spz_debug_other_state_requires_mde(struct spz_debug_controller *controller,
                                               uint32_t cpu,
                                               const struct spz_debug_cpu_state *owned)
{
    enum spz_debug_slot_kind kind;

    for (kind = SPZ_DEBUG_SLOT_BREAKPOINT; kind <= SPZ_DEBUG_SLOT_WATCHPOINT; kind++) {
        uint32_t count = spz_debug_slot_count(controller, kind);
        uint32_t slot;

        for (slot = 0U; slot < count; slot++) {
            uint64_t owner;
            uint32_t control;

            if (kind == owned->kind && slot == owned->slot)
                continue;
            if (controller->ops.read_owner(controller->ops.context, cpu, kind, slot,
                                           &owner) != 0 ||
                controller->ops.read_control(controller->ops.context, cpu, kind, slot,
                                             &control) != 0)
                return 1;
            if (owner != 0U || (control & SPZ_DEBUG_CTRL_ENABLE) != 0U)
                return 1;
        }
    }
    return 0;
}

int spz_debug_restore_cpu(struct spz_debug_controller *controller, uint32_t cpu)
{
    struct spz_debug_cpu_state *state;
    uint64_t live_value;
    uint64_t owner;
    uint64_t mdscr;
    uint64_t restored_mdscr;
    uint32_t live_control;
    uint32_t expected_control;
    int result;

    if (controller == NULL || cpu >= controller->cpu_count)
        return -EINVAL;
    state = &controller->cpu[cpu];
    if (state->state == SPZ_DEBUG_EMPTY)
        return 0;
    if (state->state == SPZ_DEBUG_QUARANTINED)
        return -EUCLEAN;
    if (state->state == SPZ_DEBUG_SNAPSHOTTED)
        expected_control = state->saved_control;
    else if (state->state == SPZ_DEBUG_ARMED)
        expected_control = state->programmed_control;
    else
        expected_control = state->disabled_control;
    result = spz_debug_live_image(controller, cpu, state, &live_value, &live_control, &owner);
    if (result != 0)
        return result;
    if (owner != 0U || live_value != state->programmed_value || live_control != expected_control) {
        spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
        return -EUCLEAN;
    }
    result = controller->ops.read_mdscr(controller->ops.context, cpu, &mdscr);
    if (result != 0)
        return result;
    restored_mdscr = mdscr;
    if (state->added_mdscr_ss != 0U)
        restored_mdscr &= ~SPZ_MDSCR_SS;
    spz_debug_transition(state, SPZ_DEBUG_RESTORING);
    if ((live_control & SPZ_DEBUG_CTRL_ENABLE) != 0U) {
        result = controller->ops.write_control(controller->ops.context, cpu, state->kind,
                                               state->slot, state->disabled_control);
        if (result != 0)
            goto quarantine;
    }
    result = controller->ops.write_value(controller->ops.context, cpu, state->kind,
                                         state->slot, state->saved_value);
    if (result != 0)
        goto quarantine;
    result = controller->ops.write_control(controller->ops.context, cpu, state->kind,
                                           state->slot, state->saved_control);
    if (result != 0)
        goto quarantine;
    if (state->added_mde != 0U &&
        !spz_debug_other_state_requires_mde(controller, cpu, state))
        restored_mdscr &= ~SPZ_MDSCR_MDE;
    if (restored_mdscr != mdscr) {
        result = controller->ops.write_mdscr(controller->ops.context, cpu, restored_mdscr);
        if (result != 0)
            goto quarantine;
    }
    controller->ops.barrier(controller->ops.context, cpu);
    spz_debug_reset_state(state);
    return 0;

quarantine:
    spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
    controller->ops.barrier(controller->ops.context, cpu);
    return result;
}

static void spz_debug_fill_event(const struct spz_debug_cpu_state *state,
                                 const struct spz_debug_frame *frame,
                                 uint64_t observed_address, uint64_t live_value,
                                 uint32_t live_control, uint64_t mdscr,
                                 struct spz_event *event)
{
    uint32_t index;

    memset(event, 0, sizeof(*event));
    event->type = state->kind == SPZ_DEBUG_SLOT_BREAKPOINT ? SPZ_EVENT_BREAKPOINT :
                                                            SPZ_EVENT_WATCHPOINT;
    event->binding_id = state->target.binding_id;
    event->breakpoint_id = state->request.id;
    event->generation = state->target.identity.generation;
    event->task_cookie = state->target.identity.task_cookie;
    event->pid = state->target.identity.pid;
    event->tgid = state->target.identity.tgid;
    event->uid = state->target.identity.uid;
    event->start_time = state->target.identity.start_time;
    event->start_boot_time = state->target.identity.start_boot_time;
    memcpy(event->comm, state->target.identity.comm, SPZ_COMM_LEN);
    event->exception_class = (uint32_t)((frame->esr >> 26U) & 0x3fU);
    event->slot_kind = (uint16_t)state->kind;
    event->slot_index = (uint16_t)state->slot;
    event->requested_address = state->request.address;
    event->observed_address = observed_address;
    event->value = live_value;
    event->control = state->programmed_control;
    event->observed_control = live_control;
    event->mdscr = mdscr;
    for (index = 0U; index < 31U; index++)
        event->registers.x[index] = frame->x[index];
    event->registers.sp = frame->sp;
    event->registers.pc = frame->pc;
    event->registers.pstate = frame->pstate;
}

static int spz_debug_handle_hit(struct spz_debug_controller *controller, uint32_t cpu,
                                const struct spz_debug_target *current,
                                struct spz_debug_frame *frame, uint64_t observed_address,
                                struct spz_event *event)
{
    struct spz_debug_cpu_state *state = &controller->cpu[cpu];
    uint64_t live_value;
    uint64_t owner;
    uint64_t mdscr;
    uint64_t stepped_mdscr;
    uint32_t live_control;
    int result;

    if (!spz_debug_target_matches(&state->target, current))
        return SPZ_DEBUG_NOT_OWNED;
    result = spz_debug_live_image(controller, cpu, state, &live_value, &live_control, &owner);
    if (result != 0)
        return result;
    if (owner != 0U || live_value != state->programmed_value ||
        live_control != state->programmed_control) {
        spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
        return -EUCLEAN;
    }
    result = controller->ops.read_mdscr(controller->ops.context, cpu, &mdscr);
    if (result != 0)
        return result;
    spz_debug_fill_event(state, frame, observed_address, live_value, live_control, mdscr, event);
    result = controller->ops.write_control(controller->ops.context, cpu, state->kind,
                                           state->slot, state->disabled_control);
    if (result != 0) {
        spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
        return result;
    }
    controller->ops.barrier(controller->ops.context, cpu);
    spz_debug_transition(state, SPZ_DEBUG_HIT_DISABLED);
    if (state->request.mode == SPZ_BREAK_ONCE ||
        (state->kind == SPZ_DEBUG_SLOT_WATCHPOINT && frame->kernel_mode != 0U)) {
        if (frame->kernel_mode != 0U)
            event->flags |= SPZ_EVENT_FLAG_KERNEL_UACCESS;
        spz_debug_transition(state, SPZ_DEBUG_ONE_SHOT_DONE);
        return SPZ_DEBUG_CONSUMED;
    }

    state->added_mdscr_ss = (mdscr & SPZ_MDSCR_SS) == 0U ? 1U : 0U;
    state->added_pstate_ss = (frame->pstate & SPZ_PSTATE_SS) == 0U ? 1U : 0U;
    state->forward_single_step =
        (state->added_mdscr_ss == 0U || state->added_pstate_ss == 0U) ? 1U : 0U;
    stepped_mdscr = mdscr | SPZ_MDSCR_SS;
    if (stepped_mdscr != mdscr) {
        result = controller->ops.write_mdscr(controller->ops.context, cpu, stepped_mdscr);
        if (result != 0) {
            spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
            return result;
        }
    }
    frame->pstate |= SPZ_PSTATE_SS;
    controller->ops.barrier(controller->ops.context, cpu);
    spz_debug_transition(state, SPZ_DEBUG_STEP_PENDING);
    return SPZ_DEBUG_CONSUMED;
}

int spz_debug_handle_break(struct spz_debug_controller *controller, uint32_t cpu,
                           const struct spz_debug_target *current,
                           struct spz_debug_frame *frame, struct spz_event *event)
{
    struct spz_debug_cpu_state *state;

    if (controller == NULL || frame == NULL || event == NULL || cpu >= controller->cpu_count)
        return -EINVAL;
    state = &controller->cpu[cpu];
    if (state->state != SPZ_DEBUG_ARMED || state->kind != SPZ_DEBUG_SLOT_BREAKPOINT ||
        frame->pc != state->request.address)
        return SPZ_DEBUG_NOT_OWNED;
    return spz_debug_handle_hit(controller, cpu, current, frame, frame->pc, event);
}

int spz_debug_handle_watch(struct spz_debug_controller *controller, uint32_t cpu,
                           const struct spz_debug_target *current,
                           struct spz_debug_frame *frame, struct spz_event *event)
{
    struct spz_debug_cpu_state *state;
    uint64_t end;
    int is_write;

    if (controller == NULL || frame == NULL || event == NULL || cpu >= controller->cpu_count)
        return -EINVAL;
    state = &controller->cpu[cpu];
    if (state->state != SPZ_DEBUG_ARMED || state->kind != SPZ_DEBUG_SLOT_WATCHPOINT)
        return SPZ_DEBUG_NOT_OWNED;
    end = state->request.address + (uint64_t)state->request.length;
    if (frame->far < state->request.address || frame->far >= end)
        return SPZ_DEBUG_NOT_OWNED;
    is_write = (frame->esr & SPZ_ESR_WNR) != 0U;
    if ((state->request.kind == SPZ_BREAK_READ && is_write) ||
        (state->request.kind == SPZ_BREAK_WRITE && !is_write))
        return SPZ_DEBUG_NOT_OWNED;
    return spz_debug_handle_hit(controller, cpu, current, frame, frame->far, event);
}

int spz_debug_handle_step(struct spz_debug_controller *controller, uint32_t cpu,
                          const struct spz_debug_target *current,
                          struct spz_debug_frame *frame)
{
    struct spz_debug_cpu_state *state;
    uint64_t live_value;
    uint64_t owner;
    uint64_t mdscr;
    uint64_t restored_mdscr;
    uint32_t live_control;
    int forward;
    int result;

    if (controller == NULL || frame == NULL || cpu >= controller->cpu_count)
        return -EINVAL;
    state = &controller->cpu[cpu];
    if (state->state != SPZ_DEBUG_STEP_PENDING ||
        !spz_debug_target_matches(&state->target, current))
        return SPZ_DEBUG_NOT_OWNED;
    result = spz_debug_live_image(controller, cpu, state, &live_value, &live_control, &owner);
    if (result != 0)
        return result;
    if (owner != 0U || live_value != state->programmed_value ||
        live_control != state->disabled_control) {
        spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
        return -EUCLEAN;
    }
    result = controller->ops.read_mdscr(controller->ops.context, cpu, &mdscr);
    if (result != 0)
        return result;
    restored_mdscr = mdscr;
    if (state->added_mdscr_ss != 0U)
        restored_mdscr &= ~SPZ_MDSCR_SS;
    if (restored_mdscr != mdscr) {
        result = controller->ops.write_mdscr(controller->ops.context, cpu, restored_mdscr);
        if (result != 0) {
            spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
            return result;
        }
    }
    if (state->added_pstate_ss != 0U)
        frame->pstate &= ~SPZ_PSTATE_SS;
    result = controller->ops.write_control(controller->ops.context, cpu, state->kind,
                                           state->slot, state->programmed_control);
    if (result != 0) {
        spz_debug_transition(state, SPZ_DEBUG_QUARANTINED);
        return result;
    }
    controller->ops.barrier(controller->ops.context, cpu);
    forward = state->forward_single_step != 0U ? SPZ_DEBUG_FORWARD_ORIGINAL :
                                                SPZ_DEBUG_CONSUMED;
    state->added_mdscr_ss = 0U;
    state->added_pstate_ss = 0U;
    state->forward_single_step = 0U;
    spz_debug_transition(state, SPZ_DEBUG_ARMED);
    return forward;
}

const struct spz_debug_cpu_state *spz_debug_cpu_state(const struct spz_debug_controller *controller,
                                                       uint32_t cpu)
{
    if (controller == NULL || cpu >= controller->cpu_count)
        return NULL;
    return &controller->cpu[cpu];
}
