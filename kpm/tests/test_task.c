#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "stackplz/task.h"
#include "test.h"

struct task_fixture {
    uint8_t task[8192];
    uint8_t cred[512];
};

struct observe_args {
    struct spz_binding *binding;
    const struct spz_device_profile *profile;
    struct task_fixture *fixture;
    uint32_t *start;
    int result;
};

static void task_put_u32(struct task_fixture *fixture, uint32_t offset, uint32_t value)
{
    memcpy(fixture->task + offset, &value, sizeof(value));
}

static void task_put_u64(struct task_fixture *fixture, uint32_t offset, uint64_t value)
{
    memcpy(fixture->task + offset, &value, sizeof(value));
}

static void init_task_fixture(struct task_fixture *fixture,
                              const struct spz_device_profile *profile,
                              uint32_t pid, uint32_t tgid, uint32_t uid,
                              uint64_t start, const char *comm)
{
    uint64_t cred_pointer;

    memset(fixture, 0, sizeof(*fixture));
    task_put_u32(fixture, profile->task.pid, pid);
    task_put_u32(fixture, profile->task.tgid, tgid);
    task_put_u64(fixture, profile->task.start_time, start - 1U);
    task_put_u64(fixture, profile->task.start_boottime, start);
    memcpy(fixture->task + profile->task.comm, comm, strlen(comm) + 1U);
    cred_pointer = (uint64_t)(uintptr_t)fixture->cred;
    task_put_u64(fixture, profile->task.cred, cred_pointer);
    memcpy(fixture->cred + profile->cred.uid, &uid, sizeof(uid));
}

static struct spz_binding_request request_for(uint32_t pid, enum spz_bind_mode mode)
{
    struct spz_binding_request request;

    memset(&request, 0, sizeof(request));
    request.binding_id = 1U;
    request.pid = pid;
    request.mode = mode;
    return request;
}

static void expect_pid_tgid_and_constraints(void)
{
    const struct spz_device_profile *profile = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    struct task_fixture fixture;
    struct spz_binding binding;
    struct spz_binding_request request;
    struct spz_task_identity identity;
    struct spz_binding_snapshot snapshot;
    uint64_t generation;

    init_task_fixture(&fixture, profile, 31337U, 31300U, 10234U, 777U, "target.proc");
    spz_binding_init(&binding);
    request = request_for(31337U, SPZ_BIND_PID);
    request.has_uid = 1U;
    request.uid = 10234U;
    request.has_comm = 1U;
    memcpy(request.comm, "target.proc", sizeof("target.proc"));
    request.has_start_boot_time = 1U;
    request.start_boot_time = 777U;
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(generation, 1U);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 1);
    SPZ_EXPECT_EQ(identity.generation, generation);
    SPZ_EXPECT_EQ(identity.pid, 31337U);
    SPZ_EXPECT_EQ(identity.tgid, 31300U);
    SPZ_EXPECT_EQ(identity.uid, 10234U);
    SPZ_EXPECT_EQ(identity.start_boot_time, 777U);
    SPZ_EXPECT(strcmp(identity.comm, "target.proc") == 0);
    SPZ_EXPECT_EQ(identity.task_cookie, (uint64_t)(uintptr_t)fixture.task);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_BOUND);
    SPZ_EXPECT_EQ(spz_binding_matches_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size,
                                              generation, &identity), 1);

    spz_binding_init(&binding);
    request = request_for(31300U, SPZ_BIND_TGID);
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 1);

    spz_binding_init(&binding);
    request = request_for(31300U, SPZ_BIND_EITHER);
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 1);

    spz_binding_init(&binding);
    request = request_for(31337U, SPZ_BIND_PID);
    request.has_uid = 1U;
    request.uid = 9999U;
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 0);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_PENDING);

    spz_binding_init(&binding);
    request = request_for(31337U, SPZ_BIND_PID);
    request.has_comm = 1U;
    memcpy(request.comm, "other.proc", sizeof("other.proc"));
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 0);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_PENDING);

    spz_binding_init(&binding);
    request = request_for(31337U, SPZ_BIND_PID);
    request.has_start_boot_time = 1U;
    request.start_boot_time = 778U;
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 0);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_PENDING);
}

static void expect_pid_reuse_and_exit(void)
{
    const struct spz_device_profile *profile = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    struct task_fixture fixture;
    struct task_fixture unrelated;
    struct spz_binding binding;
    struct spz_binding_request request = request_for(77U, SPZ_BIND_PID);
    struct spz_task_identity identity;
    struct spz_binding_snapshot snapshot;
    uint64_t generation;

    init_task_fixture(&fixture, profile, 77U, 77U, 10001U, 1000U, "reuse.test");
    spz_binding_init(&binding);
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 1);
    init_task_fixture(&unrelated, profile, 78U, 77U, 10001U, 1001U,
                      "reuse.test");
    SPZ_EXPECT_EQ(spz_binding_matches_current(&binding, profile, unrelated.task,
                                              profile->kernel.task_struct_size,
                                              generation, &identity), 0);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_BOUND);
    task_put_u64(&fixture, profile->task.start_boottime, 2000U);
    SPZ_EXPECT_EQ(spz_binding_matches_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size,
                                              generation, &identity), -ESTALE);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_STALE);

    init_task_fixture(&fixture, profile, 77U, 77U, 10001U, 3000U, "reuse.test");
    spz_binding_init(&binding);
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 1);
    SPZ_EXPECT_EQ(spz_binding_mark_exit(&binding, profile, fixture.task,
                                        profile->kernel.task_struct_size), 1);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_EXITED);
    SPZ_EXPECT_EQ(spz_binding_matches_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size,
                                              generation, &identity), -ESRCH);

    init_task_fixture(&fixture, profile, 77U, 77U, 10001U, 4000U, "reuse.test");
    spz_binding_init(&binding);
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 1);
    task_put_u32(&fixture, profile->task.exit_state, 1U);
    SPZ_EXPECT_EQ(spz_binding_matches_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size,
                                              generation, &identity), -ESRCH);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_EXITED);
}

