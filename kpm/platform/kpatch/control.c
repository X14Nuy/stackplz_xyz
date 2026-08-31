#include "stackplz/platform.h"

#include "compat.h"

struct spz_text {
    char *data;
    size_t capacity;
    size_t length;
    int error;
};

static void spz_text_bytes(struct spz_text *text, const char *value, size_t length)
{
    if (text->error != 0)
        return;
    if (length > text->capacity - text->length) {
        text->error = -ENOSPC;
        return;
    }
    memcpy(text->data + text->length, value, length);
    text->length += length;
}

static void spz_text_string(struct spz_text *text, const char *value)
{
    if (value == NULL) {
        text->error = -EINVAL;
        return;
    }
    spz_text_bytes(text, value, strlen(value));
}

static void spz_text_char(struct spz_text *text, char value)
{
    spz_text_bytes(text, &value, 1U);
}

static void spz_text_u64(struct spz_text *text, uint64_t value)
{
    char reversed[32];
    size_t length = 0U;

    do {
        reversed[length++] = (char)('0' + (value % UINT64_C(10)));
        value /= UINT64_C(10);
    } while (value != 0U);
    while (length != 0U)
        spz_text_char(text, reversed[--length]);
}

static void spz_text_i64(struct spz_text *text, int64_t value)
{
    uint64_t magnitude;

    if (value < 0) {
        spz_text_char(text, '-');
        magnitude = (uint64_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint64_t)value;
    }
    spz_text_u64(text, magnitude);
}

static void spz_text_hex(struct spz_text *text, const void *value,
                         size_t length)
{
    static const char digits[] = "0123456789abcdef";
    const uint8_t *bytes = (const uint8_t *)value;
    size_t index;

    if (value == NULL && length != 0U) {
        text->error = -EINVAL;
        return;
    }
    for (index = 0U; index < length; index++) {
        char encoded[2];

        encoded[0] = digits[bytes[index] >> 4U];
        encoded[1] = digits[bytes[index] & UINT8_C(0x0f)];
        spz_text_bytes(text, encoded, sizeof(encoded));
    }
}

static void spz_text_token_string(struct spz_text *text, const char *key,
                                  const char *value)
{
    spz_text_char(text, ' ');
    spz_text_string(text, key);
    spz_text_char(text, '=');
    spz_text_string(text, value);
}

static void spz_text_token_hex(struct spz_text *text, const char *key,
                               const void *value, size_t length)
{
    spz_text_char(text, ' ');
    spz_text_string(text, key);
    spz_text_char(text, '=');
    spz_text_hex(text, value, length);
}

static void spz_text_token_u64(struct spz_text *text, const char *key,
                               uint64_t value)
{
    spz_text_char(text, ' ');
    spz_text_string(text, key);
    spz_text_char(text, '=');
    spz_text_u64(text, value);
}

static const char *spz_binding_state_name(enum spz_binding_state state)
{
    switch (state) {
    case SPZ_BINDING_EMPTY: return "none";
    case SPZ_BINDING_PENDING: return "pending";
    case SPZ_BINDING_BOUND: return "bound";
    case SPZ_BINDING_STALE: return "stale";
    case SPZ_BINDING_EXITED: return "exited";
    default: return "invalid";
    }
}

static const char *spz_async_state_name(enum spz_async_state state)
{
    switch (state) {
    case SPZ_ASYNC_FREE: return "free";
    case SPZ_ASYNC_PENDING: return "pending";
    case SPZ_ASYNC_RUNNING: return "running";
    case SPZ_ASYNC_DONE: return "done";
    default: return "invalid";
    }
}

static const char *spz_control_reason(int status)
{
    switch (-status) {
    case 0: return "ok";
    case EINVAL: return "invalid";
    case ENOENT: return "not_found";
    case EBUSY: return "busy";
    case EAGAIN: return "pending";
    case ESTALE: return "stale";
    case ESRCH: return "exited";
    case ENOSPC: return "no_space";
    case EUCLEAN: return "quarantined";
    case ERANGE: return "range";
    case E2BIG: return "too_large";
    case EOPNOTSUPP: return "unsupported";
    default: return "failed";
    }
}

static void spz_status_fields(struct spz_module_state *module,
                              struct spz_text *extra)
{
    struct spz_binding_snapshot binding;
    struct spz_async_snapshot async;
    struct spz_maps_info maps;

