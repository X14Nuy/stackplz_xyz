#include "stackplz/platform.h"

#include "stackplz/task.h"

#define SPZ_BINDING_SNAPSHOT_ATTEMPTS 32U
#define SPZ_BINDING_WRITE_ATTEMPTS 128U

static int spz_task_read(const void *task, size_t available, uint32_t offset,
                         void *out, size_t length)
{
    if (task == NULL || out == NULL || offset > available || length > available - offset)
        return -ERANGE;
    memcpy(out, (const uint8_t *)task + offset, length);
    return 0;
}

static int spz_extract_identity(const struct spz_device_profile *profile,
                                const void *current_task, size_t available,
                                struct spz_task_identity *identity,
                                uint32_t *exit_state)
{
    uint64_t cred_pointer;
    uint32_t state;

    if (profile == NULL || current_task == NULL || identity == NULL ||
        available < profile->kernel.task_struct_size)
        return -EINVAL;
    memset(identity, 0, sizeof(*identity));
    if (spz_task_read(current_task, available, profile->task.pid,
                      &identity->pid, sizeof(identity->pid)) != 0 ||
        spz_task_read(current_task, available, profile->task.tgid,
                      &identity->tgid, sizeof(identity->tgid)) != 0 ||
        spz_task_read(current_task, available, profile->task.start_time,
                      &identity->start_time, sizeof(identity->start_time)) != 0 ||
        spz_task_read(current_task, available, profile->task.start_boottime,
                      &identity->start_boot_time, sizeof(identity->start_boot_time)) != 0 ||
        spz_task_read(current_task, available, profile->task.comm,
                      identity->comm, sizeof(identity->comm)) != 0 ||
        spz_task_read(current_task, available, profile->task.exit_state,
                      &state, sizeof(state)) != 0 ||
        spz_task_read(current_task, available, profile->task.cred,
                      &cred_pointer, sizeof(cred_pointer)) != 0)
        return -ERANGE;
    identity->comm[SPZ_COMM_LEN - 1U] = '\0';
    if (cred_pointer == 0U || cred_pointer > UINT64_MAX - profile->cred.uid ||
        profile->cred.uid > profile->kernel.cred_size ||
        sizeof(identity->uid) > profile->kernel.cred_size - profile->cred.uid)
        return -EFAULT;
    memcpy(&identity->uid, (const uint8_t *)(uintptr_t)cred_pointer + profile->cred.uid,
           sizeof(identity->uid));
    identity->task_cookie = (uint64_t)(uintptr_t)current_task;
    if (exit_state != NULL)
        *exit_state = state;
    return 0;
}

static uint64_t spz_next_generation(uint64_t generation)
{
    if (generation == UINT64_MAX)
        return 1U;
    generation++;
    return generation == 0U ? 1U : generation;
}