static void expect_generation_and_clear(void)
{
    const struct spz_device_profile *profile = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    struct task_fixture fixture;
    struct spz_binding binding;
    struct spz_binding_request request = request_for(5U, SPZ_BIND_PID);
    struct spz_binding_snapshot snapshot;
    struct spz_task_identity identity;
    uint64_t generation;

    init_task_fixture(&fixture, profile, 5U, 5U, 1000U, 55U, "gen.test");
    spz_binding_init(&binding);
    __atomic_store_n(&binding.generation, UINT64_MAX, __ATOMIC_RELAXED);
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(generation, 1U);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size, &identity), 1);
    SPZ_EXPECT_EQ(spz_binding_clear(&binding), 0);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_EMPTY);
    SPZ_EXPECT_EQ(snapshot.identity.task_cookie, 0U);
    SPZ_EXPECT_EQ(snapshot.identity.pid, 0U);
    SPZ_EXPECT(snapshot.generation != generation);
    SPZ_EXPECT_EQ(spz_binding_matches_current(&binding, profile, fixture.task,
                                              profile->kernel.task_struct_size,
                                              generation, &identity), -ESTALE);
}

static void expect_idle_task_is_an_ordinary_nonmatch(void)
{
    const struct spz_device_profile *profile =
        spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    struct task_fixture target;
    struct task_fixture idle;
    struct spz_binding binding;
    struct spz_binding_request request = request_for(91U, SPZ_BIND_PID);
    struct spz_binding_snapshot snapshot;
    struct spz_task_identity identity;
    uint64_t generation;

    init_task_fixture(&target, profile, 91U, 91U, 1000U, 9100U, "target.idle");
    init_task_fixture(&idle, profile, 0U, 0U, 0U, 0U, "swapper/0");
    spz_binding_init(&binding);
    SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
    SPZ_EXPECT_EQ(spz_binding_observe_current(&binding, profile, target.task,
                                              profile->kernel.task_struct_size,
                                              &identity), 1);

    SPZ_EXPECT_EQ(spz_binding_matches_current(&binding, profile, idle.task,
                                              profile->kernel.task_struct_size,
                                              generation, &identity), 0);
    SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
    SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_BOUND);
}

static void *observe_thread(void *opaque)
{
    struct observe_args *args = (struct observe_args *)opaque;
    struct spz_task_identity identity;

    while (__atomic_load_n(args->start, __ATOMIC_ACQUIRE) == 0U)
        ;
    args->result = spz_binding_observe_current(args->binding, args->profile,
                                               args->fixture->task,
                                               args->profile->kernel.task_struct_size,
                                               &identity);
    return NULL;
}

static void expect_concurrent_clear(void)
{
    const struct spz_device_profile *profile = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    uint32_t iteration;

    for (iteration = 0U; iteration < 100U; iteration++) {
        struct task_fixture fixture;
        struct spz_binding binding;
        struct spz_binding_request request = request_for(88U, SPZ_BIND_PID);
        struct spz_binding_snapshot snapshot;
        struct observe_args args;
        pthread_t thread;
        uint32_t start = 0U;
        uint64_t generation;

        init_task_fixture(&fixture, profile, 88U, 88U, 10088U, iteration + 1U, "race.test");
        spz_binding_init(&binding);
        SPZ_EXPECT_EQ(spz_binding_set(&binding, &request, &generation), 0);
        args.binding = &binding;
        args.profile = profile;
        args.fixture = &fixture;
        args.start = &start;
        args.result = 0;
        SPZ_EXPECT_EQ(pthread_create(&thread, NULL, observe_thread, &args), 0);
        __atomic_store_n(&start, 1U, __ATOMIC_RELEASE);
        SPZ_EXPECT_EQ(spz_binding_clear(&binding), 0);
        SPZ_EXPECT_EQ(pthread_join(thread, NULL), 0);
        SPZ_EXPECT(args.result == 0 || args.result == 1 || args.result == -ESTALE || args.result == -EAGAIN);
        SPZ_EXPECT_EQ(spz_binding_snapshot(&binding, &snapshot), 0);
        SPZ_EXPECT_EQ(snapshot.state, SPZ_BINDING_EMPTY);
        SPZ_EXPECT_EQ(snapshot.identity.task_cookie, 0U);
        SPZ_EXPECT(snapshot.generation != generation);
    }
}

int test_task(void)
{
    expect_pid_tgid_and_constraints();
    expect_pid_reuse_and_exit();
    expect_generation_and_clear();
    expect_idle_task_is_an_ordinary_nonmatch();
    expect_concurrent_clear();
    return 0;
}
