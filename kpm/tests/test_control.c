#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mock_debug_regs.h"
#include "../platform/kpatch/compat.h"
#include "test.h"

struct control_fixture {
    const struct spz_device_profile *profile;
    struct spz_module_state module;
    struct mock_debug_regs debug;
    uint8_t task[2][8192];
    uint8_t cred[2][512];
    uint32_t cpu;
    uint32_t queue_calls;
    uint32_t maps_retains;
    uint32_t maps_task_releases;
    uint32_t maps_buffer_releases;
};

static int retain_maps_task(void *opaque, const void *task)
{
    struct control_fixture *fixture = opaque;

    SPZ_EXPECT(task == fixture->task[0]);
    fixture->maps_retains++;
    return 0;
}

static void release_maps_task(void *opaque, const void *task)
{
    struct control_fixture *fixture = opaque;

    SPZ_EXPECT(task == fixture->task[0]);
    fixture->maps_task_releases++;
}

static int render_maps(void *opaque, const void *task, uint8_t **data,
                       uint32_t *length)
{
    static const char content[] = "1000-2000 r-xp 0 00:00 0 /fixture.so\n";
    struct control_fixture *fixture = opaque;

    SPZ_EXPECT(task == fixture->task[0]);
    *length = (uint32_t)(sizeof(content) - 1U);
    *data = malloc(*length);
    if (*data == NULL)
        return -ENOMEM;
    memcpy(*data, content, *length);
    return 0;
}

static void release_maps_buffer(void *opaque, uint8_t *data, uint32_t length)
{
    struct control_fixture *fixture = opaque;

    SPZ_EXPECT(data != NULL);
    SPZ_EXPECT(length != 0U);
    fixture->maps_buffer_releases++;
    free(data);
}

