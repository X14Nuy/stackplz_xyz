#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "stackplz/platform.h"
#include "stackplz/maps.h"
#include "test.h"

struct fake_maps_backend {
    unsigned int retains;
    unsigned int task_releases;
    unsigned int renders;
    unsigned int buffer_releases;
    int render_status;
    const uint8_t *render_data;
    uint32_t render_length;
};

static int fake_retain_task(void *opaque, const void *task)
{
    struct fake_maps_backend *backend = opaque;

    SPZ_EXPECT(task != NULL);
    backend->retains++;
    return 0;
}

static void fake_release_task(void *opaque, const void *task)
{
    struct fake_maps_backend *backend = opaque;

    SPZ_EXPECT(task != NULL);
    backend->task_releases++;
}

static int fake_render(void *opaque, const void *task, uint8_t **data,
                       uint32_t *length)
{
    struct fake_maps_backend *backend = opaque;

    SPZ_EXPECT(task != NULL);
    backend->renders++;
    if (backend->render_status != 0)
        return backend->render_status;
    *data = malloc(backend->render_length);
    SPZ_EXPECT(*data != NULL);
    if (*data == NULL)
        return -ENOMEM;
    memcpy(*data, backend->render_data, backend->render_length);
    *length = backend->render_length;
    return 0;
}

static void fake_release_buffer(void *opaque, uint8_t *data, uint32_t length)
{
    struct fake_maps_backend *backend = opaque;

    SPZ_EXPECT(data != NULL);
    SPZ_EXPECT(length != 0U);
    backend->buffer_releases++;
    free(data);
}

static struct spz_maps_backend backend_ops(struct fake_maps_backend *fake)
{
    struct spz_maps_backend backend;

    memset(&backend, 0, sizeof(backend));
    backend.context = fake;
    backend.max_snapshot_bytes = 4096U;
    backend.max_chunk_bytes = 16U;
    backend.retain_task = fake_retain_task;
    backend.release_task = fake_release_task;
    backend.render = fake_render;
    backend.release_buffer = fake_release_buffer;
    return backend;
}

static void expect_task_reference_lifetime(void)
{
    static const uint8_t maps[] = "1000-2000 r-xp 0 00:00 0 /a.so\n";
    struct fake_maps_backend fake = {0};
    struct spz_maps_backend backend = backend_ops(&fake);
    struct spz_maps state;
    uint64_t task_cookie = UINT64_C(0x12340000);

    fake.render_data = maps;
    fake.render_length = sizeof(maps) - 1U;
    SPZ_EXPECT_EQ(spz_maps_init(&state, &backend), 0);
    SPZ_EXPECT(spz_maps_supported(&state));
    SPZ_EXPECT_EQ(spz_maps_capture_task(&state, (const void *)(uintptr_t)task_cookie, 9U), 0);
    SPZ_EXPECT_EQ(spz_maps_capture_task(&state, (const void *)(uintptr_t)task_cookie, 9U), 0);
    SPZ_EXPECT_EQ(fake.retains, 1U);
    SPZ_EXPECT(spz_maps_has_task(&state));
    SPZ_EXPECT(spz_maps_capture_task(&state, (const void *)(uintptr_t)(task_cookie + 8U), 10U) < 0);
    SPZ_EXPECT_EQ(fake.retains, 1U);
    SPZ_EXPECT_EQ(spz_maps_clear(&state), 0);
    SPZ_EXPECT_EQ(fake.task_releases, 1U);
    SPZ_EXPECT(!spz_maps_has_task(&state));
    SPZ_EXPECT_EQ(fake.buffer_releases, 0U);
}

static void expect_snapshot_chunks_and_failed_refresh(void)
{
    static const uint8_t first[] =
        "1000-2000 r-xp 0 00:00 0 /a.so\n"
        "3000-4000 rw-p 0 00:00 0 [heap]\n";
    struct fake_maps_backend fake = {0};
    struct spz_maps_backend backend = backend_ops(&fake);
    struct spz_maps state;
    struct spz_maps_read read;
    uint8_t assembled[sizeof(first)] = {0};
    uint32_t copied = 0U;

    fake.render_data = first;
    fake.render_length = sizeof(first) - 1U;
    SPZ_EXPECT_EQ(spz_maps_init(&state, &backend), 0);
    SPZ_EXPECT_EQ(spz_maps_capture_task(&state, (const void *)(uintptr_t)0x1111U, 4U), 0);
    SPZ_EXPECT_EQ(spz_maps_snapshot(&state, 4U, 7U), 0);
    do {
        SPZ_EXPECT_EQ(spz_maps_read_begin(&state, 7U, copied, &read), 0);
        SPZ_EXPECT_EQ(read.snapshot, 7U);
        SPZ_EXPECT_EQ(read.offset, copied);
        SPZ_EXPECT_EQ(read.total, sizeof(first) - 1U);
        SPZ_EXPECT(read.length <= 16U);
        memcpy(assembled + copied, read.data, read.length);
        copied += read.length;
        if (read.eof)
            SPZ_EXPECT_EQ(copied, sizeof(first) - 1U);
        spz_maps_read_end(&state);
    } while (copied != sizeof(first) - 1U);
    SPZ_EXPECT(memcmp(assembled, first, sizeof(first) - 1U) == 0);

    SPZ_EXPECT(spz_maps_read_begin(&state, 6U, 0U, &read) < 0);
    SPZ_EXPECT(spz_maps_read_begin(&state, 7U, sizeof(first), &read) < 0);

    fake.render_status = -EIO;
    SPZ_EXPECT_EQ(spz_maps_snapshot(&state, 4U, 8U), -EIO);
    SPZ_EXPECT_EQ(spz_maps_read_begin(&state, 7U, 0U, &read), 0);
    SPZ_EXPECT_EQ(read.snapshot, 7U);
    SPZ_EXPECT_EQ(read.total, sizeof(first) - 1U);
    spz_maps_read_end(&state);

    SPZ_EXPECT_EQ(spz_maps_clear(&state), 0);
    SPZ_EXPECT_EQ(fake.task_releases, 1U);
    SPZ_EXPECT_EQ(fake.buffer_releases, 1U);
}

static void expect_unsupported_backend_is_inert(void)
{
    struct spz_maps state;
    struct spz_maps_backend backend = {0};

    SPZ_EXPECT_EQ(spz_maps_init(&state, &backend), 0);
    SPZ_EXPECT(!spz_maps_supported(&state));
    SPZ_EXPECT_EQ(spz_maps_capture_task(&state, (const void *)(uintptr_t)1U, 1U), -EOPNOTSUPP);
    SPZ_EXPECT_EQ(spz_maps_snapshot(&state, 1U, 1U), -EOPNOTSUPP);
    SPZ_EXPECT_EQ(spz_maps_clear(&state), 0);
}

int test_maps(void)
{
    expect_task_reference_lifetime();
    expect_snapshot_chunks_and_failed_refresh();
    expect_unsupported_backend_is_inert();
    return 0;
}
