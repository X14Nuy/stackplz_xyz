#ifndef STACKPLZ_ABI_H
#define STACKPLZ_ABI_H

#include <stddef.h>
#include <stdint.h>

#define SPZ_ABI_VERSION 1U
#define SPZ_EVENT_MAGIC UINT32_C(0x455a5053)
#define SPZ_EVENT_WIRE_SIZE 432U
#define SPZ_COMM_LEN 16U
#define SPZ_MAX_CPUS 8U
#define SPZ_RING_CAPACITY 64U
#define SPZ_MAX_COMMAND 512U

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "wire ABI requires little endian");
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SPZ_PACKED __attribute__((packed))
#else
#define SPZ_PACKED
#endif

enum spz_event_type {
    SPZ_EVENT_NONE = 0,
    SPZ_EVENT_BREAKPOINT = 1,
    SPZ_EVENT_WATCHPOINT = 2,
    SPZ_EVENT_INTEGRITY = 3,
    SPZ_EVENT_LOSS = 4,
    SPZ_EVENT_TASK = 5,
};

struct spz_registers {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
} SPZ_PACKED;

struct spz_event {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint16_t type;
    uint16_t flags;
    uint32_t cpu;
    uint64_t sequence;
    uint64_t timestamp;
    uint32_t binding_id;
    uint32_t breakpoint_id;
    uint64_t generation;
    uint64_t task_cookie;
    uint32_t pid;
    uint32_t tgid;
    uint32_t uid;
    uint32_t reserved0;
    uint64_t start_time;
    uint64_t start_boot_time;
    char comm[SPZ_COMM_LEN];
    uint32_t exception_class;
    uint16_t slot_kind;
    uint16_t slot_index;
    uint64_t requested_address;
    uint64_t observed_address;
    uint64_t value;
    uint32_t control;
    uint32_t observed_control;
    uint64_t mdscr;
    struct spz_registers registers;
    uint32_t reserved1;
    uint32_t crc32;
} SPZ_PACKED;

_Static_assert(sizeof(struct spz_registers) == 272U, "register wire size");
_Static_assert(sizeof(struct spz_event) == SPZ_EVENT_WIRE_SIZE, "event wire size");
_Static_assert(offsetof(struct spz_event, magic) == 0U, "magic offset");
_Static_assert(offsetof(struct spz_event, version) == 4U, "version offset");
_Static_assert(offsetof(struct spz_event, size) == 6U, "size offset");
_Static_assert(offsetof(struct spz_event, type) == 8U, "type offset");
_Static_assert(offsetof(struct spz_event, flags) == 10U, "flags offset");
_Static_assert(offsetof(struct spz_event, cpu) == 12U, "cpu offset");
_Static_assert(offsetof(struct spz_event, sequence) == 16U, "sequence offset");
_Static_assert(offsetof(struct spz_event, timestamp) == 24U, "timestamp offset");
_Static_assert(offsetof(struct spz_event, binding_id) == 32U, "binding offset");
_Static_assert(offsetof(struct spz_event, breakpoint_id) == 36U, "breakpoint offset");
_Static_assert(offsetof(struct spz_event, generation) == 40U, "generation offset");
_Static_assert(offsetof(struct spz_event, task_cookie) == 48U, "task cookie offset");
_Static_assert(offsetof(struct spz_event, pid) == 56U, "pid offset");
_Static_assert(offsetof(struct spz_event, tgid) == 60U, "tgid offset");
_Static_assert(offsetof(struct spz_event, uid) == 64U, "uid offset");
_Static_assert(offsetof(struct spz_event, start_time) == 72U, "start time offset");
_Static_assert(offsetof(struct spz_event, start_boot_time) == 80U, "start boot offset");
_Static_assert(offsetof(struct spz_event, comm) == 88U, "comm offset");
_Static_assert(offsetof(struct spz_event, exception_class) == 104U, "exception offset");
_Static_assert(offsetof(struct spz_event, slot_kind) == 108U, "slot kind offset");
_Static_assert(offsetof(struct spz_event, slot_index) == 110U, "slot index offset");
_Static_assert(offsetof(struct spz_event, requested_address) == 112U, "requested address offset");
_Static_assert(offsetof(struct spz_event, observed_address) == 120U, "observed address offset");
_Static_assert(offsetof(struct spz_event, value) == 128U, "value offset");
_Static_assert(offsetof(struct spz_event, control) == 136U, "control offset");
_Static_assert(offsetof(struct spz_event, observed_control) == 140U, "observed control offset");
_Static_assert(offsetof(struct spz_event, mdscr) == 144U, "mdscr offset");
_Static_assert(offsetof(struct spz_event, registers.x) == 152U, "x register offset");
_Static_assert(offsetof(struct spz_event, registers.sp) == 400U, "sp offset");
_Static_assert(offsetof(struct spz_event, registers.pc) == 408U, "pc offset");
_Static_assert(offsetof(struct spz_event, registers.pstate) == 416U, "pstate offset");
_Static_assert(offsetof(struct spz_event, crc32) == 428U, "crc offset");

#endif
