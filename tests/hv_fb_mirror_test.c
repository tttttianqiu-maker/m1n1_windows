/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/hv_fb_mirror.h"

static unsigned sync_calls;
static size_t sync_bytes;

static void count_sync(void *addr, size_t len)
{
    (void)addr;
    sync_calls++;
    sync_bytes += len;
}

static void test_pixel30(void)
{
    assert(hv_fb_mirror_pixel30(0x00000000u) == 0x00000000u);
    assert(hv_fb_mirror_pixel30(0x00ffffffu) == 0x3fffffffu); /* white stays full scale */
    assert(hv_fb_mirror_pixel30(0xffffffffu) == 0x3fffffffu); /* X byte ignored */
    assert(hv_fb_mirror_pixel30(0x00ff0000u) == (0x3ffu << 20));
    assert(hv_fb_mirror_pixel30(0x0000ff00u) == (0x3ffu << 10));
    assert(hv_fb_mirror_pixel30(0x000000ffu) == 0x3ffu);
    /* 0x80 -> 0x202: same bit positions as fb.c rgb2pixel_30 plus replication */
    assert(hv_fb_mirror_pixel30(0x00808080u) == ((0x202u << 20) | (0x202u << 10) | 0x202u));
}

static void test_rejects_bad_input(void)
{
    static uint32_t a[64], b[64];
    struct hv_fb_mirror m;
    uint64_t pa = (uint64_t)(uintptr_t)a, pb = (uint64_t)(uintptr_t)b;

    assert(!hv_fb_mirror_configure(&m, 0, 4, 4, 16, pb, 4, 4, 16, 30, 1, NULL));
    assert(!m.enabled);
    assert(!hv_fb_mirror_configure(&m, pa, 4, 4, 16, 0, 4, 4, 16, 30, 1, NULL));
    assert(!hv_fb_mirror_configure(&m, pa, 0, 4, 16, pb, 4, 4, 16, 30, 1, NULL));
    assert(!hv_fb_mirror_configure(&m, pa, 4, 4, 12, pb, 4, 4, 16, 30, 1, NULL)); /* stride < w*4 */
    assert(!hv_fb_mirror_configure(&m, pa, 4, 4, 18, pb, 4, 4, 16, 30, 1, NULL)); /* unaligned */
    assert(!hv_fb_mirror_configure(&m, pa, 4, 4, 16, pb, 4, 4, 16, 24, 1, NULL)); /* depth */
    /* overlapping surfaces */
    assert(!hv_fb_mirror_configure(&m, pa, 4, 4, 16, pa + 32, 4, 4, 16, 30, 1, NULL));
    /* wrap-around */
    assert(!hv_fb_mirror_configure(&m, UINT64_MAX - 15, 4, 4, 16, pb, 4, 4, 16, 30, 1, NULL));
    /* depth flags above bit 8 (retina flag) are ignored */
    assert(hv_fb_mirror_configure(&m, pa, 4, 4, 16, pb, 4, 4, 16, 0x1001e, 1, NULL));
    assert(m.enabled && m.dst_30bit);
}

static void test_copies_overlap_region_and_wraps(void)
{
    /* source 3x3 (stride 4 px), destination 2x4 (stride 3 px): copy 2x3 */
    uint32_t src[3 * 4], dst[4 * 3];
    struct hv_fb_mirror m;

    for (unsigned i = 0; i < 12; i++)
        src[i] = 0x00010203u * (i + 1);
    memset(dst, 0xee, sizeof(dst));
    sync_calls = 0;
    sync_bytes = 0;
    assert(hv_fb_mirror_configure(&m, (uint64_t)(uintptr_t)src, 3, 3, 16,
                                  (uint64_t)(uintptr_t)dst, 2, 4, 12, 30, 2, count_sync));
    assert(m.width == 2 && m.height == 3 && m.rows_per_tick == 2);

    hv_fb_mirror_tick(&m); /* rows 0, 1 */
    hv_fb_mirror_tick(&m); /* row 2, then row 0 again (wrap) */
    assert(m.frames == 1 && m.next_row == 1);
    assert(sync_calls == 4 && sync_bytes == 4 * 2 * 4);
    for (unsigned y = 0; y < 3; y++) {
        for (unsigned x = 0; x < 2; x++)
            assert(dst[y * 3 + x] == hv_fb_mirror_pixel30(src[y * 4 + x]));
        assert(dst[y * 3 + 2] == 0xeeeeeeeeu); /* destination padding untouched */
    }
    for (unsigned x = 0; x < 3; x++)
        assert(dst[3 * 3 + x] == 0xeeeeeeeeu); /* row beyond the source untouched */

    hv_fb_mirror_disable(&m);
    memset(dst, 0, sizeof(dst));
    hv_fb_mirror_tick(&m);
    for (unsigned i = 0; i < 12; i++)
        assert(dst[i] == 0);
}

static void test_32bit_destination_is_plain_copy(void)
{
    uint32_t src[4] = {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u}, dst[4] = {0};
    struct hv_fb_mirror m;

    assert(hv_fb_mirror_configure(&m, (uint64_t)(uintptr_t)src, 2, 2, 8,
                                  (uint64_t)(uintptr_t)dst, 2, 2, 8, 32, 0, NULL));
    assert(m.rows_per_tick == 2); /* default 4, clamped to the height */
    hv_fb_mirror_tick(&m);
    assert(!memcmp(src, dst, sizeof(src)));
}

static void test_rows_per_tick_clamp(void)
{
    static uint32_t src[200], dst[200];
    struct hv_fb_mirror m;

    assert(hv_fb_mirror_configure(&m, (uint64_t)(uintptr_t)src, 1, 200, 4,
                                  (uint64_t)(uintptr_t)dst, 1, 200, 4, 30, 1000, NULL));
    assert(m.rows_per_tick == HV_FB_MIRROR_MAX_ROWS_PER_TICK);
}

int main(void)
{
    test_pixel30();
    test_rejects_bad_input();
    test_copies_overlap_region_and_wraps();
    test_32bit_destination_is_plain_copy();
    test_rows_per_tick_clamp();
    puts("hv_fb_mirror_test: ok");
    return 0;
}
