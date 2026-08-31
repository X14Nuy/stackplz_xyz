#include "stackplz/platform.h"

#include "stackplz/core.h"
#include "stackplz/maps.h"

#define SPZ_MAPS_WRITER_WAIT_ATTEMPTS 100000U

static int spz_maps_backend_supported(const struct spz_maps_backend *backend)
{
    return backend != NULL && backend->retain_task != NULL &&
           backend->release_task != NULL && backend->render != NULL &&
           backend->release_buffer != NULL &&
           backend->max_snapshot_bytes != 0U &&
           backend->max_snapshot_bytes <= SPZ_MAPS_MAX_SNAPSHOT_BYTES &&
           backend->max_chunk_bytes != 0U &&
           backend->max_chunk_bytes <= SPZ_MAPS_MAX_CHUNK_BYTES;
}

static int spz_maps_write_begin(struct spz_maps *maps)
{
    uint32_t expected = 0U;
    unsigned int attempt;

    if (!__atomic_compare_exchange_n(&maps->writer, &expected, 1U, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -EBUSY;
    for (attempt = 0U; attempt < SPZ_MAPS_WRITER_WAIT_ATTEMPTS; attempt++) {
        if (__atomic_load_n(&maps->readers, __ATOMIC_ACQUIRE) == 0U)
            return 0;
        __asm__ volatile("" ::: "memory");
    }
    __atomic_store_n(&maps->writer, 0U, __ATOMIC_RELEASE);
    return -EBUSY;
}

static void spz_maps_write_end(struct spz_maps *maps)
{
    __atomic_store_n(&maps->writer, 0U, __ATOMIC_RELEASE);
}

int spz_maps_init(struct spz_maps *maps, const struct spz_maps_backend *backend)
{
    if (maps == NULL)
        return -EINVAL;
    memset(maps, 0, sizeof(*maps));
    if (backend != NULL)
        maps->backend = *backend;
    maps->state = spz_maps_backend_supported(backend) ?
        (uint32_t)SPZ_MAPS_EMPTY : (uint32_t)SPZ_MAPS_UNSUPPORTED;
    return 0;
}

int spz_maps_supported(const struct spz_maps *maps)
{
    return maps != NULL &&
           __atomic_load_n(&maps->state, __ATOMIC_ACQUIRE) !=
               (uint32_t)SPZ_MAPS_UNSUPPORTED;
}

int spz_maps_has_task(const struct spz_maps *maps)
{
    if (!spz_maps_supported(maps))
        return 0;
    return __atomic_load_n(&maps->task, __ATOMIC_ACQUIRE) != NULL;
}

int spz_maps_capture_task(struct spz_maps *maps, const void *task,
                          uint64_t generation)
{
    int result;

    if (maps == NULL || task == NULL || generation == 0U)
        return -EINVAL;
    if (!spz_maps_supported(maps))
        return -EOPNOTSUPP;
    result = spz_maps_write_begin(maps);
    if (result != 0)
        return result;
    if (maps->task != NULL) {
        result = maps->task == task && maps->task_generation == generation ?
            0 : -EBUSY;
        spz_maps_write_end(maps);
        return result;
    }
    result = maps->backend.retain_task(maps->backend.context, task);
    if (result == 0) {
        maps->task = task;
        maps->task_generation = generation;
        __atomic_store_n(&maps->state, (uint32_t)SPZ_MAPS_TASK_CAPTURED,
                         __ATOMIC_RELEASE);
    }
    spz_maps_write_end(maps);
    return result;
}

int spz_maps_snapshot(struct spz_maps *maps, uint64_t generation,
                      uint64_t snapshot)
{
    uint8_t *new_data = NULL;
    uint8_t *old_data;
    uint32_t new_length = 0U;
    uint32_t old_length;
    int result;

    if (maps == NULL || generation == 0U || snapshot == 0U)
        return -EINVAL;
    if (!spz_maps_supported(maps))
        return -EOPNOTSUPP;
    result = spz_maps_write_begin(maps);
    if (result != 0)
        return result;
    if (maps->task == NULL || maps->task_generation != generation) {
        spz_maps_write_end(maps);
        return -ESTALE;
    }
    result = maps->backend.render(maps->backend.context, maps->task,
                                  &new_data, &new_length);
    if (result != 0)
        goto out;
    if (new_data == NULL || new_length == 0U ||
        new_length > maps->backend.max_snapshot_bytes) {
        result = new_length > maps->backend.max_snapshot_bytes ? -E2BIG :
                                                                  -EUCLEAN;
        goto out;
    }
    old_data = maps->data;
    old_length = maps->length;
    maps->data = new_data;
    maps->length = new_length;
    maps->crc32 = spz_crc32_ieee(new_data, new_length);
    maps->snapshot = snapshot;
    __atomic_store_n(&maps->state, (uint32_t)SPZ_MAPS_READY,
                     __ATOMIC_RELEASE);
    new_data = old_data;
    new_length = old_length;
    result = 0;
out:
    spz_maps_write_end(maps);
    if (new_data != NULL)
        maps->backend.release_buffer(maps->backend.context, new_data,
                                     new_length);
    return result;
}

int spz_maps_read_begin(struct spz_maps *maps, uint64_t snapshot,
                        uint32_t offset, struct spz_maps_read *read)
{
    uint32_t remaining;
    uint32_t length;

    if (maps == NULL || read == NULL || snapshot == 0U)
        return -EINVAL;
    memset(read, 0, sizeof(*read));
    if (!spz_maps_supported(maps))
        return -EOPNOTSUPP;
    if (__atomic_load_n(&maps->writer, __ATOMIC_ACQUIRE) != 0U)
        return -EBUSY;
    (void)__atomic_add_fetch(&maps->readers, 1U, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&maps->writer, __ATOMIC_ACQUIRE) != 0U) {
        (void)__atomic_sub_fetch(&maps->readers, 1U, __ATOMIC_ACQ_REL);
        return -EBUSY;
    }
    if (__atomic_load_n(&maps->state, __ATOMIC_ACQUIRE) !=
            (uint32_t)SPZ_MAPS_READY ||
        maps->snapshot != snapshot || maps->data == NULL ||
        offset >= maps->length) {
        (void)__atomic_sub_fetch(&maps->readers, 1U, __ATOMIC_ACQ_REL);
        return maps->snapshot != snapshot ? -ESTALE : -ERANGE;
    }
    remaining = maps->length - offset;
    length = remaining < maps->backend.max_chunk_bytes ?
        remaining : maps->backend.max_chunk_bytes;
    read->data = maps->data + offset;
    read->snapshot = maps->snapshot;
    read->offset = offset;
    read->total = maps->length;
    read->length = length;
    read->crc32 = maps->crc32;
    read->eof = length == remaining ? 1U : 0U;
    return 0;
}

void spz_maps_read_end(struct spz_maps *maps)
{
    if (maps != NULL)
        (void)__atomic_sub_fetch(&maps->readers, 1U, __ATOMIC_ACQ_REL);
}

int spz_maps_info(const struct spz_maps *maps, struct spz_maps_info *info)
{
    if (maps == NULL || info == NULL)
        return -EINVAL;
    memset(info, 0, sizeof(*info));
    info->state = (enum spz_maps_state)__atomic_load_n(&maps->state,
                                                       __ATOMIC_ACQUIRE);
    info->task_generation = maps->task_generation;
    info->snapshot = maps->snapshot;
    info->length = maps->length;
    info->crc32 = maps->crc32;
    return 0;
}

int spz_maps_clear(struct spz_maps *maps)
{
    const void *task;
    uint8_t *data;
    uint32_t length;
    int result;

    if (maps == NULL)
        return -EINVAL;
    if (!spz_maps_supported(maps))
        return 0;
    result = spz_maps_write_begin(maps);
    if (result != 0)
        return result;
    task = maps->task;
    data = maps->data;
    length = maps->length;
    maps->task = NULL;
    maps->data = NULL;
    maps->task_generation = 0U;
    maps->snapshot = 0U;
    maps->length = 0U;
    maps->crc32 = 0U;
    __atomic_store_n(&maps->state, (uint32_t)SPZ_MAPS_EMPTY,
                     __ATOMIC_RELEASE);
    spz_maps_write_end(maps);
    if (data != NULL)
        maps->backend.release_buffer(maps->backend.context, data, length);
    if (task != NULL)
        maps->backend.release_task(maps->backend.context, task);
    return 0;
}
