#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mock_debug_regs.h"
#include "stackplz/debug.h"
#include "test.h"

#ifndef SPZ_MODEL_STEPS
#define SPZ_MODEL_STEPS 100000U
#endif

static struct spz_debug_request execute_request(enum spz_break_mode mode)
{
    struct spz_debug_request request;

    memset(&request, 0, sizeof(request));
    request.id = 7U;
    request.kind = SPZ_BREAK_EXECUTE;
    request.address = UINT64_C(0x1000);
    request.length = 4U;
    request.mode = mode;
    return request;
}

static struct spz_debug_request watch_request(enum spz_break_kind kind,
                                               enum spz_break_mode mode)
{
    struct spz_debug_request request;

    memset(&request, 0, sizeof(request));
    request.id = 8U;
    request.kind = kind;
    request.address = UINT64_C(0x1003);
    request.length = 2U;
    request.mode = mode;
    return request;
}

static struct spz_debug_target debug_target(void)
{
    struct spz_debug_target target;

    memset(&target, 0, sizeof(target));
    target.binding_id = 9U;
    target.identity.generation = 33U;
    target.identity.task_cookie = UINT64_C(0x12345678);
    target.identity.pid = 31337U;
    target.identity.tgid = 31300U;
    target.identity.uid = 10234U;
    target.identity.start_time = 111U;
    target.identity.start_boot_time = 222U;
    memcpy(target.identity.comm, "target.proc", sizeof("target.proc"));
    return target;
}

static struct spz_debug_frame debug_frame(uint64_t pc)
{
    struct spz_debug_frame frame;
    uint32_t index;

    memset(&frame, 0, sizeof(frame));
    for (index = 0U; index < 31U; index++)
        frame.x[index] = UINT64_C(0x100) + index;
    frame.sp = UINT64_C(0x7ffffff000);
    frame.pc = pc;
    frame.pstate = UINT64_C(0x600003c0);
    frame.esr = UINT64_C(0x22) << 26U;
    frame.far = pc;
    return frame;
}

static void init_controller(struct mock_debug_regs *mock,
                            struct spz_debug_controller *controller)
{
    struct spz_debug_ops ops;

    mock_debug_init(mock, 6U, 4U);
    ops = mock_debug_ops(mock);
    SPZ_EXPECT_EQ(spz_debug_controller_init(controller, &ops, 2U), 0);
}

static void expect_request_validation(void)
{
    struct spz_debug_request request = execute_request(SPZ_BREAK_ONCE);
    uint64_t value;
    uint32_t control;
    uint8_t bas;

    SPZ_EXPECT_EQ(spz_debug_validate_request(&request, &value, &control, &bas), 0);
    SPZ_EXPECT_EQ(value, UINT64_C(0x1000));
    SPZ_EXPECT_EQ(bas, UINT8_C(0x0f));
    SPZ_EXPECT_EQ(control, UINT32_C(0x1e5));

    request = watch_request(SPZ_BREAK_READ_WRITE, SPZ_BREAK_REPEAT);
    SPZ_EXPECT_EQ(spz_debug_validate_request(&request, &value, &control, &bas), 0);
    SPZ_EXPECT_EQ(value, UINT64_C(0x1000));
    SPZ_EXPECT_EQ(bas, UINT8_C(0x18));
    SPZ_EXPECT_EQ(control, UINT32_C(0x31d));

    request = execute_request(SPZ_BREAK_ONCE);
    request.id = 0U;
    SPZ_EXPECT(spz_debug_validate_request(&request, &value, &control, &bas) < 0);
    request = execute_request(SPZ_BREAK_ONCE);
    request.address = UINT64_C(0xffff000000001000);
    SPZ_EXPECT(spz_debug_validate_request(&request, &value, &control, &bas) < 0);
    request = execute_request(SPZ_BREAK_ONCE);
    request.address += 2U;
    SPZ_EXPECT(spz_debug_validate_request(&request, &value, &control, &bas) < 0);
    request = execute_request(SPZ_BREAK_ONCE);
    request.length = 2U;
    SPZ_EXPECT(spz_debug_validate_request(&request, &value, &control, &bas) < 0);
    request = watch_request(SPZ_BREAK_WRITE, SPZ_BREAK_ONCE);
    request.length = 3U;
    SPZ_EXPECT(spz_debug_validate_request(&request, &value, &control, &bas) < 0);
    request = watch_request(SPZ_BREAK_WRITE, SPZ_BREAK_ONCE);
    request.address = UINT64_C(0x1007);
    SPZ_EXPECT(spz_debug_validate_request(&request, &value, &control, &bas) < 0);
    request = watch_request(SPZ_BREAK_INVALID, SPZ_BREAK_ONCE);
    SPZ_EXPECT(spz_debug_validate_request(&request, &value, &control, &bas) < 0);
    request = execute_request(SPZ_BREAK_MODE_INVALID);
    SPZ_EXPECT(spz_debug_validate_request(&request, &value, &control, &bas) < 0);
}

