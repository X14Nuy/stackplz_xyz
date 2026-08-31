#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "stackplz/platform.h"
#include "../platform/kpatch/compat.h"
#include "test.h"

struct renderer_fixture {
    const struct spz_device_profile *profile;
    uint8_t mm[2048];
    uint8_t vma[2][512];
    uint32_t allocs;
    uint32_t frees;
    uint32_t mmputs;
    uint32_t locks;
    uint32_t unlocks;
    uint32_t shows;
    int lock_status;
    int mismatch_iterator;
    int overflow_seq;
};

static void write_u64(uint8_t *bytes, uint32_t offset, uint64_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static uint64_t read_u64(const uint8_t *bytes, uint32_t offset)
{
    uint64_t value;

    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static void *renderer_alloc(void *opaque, uint32_t size)
{
    struct renderer_fixture *fixture = opaque;

    SPZ_EXPECT_EQ(size, fixture->profile->maps.max_snapshot_bytes);
    fixture->allocs++;
    return malloc(size);
}

static void renderer_free(void *opaque, void *data)
{
    struct renderer_fixture *fixture = opaque;

    SPZ_EXPECT(data != NULL);
    fixture->frees++;
    free(data);
}

static void *renderer_get_task_mm(void *opaque, const void *task)
{
    struct renderer_fixture *fixture = opaque;

    SPZ_EXPECT_EQ((uintptr_t)task, UINT64_C(0x1234));
    return fixture->mm;
}

static void renderer_mmput(void *opaque, void *mm)
{
    struct renderer_fixture *fixture = opaque;

    SPZ_EXPECT(mm == fixture->mm);
    fixture->mmputs++;
}

static int renderer_lock(void *opaque, void *mm)
{
    struct renderer_fixture *fixture = opaque;

    SPZ_EXPECT(mm == fixture->mm);
    fixture->locks++;
    return fixture->lock_status;
}

static void renderer_unlock(void *opaque, void *mm)
{
    struct renderer_fixture *fixture = opaque;

    SPZ_EXPECT(mm == fixture->mm);
    fixture->unlocks++;
}

static void *renderer_find_vma(void *opaque, void *mm, uint64_t address)
{
    struct renderer_fixture *fixture = opaque;

    SPZ_EXPECT(mm == fixture->mm);
    if (address < UINT64_C(0x2000))
        return fixture->vma[0];
    if (address < UINT64_C(0x4000))
        return fixture->vma[1];
    return NULL;
}

static void *renderer_mas_walk(void *opaque, void *iterator)
{
    struct renderer_fixture *fixture = opaque;
    const struct spz_maps_profile *maps = &fixture->profile->maps;
    uint8_t *bytes = iterator;
    uint64_t tree = read_u64(bytes, maps->mas_tree);
    uint64_t index = read_u64(bytes, maps->mas_index);

    SPZ_EXPECT_EQ(tree, (uint64_t)(uintptr_t)(fixture->mm + maps->mm_mt));
    SPZ_EXPECT_EQ(read_u64(bytes, maps->mas_last), index);
    SPZ_EXPECT_EQ(read_u64(bytes, maps->mas_node), maps->mas_start_node);
    SPZ_EXPECT_EQ(*(uint32_t *)(void *)(bytes + maps->mas_status),
                  maps->ma_start_status);
    if (fixture->mismatch_iterator)
        return fixture->vma[1];
    return index == UINT64_C(0x1000) ? fixture->vma[0] : fixture->vma[1];
}

static void renderer_show(void *opaque, void *seq, void *vma)
{
    static const char first[] = "1000-2000 r-xp 0 00:00 0 /a.so\n";
    static const char second[] = "3000-4000 rw-p 0 00:00 0 [heap]\n";
    struct renderer_fixture *fixture = opaque;
    const struct spz_maps_profile *maps = &fixture->profile->maps;
    uint8_t *bytes = seq;
    uint8_t *buffer = (uint8_t *)(uintptr_t)read_u64(bytes, maps->seq_buf);
    uint64_t size = read_u64(bytes, maps->seq_size);
    uint64_t count = read_u64(bytes, maps->seq_count);
    const char *line = vma == fixture->vma[0] ? first : second;
    uint64_t length = strlen(line);

    fixture->shows++;
    if (fixture->overflow_seq) {
        write_u64(bytes, maps->seq_count, size);
        return;
    }
    SPZ_EXPECT(count + length < size);
    memcpy(buffer + count, line, (size_t)length);
    write_u64(bytes, maps->seq_count, count + length);
}

static struct spz_maps_kernel_ops renderer_ops(struct renderer_fixture *fixture)
{
    struct spz_maps_kernel_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = fixture;
    ops.allocate = renderer_alloc;
    ops.free = renderer_free;
    ops.get_task_mm = renderer_get_task_mm;
    ops.mmput = renderer_mmput;
    ops.mmap_read_lock_killable = renderer_lock;
    ops.mmap_read_unlock = renderer_unlock;
    ops.find_vma = renderer_find_vma;
    ops.mas_walk = renderer_mas_walk;
    ops.show_map_vma = renderer_show;
    return ops;
}

static void init_renderer(struct renderer_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->profile = spz_profile_select("oneplus-plk110-a16-b4999618-d05");
    write_u64(fixture->vma[0], fixture->profile->maps.vma_start,
              UINT64_C(0x1000));
    write_u64(fixture->vma[0], fixture->profile->maps.vma_end,
              UINT64_C(0x2000));
    write_u64(fixture->vma[1], fixture->profile->maps.vma_start,
              UINT64_C(0x3000));
    write_u64(fixture->vma[1], fixture->profile->maps.vma_end,
              UINT64_C(0x4000));
}

static void expect_renderer_builds_profiled_iterator_and_output(void)
{
    static const char expected[] =
        "1000-2000 r-xp 0 00:00 0 /a.so\n"
        "3000-4000 rw-p 0 00:00 0 [heap]\n";
    struct renderer_fixture fixture;
    struct spz_maps_kernel_ops ops;
    uint8_t *data = NULL;
    uint32_t length = 0U;

    init_renderer(&fixture);
    ops = renderer_ops(&fixture);
    SPZ_EXPECT_EQ(spz_maps_render_vmas(fixture.profile, &ops,
                                       (const void *)(uintptr_t)0x1234U,
                                       &data, &length), 0);
    SPZ_EXPECT_EQ(length, sizeof(expected) - 1U);
    SPZ_EXPECT(memcmp(data, expected, length) == 0);
    SPZ_EXPECT_EQ(fixture.allocs, 1U);
    SPZ_EXPECT_EQ(fixture.frees, 0U);
    SPZ_EXPECT_EQ(fixture.locks, 1U);
    SPZ_EXPECT_EQ(fixture.unlocks, 1U);
    SPZ_EXPECT_EQ(fixture.mmputs, 1U);
    SPZ_EXPECT_EQ(fixture.shows, 2U);
    ops.free(ops.context, data);
    SPZ_EXPECT_EQ(fixture.frees, 1U);
}

static void expect_renderer_cleans_up_failures(void)
{
    struct renderer_fixture fixture;
    struct spz_maps_kernel_ops ops;
    uint8_t *data = NULL;
    uint32_t length = 0U;

    init_renderer(&fixture);
    fixture.mismatch_iterator = 1;
    ops = renderer_ops(&fixture);
    SPZ_EXPECT(spz_maps_render_vmas(fixture.profile, &ops,
                                    (const void *)(uintptr_t)0x1234U,
                                    &data, &length) < 0);
    SPZ_EXPECT(data == NULL);
    SPZ_EXPECT_EQ(fixture.frees, 1U);
    SPZ_EXPECT_EQ(fixture.unlocks, 1U);
    SPZ_EXPECT_EQ(fixture.mmputs, 1U);

    init_renderer(&fixture);
    fixture.overflow_seq = 1;
    ops = renderer_ops(&fixture);
    SPZ_EXPECT_EQ(spz_maps_render_vmas(fixture.profile, &ops,
                                       (const void *)(uintptr_t)0x1234U,
                                       &data, &length), -E2BIG);
    SPZ_EXPECT_EQ(fixture.frees, 1U);
    SPZ_EXPECT_EQ(fixture.unlocks, 1U);
    SPZ_EXPECT_EQ(fixture.mmputs, 1U);
}

int test_maps_renderer(void)
{
    expect_renderer_builds_profiled_iterator_and_output();
    expect_renderer_cleans_up_failures();
    return 0;
}
