#ifndef STACKPLZ_CORE_H
#define STACKPLZ_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "stackplz/abi.h"

enum spz_command_kind {
    SPZ_COMMAND_INVALID = 0,
    SPZ_COMMAND_STATUS,
    SPZ_COMMAND_PROFILE,
    SPZ_COMMAND_BIND,
    SPZ_COMMAND_BREAK,
    SPZ_COMMAND_ENABLE,
    SPZ_COMMAND_DISABLE,
    SPZ_COMMAND_CLEAR,
    SPZ_COMMAND_POLL,
    SPZ_COMMAND_AUDIT,
    SPZ_COMMAND_MAPS,
    SPZ_COMMAND_MAPS_READ,
};

enum spz_bind_mode {
    SPZ_BIND_INVALID = 0,
    SPZ_BIND_PID,
    SPZ_BIND_TGID,
    SPZ_BIND_EITHER,
};

enum spz_break_kind {
    SPZ_BREAK_INVALID = 0,
    SPZ_BREAK_EXECUTE,
    SPZ_BREAK_READ,
    SPZ_BREAK_WRITE,
    SPZ_BREAK_READ_WRITE,
};

enum spz_break_mode {
    SPZ_BREAK_MODE_INVALID = 0,
    SPZ_BREAK_ONCE,
    SPZ_BREAK_REPEAT,
};

struct spz_bind_command {
    uint32_t pid;
    enum spz_bind_mode mode;
    uint32_t uid;
    uint64_t start_boot_time;
    char comm[SPZ_COMM_LEN];
    uint8_t has_uid;
    uint8_t has_comm;
    uint8_t has_start_boot_time;
};

struct spz_break_command {
    uint32_t id;
    enum spz_break_kind kind;
    uint64_t address;
    uint8_t length;
    enum spz_break_mode mode;
};

struct spz_id_command {
    uint32_t id;
};

struct spz_poll_command {
    uint64_t after;
    uint8_t has_after;
};

struct spz_maps_read_command {
    uint64_t snapshot;
    uint32_t offset;
};

struct spz_command {
    enum spz_command_kind kind;
    union {
        struct spz_bind_command bind;
        struct spz_break_command breakpoint;
        struct spz_id_command id;
        struct spz_poll_command poll;
        struct spz_maps_read_command maps_read;
    } value;
};

struct spz_ring_slot {
    uint64_t commit;
    struct spz_event event;
};

struct spz_cpu_ring {
    uint64_t head;
    uint64_t tail;
    uint64_t lost;
    uint64_t loss_pending;
    struct spz_ring_slot slots[SPZ_RING_CAPACITY];
};

struct spz_ring {
    uint64_t next_sequence;
    uint64_t consumer_after;
    struct spz_cpu_ring cpu[SPZ_MAX_CPUS];
};

int spz_parse_command(char *input, size_t length, struct spz_command *out);
uint32_t spz_crc32_ieee(const void *data, size_t length);
void spz_ring_init(struct spz_ring *ring);
int spz_ring_push(struct spz_ring *ring, uint32_t cpu, const struct spz_event *event);
int spz_ring_pop_after(struct spz_ring *ring, uint64_t after, struct spz_event *out);
uint64_t spz_ring_lost(const struct spz_ring *ring, uint32_t cpu);

#endif
