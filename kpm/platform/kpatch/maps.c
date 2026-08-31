#include "stackplz/platform.h"

#include "compat.h"

#define SPZ_MAPS_SEQ_STORAGE_MAX 256U
#define SPZ_MAPS_PRIVATE_STORAGE_MAX 512U

static int spz_maps_write_u64(uint8_t *storage, uint32_t size,
                              uint32_t offset, uint64_t value)
{
    if (storage == NULL || offset > size || sizeof(value) > size - offset)
        return -ERANGE;
    memcpy(storage + offset, &value, sizeof(value));
    return 0;
}

static int spz_maps_write_u32(uint8_t *storage, uint32_t size,
                              uint32_t offset, uint32_t value)
{
    if (storage == NULL || offset > size || sizeof(value) > size - offset)
        return -ERANGE;
    memcpy(storage + offset, &value, sizeof(value));
    return 0;
}

static int spz_maps_read_u64(const void *object, uint32_t object_size,
                             uint32_t offset, uint64_t *value)
{
    if (object == NULL || value == NULL || offset > object_size ||
        sizeof(*value) > object_size - offset)
        return -ERANGE;
    memcpy(value, (const uint8_t *)object + offset, sizeof(*value));
    return 0;
}

static int spz_maps_render_arguments_valid(
    const struct spz_device_profile *profile,
    const struct spz_maps_kernel_ops *ops, const void *task, uint8_t **data,
    uint32_t *length)
{
    const struct spz_maps_profile *maps;

    if (profile == NULL || ops == NULL || task == NULL || data == NULL ||
        length == NULL || ops->allocate == NULL || ops->free == NULL ||
        ops->get_task_mm == NULL || ops->mmput == NULL ||
        ops->mmap_read_lock_killable == NULL ||
        ops->mmap_read_unlock == NULL || ops->find_vma == NULL ||
        ops->mas_walk == NULL || ops->show_map_vma == NULL)
        return 0;
    maps = &profile->maps;
    if (maps->show_map_vma_args != 2U || maps->max_snapshot_bytes == 0U ||
        maps->max_snapshot_bytes > SPZ_MAPS_MAX_SNAPSHOT_BYTES ||
        maps->seq_file_size == 0U ||
        maps->seq_file_size > SPZ_MAPS_SEQ_STORAGE_MAX ||
        maps->proc_maps_private_size == 0U ||
        maps->proc_maps_private_size > SPZ_MAPS_PRIVATE_STORAGE_MAX ||
        maps->vma_iterator_size == 0U ||
        maps->proc_iter > maps->proc_maps_private_size ||
        maps->vma_iterator_size > maps->proc_maps_private_size - maps->proc_iter)
        return 0;
    return 1;
}

