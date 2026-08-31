#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>

#include "stackplz/core.h"
#include "test.h"

static struct spz_ring test_ring_storage;

#define SPZ_STRESS_CPUS 4U
#define SPZ_STRESS_EVENTS 128U

struct producer_args {
    struct spz_ring *ring;
    uint32_t cpu;
    uint32_t *done;
    int error;
};

static struct spz_event event_with_value(uint64_t value)
{
    struct spz_event event;

    memset(&event, 0, sizeof(event));
    event.type = SPZ_EVENT_BREAKPOINT;
    event.value = value;
    return event;
}

static void expect_crc(void)
{
    static const char input[] = "123456789";

    SPZ_EXPECT_EQ(spz_crc32_ieee(input, sizeof(input) - 1U), UINT32_C(0xcbf43926));
    SPZ_EXPECT_EQ(spz_crc32_ieee(input, sizeof(input) - 1U), spz_crc32_ieee(input, sizeof(input) - 1U));
}

static void expect_global_order(void)
{
    struct spz_event first = event_with_value(11U);
    struct spz_event second = event_with_value(22U);
    struct spz_event output;

    spz_ring_init(&test_ring_storage);
    SPZ_EXPECT_EQ(spz_ring_push(&test_ring_storage, 3U, &first), 0);
    SPZ_EXPECT_EQ(spz_ring_push(&test_ring_storage, 0U, &second), 0);
    SPZ_EXPECT_EQ(spz_ring_pop_after(&test_ring_storage, 0U, &output), 1);
    SPZ_EXPECT_EQ(output.sequence, 1U);
    SPZ_EXPECT_EQ(output.cpu, 3U);
    SPZ_EXPECT_EQ(output.value, 11U);
    SPZ_EXPECT_EQ(output.magic, SPZ_EVENT_MAGIC);
    SPZ_EXPECT_EQ(output.version, SPZ_ABI_VERSION);
    SPZ_EXPECT_EQ(output.size, SPZ_EVENT_WIRE_SIZE);
    SPZ_EXPECT_EQ(output.crc32, spz_crc32_ieee(&output, offsetof(struct spz_event, crc32)));
    SPZ_EXPECT_EQ(spz_ring_pop_after(&test_ring_storage, 1U, &output), 1);
    SPZ_EXPECT_EQ(output.sequence, 2U);
    SPZ_EXPECT_EQ(output.cpu, 0U);
    SPZ_EXPECT_EQ(output.value, 22U);
    SPZ_EXPECT_EQ(spz_ring_pop_after(&test_ring_storage, 2U, &output), 0);
    SPZ_EXPECT_EQ(spz_ring_pop_after(&test_ring_storage, 1U, &output), -ESTALE);
}

static void expect_wrap_without_overwrite(void)
{
    struct spz_event output;
    uint64_t after = 0U;
    uint64_t index;

    spz_ring_init(&test_ring_storage);
    for (index = 0U; index < SPZ_RING_CAPACITY * 3U; index++) {
        struct spz_event input = event_with_value(index + 100U);

        SPZ_EXPECT_EQ(spz_ring_push(&test_ring_storage, 2U, &input), 0);
        SPZ_EXPECT_EQ(spz_ring_pop_after(&test_ring_storage, after, &output), 1);
        after = output.sequence;
        SPZ_EXPECT_EQ(output.value, index + 100U);
    }
    SPZ_EXPECT_EQ(after, SPZ_RING_CAPACITY * 3U);
}

static void expect_loss_event(void)
{
    struct spz_event output;
    uint64_t after = 0U;
    uint64_t index;

    spz_ring_init(&test_ring_storage);
    for (index = 0U; index < SPZ_RING_CAPACITY; index++) {
        struct spz_event input = event_with_value(index);

        SPZ_EXPECT_EQ(spz_ring_push(&test_ring_storage, 1U, &input), 0);
    }
    {
        struct spz_event rejected = event_with_value(UINT64_C(0xdead));

        SPZ_EXPECT_EQ(spz_ring_push(&test_ring_storage, 1U, &rejected), -ENOSPC);
        SPZ_EXPECT_EQ(spz_ring_push(&test_ring_storage, 1U, &rejected), -ENOSPC);
    }
    SPZ_EXPECT_EQ(spz_ring_lost(&test_ring_storage, 1U), 2U);

    for (index = 0U; index < SPZ_RING_CAPACITY; index++) {
        SPZ_EXPECT_EQ(spz_ring_pop_after(&test_ring_storage, after, &output), 1);
        after = output.sequence;
        SPZ_EXPECT_EQ(output.type, SPZ_EVENT_BREAKPOINT);
        SPZ_EXPECT_EQ(output.value, index);
    }
    {
        struct spz_event accepted = event_with_value(UINT64_C(0xbeef));

        SPZ_EXPECT_EQ(spz_ring_push(&test_ring_storage, 1U, &accepted), 0);
    }
    SPZ_EXPECT_EQ(spz_ring_pop_after(&test_ring_storage, after, &output), 1);
    after = output.sequence;
    SPZ_EXPECT_EQ(output.type, SPZ_EVENT_LOSS);
    SPZ_EXPECT_EQ(output.value, 2U);
    SPZ_EXPECT_EQ(spz_ring_pop_after(&test_ring_storage, after, &output), 1);
    SPZ_EXPECT_EQ(output.type, SPZ_EVENT_BREAKPOINT);
    SPZ_EXPECT_EQ(output.value, UINT64_C(0xbeef));
}