static void put_u32(uint8_t *bytes, uint32_t offset, uint32_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static void put_u64(uint8_t *bytes, uint32_t offset, uint64_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static void init_task(struct control_fixture *fixture, uint32_t cpu, uint32_t pid,
                      uint32_t tgid, uint32_t uid, const char *comm)
{
    uint64_t cred = (uint64_t)(uintptr_t)fixture->cred[cpu];

    put_u32(fixture->task[cpu], fixture->profile->task.pid, pid);
    put_u32(fixture->task[cpu], fixture->profile->task.tgid, tgid);
    put_u64(fixture->task[cpu], fixture->profile->task.start_time,
            UINT64_C(100) + cpu);
    put_u64(fixture->task[cpu], fixture->profile->task.start_boottime,
            UINT64_C(200) + cpu);
    put_u64(fixture->task[cpu], fixture->profile->task.cred, cred);
    put_u32(fixture->cred[cpu], fixture->profile->cred.uid, uid);
    memcpy(fixture->task[cpu] + fixture->profile->task.comm, comm,
           strlen(comm) + 1U);
}

static int current_cpu(void *opaque, uint32_t *cpu)
{
    *cpu = ((struct control_fixture *)opaque)->cpu;
    return 0;
}

static const void *current_task(void *opaque)
{
    struct control_fixture *fixture = (struct control_fixture *)opaque;

    return fixture->task[fixture->cpu];
}

static uint64_t timestamp_ns(void *opaque)
{
    (void)opaque;
    return UINT64_C(987654321);
}

static uint32_t cpu_count(void *opaque)
{
    (void)opaque;
    return 2U;
}

static int each_cpu(void *opaque,
                    int (*callback)(void *callback_context, uint32_t cpu),
                    void *callback_context)
{
    struct control_fixture *fixture = (struct control_fixture *)opaque;
    uint32_t saved = fixture->cpu;
    uint32_t cpu;
    int result = 0;

    for (cpu = 0U; cpu < 2U; cpu++) {
        fixture->cpu = cpu;
        result = callback(callback_context, cpu);
        if (result != 0)
            break;
    }
    fixture->cpu = saved;
    return result;
}

static int queue_async(void *opaque)
{
    ((struct control_fixture *)opaque)->queue_calls++;
    return 0;
}

static void init_control_fixture(struct control_fixture *fixture)
{
    struct spz_profile_runtime runtime;
    struct spz_profile_runtime_ops profile_ops;
    struct spz_platform_ops platform;
    struct spz_async_backend async;
    struct spz_maps_backend maps;
    struct spz_debug_ops debug_ops;

    memset(fixture, 0, sizeof(*fixture));
    fixture->profile = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    init_task(fixture, 0U, 31337U, 31300U, 10234U, "Profile Saver");
    init_task(fixture, 1U, 42U, 42U, 1000U, "other.proc");
    memset(&runtime, 0, sizeof(runtime));
    runtime.profile = fixture->profile;
    runtime.state = SPZ_PROFILE_READY;
    runtime.hooks_allowed = 1U;
    runtime.initial_cpu_count = 2U;
    memset(&profile_ops, 0, sizeof(profile_ops));
    profile_ops.context = fixture;
    profile_ops.cpu_count = cpu_count;
    memset(&platform, 0, sizeof(platform));
    platform.context = fixture;
    platform.current_cpu = current_cpu;
    platform.current_task = current_task;
    platform.timestamp_ns = timestamp_ns;
    platform.run_each_cpu = each_cpu;
    memset(&async, 0, sizeof(async));
    async.queue_context = fixture;
    async.queue = queue_async;
    memset(&maps, 0, sizeof(maps));
    maps.context = fixture;
    maps.max_snapshot_bytes = 4096U;
    maps.max_chunk_bytes = 16U;
    maps.retain_task = retain_maps_task;
    maps.release_task = release_maps_task;
    maps.render = render_maps;
    maps.release_buffer = release_maps_buffer;
    mock_debug_init(&fixture->debug, 6U, 4U);
    debug_ops = mock_debug_ops(&fixture->debug);
    SPZ_EXPECT_EQ(spz_module_core_init(&fixture->module, fixture->profile,
                                       &runtime, &profile_ops, &platform,
                                       &debug_ops, &async, &maps), 0);
}

static int control(struct control_fixture *fixture, const char *literal,
                   char *response, size_t response_capacity)
{
    char command[SPZ_MAX_COMMAND + 1U];
    size_t length = strlen(literal);

    SPZ_EXPECT(length <= SPZ_MAX_COMMAND);
    memcpy(command, literal, length + 1U);
    return spz_control_execute(&fixture->module, command, length, response,
                               response_capacity);
}

static void expect_maps_control_and_task_lifetime(void)
{
    struct control_fixture fixture;
    char response[SPZ_CONTROL_RESPONSE_MAX];
    int length;

    init_control_fixture(&fixture);
    length = control(&fixture, "status", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "maps_supported=1") != NULL);
    SPZ_EXPECT(strstr(response, "maps_state=empty") != NULL);

    length = control(&fixture, "bind pid=31337 mode=pid uid=10234",
                     response, sizeof(response));
    SPZ_EXPECT(length > 0);
    spz_module_finish_after(&fixture.module);
    spz_module_finish_after(&fixture.module);
    SPZ_EXPECT_EQ(fixture.maps_retains, 1U);

    length = control(&fixture, "bind pid=42 mode=pid", response,
                     sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "status=-16 version=1 reason=busy") == response);

    length = control(&fixture, "maps", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "status=0 version=1 request=1") == response);
    SPZ_EXPECT_EQ(fixture.queue_calls, 1U);
    SPZ_EXPECT_EQ(spz_async_run(&fixture.module.async), 0);

    length = control(&fixture, "status", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "maps_state=ready") != NULL);
    SPZ_EXPECT(strstr(response, "maps_snapshot=1") != NULL);
    SPZ_EXPECT(strstr(response, "maps_size=37") != NULL);

    length = control(&fixture, "maps-read snapshot=1 offset=0", response,
                     sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "snapshot=1 offset=0 total=37") != NULL);
    SPZ_EXPECT(strstr(response, "eof=0") != NULL);
    SPZ_EXPECT(strstr(response, "data=313030302d3230303020722d787020") != NULL);

    length = control(&fixture, "maps-read snapshot=1 offset=32", response,
                     sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "offset=32 total=37") != NULL);
    SPZ_EXPECT(strstr(response, "eof=1") != NULL);

    length = control(&fixture, "clear", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "request=2") != NULL);
    SPZ_EXPECT_EQ(spz_async_run(&fixture.module.async), 0);
    SPZ_EXPECT_EQ(fixture.maps_task_releases, 1U);
    SPZ_EXPECT_EQ(fixture.maps_buffer_releases, 1U);
    length = control(&fixture, "status", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "binding=none") != NULL);
    SPZ_EXPECT(strstr(response, "maps_state=empty") != NULL);
    SPZ_EXPECT(strstr(response, "maps_snapshot=0") != NULL);
}