int spz_maps_render_vmas(const struct spz_device_profile *profile,
                         const struct spz_maps_kernel_ops *ops,
                         const void *task, uint8_t **data, uint32_t *length)
{
    const struct spz_maps_profile *maps;
    uint8_t seq_storage[SPZ_MAPS_SEQ_STORAGE_MAX];
    uint8_t private_storage[SPZ_MAPS_PRIVATE_STORAGE_MAX];
    uint8_t *iterator;
    uint8_t *output = NULL;
    void *mm = NULL;
    uint64_t cursor = 0U;
    uint64_t count = 0U;
    int locked = 0;
    int result = -EINVAL;

    if (!spz_maps_render_arguments_valid(profile, ops, task, data, length))
        return -EINVAL;
    *data = NULL;
    *length = 0U;
    maps = &profile->maps;
    output = ops->allocate(ops->context, maps->max_snapshot_bytes);
    if (output == NULL)
        return -ENOMEM;
    memset(output, 0, maps->max_snapshot_bytes);
    mm = ops->get_task_mm(ops->context, task);
    if (mm == NULL) {
        result = -ESRCH;
        goto out;
    }
    result = ops->mmap_read_lock_killable(ops->context, mm);
    if (result != 0) {
        if (result > 0)
            result = -EINTR;
        goto out;
    }
    locked = 1;
    memset(seq_storage, 0, sizeof(seq_storage));
    memset(private_storage, 0, sizeof(private_storage));
    iterator = private_storage + maps->proc_iter;
    if (spz_maps_write_u64(seq_storage, maps->seq_file_size, maps->seq_buf,
                           (uint64_t)(uintptr_t)output) != 0 ||
        spz_maps_write_u64(seq_storage, maps->seq_file_size, maps->seq_size,
                           maps->max_snapshot_bytes) != 0 ||
        spz_maps_write_u64(seq_storage, maps->seq_file_size, maps->seq_private,
                           (uint64_t)(uintptr_t)private_storage) != 0 ||
        spz_maps_write_u64(private_storage, maps->proc_maps_private_size,
                           maps->proc_task,
                           (uint64_t)(uintptr_t)task) != 0 ||
        spz_maps_write_u64(private_storage, maps->proc_maps_private_size,
                           maps->proc_mm, (uint64_t)(uintptr_t)mm) != 0) {
        result = -ERANGE;
        goto out;
    }
    for (;;) {
        void *vma = ops->find_vma(ops->context, mm, cursor);
        void *positioned;
        uint64_t start;
        uint64_t end;
        uint64_t tree;

        if (vma == NULL)
            break;
        if (spz_maps_read_u64(vma, maps->vma_struct_size, maps->vma_start,
                              &start) != 0 ||
            spz_maps_read_u64(vma, maps->vma_struct_size, maps->vma_end,
                              &end) != 0 ||
            start < cursor || end <= start || end <= cursor) {
            result = -EUCLEAN;
            goto out;
        }
        memset(iterator, 0, maps->vma_iterator_size);
        tree = (uint64_t)(uintptr_t)((uint8_t *)mm + maps->mm_mt);
        if (spz_maps_write_u64(iterator, maps->vma_iterator_size,
                               maps->mas_tree, tree) != 0 ||
            spz_maps_write_u64(iterator, maps->vma_iterator_size,
                               maps->mas_index, start) != 0 ||
            spz_maps_write_u64(iterator, maps->vma_iterator_size,
                               maps->mas_last, start) != 0 ||
            spz_maps_write_u64(iterator, maps->vma_iterator_size,
                               maps->mas_node, maps->mas_start_node) != 0 ||
            spz_maps_write_u32(iterator, maps->vma_iterator_size,
                               maps->mas_status,
                               maps->ma_start_status) != 0) {
            result = -ERANGE;
            goto out;
        }
        positioned = ops->mas_walk(ops->context, iterator);
        if (positioned != vma) {
            result = -EUCLEAN;
            goto out;
        }
        ops->show_map_vma(ops->context, seq_storage, vma);
        if (spz_maps_read_u64(seq_storage, maps->seq_file_size,
                              maps->seq_count, &count) != 0) {
            result = -ERANGE;
            goto out;
        }
        if (count >= maps->max_snapshot_bytes) {
            result = -E2BIG;
            goto out;
        }
        cursor = end;
    }
    if (count == 0U || count > UINT32_MAX) {
        result = -EUCLEAN;
        goto out;
    }
    *data = output;
    *length = (uint32_t)count;
    output = NULL;
    result = 0;
out:
    if (locked)
        ops->mmap_read_unlock(ops->context, mm);
    if (mm != NULL)
        ops->mmput(ops->context, mm);
    if (output != NULL)
        ops->free(ops->context, output);
    return result;
}

#if defined(SPZ_KPATCH_BUILD)

#include <kallsyms.h>

typedef void *(*spz_vmalloc_fn)(unsigned long size);
typedef void (*spz_vfree_fn)(const void *address);
typedef void *(*spz_get_task_mm_fn)(const void *task);
typedef void (*spz_mmput_fn)(void *mm);
typedef int (*spz_mmap_lock_fn)(void *mm);
typedef void (*spz_mmap_unlock_fn)(void *mm);
typedef void *(*spz_find_vma_fn)(void *mm, unsigned long address);
typedef void *(*spz_mas_walk_fn)(void *iterator);
typedef void (*spz_show_map_vma_fn)(void *seq_file, void *vma);
typedef void (*spz_task_ref_fn)(const void *task);

