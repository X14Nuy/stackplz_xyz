#include "stackplz/platform.h"

#include "stackplz/core.h"

#define SPZ_MAX_TOKENS 8U

struct spz_token {
    char *data;
    size_t length;
};

static int spz_token_equals(const struct spz_token *token, const char *literal)
{
    size_t length = strlen(literal);

    return token->length == length && memcmp(token->data, literal, length) == 0;
}

static int spz_parse_u64_decimal(const struct spz_token *token, uint64_t *out)
{
    uint64_t value = 0U;
    size_t index;

    if (token->length == 0U)
        return -EINVAL;
    for (index = 0U; index < token->length; index++) {
        uint8_t byte = (uint8_t)token->data[index];
        uint64_t digit;

        if (byte < (uint8_t)'0' || byte > (uint8_t)'9')
            return -EINVAL;
        digit = (uint64_t)(byte - (uint8_t)'0');
        if (value > (UINT64_MAX - digit) / UINT64_C(10))
            return -ERANGE;
        value = value * UINT64_C(10) + digit;
    }
    *out = value;
    return 0;
}

static int spz_parse_u32_decimal(const struct spz_token *token, uint32_t *out)
{
    uint64_t value;
    int result = spz_parse_u64_decimal(token, &value);

    if (result != 0)
        return result;
    if (value > UINT32_MAX)
        return -ERANGE;
    *out = (uint32_t)value;
    return 0;
}

static int spz_parse_u8_decimal(const struct spz_token *token, uint8_t *out)
{
    uint64_t value;
    int result = spz_parse_u64_decimal(token, &value);

    if (result != 0)
        return result;
    if (value > UINT8_MAX)
        return -ERANGE;
    *out = (uint8_t)value;
    return 0;
}

static int spz_parse_u64_hex(const struct spz_token *token, uint64_t *out)
{
    uint64_t value = 0U;
    size_t index;

    if (token->length < 3U || token->data[0] != '0' || token->data[1] != 'x')
        return -EINVAL;
    for (index = 2U; index < token->length; index++) {
        uint8_t byte = (uint8_t)token->data[index];
        uint64_t digit;

        if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9')
            digit = (uint64_t)(byte - (uint8_t)'0');
        else if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f')
            digit = (uint64_t)(byte - (uint8_t)'a') + UINT64_C(10);
        else if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F')
            digit = (uint64_t)(byte - (uint8_t)'A') + UINT64_C(10);
        else
            return -EINVAL;
        if (value > (UINT64_MAX - digit) / UINT64_C(16))
            return -ERANGE;
        value = value * UINT64_C(16) + digit;
    }
    *out = value;
    return 0;
}

