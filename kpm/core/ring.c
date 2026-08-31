#include "stackplz/platform.h"

#include "stackplz/core.h"

static int spz_next_sequence(struct spz_ring *ring, uint64_t *out)
{
    uint64_t previous = __atomic_load_n(&ring->next_sequence, __ATOMIC_RELAXED);

    for (;;) {
        if (previous == UINT64_MAX)
            return -EOVERFLOW;
        if (__atomic_compare_exchange_n(&ring->next_sequence, &previous, previous + 1U, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            *out = previous + 1U;
            return 0;
        }
    }
}

static uint64_t spz_cpu_used(const struct spz_cpu_ring *cpu_ring)
{
    uint64_t head = __atomic_load_n(&cpu_ring->head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&cpu_ring->tail, __ATOMIC_ACQUIRE);

    return head - tail;
}

static int spz_publish(struct spz_ring *ring, uint32_t cpu, const struct spz_event *source)
{
    struct spz_cpu_ring *cpu_ring = &ring->cpu[cpu];
    struct spz_ring_slot *slot;
    struct spz_event event;
    uint64_t head;
    uint64_t sequence;
    int result;

    if (spz_cpu_used(cpu_ring) >= SPZ_RING_CAPACITY)
        return -ENOSPC;
    head = __atomic_load_n(&cpu_ring->head, __ATOMIC_RELAXED);
    slot = &cpu_ring->slots[head % SPZ_RING_CAPACITY];
    if (__atomic_load_n(&slot->commit, __ATOMIC_ACQUIRE) != 0U)
        return -EBUSY;
    result = spz_next_sequence(ring, &sequence);
    if (result != 0)
        return result;

    memcpy(&event, source, sizeof(event));
    event.magic = SPZ_EVENT_MAGIC;
    event.version = SPZ_ABI_VERSION;
    event.size = SPZ_EVENT_WIRE_SIZE;
    event.cpu = cpu;
    event.sequence = sequence;
    event.reserved0 = 0U;
    event.reserved1 = 0U;
    event.crc32 = 0U;
    event.crc32 = spz_crc32_ieee(&event, offsetof(struct spz_event, crc32));
    memcpy(&slot->event, &event, sizeof(event));
    __atomic_store_n(&slot->commit, sequence, __ATOMIC_RELEASE);
    __atomic_store_n(&cpu_ring->head, head + 1U, __ATOMIC_RELEASE);
    return 0;
}

void spz_ring_init(struct spz_ring *ring)
{
    if (ring != NULL)
        memset(ring, 0, sizeof(*ring));
}

int spz_ring_push(struct spz_ring *ring, uint32_t cpu, const struct spz_event *event)
{
    struct spz_cpu_ring *cpu_ring;
    uint64_t pending;
    uint64_t free_slots;
    int result;

    if (ring == NULL || event == NULL || cpu >= SPZ_MAX_CPUS)
        return -EINVAL;
    cpu_ring = &ring->cpu[cpu];
    free_slots = SPZ_RING_CAPACITY - spz_cpu_used(cpu_ring);
    if (free_slots == 0U) {
        (void)__atomic_add_fetch(&cpu_ring->lost, 1U, __ATOMIC_RELAXED);
        (void)__atomic_add_fetch(&cpu_ring->loss_pending, 1U, __ATOMIC_RELAXED);
        return -ENOSPC;
    }

    pending = __atomic_load_n(&cpu_ring->loss_pending, __ATOMIC_RELAXED);
    if (pending != 0U && free_slots >= 2U) {
        struct spz_event loss_event;

        memset(&loss_event, 0, sizeof(loss_event));
        loss_event.type = SPZ_EVENT_LOSS;
        loss_event.value = pending;
        loss_event.observed_address = __atomic_load_n(&cpu_ring->lost, __ATOMIC_RELAXED);
        result = spz_publish(ring, cpu, &loss_event);
        if (result != 0)
            return result;
        (void)__atomic_fetch_sub(&cpu_ring->loss_pending, pending, __ATOMIC_RELAXED);
    }

    result = spz_publish(ring, cpu, event);
    if (result == -ENOSPC) {
        (void)__atomic_add_fetch(&cpu_ring->lost, 1U, __ATOMIC_RELAXED);
        (void)__atomic_add_fetch(&cpu_ring->loss_pending, 1U, __ATOMIC_RELAXED);
    }
    return result;
}

static void spz_discard_through(struct spz_cpu_ring *cpu_ring, uint64_t after)
{
    uint64_t tail = __atomic_load_n(&cpu_ring->tail, __ATOMIC_RELAXED);
    uint64_t head = __atomic_load_n(&cpu_ring->head, __ATOMIC_ACQUIRE);

    while (tail < head) {
        struct spz_ring_slot *slot = &cpu_ring->slots[tail % SPZ_RING_CAPACITY];
        uint64_t commit = __atomic_load_n(&slot->commit, __ATOMIC_ACQUIRE);

        if (commit == 0U || commit > after)
            break;
        __atomic_store_n(&slot->commit, 0U, __ATOMIC_RELEASE);
        tail++;
        __atomic_store_n(&cpu_ring->tail, tail, __ATOMIC_RELEASE);
    }
}

int spz_ring_pop_after(struct spz_ring *ring, uint64_t after, struct spz_event *out)
{
    struct spz_ring_slot *selected = NULL;
    struct spz_cpu_ring *selected_cpu = NULL;
    uint64_t selected_tail = 0U;
    uint64_t consumer_after;
    uint64_t latest;
    uint64_t expected;
    uint32_t cpu;

    if (ring == NULL || out == NULL)
        return -EINVAL;
    consumer_after = __atomic_load_n(&ring->consumer_after, __ATOMIC_ACQUIRE);
    if (after < consumer_after)
        return -ESTALE;
    latest = __atomic_load_n(&ring->next_sequence, __ATOMIC_ACQUIRE);
    if (after > latest)
        return -ERANGE;
    if (after == UINT64_MAX)
        return -EOVERFLOW;

    for (cpu = 0U; cpu < SPZ_MAX_CPUS; cpu++)
        spz_discard_through(&ring->cpu[cpu], after);
    if (after > consumer_after)
        __atomic_store_n(&ring->consumer_after, after, __ATOMIC_RELEASE);
    expected = after + 1U;

    for (cpu = 0U; cpu < SPZ_MAX_CPUS; cpu++) {
        struct spz_cpu_ring *cpu_ring = &ring->cpu[cpu];
        uint64_t tail = __atomic_load_n(&cpu_ring->tail, __ATOMIC_RELAXED);
        uint64_t head = __atomic_load_n(&cpu_ring->head, __ATOMIC_ACQUIRE);
        struct spz_ring_slot *slot;
        uint64_t commit;

        if (tail >= head)
            continue;
        slot = &cpu_ring->slots[tail % SPZ_RING_CAPACITY];
        commit = __atomic_load_n(&slot->commit, __ATOMIC_ACQUIRE);
        if (commit != expected)
            continue;
        selected = slot;
        selected_cpu = cpu_ring;
        selected_tail = tail;
        break;
    }
    if (selected == NULL)
        return 0;

    memcpy(out, &selected->event, sizeof(*out));
    if (__atomic_load_n(&selected->commit, __ATOMIC_ACQUIRE) != expected)
        return -EAGAIN;
    __atomic_store_n(&selected->commit, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&selected_cpu->tail, selected_tail + 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->consumer_after, expected, __ATOMIC_RELEASE);
    return 1;
}

uint64_t spz_ring_lost(const struct spz_ring *ring, uint32_t cpu)
{
    if (ring == NULL || cpu >= SPZ_MAX_CPUS)
        return 0U;
    return __atomic_load_n(&ring->cpu[cpu].lost, __ATOMIC_ACQUIRE);
}