#define SPZ_MAPS_CONVERT(name, type, field)                                      \
    static type name(struct spz_kpatch_runtime_context *context)                 \
    {                                                                            \
        union { uint64_t address; type function; } conversion;                   \
        conversion.address = context->field;                                     \
        return conversion.function;                                              \
    }

SPZ_MAPS_CONVERT(spz_vmalloc, spz_vmalloc_fn, vmalloc_address)
SPZ_MAPS_CONVERT(spz_vfree, spz_vfree_fn, vfree_address)
SPZ_MAPS_CONVERT(spz_get_task_mm, spz_get_task_mm_fn, get_task_mm_address)
SPZ_MAPS_CONVERT(spz_mmput, spz_mmput_fn, mmput_address)
SPZ_MAPS_CONVERT(spz_mmap_lock, spz_mmap_lock_fn,
                 mmap_read_lock_killable_address)
SPZ_MAPS_CONVERT(spz_mmap_unlock, spz_mmap_unlock_fn,
                 mmap_read_unlock_address)
SPZ_MAPS_CONVERT(spz_find_vma, spz_find_vma_fn, find_vma_address)
SPZ_MAPS_CONVERT(spz_mas_walk, spz_mas_walk_fn, mas_walk_address)
SPZ_MAPS_CONVERT(spz_show_map_vma, spz_show_map_vma_fn, show_map_vma_address)
SPZ_MAPS_CONVERT(spz_get_task_struct, spz_task_ref_fn, get_task_struct_address)
SPZ_MAPS_CONVERT(spz_put_task_struct, spz_task_ref_fn, put_task_struct_address)

static void *spz_kpatch_maps_allocate(void *opaque, uint32_t size)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_vmalloc_fn function = spz_vmalloc(context);

    return function == NULL ? NULL : function((unsigned long)size);
}

static void spz_kpatch_maps_free(void *opaque, void *data)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_vfree_fn function = spz_vfree(context);

    if (function != NULL && data != NULL)
        function(data);
}

static void spz_kpatch_maps_release_buffer(void *opaque, uint8_t *data,
                                           uint32_t length)
{
    (void)length;
    spz_kpatch_maps_free(opaque, data);
}

static void *spz_kpatch_maps_get_mm(void *opaque, const void *task)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_get_task_mm_fn function = spz_get_task_mm(context);

    return function == NULL ? NULL : function(task);
}

static void spz_kpatch_maps_mmput(void *opaque, void *mm)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_mmput_fn function = spz_mmput(context);

    if (function != NULL && mm != NULL)
        function(mm);
}

static int spz_kpatch_maps_lock(void *opaque, void *mm)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_mmap_lock_fn function = spz_mmap_lock(context);

    return function == NULL ? -ENOENT : function(mm);
}

static void spz_kpatch_maps_unlock(void *opaque, void *mm)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_mmap_unlock_fn function = spz_mmap_unlock(context);

    if (function != NULL)
        function(mm);
}

static void *spz_kpatch_maps_find_vma(void *opaque, void *mm,
                                     uint64_t address)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_find_vma_fn function = spz_find_vma(context);

    return function == NULL ? NULL : function(mm, (unsigned long)address);
}

static void *spz_kpatch_maps_mas_walk(void *opaque, void *iterator)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_mas_walk_fn function = spz_mas_walk(context);

    return function == NULL ? NULL : function(iterator);
}

static void spz_kpatch_maps_show(void *opaque, void *seq_file, void *vma)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_show_map_vma_fn function = spz_show_map_vma(context);

    if (function != NULL)
        function(seq_file, vma);
}

static int spz_kpatch_maps_retain_task(void *opaque, const void *task)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_task_ref_fn function = spz_get_task_struct(context);

    if (function == NULL)
        return -ENOENT;
    function(task);
    return 0;
}

static void spz_kpatch_maps_release_task(void *opaque, const void *task)
{
    struct spz_kpatch_runtime_context *context = opaque;
    spz_task_ref_fn function = spz_put_task_struct(context);

    if (function != NULL)
        function(task);
}