static void expect_highest_free_slot_and_exact_restore(void)
{
    struct mock_debug_regs mock;
    struct spz_debug_controller controller;
    struct spz_debug_request request = execute_request(SPZ_BREAK_ONCE);
    struct spz_debug_target target = debug_target();
    const struct spz_debug_cpu_state *state;

    init_controller(&mock, &controller);
    mock.owner[0][0][5] = UINT64_C(0xabc);
    mock.control[0][0][5] = SPZ_DEBUG_CTRL_ENABLE;
    mock.value[0][0][4] = UINT64_C(0xfeed);
    mock.mdscr[0] = UINT64_C(0x100);
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    state = spz_debug_cpu_state(&controller, 0U);
    SPZ_EXPECT_EQ(state->state, SPZ_DEBUG_ARMED);
    SPZ_EXPECT_EQ(state->slot, 4U);
    SPZ_EXPECT_EQ(state->saved_value, UINT64_C(0xfeed));
    SPZ_EXPECT_EQ(mock.value[0][0][4], UINT64_C(0x1000));
    SPZ_EXPECT_EQ(mock.control[0][0][4], UINT32_C(0x1e5));
    SPZ_EXPECT_EQ(mock.mdscr[0], UINT64_C(0x100) | SPZ_MDSCR_MDE);
    SPZ_EXPECT_EQ(mock.log[0].kind, MOCK_WRITE_VALUE);
    SPZ_EXPECT_EQ(mock.log[1].kind, MOCK_WRITE_CONTROL);
    SPZ_EXPECT_EQ(mock.log[2].kind, MOCK_WRITE_MDSCR);
    SPZ_EXPECT_EQ(mock.log[3].kind, MOCK_WRITE_BARRIER);

    SPZ_EXPECT_EQ(spz_debug_restore_cpu(&controller, 0U), 0);
    state = spz_debug_cpu_state(&controller, 0U);
    SPZ_EXPECT_EQ(state->state, SPZ_DEBUG_EMPTY);
    SPZ_EXPECT_EQ(state->saved_value, 0U);
    SPZ_EXPECT_EQ(mock.value[0][0][4], UINT64_C(0xfeed));
    SPZ_EXPECT_EQ(mock.control[0][0][4], 0U);
    SPZ_EXPECT_EQ(mock.mdscr[0], UINT64_C(0x100) | SPZ_MDSCR_MDE);
}

