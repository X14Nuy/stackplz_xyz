#include <errno.h>
#include <stdint.h>

#include "../platform/arm64/debug_regs.h"
#include "stackplz/debug.h"
#include "test.h"

static void expect_controls(void)
{
    struct spz_debug_request request;
    uint64_t value;
    uint32_t control;
    uint8_t bas;

    request.id = 1U;
    request.kind = SPZ_BREAK_EXECUTE;
    request.address = UINT64_C(0x1000);
    request.length = 4U;
    request.mode = SPZ_BREAK_ONCE;
    SPZ_EXPECT_EQ(spz_debug_validate_request(&request, &value, &control, &bas), 0);
    SPZ_EXPECT_EQ(value, UINT64_C(0x1000));
    SPZ_EXPECT_EQ(bas, UINT8_C(0x0f));
    SPZ_EXPECT_EQ(control, UINT32_C(0x1e5));

    request.kind = SPZ_BREAK_READ_WRITE;
    request.address = UINT64_C(0x1003);
    request.length = 2U;
    SPZ_EXPECT_EQ(spz_debug_validate_request(&request, &value, &control, &bas), 0);
    SPZ_EXPECT_EQ(value, UINT64_C(0x1000));
    SPZ_EXPECT_EQ(bas, UINT8_C(0x18));
    SPZ_EXPECT_EQ(control, UINT32_C(0x31d));
    SPZ_EXPECT_EQ(control & (UINT32_C(3) << 3U), UINT32_C(3) << 3U);
}

static void expect_dfr0_counts(void)
{
    const uint64_t dfr0 = UINT64_C(0x000011f010305719);

    SPZ_EXPECT_EQ(spz_arm64_num_brps_from_dfr0(dfr0), 6U);
    SPZ_EXPECT_EQ(spz_arm64_num_wrps_from_dfr0(dfr0), 4U);
}

static void expect_tcr_page_granule(void)
{
    SPZ_EXPECT_EQ(spz_arm64_page_size_from_tcr(UINT64_C(1) << 30U), 16384U);
    SPZ_EXPECT_EQ(spz_arm64_page_size_from_tcr(UINT64_C(2) << 30U), 4096U);
    SPZ_EXPECT_EQ(spz_arm64_page_size_from_tcr(UINT64_C(3) << 30U), 65536U);
    SPZ_EXPECT_EQ(spz_arm64_page_size_from_tcr(0U), 0U);
}

static void expect_index_bounds(void)
{
#if !defined(__aarch64__)
    uint64_t value64;
    uint32_t value32;

    SPZ_EXPECT_EQ(spz_arm64_read_bvr(16U, &value64), -ERANGE);
    SPZ_EXPECT_EQ(spz_arm64_write_bvr(16U, 0U), -ERANGE);
    SPZ_EXPECT_EQ(spz_arm64_read_bcr(16U, &value32), -ERANGE);
    SPZ_EXPECT_EQ(spz_arm64_write_bcr(16U, 0U), -ERANGE);
    SPZ_EXPECT_EQ(spz_arm64_read_wvr(16U, &value64), -ERANGE);
    SPZ_EXPECT_EQ(spz_arm64_write_wvr(16U, 0U), -ERANGE);
    SPZ_EXPECT_EQ(spz_arm64_read_wcr(16U, &value32), -ERANGE);
    SPZ_EXPECT_EQ(spz_arm64_write_wcr(16U, 0U), -ERANGE);
    SPZ_EXPECT_EQ(spz_arm64_read_bvr(0U, &value64), -EOPNOTSUPP);
#endif
}

#if defined(__aarch64__)
int spz_arm64_compile_probe(void)
{
    uint64_t value64 = 0U;
    uint32_t value32 = 0U;
    int result = 0;

    result |= spz_arm64_read_bvr(0U, &value64);
    result |= spz_arm64_read_bvr(5U, &value64);
    result |= spz_arm64_read_bvr(15U, &value64);
    result |= spz_arm64_write_bvr(0U, value64);
    result |= spz_arm64_write_bvr(5U, value64);
    result |= spz_arm64_write_bvr(15U, value64);
    result |= spz_arm64_read_bcr(0U, &value32);
    result |= spz_arm64_read_bcr(5U, &value32);
    result |= spz_arm64_read_bcr(15U, &value32);
    result |= spz_arm64_write_bcr(0U, value32);
    result |= spz_arm64_write_bcr(5U, value32);
    result |= spz_arm64_write_bcr(15U, value32);
    result |= spz_arm64_read_wvr(0U, &value64);
    result |= spz_arm64_read_wvr(5U, &value64);
    result |= spz_arm64_read_wvr(15U, &value64);
    result |= spz_arm64_write_wvr(0U, value64);
    result |= spz_arm64_write_wvr(5U, value64);
    result |= spz_arm64_write_wvr(15U, value64);
    result |= spz_arm64_read_wcr(0U, &value32);
    result |= spz_arm64_read_wcr(5U, &value32);
    result |= spz_arm64_read_wcr(15U, &value32);
    result |= spz_arm64_write_wcr(0U, value32);
    result |= spz_arm64_write_wcr(5U, value32);
    result |= spz_arm64_write_wcr(15U, value32);
    return result;
}
#endif

int test_arm64_encoding(void)
{
    expect_controls();
    expect_dfr0_counts();
    expect_tcr_page_granule();
    expect_index_bounds();
    return 0;
}