static int spz_safe_comm(const struct spz_token *value)
{
    size_t index;

    if (value->length == 0U || value->length >= SPZ_COMM_LEN)
        return 0;
    for (index = 0U; index < value->length; index++) {
        uint8_t byte = (uint8_t)value->data[index];

        if ((byte >= (uint8_t)'a' && byte <= (uint8_t)'z') ||
            (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') ||
            (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') || byte == (uint8_t)'.' ||
            byte == (uint8_t)'_' || byte == (uint8_t)':' || byte == (uint8_t)'-')
            continue;
        return 0;
    }
    return 1;
}

static int spz_split_key_value(const struct spz_token *token, struct spz_token *key,
                               struct spz_token *value)
{
    size_t index;
    size_t separator = token->length;

    for (index = 0U; index < token->length; index++) {
        if (token->data[index] != '=')
            continue;
        if (separator != token->length)
            return -EINVAL;
        separator = index;
    }
    if (separator == 0U || separator + 1U >= token->length)
        return -EINVAL;
    key->data = token->data;
    key->length = separator;
    value->data = token->data + separator + 1U;
    value->length = token->length - separator - 1U;
    return 0;
}

static int spz_tokenize(char *input, size_t length, struct spz_token tokens[SPZ_MAX_TOKENS],
                        size_t *count)
{
    size_t start = 0U;
    size_t index;
    size_t used = 0U;

    if (input[0] == ' ' || input[length - 1U] == ' ')
        return -EINVAL;
    for (index = 0U; index < length; index++) {
        uint8_t byte = (uint8_t)input[index];

        if (byte < UINT8_C(0x20) || byte > UINT8_C(0x7e))
            return -EINVAL;
        if (byte != (uint8_t)' ')
            continue;
        if (index == start)
            return -EINVAL;
        if (used >= SPZ_MAX_TOKENS)
            return -E2BIG;
        tokens[used].data = input + start;
        tokens[used].length = index - start;
        used++;
        input[index] = '\0';
        start = index + 1U;
    }
    if (used >= SPZ_MAX_TOKENS)
        return -E2BIG;
    tokens[used].data = input + start;
    tokens[used].length = length - start;
    *count = used + 1U;
    return 0;
}

static int spz_parse_no_args(size_t count, enum spz_command_kind kind,
                             struct spz_command *out)
{
    if (count != 1U)
        return -EINVAL;
    out->kind = kind;
    return 0;
}

static int spz_parse_bind(const struct spz_token *tokens, size_t count, struct spz_command *out)
{
    enum {
        SPZ_BIND_SEEN_PID = 1U << 0,
        SPZ_BIND_SEEN_MODE = 1U << 1,
        SPZ_BIND_SEEN_UID = 1U << 2,
        SPZ_BIND_SEEN_COMM = 1U << 3,
        SPZ_BIND_SEEN_START = 1U << 4,
    };
    unsigned int seen = 0U;
    size_t index;

    if (count < 3U || count > 6U)
        return -EINVAL;
    out->kind = SPZ_COMMAND_BIND;
    for (index = 1U; index < count; index++) {
        struct spz_token key;
        struct spz_token value;
        unsigned int bit;
        int result = spz_split_key_value(&tokens[index], &key, &value);

        if (result != 0)
            return result;
        if (spz_token_equals(&key, "pid")) {
            bit = SPZ_BIND_SEEN_PID;
            result = spz_parse_u32_decimal(&value, &out->value.bind.pid);
            if (result == 0 && out->value.bind.pid == 0U)
                result = -EINVAL;
        } else if (spz_token_equals(&key, "mode")) {
            bit = SPZ_BIND_SEEN_MODE;
            if (spz_token_equals(&value, "pid"))
                out->value.bind.mode = SPZ_BIND_PID;
            else if (spz_token_equals(&value, "tgid"))
                out->value.bind.mode = SPZ_BIND_TGID;
            else if (spz_token_equals(&value, "either"))
                out->value.bind.mode = SPZ_BIND_EITHER;
            else
                result = -EINVAL;
        } else if (spz_token_equals(&key, "uid")) {
            bit = SPZ_BIND_SEEN_UID;
            result = spz_parse_u32_decimal(&value, &out->value.bind.uid);
            if (result == 0)
                out->value.bind.has_uid = 1U;
        } else if (spz_token_equals(&key, "comm")) {
            bit = SPZ_BIND_SEEN_COMM;
            if (!spz_safe_comm(&value))
                result = -EINVAL;
            else {
                memcpy(out->value.bind.comm, value.data, value.length);
                out->value.bind.comm[value.length] = '\0';
                out->value.bind.has_comm = 1U;
            }
        } else if (spz_token_equals(&key, "start")) {
            bit = SPZ_BIND_SEEN_START;
            result = spz_parse_u64_decimal(&value, &out->value.bind.start_boot_time);
            if (result == 0)
                out->value.bind.has_start_boot_time = 1U;
        } else {
            return -EINVAL;
        }
        if ((seen & bit) != 0U)
            return -EINVAL;
        if (result != 0)
            return result;
        seen |= bit;
    }
    if ((seen & (SPZ_BIND_SEEN_PID | SPZ_BIND_SEEN_MODE)) !=
        (SPZ_BIND_SEEN_PID | SPZ_BIND_SEEN_MODE))
        return -EINVAL;
    return 0;
}

static int spz_parse_break(const struct spz_token *tokens, size_t count, struct spz_command *out)
{
    enum {
        SPZ_BREAK_SEEN_ID = 1U << 0,
        SPZ_BREAK_SEEN_KIND = 1U << 1,
        SPZ_BREAK_SEEN_ADDRESS = 1U << 2,
        SPZ_BREAK_SEEN_LENGTH = 1U << 3,
        SPZ_BREAK_SEEN_MODE = 1U << 4,
    };
    const unsigned int required = SPZ_BREAK_SEEN_ID | SPZ_BREAK_SEEN_KIND |
                                  SPZ_BREAK_SEEN_ADDRESS | SPZ_BREAK_SEEN_LENGTH |
                                  SPZ_BREAK_SEEN_MODE;
    unsigned int seen = 0U;
    size_t index;

    if (count != 6U)
        return -EINVAL;
    out->kind = SPZ_COMMAND_BREAK;
    for (index = 1U; index < count; index++) {
        struct spz_token key;
        struct spz_token value;
        unsigned int bit;
        int result = spz_split_key_value(&tokens[index], &key, &value);

        if (result != 0)
            return result;
        if (spz_token_equals(&key, "id")) {
            bit = SPZ_BREAK_SEEN_ID;
            result = spz_parse_u32_decimal(&value, &out->value.breakpoint.id);
            if (result == 0 && out->value.breakpoint.id == 0U)
                result = -EINVAL;
        } else if (spz_token_equals(&key, "kind")) {
            bit = SPZ_BREAK_SEEN_KIND;
            if (spz_token_equals(&value, "x"))
                out->value.breakpoint.kind = SPZ_BREAK_EXECUTE;
            else if (spz_token_equals(&value, "r"))
                out->value.breakpoint.kind = SPZ_BREAK_READ;
            else if (spz_token_equals(&value, "w"))
                out->value.breakpoint.kind = SPZ_BREAK_WRITE;
            else if (spz_token_equals(&value, "rw"))
                out->value.breakpoint.kind = SPZ_BREAK_READ_WRITE;
            else
                result = -EINVAL;
        } else if (spz_token_equals(&key, "addr")) {
            bit = SPZ_BREAK_SEEN_ADDRESS;
            result = spz_parse_u64_hex(&value, &out->value.breakpoint.address);
        } else if (spz_token_equals(&key, "len")) {
            bit = SPZ_BREAK_SEEN_LENGTH;
            result = spz_parse_u8_decimal(&value, &out->value.breakpoint.length);
        } else if (spz_token_equals(&key, "mode")) {
            bit = SPZ_BREAK_SEEN_MODE;
            if (spz_token_equals(&value, "once"))
                out->value.breakpoint.mode = SPZ_BREAK_ONCE;
            else if (spz_token_equals(&value, "repeat"))
                out->value.breakpoint.mode = SPZ_BREAK_REPEAT;
            else
                result = -EINVAL;
        } else {
            return -EINVAL;
        }
        if ((seen & bit) != 0U)
            return -EINVAL;
        if (result != 0)
            return result;
        seen |= bit;
    }
    if (seen != required)
        return -EINVAL;
    if (out->value.breakpoint.address == 0U || (out->value.breakpoint.address >> 56U) != 0U)
        return -EINVAL;
    if (out->value.breakpoint.kind == SPZ_BREAK_EXECUTE) {
        if (out->value.breakpoint.length != 4U || (out->value.breakpoint.address & 3U) != 0U)
            return -EINVAL;
    } else {
        uint64_t end;

        if (out->value.breakpoint.length != 1U && out->value.breakpoint.length != 2U &&
            out->value.breakpoint.length != 4U && out->value.breakpoint.length != 8U)
            return -EINVAL;
        end = out->value.breakpoint.address + (uint64_t)out->value.breakpoint.length - 1U;
        if (end < out->value.breakpoint.address ||
            (out->value.breakpoint.address >> 3U) != (end >> 3U))
            return -EINVAL;
    }
    return 0;
}

static int spz_parse_id(const struct spz_token *tokens, size_t count,
                        enum spz_command_kind kind, struct spz_command *out)
{
    struct spz_token key;
    struct spz_token value;
    int result;

    if (count != 2U)
        return -EINVAL;
    result = spz_split_key_value(&tokens[1], &key, &value);
    if (result != 0 || !spz_token_equals(&key, "id"))
        return -EINVAL;
    result = spz_parse_u32_decimal(&value, &out->value.id.id);
    if (result != 0)
        return result;
    if (out->value.id.id == 0U)
        return -EINVAL;
    out->kind = kind;
    return 0;
}

static int spz_parse_poll(const struct spz_token *tokens, size_t count, struct spz_command *out)
{
    struct spz_token key;
    struct spz_token value;
    int result;

    if (count > 2U)
        return -EINVAL;
    out->kind = SPZ_COMMAND_POLL;
    if (count == 1U)
        return 0;
    result = spz_split_key_value(&tokens[1], &key, &value);
    if (result != 0 || !spz_token_equals(&key, "after"))
        return -EINVAL;
    result = spz_parse_u64_decimal(&value, &out->value.poll.after);
    if (result == 0)
        out->value.poll.has_after = 1U;
    return result;
}

static int spz_parse_maps_read(const struct spz_token *tokens, size_t count,
                               struct spz_command *out)
{
    enum {
        SPZ_MAPS_READ_SEEN_SNAPSHOT = 1U << 0,
        SPZ_MAPS_READ_SEEN_OFFSET = 1U << 1,
    };
    const unsigned int required = SPZ_MAPS_READ_SEEN_SNAPSHOT |
                                  SPZ_MAPS_READ_SEEN_OFFSET;
    unsigned int seen = 0U;
    size_t index;

    if (count != 3U)
        return -EINVAL;
    out->kind = SPZ_COMMAND_MAPS_READ;
    for (index = 1U; index < count; index++) {
        struct spz_token key;
        struct spz_token value;
        unsigned int bit;
        int result = spz_split_key_value(&tokens[index], &key, &value);

        if (result != 0)
            return result;
        if (spz_token_equals(&key, "snapshot")) {
            bit = SPZ_MAPS_READ_SEEN_SNAPSHOT;
            result = spz_parse_u64_decimal(&value,
                                            &out->value.maps_read.snapshot);
            if (result == 0 && out->value.maps_read.snapshot == 0U)
                result = -EINVAL;
        } else if (spz_token_equals(&key, "offset")) {
            bit = SPZ_MAPS_READ_SEEN_OFFSET;
            result = spz_parse_u32_decimal(&value,
                                            &out->value.maps_read.offset);
        } else {
            return -EINVAL;
        }
        if ((seen & bit) != 0U)
            return -EINVAL;
        if (result != 0)
            return result;
        seen |= bit;
    }
    return seen == required ? 0 : -EINVAL;
}

int spz_parse_command(char *input, size_t length, struct spz_command *out)
{
    struct spz_token tokens[SPZ_MAX_TOKENS];
    struct spz_command parsed;
    size_t count;
    int result;

    if (input == NULL || out == NULL || length == 0U)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (length > SPZ_MAX_COMMAND)
        return -E2BIG;
    memset(&parsed, 0, sizeof(parsed));
    result = spz_tokenize(input, length, tokens, &count);
    if (result != 0)
        return result;

    if (spz_token_equals(&tokens[0], "status"))
        result = spz_parse_no_args(count, SPZ_COMMAND_STATUS, &parsed);
    else if (spz_token_equals(&tokens[0], "profile"))
        result = spz_parse_no_args(count, SPZ_COMMAND_PROFILE, &parsed);
    else if (spz_token_equals(&tokens[0], "bind"))
        result = spz_parse_bind(tokens, count, &parsed);
    else if (spz_token_equals(&tokens[0], "break"))
        result = spz_parse_break(tokens, count, &parsed);
    else if (spz_token_equals(&tokens[0], "enable"))
        result = spz_parse_id(tokens, count, SPZ_COMMAND_ENABLE, &parsed);
    else if (spz_token_equals(&tokens[0], "disable"))
        result = spz_parse_id(tokens, count, SPZ_COMMAND_DISABLE, &parsed);
    else if (spz_token_equals(&tokens[0], "clear"))
        result = spz_parse_no_args(count, SPZ_COMMAND_CLEAR, &parsed);
    else if (spz_token_equals(&tokens[0], "poll"))
        result = spz_parse_poll(tokens, count, &parsed);
    else if (spz_token_equals(&tokens[0], "audit"))
        result = spz_parse_no_args(count, SPZ_COMMAND_AUDIT, &parsed);
    else if (spz_token_equals(&tokens[0], "maps"))
        result = spz_parse_no_args(count, SPZ_COMMAND_MAPS, &parsed);
    else if (spz_token_equals(&tokens[0], "maps-read"))
        result = spz_parse_maps_read(tokens, count, &parsed);
    else
        result = -EINVAL;

    if (result == 0)
        *out = parsed;
    return result;
}