static void expect_slot_disagreement_fails_closed(void)
{
    struct mock_debug_regs mock;
    struct spz_debug_controller controller;
    struct spz_debug_request request = execute_request(SPZ_BREAK_ONCE);
    struct spz_debug_target target = debug_target();
    uint32_t slot;

    init_controller(&mock, &controller);
    for (slot = 0U; slot < 6U; slot++) {
        mock.owner[0][0][slot] = slot + 1U;
        mock.control[0][0][slot] = SPZ_DEBUG_CTRL_ENABLE;
    }
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), -EBUSY);
    SPZ_EXPECT_EQ(mock_debug_write_count(&mock), 0U);

    init_controller(&mock, &controller);
    mock.control[0][0][5] = SPZ_DEBUG_CTRL_ENABLE;
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), -EBUSY);
    SPZ_EXPECT_EQ(mock_debug_write_count(&mock), 0U);

    init_controller(&mock, &controller);
    mock.owner[0][0][5] = 1U;
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), -EBUSY);
    SPZ_EXPECT_EQ(mock_debug_write_count(&mock), 0U);
}

static void expect_foreign_change_quarantines_without_write(void)
{
    struct mock_debug_regs mock;
    struct spz_debug_controller controller;
    struct spz_debug_request request = execute_request(SPZ_BREAK_ONCE);
    struct spz_debug_target target = debug_target();
    const struct spz_debug_cpu_state *state;
    uint32_t writes;

    init_controller(&mock, &controller);
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    state = spz_debug_cpu_state(&controller, 0U);
    mock.value[0][0][state->slot] ^= UINT64_C(0x100);
    writes = mock_debug_write_count(&mock);
    SPZ_EXPECT_EQ(spz_debug_restore_cpu(&controller, 0U), -EUCLEAN);
    SPZ_EXPECT_EQ(mock_debug_write_count(&mock), writes);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&controller, 0U)->state, SPZ_DEBUG_QUARANTINED);
    SPZ_EXPECT_EQ(spz_debug_restore_cpu(&controller, 0U), -EUCLEAN);
    SPZ_EXPECT_EQ(mock_debug_write_count(&mock), writes);
}

static void expect_one_shot_break_capture(void)
{
    struct mock_debug_regs mock;
    struct spz_debug_controller controller;
    struct spz_debug_request request = execute_request(SPZ_BREAK_ONCE);
    struct spz_debug_target target = debug_target();
    struct spz_debug_frame frame = debug_frame(request.address);
    struct spz_event event;
    uint32_t writes;

    init_controller(&mock, &controller);
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    memset(&event, 0, sizeof(event));
    mock.copy_probe_event = &event;
    writes = mock_debug_write_count(&mock);
    {
        struct spz_debug_target wrong_target = target;

        wrong_target.identity.generation++;
        SPZ_EXPECT_EQ(spz_debug_handle_break(&controller, 0U, &wrong_target, &frame, &event),
                      SPZ_DEBUG_NOT_OWNED);
        SPZ_EXPECT_EQ(mock_debug_write_count(&mock), writes);
    }
    frame.pc += 4U;
    SPZ_EXPECT_EQ(spz_debug_handle_break(&controller, 0U, &target, &frame, &event),
                  SPZ_DEBUG_NOT_OWNED);
    SPZ_EXPECT_EQ(mock_debug_write_count(&mock), writes);
    frame.pc = request.address;
    SPZ_EXPECT_EQ(spz_debug_handle_break(&controller, 0U, &target, &frame, &event),
                  SPZ_DEBUG_CONSUMED);
    SPZ_EXPECT(mock.copied_before_disable != 0U);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&controller, 0U)->state, SPZ_DEBUG_ONE_SHOT_DONE);
    SPZ_EXPECT_EQ(mock.control[0][0][5] & SPZ_DEBUG_CTRL_ENABLE, 0U);
    SPZ_EXPECT_EQ(event.type, SPZ_EVENT_BREAKPOINT);
    SPZ_EXPECT_EQ(event.binding_id, target.binding_id);
    SPZ_EXPECT_EQ(event.breakpoint_id, request.id);
    SPZ_EXPECT_EQ(event.generation, target.identity.generation);
    SPZ_EXPECT_EQ(event.pid, target.identity.pid);
    SPZ_EXPECT_EQ(event.requested_address, request.address);
    SPZ_EXPECT_EQ(event.observed_address, frame.pc);
    SPZ_EXPECT_EQ(event.registers.x[30], frame.x[30]);
    SPZ_EXPECT_EQ(event.registers.pc, frame.pc);
}