    spz_text_token_string(extra, "state", module->ready != 0U ? "ready" :
                                                               "rejected");
    spz_text_token_string(extra, "profile", module->profile != NULL ?
        module->profile->id : "none");
    if (spz_binding_snapshot(&module->binding, &binding) == 0) {
        spz_text_token_string(extra, "binding",
                              spz_binding_state_name(binding.state));
        if (binding.state == SPZ_BINDING_BOUND) {
            spz_text_token_u64(extra, "generation", binding.identity.generation);
            spz_text_token_u64(extra, "task_cookie", binding.identity.task_cookie);
            spz_text_token_u64(extra, "pid", binding.identity.pid);
            spz_text_token_u64(extra, "tgid", binding.identity.tgid);
            spz_text_token_u64(extra, "uid", binding.identity.uid);
            spz_text_token_u64(extra, "start_time", binding.identity.start_time);
            spz_text_token_u64(extra, "start_boottime",
                               binding.identity.start_boot_time);
            spz_text_token_hex(extra, "comm_hex", binding.identity.comm,
                               sizeof(binding.identity.comm));
        }
    }
    spz_text_token_u64(extra, "configured",
                       __atomic_load_n(&module->breakpoint_configured,
                                       __ATOMIC_ACQUIRE));
    spz_text_token_u64(extra, "enabled",
                       __atomic_load_n(&module->breakpoint_enabled,
                                       __ATOMIC_ACQUIRE));
    if (spz_maps_info(&module->maps, &maps) == 0) {
        const char *state = "invalid";

        switch (maps.state) {
        case SPZ_MAPS_UNSUPPORTED: state = "unsupported"; break;
        case SPZ_MAPS_EMPTY: state = "empty"; break;
        case SPZ_MAPS_TASK_CAPTURED: state = "task"; break;
        case SPZ_MAPS_READY: state = "ready"; break;
        default: break;
        }
        spz_text_token_u64(extra, "maps_supported",
                           maps.state == SPZ_MAPS_UNSUPPORTED ? 0U : 1U);
        spz_text_token_string(extra, "maps_state", state);
        spz_text_token_u64(extra, "maps_snapshot", maps.snapshot);
        spz_text_token_u64(extra, "maps_size", maps.length);
    }
    if (spz_async_snapshot(&module->async, &async) == 0) {
        spz_text_token_u64(extra, "request", async.request_id);
        spz_text_token_string(extra, "request_state",
                              spz_async_state_name(async.state));
        spz_text_char(extra, ' ');
        spz_text_string(extra, "request_status=");
        spz_text_i64(extra, async.status);
    }
}

static int spz_control_bind(struct spz_module_state *module,
                            const struct spz_bind_command *command,
                            struct spz_text *extra)
{
    struct spz_binding_request request;
    uint32_t binding_id;
    uint64_t generation;
    int result;

    if (module->accepting_commands == 0U || spz_async_busy(&module->async) ||
        spz_maps_has_task(&module->maps) ||
        module->breakpoint_configured != 0U)
        return -EBUSY;
    memset(&request, 0, sizeof(request));
    binding_id = module->binding_counter + 1U;
    if (binding_id == 0U)
        binding_id = 1U;
    request.binding_id = binding_id;
    request.pid = command->pid;
    request.mode = command->mode;
    request.uid = command->uid;
    request.start_boot_time = command->start_boot_time;
    memcpy(request.comm, command->comm, sizeof(request.comm));
    request.has_uid = command->has_uid;
    request.has_comm = command->has_comm;
    request.has_start_boot_time = command->has_start_boot_time;
    result = spz_binding_set(&module->binding, &request, &generation);
    if (result != 0)
        return result;
    module->binding_counter = binding_id;
    spz_text_token_u64(extra, "binding_id", binding_id);
    spz_text_token_u64(extra, "generation", generation);
    return 0;
}

static int spz_control_break(struct spz_module_state *module,
                             const struct spz_break_command *command,
                             struct spz_text *extra)
{
    struct spz_binding_snapshot binding;
    struct spz_debug_request request;
    uint64_t value;
    uint32_t control;
    uint8_t bas;
    int result;

    if (module->accepting_commands == 0U || spz_async_busy(&module->async) ||
        module->breakpoint_enabled != 0U)
        return -EBUSY;
    result = spz_binding_snapshot(&module->binding, &binding);
    if (result != 0)
        return result;
    if (binding.state == SPZ_BINDING_EMPTY)
        return -ENOENT;
    memset(&request, 0, sizeof(request));
    request.id = command->id;
    request.kind = command->kind;
    request.address = command->address;
    request.length = command->length;
    request.mode = command->mode;
    result = spz_debug_validate_request(&request, &value, &control, &bas);
    if (result != 0)
        return result;
    module->breakpoint = request;
    __atomic_store_n(&module->breakpoint_configured, 1U, __ATOMIC_RELEASE);
    spz_text_token_u64(extra, "id", request.id);
    spz_text_token_u64(extra, "value", value);
    spz_text_token_u64(extra, "control", control);
    spz_text_token_u64(extra, "bas", bas);
    return 0;
}

static int spz_control_async(struct spz_module_state *module,
                             enum spz_async_operation operation,
                             uint32_t target_id, struct spz_text *extra)
{
    uint64_t request_id;
    int result;

    if (module->accepting_commands == 0U)
        return -EBUSY;
    result = spz_async_submit(&module->async, operation, target_id,
                              &request_id);
    if (result == 0)
        spz_text_token_u64(extra, "request", request_id);
    return result;
}