static void expect_control_lifecycle_and_bounds(void)
{
    struct control_fixture fixture;
    struct spz_task_identity identity;
    struct spz_event event;
    char response[SPZ_CONTROL_RESPONSE_MAX];
    char tiny[8];
    char oversized[SPZ_MAX_COMMAND + 2U];
    int length;

    init_control_fixture(&fixture);
    length = control(&fixture, "status", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "status=0 version=1") == response);
    SPZ_EXPECT(strstr(response, "state=ready") != NULL);
    SPZ_EXPECT(strstr(response, "binding=none") != NULL);

    length = control(&fixture, "bind pid=31337 mode=pid uid=10234",
                     response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "status=0 version=1") == response);
    SPZ_EXPECT(strstr(response, "generation=1") != NULL);
    SPZ_EXPECT_EQ(spz_binding_observe_current(
                      &fixture.module.binding, fixture.profile, fixture.task[0],
                      fixture.profile->kernel.task_struct_size, &identity), 1);

    length = control(&fixture,
                     "break id=7 kind=x addr=0x1000 len=4 mode=once",
                     response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "status=0 version=1") == response);
    length = control(&fixture, "enable id=7", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "request=1") != NULL);
    SPZ_EXPECT_EQ(fixture.queue_calls, 1U);
    SPZ_EXPECT_EQ(fixture.module.breakpoint_enabled, 0U);
    SPZ_EXPECT_EQ(spz_async_run(&fixture.module.async), 0);
    SPZ_EXPECT_EQ(fixture.module.breakpoint_enabled, 1U);

    length = control(&fixture, "status", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "binding=bound") != NULL);
    SPZ_EXPECT(strstr(response, "pid=31337") != NULL);
    SPZ_EXPECT(strstr(response,
                      "comm_hex=50726f66696c65205361766572000000") != NULL);
    SPZ_EXPECT(strstr(response, " comm=") == NULL);
    SPZ_EXPECT(strstr(response, "request_state=done") != NULL);
    SPZ_EXPECT(strstr(response, "request_status=0") != NULL);

    memset(&event, 0, sizeof(event));
    event.type = SPZ_EVENT_TASK;
    event.timestamp = UINT64_C(123);
    SPZ_EXPECT_EQ(spz_ring_push(&fixture.module.ring, 0U, &event), 0);
    length = control(&fixture, "poll after=0", response, sizeof(response));
    SPZ_EXPECT(length > (int)(SPZ_EVENT_WIRE_SIZE * 2U));
    SPZ_EXPECT(strstr(response, " event=53505a45") != NULL);

    SPZ_EXPECT_EQ(control(&fixture, "status", tiny, sizeof(tiny)), -ENOSPC);
    memset(oversized, 'a', sizeof(oversized));
    oversized[sizeof(oversized) - 1U] = '\0';
    length = spz_control_execute(&fixture.module, oversized,
                                 sizeof(oversized) - 1U, response,
                                 sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "status=-7 version=1 reason=too_large") ==
               response);

    length = control(&fixture, "break id=7 id=8 kind=x addr=0x1000 len=4 mode=once",
                     response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "status=-22 version=1 reason=invalid") ==
               response);

    __atomic_store_n(&fixture.module.control_busy, 1U, __ATOMIC_RELEASE);
    length = control(&fixture, "status", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "status=-16 version=1 reason=busy") == response);
    __atomic_store_n(&fixture.module.control_busy, 0U, __ATOMIC_RELEASE);

    length = control(&fixture, "disable id=7", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "request=2") != NULL);
    SPZ_EXPECT_EQ(spz_async_run(&fixture.module.async), 0);
    length = control(&fixture, "clear", response, sizeof(response));
    SPZ_EXPECT(length > 0);
    SPZ_EXPECT(strstr(response, "request=3") != NULL);
    SPZ_EXPECT_EQ(spz_async_run(&fixture.module.async), 0);
    SPZ_EXPECT_EQ(spz_module_can_exit(&fixture.module), 0);
}

int test_control(void)
{
    expect_control_lifecycle_and_bounds();
    expect_maps_control_and_task_lifetime();
    return 0;
}