static void expect_repeat_and_preexisting_step(void)
{
    struct mock_debug_regs mock;
    struct spz_debug_controller controller;
    struct spz_debug_request request = execute_request(SPZ_BREAK_REPEAT);
    struct spz_debug_target target = debug_target();
    struct spz_debug_frame frame = debug_frame(request.address);
    struct spz_event event;

    init_controller(&mock, &controller);
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    SPZ_EXPECT_EQ(spz_debug_handle_break(&controller, 0U, &target, &frame, &event),
                  SPZ_DEBUG_CONSUMED);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&controller, 0U)->state, SPZ_DEBUG_STEP_PENDING);
    SPZ_EXPECT((frame.pstate & SPZ_PSTATE_SS) != 0U);
    SPZ_EXPECT((mock.mdscr[0] & SPZ_MDSCR_SS) != 0U);

    SPZ_EXPECT_EQ(spz_debug_handle_step(&controller, 0U, &target, &frame),
                  SPZ_DEBUG_CONSUMED);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&controller, 0U)->state, SPZ_DEBUG_ARMED);
    SPZ_EXPECT((frame.pstate & SPZ_PSTATE_SS) == 0U);
    SPZ_EXPECT((mock.mdscr[0] & SPZ_MDSCR_SS) == 0U);
    SPZ_EXPECT((mock.control[0][0][5] & SPZ_DEBUG_CTRL_ENABLE) != 0U);

    init_controller(&mock, &controller);
    frame = debug_frame(request.address);
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    SPZ_EXPECT_EQ(spz_debug_handle_break(&controller, 0U, &target, &frame, &event),
                  SPZ_DEBUG_CONSUMED);
    {
        const struct spz_debug_cpu_state *state = spz_debug_cpu_state(&controller, 0U);
        uint32_t writes;

        mock.control[0][0][state->slot] ^= UINT32_C(0x20);
        writes = mock_debug_write_count(&mock);
        SPZ_EXPECT_EQ(spz_debug_handle_step(&controller, 0U, &target, &frame), -EUCLEAN);
        SPZ_EXPECT_EQ(mock_debug_write_count(&mock), writes);
        SPZ_EXPECT_EQ(spz_debug_cpu_state(&controller, 0U)->state, SPZ_DEBUG_QUARANTINED);
    }

    init_controller(&mock, &controller);
    mock.mdscr[0] = SPZ_MDSCR_SS | SPZ_MDSCR_MDE;
    frame = debug_frame(request.address);
    frame.pstate |= SPZ_PSTATE_SS;
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    SPZ_EXPECT_EQ(spz_debug_handle_break(&controller, 0U, &target, &frame, &event),
                  SPZ_DEBUG_CONSUMED);
    SPZ_EXPECT_EQ(spz_debug_handle_step(&controller, 0U, &target, &frame),
                  SPZ_DEBUG_FORWARD_ORIGINAL);
    SPZ_EXPECT((frame.pstate & SPZ_PSTATE_SS) != 0U);
    SPZ_EXPECT((mock.mdscr[0] & SPZ_MDSCR_SS) != 0U);
}