static void expect_invalid_arguments(void)
{
    struct spz_event event = event_with_value(1U);

    spz_ring_init(&test_ring_storage);
    SPZ_EXPECT(spz_ring_push(NULL, 0U, &event) < 0);
    SPZ_EXPECT(spz_ring_push(&test_ring_storage, SPZ_MAX_CPUS, &event) < 0);
    SPZ_EXPECT(spz_ring_push(&test_ring_storage, 0U, NULL) < 0);
    SPZ_EXPECT(spz_ring_pop_after(NULL, 0U, &event) < 0);
    SPZ_EXPECT(spz_ring_pop_after(&test_ring_storage, 0U, NULL) < 0);
    SPZ_EXPECT_EQ(spz_ring_lost(NULL, 0U), 0U);
    SPZ_EXPECT_EQ(spz_ring_lost(&test_ring_storage, SPZ_MAX_CPUS), 0U);
}

static void *stress_producer(void *opaque)
{
    struct producer_args *args = (struct producer_args *)opaque;
    uint32_t index;

    for (index = 0U; index < SPZ_STRESS_EVENTS; index++) {
        struct spz_event event = event_with_value(((uint64_t)args->cpu << 32U) | index);
        int result;

        do {
            result = spz_ring_push(args->ring, args->cpu, &event);
            if (result == -ENOSPC)
                (void)sched_yield();
        } while (result == -ENOSPC);
        if (result != 0) {
            args->error = result;
            break;
        }
    }
    (void)__atomic_add_fetch(args->done, 1U, __ATOMIC_RELEASE);
    return NULL;
}

static void expect_concurrent_cpu_producers(void)
{
    pthread_t threads[SPZ_STRESS_CPUS];
    struct producer_args args[SPZ_STRESS_CPUS];
    uint32_t seen[SPZ_STRESS_CPUS] = {0U};
    uint32_t done = 0U;
    uint32_t normal_events = 0U;
    uint64_t after = 0U;
    uint32_t cpu;

    spz_ring_init(&test_ring_storage);
    for (cpu = 0U; cpu < SPZ_STRESS_CPUS; cpu++) {
        args[cpu].ring = &test_ring_storage;
        args[cpu].cpu = cpu;
        args[cpu].done = &done;
        args[cpu].error = 0;
        SPZ_EXPECT_EQ(pthread_create(&threads[cpu], NULL, stress_producer, &args[cpu]), 0);
    }

    while (normal_events < SPZ_STRESS_CPUS * SPZ_STRESS_EVENTS) {
        struct spz_event event;
        int result = spz_ring_pop_after(&test_ring_storage, after, &event);

        if (result == 0) {
            if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) == SPZ_STRESS_CPUS)
                break;
            (void)sched_yield();
            continue;
        }
        SPZ_EXPECT_EQ(result, 1);
        if (result != 1)
            break;
        SPZ_EXPECT_EQ(event.sequence, after + 1U);
        SPZ_EXPECT_EQ(event.crc32, spz_crc32_ieee(&event, offsetof(struct spz_event, crc32)));
        after = event.sequence;
        if (event.type == SPZ_EVENT_LOSS)
            continue;
        SPZ_EXPECT(event.cpu < SPZ_STRESS_CPUS);
        if (event.cpu >= SPZ_STRESS_CPUS)
            continue;
        SPZ_EXPECT_EQ(event.value >> 32U, event.cpu);
        SPZ_EXPECT_EQ((uint32_t)event.value, seen[event.cpu]);
        seen[event.cpu]++;
        normal_events++;
    }
    for (cpu = 0U; cpu < SPZ_STRESS_CPUS; cpu++) {
        SPZ_EXPECT_EQ(pthread_join(threads[cpu], NULL), 0);
        SPZ_EXPECT_EQ(args[cpu].error, 0);
        SPZ_EXPECT_EQ(seen[cpu], SPZ_STRESS_EVENTS);
    }
    SPZ_EXPECT_EQ(normal_events, SPZ_STRESS_CPUS * SPZ_STRESS_EVENTS);
}

int test_ring(void)
{
    expect_crc();
    expect_global_order();
    expect_wrap_without_overwrite();
    expect_loss_event();
    expect_invalid_arguments();
    expect_concurrent_cpu_producers();
    return 0;
}