static int spz_write_begin(struct spz_binding *binding, uint64_t *odd_sequence)
{
    unsigned int attempt;

    for (attempt = 0U; attempt < SPZ_BINDING_WRITE_ATTEMPTS; attempt++) {
        uint64_t sequence = __atomic_load_n(&binding->sequence, __ATOMIC_ACQUIRE);
        uint64_t desired;

        if ((sequence & 1U) != 0U)
            continue;
        desired = sequence + 1U;
        if (__atomic_compare_exchange_n(&binding->sequence, &sequence, desired, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            *odd_sequence = desired;
            return 0;
        }
    }
    return -EAGAIN;
}

static void spz_write_end(struct spz_binding *binding, uint64_t odd_sequence)
{
    __atomic_store_n(&binding->sequence, odd_sequence + 1U, __ATOMIC_RELEASE);
}

static void spz_atomic_store_bytes(char *destination, const char *source, size_t length)
{
    size_t index;

    for (index = 0U; index < length; index++)
        __atomic_store_n(&destination[index], source[index], __ATOMIC_RELAXED);
}

static void spz_atomic_load_bytes(const char *source, char *destination, size_t length)
{
    size_t index;

    for (index = 0U; index < length; index++)
        destination[index] = __atomic_load_n(&source[index], __ATOMIC_RELAXED);
}

static void spz_store_request(struct spz_binding *binding,
                              const struct spz_binding_request *request)
{
    __atomic_store_n(&binding->request.binding_id, request->binding_id, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->request.pid, request->pid, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->request.mode, request->mode, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->request.uid, request->uid, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->request.start_boot_time, request->start_boot_time, __ATOMIC_RELAXED);
    spz_atomic_store_bytes(binding->request.comm, request->comm, SPZ_COMM_LEN);
    __atomic_store_n(&binding->request.has_uid, request->has_uid, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->request.has_comm, request->has_comm, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->request.has_start_boot_time,
                     request->has_start_boot_time, __ATOMIC_RELAXED);
}

static void spz_load_request(const struct spz_binding *binding,
                             struct spz_binding_request *request)
{
    request->binding_id = __atomic_load_n(&binding->request.binding_id, __ATOMIC_RELAXED);
    request->pid = __atomic_load_n(&binding->request.pid, __ATOMIC_RELAXED);
    request->mode = __atomic_load_n(&binding->request.mode, __ATOMIC_RELAXED);
    request->uid = __atomic_load_n(&binding->request.uid, __ATOMIC_RELAXED);
    request->start_boot_time =
        __atomic_load_n(&binding->request.start_boot_time, __ATOMIC_RELAXED);
    spz_atomic_load_bytes(binding->request.comm, request->comm, SPZ_COMM_LEN);
    request->has_uid = __atomic_load_n(&binding->request.has_uid, __ATOMIC_RELAXED);
    request->has_comm = __atomic_load_n(&binding->request.has_comm, __ATOMIC_RELAXED);
    request->has_start_boot_time =
        __atomic_load_n(&binding->request.has_start_boot_time, __ATOMIC_RELAXED);
}

static void spz_store_identity(struct spz_binding *binding,
                               const struct spz_task_identity *identity)
{
    __atomic_store_n(&binding->identity.generation, identity->generation, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->identity.task_cookie, identity->task_cookie, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->identity.pid, identity->pid, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->identity.tgid, identity->tgid, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->identity.uid, identity->uid, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->identity.start_time, identity->start_time, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->identity.start_boot_time,
                     identity->start_boot_time, __ATOMIC_RELAXED);
    spz_atomic_store_bytes(binding->identity.comm, identity->comm, SPZ_COMM_LEN);
}

static void spz_load_identity(const struct spz_binding *binding,
                              struct spz_task_identity *identity)
{
    identity->generation = __atomic_load_n(&binding->identity.generation, __ATOMIC_RELAXED);
    identity->task_cookie = __atomic_load_n(&binding->identity.task_cookie, __ATOMIC_RELAXED);
    identity->pid = __atomic_load_n(&binding->identity.pid, __ATOMIC_RELAXED);
    identity->tgid = __atomic_load_n(&binding->identity.tgid, __ATOMIC_RELAXED);
    identity->uid = __atomic_load_n(&binding->identity.uid, __ATOMIC_RELAXED);
    identity->start_time = __atomic_load_n(&binding->identity.start_time, __ATOMIC_RELAXED);
    identity->start_boot_time =
        __atomic_load_n(&binding->identity.start_boot_time, __ATOMIC_RELAXED);
    spz_atomic_load_bytes(binding->identity.comm, identity->comm, SPZ_COMM_LEN);
}

static int spz_safe_request_comm(const char comm[SPZ_COMM_LEN])
{
    size_t index;

    for (index = 0U; index < SPZ_COMM_LEN; index++) {
        uint8_t byte = (uint8_t)comm[index];

        if (byte == 0U)
            return index != 0U;
        if ((byte >= (uint8_t)'a' && byte <= (uint8_t)'z') ||
            (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') ||
            (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') || byte == (uint8_t)'.' ||
            byte == (uint8_t)'_' || byte == (uint8_t)':' || byte == (uint8_t)'-')
            continue;
        return 0;
    }
    return 0;
}

static int spz_request_valid(const struct spz_binding_request *request)
{
    if (request == NULL || request->binding_id == 0U || request->pid == 0U)
        return 0;
    if (request->mode != SPZ_BIND_PID && request->mode != SPZ_BIND_TGID &&
        request->mode != SPZ_BIND_EITHER)
        return 0;
    if (request->has_uid > 1U || request->has_comm > 1U ||
        request->has_start_boot_time > 1U)
        return 0;
    if (request->has_comm != 0U && !spz_safe_request_comm(request->comm))
        return 0;
    return 1;
}

void spz_binding_init(struct spz_binding *binding)
{
    if (binding != NULL)
        memset(binding, 0, sizeof(*binding));
}

int spz_binding_snapshot(const struct spz_binding *binding,
                         struct spz_binding_snapshot *snapshot)
{
    unsigned int attempt;

    if (binding == NULL || snapshot == NULL)
        return -EINVAL;
    for (attempt = 0U; attempt < SPZ_BINDING_SNAPSHOT_ATTEMPTS; attempt++) {
        uint64_t before = __atomic_load_n(&binding->sequence, __ATOMIC_ACQUIRE);
        uint64_t after;

        if ((before & 1U) != 0U)
            continue;
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->generation = __atomic_load_n(&binding->generation, __ATOMIC_RELAXED);
        snapshot->state = (enum spz_binding_state)__atomic_load_n(&binding->state, __ATOMIC_RELAXED);
        spz_load_request(binding, &snapshot->request);
        spz_load_identity(binding, &snapshot->identity);
        after = __atomic_load_n(&binding->sequence, __ATOMIC_ACQUIRE);
        if (before == after && (after & 1U) == 0U)
            return 0;
    }
    return -EAGAIN;
}

int spz_binding_set(struct spz_binding *binding, const struct spz_binding_request *request,
                    uint64_t *generation)
{
    struct spz_task_identity empty_identity;
    uint64_t odd_sequence;
    uint64_t next;
    int result;

    if (binding == NULL || !spz_request_valid(request))
        return -EINVAL;
    result = spz_write_begin(binding, &odd_sequence);
    if (result != 0)
        return result;
    next = spz_next_generation(__atomic_load_n(&binding->generation, __ATOMIC_RELAXED));
    memset(&empty_identity, 0, sizeof(empty_identity));
    spz_store_request(binding, request);
    spz_store_identity(binding, &empty_identity);
    __atomic_store_n(&binding->generation, next, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->state, (uint32_t)SPZ_BINDING_PENDING, __ATOMIC_RELAXED);
    spz_write_end(binding, odd_sequence);
    if (generation != NULL)
        *generation = next;
    return 0;
}

static int spz_selector_matches(const struct spz_binding_request *request,
                                const struct spz_task_identity *identity)
{
    int id_matches;

    if (request->mode == SPZ_BIND_PID)
        id_matches = identity->pid == request->pid;
    else if (request->mode == SPZ_BIND_TGID)
        id_matches = identity->tgid == request->pid;
    else
        id_matches = identity->pid == request->pid || identity->tgid == request->pid;
    if (!id_matches)
        return 0;
    if (request->has_uid != 0U && identity->uid != request->uid)
        return 0;
    if (request->has_comm != 0U && strncmp(identity->comm, request->comm, SPZ_COMM_LEN) != 0)
        return 0;
    if (request->has_start_boot_time != 0U &&
        identity->start_boot_time != request->start_boot_time)
        return 0;
    return 1;
}

static int spz_same_strong_identity(const struct spz_task_identity *left,
                                    const struct spz_task_identity *right)
{
    return left->pid == right->pid && left->tgid == right->tgid &&
           left->start_time == right->start_time &&
           left->start_boot_time == right->start_boot_time;
}

static int spz_mark_state_if_generation(struct spz_binding *binding, uint64_t generation,
                                        enum spz_binding_state required,
                                        enum spz_binding_state replacement)
{
    uint64_t odd_sequence;
    int result = spz_write_begin(binding, &odd_sequence);

    if (result != 0)
        return result;
    if (__atomic_load_n(&binding->generation, __ATOMIC_RELAXED) != generation ||
        __atomic_load_n(&binding->state, __ATOMIC_RELAXED) != (uint32_t)required) {
        spz_write_end(binding, odd_sequence);
        return -ESTALE;
    }
    __atomic_store_n(&binding->state, (uint32_t)replacement, __ATOMIC_RELAXED);
    spz_write_end(binding, odd_sequence);
    return 0;
}

int spz_binding_matches_current(struct spz_binding *binding,
                                const struct spz_device_profile *profile,
                                const void *current_task, size_t available,
                                uint64_t expected_generation,
                                struct spz_task_identity *identity)
{
    struct spz_binding_snapshot snapshot;
    struct spz_task_identity current;
    uint32_t exit_state;
    int result;

    if (binding == NULL || profile == NULL || current_task == NULL)
        return -EINVAL;
    result = spz_binding_snapshot(binding, &snapshot);
    if (result != 0)
        return result;
    if (snapshot.generation != expected_generation)
        return -ESTALE;
    if (snapshot.state == SPZ_BINDING_STALE)
        return -ESTALE;
    if (snapshot.state == SPZ_BINDING_EXITED)
        return -ESRCH;
    if (snapshot.state != SPZ_BINDING_BOUND)
        return 0;
    result = spz_extract_identity(profile, current_task, available, &current, &exit_state);
    if (result != 0)
        return result;
    current.generation = snapshot.generation;
    if (current.pid != snapshot.identity.pid)
        return 0;
    if (!spz_same_strong_identity(&snapshot.identity, &current)) {
        (void)spz_mark_state_if_generation(binding, snapshot.generation,
                                           SPZ_BINDING_BOUND, SPZ_BINDING_STALE);
        return -ESTALE;
    }
    if (exit_state != 0U) {
        (void)spz_mark_state_if_generation(binding, snapshot.generation,
                                           SPZ_BINDING_BOUND, SPZ_BINDING_EXITED);
        return -ESRCH;
    }
    if (identity != NULL)
        *identity = current;
    return 1;
}

int spz_binding_observe_current(struct spz_binding *binding,
                                const struct spz_device_profile *profile,
                                const void *current_task, size_t available,
                                struct spz_task_identity *identity)
{
    struct spz_binding_snapshot snapshot;
    struct spz_task_identity current;
    uint32_t exit_state;
    uint64_t odd_sequence;
    int result;

    if (binding == NULL || profile == NULL || current_task == NULL)
        return -EINVAL;
    result = spz_binding_snapshot(binding, &snapshot);
    if (result != 0)
        return result;
    if (snapshot.state == SPZ_BINDING_BOUND)
        return spz_binding_matches_current(binding, profile, current_task, available,
                                           snapshot.generation, identity);
    if (snapshot.state == SPZ_BINDING_STALE)
        return -ESTALE;
    if (snapshot.state == SPZ_BINDING_EXITED)
        return -ESRCH;
    if (snapshot.state != SPZ_BINDING_PENDING)
        return 0;
    result = spz_extract_identity(profile, current_task, available, &current, &exit_state);
    if (result != 0)
        return result;
    if (exit_state != 0U || !spz_selector_matches(&snapshot.request, &current))
        return 0;
    current.generation = snapshot.generation;

    result = spz_write_begin(binding, &odd_sequence);
    if (result != 0)
        return result;
    if (__atomic_load_n(&binding->generation, __ATOMIC_RELAXED) != snapshot.generation ||
        __atomic_load_n(&binding->state, __ATOMIC_RELAXED) != (uint32_t)SPZ_BINDING_PENDING) {
        spz_write_end(binding, odd_sequence);
        return -ESTALE;
    }
    spz_store_identity(binding, &current);
    __atomic_store_n(&binding->state, (uint32_t)SPZ_BINDING_BOUND, __ATOMIC_RELAXED);
    spz_write_end(binding, odd_sequence);
    if (identity != NULL)
        *identity = current;
    return 1;
}

int spz_binding_mark_exit(struct spz_binding *binding,
                          const struct spz_device_profile *profile,
                          const void *current_task, size_t available)
{
    struct spz_binding_snapshot snapshot;
    struct spz_task_identity current;
    int result;

    if (binding == NULL || profile == NULL || current_task == NULL)
        return -EINVAL;
    result = spz_binding_snapshot(binding, &snapshot);
    if (result != 0)
        return result;
    if (snapshot.state != SPZ_BINDING_BOUND)
        return 0;
    result = spz_extract_identity(profile, current_task, available, &current, NULL);
    if (result != 0)
        return result;
    if (!spz_same_strong_identity(&snapshot.identity, &current))
        return 0;
    result = spz_mark_state_if_generation(binding, snapshot.generation,
                                          SPZ_BINDING_BOUND, SPZ_BINDING_EXITED);
    return result == 0 ? 1 : result;
}

int spz_binding_clear(struct spz_binding *binding)
{
    struct spz_binding_request empty_request;
    struct spz_task_identity empty_identity;
    uint64_t odd_sequence;
    uint64_t next;
    int result;

    if (binding == NULL)
        return -EINVAL;
    result = spz_write_begin(binding, &odd_sequence);
    if (result != 0)
        return result;
    next = spz_next_generation(__atomic_load_n(&binding->generation, __ATOMIC_RELAXED));
    memset(&empty_request, 0, sizeof(empty_request));
    memset(&empty_identity, 0, sizeof(empty_identity));
    spz_store_request(binding, &empty_request);
    spz_store_identity(binding, &empty_identity);
    __atomic_store_n(&binding->generation, next, __ATOMIC_RELAXED);
    __atomic_store_n(&binding->state, (uint32_t)SPZ_BINDING_EMPTY, __ATOMIC_RELAXED);
    spz_write_end(binding, odd_sequence);
    return 0;
}