static void expect_watch_direction_and_kernel_degrade(void)
{
    struct mock_debug_regs mock;
    struct spz_debug_controller controller;
    struct spz_debug_request request = watch_request(SPZ_BREAK_READ, SPZ_BREAK_ONCE);
    struct spz_debug_target target = debug_target();
    struct spz_debug_frame frame = debug_frame(UINT64_C(0x2000));
    struct spz_event event;
    uint32_t writes;

    init_controller(&mock, &controller);
    frame.far = request.address;
    frame.esr |= SPZ_ESR_WNR;
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    writes = mock_debug_write_count(&mock);
    SPZ_EXPECT_EQ(spz_debug_handle_watch(&controller, 0U, &target, &frame, &event),
                  SPZ_DEBUG_NOT_OWNED);
    SPZ_EXPECT_EQ(mock_debug_write_count(&mock), writes);
    frame.esr &= ~SPZ_ESR_WNR;
    SPZ_EXPECT_EQ(spz_debug_handle_watch(&controller, 0U, &target, &frame, &event),
                  SPZ_DEBUG_CONSUMED);
    SPZ_EXPECT_EQ(event.type, SPZ_EVENT_WATCHPOINT);
    SPZ_EXPECT_EQ(event.observed_address, request.address);

    init_controller(&mock, &controller);
    request = watch_request(SPZ_BREAK_READ_WRITE, SPZ_BREAK_REPEAT);
    frame = debug_frame(UINT64_C(0x2000));
    frame.far = request.address + 1U;
    frame.kernel_mode = 1U;
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    SPZ_EXPECT_EQ(spz_debug_handle_watch(&controller, 0U, &target, &frame, &event),
                  SPZ_DEBUG_CONSUMED);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&controller, 0U)->state, SPZ_DEBUG_ONE_SHOT_DONE);
    SPZ_EXPECT((event.flags & SPZ_EVENT_FLAG_KERNEL_UACCESS) != 0U);
    SPZ_EXPECT((frame.pstate & SPZ_PSTATE_SS) == 0U);
}

static void expect_migration_and_mdscr_coexistence(void)
{
    struct mock_debug_regs mock;
    struct spz_debug_controller controller;
    struct spz_debug_request request = execute_request(SPZ_BREAK_ONCE);
    struct spz_debug_target target = debug_target();

    init_controller(&mock, &controller);
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    SPZ_EXPECT_EQ(spz_debug_restore_cpu(&controller, 0U), 0);
    SPZ_EXPECT((mock.mdscr[0] & SPZ_MDSCR_MDE) == 0U);
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 1U, &request, &target), 0);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&controller, 0U)->state, SPZ_DEBUG_EMPTY);
    SPZ_EXPECT_EQ(spz_debug_cpu_state(&controller, 1U)->state, SPZ_DEBUG_ARMED);

    SPZ_EXPECT_EQ(spz_debug_restore_cpu(&controller, 1U), 0);
    SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
    mock.owner[0][0][0] = 1U;
    mock.control[0][0][0] = SPZ_DEBUG_CTRL_ENABLE;
    SPZ_EXPECT_EQ(spz_debug_restore_cpu(&controller, 0U), 0);
    SPZ_EXPECT((mock.mdscr[0] & SPZ_MDSCR_MDE) != 0U);
}

static uint32_t model_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void assert_model_invariants(const struct mock_debug_regs *mock,
                                    const struct spz_debug_controller *controller)
{
    const struct spz_debug_cpu_state *state = spz_debug_cpu_state(controller, 0U);
    uint32_t slot;

    SPZ_EXPECT(state != NULL);
    for (slot = 0U; slot < mock->brp_count; slot++) {
        if (state->state != SPZ_DEBUG_EMPTY && state->kind == SPZ_DEBUG_SLOT_BREAKPOINT &&
            state->slot == slot)
            continue;
        SPZ_EXPECT_EQ(mock->value[0][0][slot], 0U);
        SPZ_EXPECT_EQ(mock->control[0][0][slot], 0U);
    }
    if (state->state == SPZ_DEBUG_EMPTY)
        SPZ_EXPECT_EQ(state->saved_value, 0U);
    if (state->state == SPZ_DEBUG_ARMED)
        SPZ_EXPECT_EQ(mock->control[0][0][state->slot], state->programmed_control);
    if (state->state == SPZ_DEBUG_STEP_PENDING || state->state == SPZ_DEBUG_ONE_SHOT_DONE)
        SPZ_EXPECT_EQ(mock->control[0][0][state->slot], state->disabled_control);
}