static void spz_event_hex(struct spz_text *extra, const struct spz_event *event)
{
    spz_text_string(extra, " event=");
    spz_text_hex(extra, event, sizeof(*event));
}

static int spz_control_poll(struct spz_module_state *module,
                            const struct spz_poll_command *command,
                            struct spz_text *extra)
{
    struct spz_event event;
    uint64_t after = command->has_after != 0U ? command->after : 0U;
    int result = spz_ring_pop_after(&module->ring, after, &event);

    if (result < 0)
        return result;
    if (result == 0)
        spz_text_token_u64(extra, "empty", 1U);
    else
        spz_event_hex(extra, &event);
    return 0;
}

static int spz_control_maps_read(struct spz_module_state *module,
                                 const struct spz_maps_read_command *command,
                                 struct spz_text *extra)
{
    struct spz_maps_read read;
    int result = spz_maps_read_begin(&module->maps, command->snapshot,
                                     command->offset, &read);

    if (result != 0)
        return result;
    spz_text_token_u64(extra, "snapshot", read.snapshot);
    spz_text_token_u64(extra, "offset", read.offset);
    spz_text_token_u64(extra, "total", read.total);
    spz_text_token_u64(extra, "crc32", read.crc32);
    spz_text_token_u64(extra, "eof", read.eof);
    spz_text_token_hex(extra, "data", read.data, read.length);
    result = extra->error;
    spz_maps_read_end(&module->maps);
    return result;
}

static int spz_control_dispatch(struct spz_module_state *module,
                                const struct spz_command *command,
                                struct spz_text *extra)
{
    switch (command->kind) {
    case SPZ_COMMAND_STATUS:
        spz_status_fields(module, extra);
        return extra->error;
    case SPZ_COMMAND_PROFILE:
        spz_text_token_string(extra, "profile", module->profile->id);
        spz_text_token_string(extra, "kernel", module->profile->kernel.release);
        spz_text_token_string(extra, "kpatch", module->profile->kpatch.kpver);
        return extra->error;
    case SPZ_COMMAND_BIND:
        return spz_control_bind(module, &command->value.bind, extra);
    case SPZ_COMMAND_BREAK:
        return spz_control_break(module, &command->value.breakpoint, extra);
    case SPZ_COMMAND_ENABLE:
        return spz_control_async(module, SPZ_ASYNC_ENABLE,
                                 command->value.id.id, extra);
    case SPZ_COMMAND_DISABLE:
        if (module->breakpoint_enabled == 0U) {
            spz_text_token_u64(extra, "request", 0U);
            return 0;
        }
        return spz_control_async(module, SPZ_ASYNC_DISABLE,
                                 command->value.id.id, extra);
    case SPZ_COMMAND_CLEAR:
        return spz_control_async(module, SPZ_ASYNC_CLEAR, 0U, extra);
    case SPZ_COMMAND_POLL:
        return spz_control_poll(module, &command->value.poll, extra);
    case SPZ_COMMAND_AUDIT:
        return spz_control_async(module, SPZ_ASYNC_AUDIT, 0U, extra);
    case SPZ_COMMAND_MAPS:
        if (!spz_maps_supported(&module->maps))
            return -EOPNOTSUPP;
        return spz_control_async(module, SPZ_ASYNC_MAPS, 0U, extra);
    case SPZ_COMMAND_MAPS_READ:
        return spz_control_maps_read(module, &command->value.maps_read, extra);
    default:
        return -EINVAL;
    }
}

int spz_control_execute(struct spz_module_state *module, char *command,
                        size_t command_length, char *response,
                        size_t response_capacity)
{
    struct spz_text extra;
    struct spz_text output;
    struct spz_command parsed;
    uint32_t expected = 0U;
    int status;

    if (module == NULL || command == NULL || response == NULL ||
        response_capacity == 0U || response_capacity > SPZ_CONTROL_RESPONSE_MAX)
        return -EINVAL;
    if (!__atomic_compare_exchange_n(&module->control_busy, &expected, 1U, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        status = -EBUSY;
    else {
        memset(&extra, 0, sizeof(extra));
        extra.data = module->control_scratch;
        extra.capacity = sizeof(module->control_scratch);
        status = spz_parse_command(command, command_length, &parsed);
        if (status == 0)
            status = spz_control_dispatch(module, &parsed, &extra);
        if (status == 0 && extra.error != 0)
            status = extra.error;
        __atomic_store_n(&module->control_busy, 0U, __ATOMIC_RELEASE);
    }

    memset(&output, 0, sizeof(output));
    output.data = response;
    output.capacity = response_capacity;
    spz_text_string(&output, "status=");
    spz_text_i64(&output, status);
    spz_text_string(&output, " version=1");
    if (status == 0)
        spz_text_bytes(&output, extra.data, extra.length);
    else
        spz_text_token_string(&output, "reason", spz_control_reason(status));
    if (output.error != 0)
        return output.error;
    if (output.length == output.capacity)
        return -ENOSPC;
    output.data[output.length] = '\0';
    return (int)output.length;
}