static int spz_kpatch_maps_render(void *opaque, const void *task,
                                 uint8_t **data, uint32_t *length)
{
    struct spz_kpatch_runtime_context *context = opaque;
    struct spz_maps_kernel_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = context;
    ops.allocate = spz_kpatch_maps_allocate;
    ops.free = spz_kpatch_maps_free;
    ops.get_task_mm = spz_kpatch_maps_get_mm;
    ops.mmput = spz_kpatch_maps_mmput;
    ops.mmap_read_lock_killable = spz_kpatch_maps_lock;
    ops.mmap_read_unlock = spz_kpatch_maps_unlock;
    ops.find_vma = spz_kpatch_maps_find_vma;
    ops.mas_walk = spz_kpatch_maps_mas_walk;
    ops.show_map_vma = spz_kpatch_maps_show;
    return spz_maps_render_vmas(context->module->profile, &ops, task, data,
                                length);
}

static uint64_t spz_kpatch_maps_lookup(const char *name)
{
    if (name == NULL || kallsyms_lookup_name == NULL)
        return 0U;
    return (uint64_t)kallsyms_lookup_name(name);
}

int spz_kpatch_maps_backend_init(struct spz_kpatch_runtime_context *context,
                                struct spz_maps_backend *backend)
{
    const struct spz_symbols_profile *symbols;

    if (context == NULL || context->module == NULL ||
        context->module->profile == NULL || backend == NULL)
        return -EINVAL;
    memset(backend, 0, sizeof(*backend));
    symbols = &context->module->profile->symbols;
#define SPZ_MAPS_RESOLVE(field, name)                                             \
    context->field = spz_kpatch_maps_lookup(symbols->name)
    SPZ_MAPS_RESOLVE(show_map_vma_address, show_map_vma);
    SPZ_MAPS_RESOLVE(find_vma_address, find_vma);
    SPZ_MAPS_RESOLVE(mas_walk_address, mas_walk);
    SPZ_MAPS_RESOLVE(get_task_mm_address, get_task_mm);
    SPZ_MAPS_RESOLVE(mmput_address, mmput);
    SPZ_MAPS_RESOLVE(mmap_read_lock_killable_address,
                     mmap_read_lock_killable);
    SPZ_MAPS_RESOLVE(mmap_read_unlock_address, mmap_read_unlock);
    SPZ_MAPS_RESOLVE(get_task_struct_address, rust_helper_get_task_struct);
    SPZ_MAPS_RESOLVE(put_task_struct_address, rust_helper_put_task_struct);
    SPZ_MAPS_RESOLVE(vmalloc_address, vmalloc_noprof);
    SPZ_MAPS_RESOLVE(vfree_address, vfree);
#undef SPZ_MAPS_RESOLVE
    if (context->show_map_vma_address == 0U ||
        context->find_vma_address == 0U || context->mas_walk_address == 0U ||
        context->get_task_mm_address == 0U || context->mmput_address == 0U ||
        context->mmap_read_lock_killable_address == 0U ||
        context->mmap_read_unlock_address == 0U ||
        context->get_task_struct_address == 0U ||
        context->put_task_struct_address == 0U ||
        context->vmalloc_address == 0U || context->vfree_address == 0U)
        return 0;
    backend->context = context;
    backend->max_snapshot_bytes =
        context->module->profile->maps.max_snapshot_bytes;
    backend->max_chunk_bytes = context->module->profile->maps.max_chunk_bytes;
    backend->retain_task = spz_kpatch_maps_retain_task;
    backend->release_task = spz_kpatch_maps_release_task;
    backend->render = spz_kpatch_maps_render;
    backend->release_buffer = spz_kpatch_maps_release_buffer;
    return 0;
}

#else

int spz_kpatch_maps_backend_init(struct spz_kpatch_runtime_context *context,
                                struct spz_maps_backend *backend)
{
    (void)context;
    if (backend == NULL)
        return -EINVAL;
    memset(backend, 0, sizeof(*backend));
    return 0;
}

#endif
