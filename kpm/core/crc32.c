#include "stackplz/platform.h"

#include "stackplz/core.h"

uint32_t spz_crc32_ieee(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    size_t index;

    if (data == NULL && length != 0U)
        return 0U;
    for (index = 0U; index < length; index++) {
        unsigned int bit;

        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; bit++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));

            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}
