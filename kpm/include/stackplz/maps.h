#ifndef STACKPLZ_MAPS_H
#define STACKPLZ_MAPS_H

#include <stddef.h>
#include <stdint.h>

#define SPZ_MAPS_MAX_SNAPSHOT_BYTES (16U * 1024U * 1024U)
#define SPZ_MAPS_MAX_CHUNK_BYTES 1536U

enum spz_maps_state {
    SPZ_MAPS_UNSUPPORTED = 0,
    SPZ_MAPS_EMPTY,
    SPZ_MAPS_TASK_CAPTURED,
    SPZ_MAPS_READY,
};

struct spz_maps_backend {
    void *context;
    uint32_t max_snapshot_bytes;
    uint32_t max_chunk_bytes;
    int (*retain_task)(void *context, const void *task);
    void (*release_task)(void *context, const void *task);
    int (*render)(void *context, const void *task, uint8_t **data,
                  uint32_t *length);
    void (*release_buffer)(void *context, uint8_t *data, uint32_t length);
};

struct spz_maps {
    struct spz_maps_backend backend;
    const void *task;
    uint8_t *data;
    uint64_t task_generation;
    uint64_t snapshot;
    uint32_t length;
    uint32_t crc32;
    uint32_t state;
    uint32_t writer;
    uint32_t readers;
};

struct spz_maps_read {
    const uint8_t *data;
    uint64_t snapshot;
    uint32_t offset;
    uint32_t total;
    uint32_t length;
    uint32_t crc32;
    uint8_t eof;
};

struct spz_maps_info {
    enum spz_maps_state state;
    uint64_t task_generation;
    uint64_t snapshot;
    uint32_t length;
    uint32_t crc32;
};

int spz_maps_init(struct spz_maps *maps, const struct spz_maps_backend *backend);
int spz_maps_supported(const struct spz_maps *maps);
int spz_maps_has_task(const struct spz_maps *maps);
int spz_maps_capture_task(struct spz_maps *maps, const void *task,
                          uint64_t generation);
int spz_maps_snapshot(struct spz_maps *maps, uint64_t generation,
                      uint64_t snapshot);
int spz_maps_read_begin(struct spz_maps *maps, uint64_t snapshot,
                        uint32_t offset, struct spz_maps_read *read);
void spz_maps_read_end(struct spz_maps *maps);
int spz_maps_info(const struct spz_maps *maps, struct spz_maps_info *info);
int spz_maps_clear(struct spz_maps *maps);

#endif
