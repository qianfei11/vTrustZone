/*
 * QEMU TZC-400 tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define TZC400_BASE                 0x090c0000ULL
#define TZC_BUILD_CONFIG            0x000
#define TZC_ACTION                  0x004
#define TZC_GATE_KEEPER             0x008
#define TZC_REGION_BASE_LOW(n)      (0x100 + ((n) * 0x20))
#define TZC_REGION_BASE_HIGH(n)     (0x104 + ((n) * 0x20))
#define TZC_REGION_TOP_LOW(n)       (0x108 + ((n) * 0x20))
#define TZC_REGION_TOP_HIGH(n)      (0x10c + ((n) * 0x20))
#define TZC_REGION_ATTRIBUTES(n)    (0x110 + ((n) * 0x20))
#define TZC_REGION_ID_ACCESS(n)     (0x114 + ((n) * 0x20))
#define TZC_CID0                    0xff0
#define TZC_CID1                    0xff4
#define TZC_CID2                    0xff8
#define TZC_CID3                    0xffc
#define QEMU_TZC400_REGIONS         9
#define QEMU_TZC400_FILTERS         1
#define QEMU_TZC400_ADDR_BITS       40

static uint32_t tzc_readl(uint64_t offset)
{
    return qtest_readl(global_qtest, TZC400_BASE + offset);
}

static void tzc_writel(uint64_t offset, uint32_t value)
{
    qtest_writel(global_qtest, TZC400_BASE + offset, value);
}

static void test_tzc400_ids(void)
{
    uint32_t cid = tzc_readl(TZC_CID0) |
                   (tzc_readl(TZC_CID1) << 8) |
                   (tzc_readl(TZC_CID2) << 16) |
                   (tzc_readl(TZC_CID3) << 24);

    g_assert_cmphex(cid, ==, 0xb105f00d);
    g_assert_cmphex(tzc_readl(TZC_BUILD_CONFIG), ==,
                    ((QEMU_TZC400_FILTERS - 1) << 24) |
                    ((QEMU_TZC400_ADDR_BITS - 1) << 8) |
                    (QEMU_TZC400_REGIONS - 1));
    g_assert_cmphex(tzc_readl(TZC_GATE_KEEPER), ==, 0);
}

static void test_tzc400_region_programming(void)
{
    tzc_writel(TZC_ACTION, 1);
    tzc_writel(TZC_REGION_BASE_LOW(1), 0x40000000);
    tzc_writel(TZC_REGION_BASE_HIGH(1), 0);
    tzc_writel(TZC_REGION_TOP_LOW(1), 0x4000ffff);
    tzc_writel(TZC_REGION_TOP_HIGH(1), 0);
    tzc_writel(TZC_REGION_ATTRIBUTES(1), (3u << 30) | 1u);
    tzc_writel(TZC_REGION_ID_ACCESS(1), (1u << 1) | (1u << (16 + 1)));

    g_assert_cmphex(tzc_readl(TZC_REGION_ATTRIBUTES(1)), ==, (3u << 30) | 1u);
    g_assert_cmphex(tzc_readl(TZC_REGION_ID_ACCESS(1)), ==,
                    (1u << 1) | (1u << (16 + 1)));
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_start("-machine virt,secure=on,tzc400=on -accel tcg "
                "-cpu max -smp 2 -m 512M");
    qtest_add_func("/tzc400/ids", test_tzc400_ids);
    qtest_add_func("/tzc400/region-programming", test_tzc400_region_programming);
    ret = g_test_run();
    qtest_end();

    return ret;
}
