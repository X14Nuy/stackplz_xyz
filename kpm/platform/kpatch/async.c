#include "stackplz/platform.h"

#include "compat.h"

void spz_async_init(struct spz_async_request *request,
                    const struct spz_async_backend *backend)
{
    if (request == NULL)
        return;
    memset(request, 0, sizeof(*request));
    if (backend != NULL)
        request->backend = *backend;
}

static uint64_t spz_async_next_id(struct spz_async_request *request)
{
    uint64_t next = request->next_request_id + 1U;

    if (next == 0U)
        next = 1U;
    request->next_request_id = next;
    return next;
}

int spz_async_submit(struct spz_async_request *request,
                     enum spz_async_operation operation, uint32_t target_id,
                     uint64_t *request_id)
{
    uint32_t observed;
    int result;

    if (request == NULL || request->backend.queue == NULL ||
        request->backend.execute == NULL || operation <= SPZ_ASYNC_NONE ||
        operation > SPZ_ASYNC_MAPS)
        return -EINVAL;
    observed = __atomic_load_n(&request->state, __ATOMIC_ACQUIRE);
    for (;;) {
        if (observed == (uint32_t)SPZ_ASYNC_PENDING ||
            observed == (uint32_t)SPZ_ASYNC_RUNNING)
            return -EBUSY;
        if (observed != (uint32_t)SPZ_ASYNC_FREE &&
            observed != (uint32_t)SPZ_ASYNC_DONE)
            return -EUCLEAN;
        if (__atomic_compare_exchange_n(&request->state, &observed,
                                        (uint32_t)SPZ_ASYNC_RUNNING, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }

    request->operation = (uint32_t)operation;
    request->target_id = target_id;
    request->status = -EINPROGRESS;
    request->request_id = spz_async_next_id(request);
    __atomic_store_n(&request->state, (uint32_t)SPZ_ASYNC_PENDING,
                     __ATOMIC_RELEASE);
    result = request->backend.queue(request->backend.queue_context);
    if (result != 0) {
        request->status = result < 0 ? result : -EIO;
        __atomic_store_n(&request->state, (uint32_t)SPZ_ASYNC_DONE,
                         __ATOMIC_RELEASE);
        return request->status;
    }
    if (request_id != NULL)
        *request_id = request->request_id;
    return 0;
}

int spz_async_run(struct spz_async_request *request)
{
    uint32_t expected = (uint32_t)SPZ_ASYNC_PENDING;
    int status;

    if (request == NULL || request->backend.execute == NULL)
        return -EINVAL;
    if (!__atomic_compare_exchange_n(&request->state, &expected,
                                     (uint32_t)SPZ_ASYNC_RUNNING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return expected == (uint32_t)SPZ_ASYNC_DONE ? request->status : -EBUSY;
    status = request->backend.execute(
        request->backend.execute_context,
        (enum spz_async_operation)request->operation, request->target_id);
    request->status = status;
    __atomic_store_n(&request->state, (uint32_t)SPZ_ASYNC_DONE,
                     __ATOMIC_RELEASE);
    return status;
}

int spz_async_snapshot(const struct spz_async_request *request,
                       struct spz_async_snapshot *snapshot)
{
    uint32_t before;
    uint32_t after;
    unsigned int attempt;

    if (request == NULL || snapshot == NULL)
        return -EINVAL;
    for (attempt = 0U; attempt < 16U; attempt++) {
        before = __atomic_load_n(&request->state, __ATOMIC_ACQUIRE);
        snapshot->state = (enum spz_async_state)before;
        snapshot->operation = (enum spz_async_operation)
            __atomic_load_n(&request->operation, __ATOMIC_RELAXED);
        snapshot->request_id =
            __atomic_load_n(&request->request_id, __ATOMIC_RELAXED);
        snapshot->target_id =
            __atomic_load_n(&request->target_id, __ATOMIC_RELAXED);
        snapshot->status = __atomic_load_n(&request->status, __ATOMIC_RELAXED);
        after = __atomic_load_n(&request->state, __ATOMIC_ACQUIRE);
        if (before == after)
            return 0;
    }
    return -EAGAIN;
}

int spz_async_busy(const struct spz_async_request *request)
{
    uint32_t state;

    if (request == NULL)
        return 0;
    state = __atomic_load_n(&request->state, __ATOMIC_ACQUIRE);
    return state == (uint32_t)SPZ_ASYNC_PENDING ||
           state == (uint32_t)SPZ_ASYNC_RUNNING;
}

#if defined(SPZ_KPATCH_BUILD)

typedef int (*spz_queue_work_on_fn)(int cpu, void *workqueue, void *work);
typedef int (*spz_flush_work_fn)(void *work);
typedef int (*spz_schedule_on_each_cpu_fn)(void (*worker)(void *work));
typedef void (*spz_work_fn)(void *work);

static struct spz_kpatch_runtime_context *spz_transport_context;

static spz_queue_work_on_fn spz_queue_work_on_from_address(uint64_t address)
{
    union {
        uint64_t address;
        spz_queue_work_on_fn function;
    } conversion;

    conversion.address = address;
    return conversion.function;
}

static spz_flush_work_fn spz_flush_work_from_address(uint64_t address)
{
    union {
        uint64_t address;
        spz_flush_work_fn function;
    } conversion;

    conversion.address = address;
    return conversion.function;
}

static spz_schedule_on_each_cpu_fn
spz_schedule_on_each_cpu_from_address(uint64_t address)
{
    union {
        uint64_t address;
        spz_schedule_on_each_cpu_fn function;
    } conversion;

    conversion.address = address;
    return conversion.function;
}

static uint64_t spz_work_function_address(spz_work_fn function)
{
    union {
        uint64_t address;
        spz_work_fn function;
    } conversion;

    conversion.function = function;
    return conversion.address;
}

static void spz_kpatch_async_worker(void *work)
{
    struct spz_kpatch_runtime_context *context =
        __atomic_load_n(&spz_transport_context, __ATOMIC_ACQUIRE);

    if (context == NULL || context->module == NULL ||
        work != (void *)context->work_storage)
        return;
    (void)spz_async_run(&context->module->async);
}

static void spz_kpatch_each_cpu_worker(void *work)
{
    struct spz_kpatch_runtime_context *context =
        __atomic_load_n(&spz_transport_context, __ATOMIC_ACQUIRE);
    int (*callback)(void *, uint32_t);
    uint32_t cpu;
    int expected;
    int result;

    (void)work;
    if (context == NULL ||
        __atomic_load_n(&context->each_cpu_active, __ATOMIC_ACQUIRE) == 0U)
        return;
    callback = context->each_cpu_callback;
    if (callback == NULL || context->module == NULL ||
        context->module->platform.current_cpu == NULL)
        result = -EINVAL;
    else {
        result = context->module->platform.current_cpu(
            context->module->platform.context, &cpu);
        if (result == 0)
            result = callback(context->each_cpu_context, cpu);
    }
    if (result != 0) {
        expected = 0;
        (void)__atomic_compare_exchange_n(&context->each_cpu_status, &expected,
                                          result < 0 ? result : -EIO, 0,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
}

int spz_kpatch_async_transport_init(struct spz_kpatch_runtime_context *context)
{
    const struct spz_layout_profile *layout;
    uintptr_t entry;
    uint64_t data;
    uint64_t function;
    uint64_t list_pointer;
    struct spz_kpatch_runtime_context *expected;

    if (context == NULL || context->module == NULL ||
        context->module->profile == NULL ||
        context->queue_work_on_address == 0U ||
        context->schedule_on_each_cpu_address == 0U ||
        context->system_unbound_wq_address == 0U)
        return -EINVAL;
    layout = &context->module->profile->layout;
    if (layout->work_struct_size < 32U ||
        layout->work_struct_size > sizeof(context->work_storage) ||
        layout->work_data > layout->work_struct_size - sizeof(uint64_t) ||
        layout->work_entry > layout->work_struct_size - 2U * sizeof(uint64_t) ||
        layout->work_func > layout->work_struct_size - sizeof(uint64_t))
        return -ERANGE;
    memset(context->work_storage, 0, sizeof(context->work_storage));
    data = layout->work_data_init;
    memcpy(context->work_storage + layout->work_data, &data, sizeof(data));
    entry = (uintptr_t)(context->work_storage + layout->work_entry);
    list_pointer = (uint64_t)entry;
    memcpy(context->work_storage + layout->work_entry, &list_pointer,
           sizeof(list_pointer));
    memcpy(context->work_storage + layout->work_entry + sizeof(list_pointer),
           &list_pointer, sizeof(list_pointer));
    function = spz_work_function_address(spz_kpatch_async_worker);
    memcpy(context->work_storage + layout->work_func, &function,
           sizeof(function));
    if (spz_kpatch_nofault_read(context, context->system_unbound_wq_address,
                                &context->system_unbound_wq,
                                sizeof(context->system_unbound_wq)) != 0)
        return -EFAULT;
    if (context->system_unbound_wq == NULL)
        return -ENOENT;
    expected = NULL;
    if (!__atomic_compare_exchange_n(&spz_transport_context, &expected, context,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
        expected != context)
        return -EBUSY;
    __atomic_store_n(&context->transport_ready, 1U, __ATOMIC_RELEASE);
    return 0;
}

void spz_kpatch_async_transport_reset(
    struct spz_kpatch_runtime_context *context)
{
    struct spz_kpatch_runtime_context *expected = context;

    if (context == NULL)
        return;
    __atomic_store_n(&context->transport_ready, 0U, __ATOMIC_RELEASE);
    (void)__atomic_compare_exchange_n(&spz_transport_context, &expected, NULL,
                                      0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

int spz_kpatch_async_queue(void *opaque)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    spz_queue_work_on_fn function;
    int queued;

    if (context == NULL ||
        __atomic_load_n(&context->transport_ready, __ATOMIC_ACQUIRE) == 0U ||
        context->system_unbound_wq == NULL)
        return -EINVAL;
    function = spz_queue_work_on_from_address(context->queue_work_on_address);
    if (function == NULL)
        return -ENOENT;
    queued = function(0, context->system_unbound_wq,
                      (void *)context->work_storage);
    return queued != 0 ? 0 : -EBUSY;
}

int spz_kpatch_async_quiesce(void *opaque)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    spz_flush_work_fn function;

    if (context == NULL ||
        __atomic_load_n(&context->transport_ready, __ATOMIC_ACQUIRE) == 0U)
        return -EINVAL;
    function = spz_flush_work_from_address(context->flush_work_address);
    if (function == NULL)
        return -ENOENT;
    (void)function((void *)context->work_storage);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return spz_async_busy(&context->module->async) ? -EBUSY : 0;
}

int spz_kpatch_run_each_cpu(
    void *opaque, int (*callback)(void *callback_context, uint32_t cpu),
    void *callback_context)
{
    struct spz_kpatch_runtime_context *context =
        (struct spz_kpatch_runtime_context *)opaque;
    spz_schedule_on_each_cpu_fn function;
    uint32_t expected = 0U;
    int result;

    if (context == NULL || callback == NULL ||
        __atomic_load_n(&context->transport_ready, __ATOMIC_ACQUIRE) == 0U)
        return -EINVAL;
    if (!__atomic_compare_exchange_n(&context->each_cpu_active, &expected, 1U,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -EBUSY;
    context->each_cpu_callback = callback;
    context->each_cpu_context = callback_context;
    __atomic_store_n(&context->each_cpu_status, 0, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    function = spz_schedule_on_each_cpu_from_address(
        context->schedule_on_each_cpu_address);
    result = function == NULL ? -ENOENT : function(spz_kpatch_each_cpu_worker);
    if (result > 0)
        result = -EIO;
    if (result == 0)
        result = __atomic_load_n(&context->each_cpu_status, __ATOMIC_ACQUIRE);
    context->each_cpu_callback = NULL;
    context->each_cpu_context = NULL;
    __atomic_store_n(&context->each_cpu_active, 0U, __ATOMIC_RELEASE);
    return result;
}

#endif