static void expect_randomized_model(void)
{
    const uint32_t seed = UINT32_C(0x5a17c0de);
    uint32_t random_state = seed;
    struct mock_debug_regs mock;
    struct spz_debug_controller controller;
    struct spz_debug_target target = debug_target();
    struct spz_debug_request request = execute_request(SPZ_BREAK_REPEAT);
    uint32_t step;

    init_controller(&mock, &controller);
    for (step = 0U; step < (uint32_t)SPZ_MODEL_STEPS; step++) {
        const struct spz_debug_cpu_state *state = spz_debug_cpu_state(&controller, 0U);
        uint32_t operation = model_random(&random_state) % 7U;

        if (state->state == SPZ_DEBUG_EMPTY) {
            request.mode = (operation & 1U) != 0U ? SPZ_BREAK_REPEAT : SPZ_BREAK_ONCE;
            SPZ_EXPECT_EQ(spz_debug_arm_current(&controller, 0U, &request, &target), 0);
        } else if (state->state == SPZ_DEBUG_ARMED && operation <= 2U) {
            struct spz_debug_frame frame = debug_frame(request.address);
            struct spz_event event;

            SPZ_EXPECT_EQ(spz_debug_handle_break(&controller, 0U, &target, &frame, &event),
                          SPZ_DEBUG_CONSUMED);
        } else if (state->state == SPZ_DEBUG_ARMED && operation == 3U) {
            struct spz_debug_frame frame = debug_frame(request.address + 4U);
            struct spz_event event;
            uint32_t writes = mock_debug_write_count(&mock);

            SPZ_EXPECT_EQ(spz_debug_handle_break(&controller, 0U, &target, &frame, &event),
                          SPZ_DEBUG_NOT_OWNED);
            SPZ_EXPECT_EQ(mock_debug_write_count(&mock), writes);
        } else if (state->state == SPZ_DEBUG_ARMED && operation == 4U) {
            uint32_t slot = state->slot;
            uint32_t writes;

            mock.value[0][0][slot] ^= UINT64_C(0x40);
            writes = mock_debug_write_count(&mock);
            SPZ_EXPECT_EQ(spz_debug_restore_cpu(&controller, 0U), -EUCLEAN);
            SPZ_EXPECT_EQ(mock_debug_write_count(&mock), writes);
        } else if (state->state == SPZ_DEBUG_STEP_PENDING) {
            struct spz_debug_frame frame = debug_frame(request.address + 4U);
            int result = spz_debug_handle_step(&controller, 0U, &target, &frame);

            SPZ_EXPECT(result == SPZ_DEBUG_CONSUMED || result == SPZ_DEBUG_FORWARD_ORIGINAL);
        } else if (state->state == SPZ_DEBUG_QUARANTINED) {
            uint32_t writes = mock_debug_write_count(&mock);

            SPZ_EXPECT_EQ(spz_debug_restore_cpu(&controller, 0U), -EUCLEAN);
            SPZ_EXPECT_EQ(mock_debug_write_count(&mock), writes);
            init_controller(&mock, &controller);
        } else {
            SPZ_EXPECT_EQ(spz_debug_restore_cpu(&controller, 0U), 0);
        }
        assert_model_invariants(&mock, &controller);
    }
    printf("debug-model seed=0x%08x steps=%u\n", seed, (unsigned int)SPZ_MODEL_STEPS);
}

int test_debug(void)
{
    expect_request_validation();
    expect_highest_free_slot_and_exact_restore();
    expect_slot_disagreement_fails_closed();
    expect_foreign_change_quarantines_without_write();
    expect_one_shot_break_capture();
    expect_repeat_and_preexisting_step();
    expect_watch_direction_and_kernel_degrade();
    expect_migration_and_mdscr_coexistence();
    expect_randomized_model();
    return 0;
}
