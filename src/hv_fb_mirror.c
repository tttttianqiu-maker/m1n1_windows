/* SPDX-License-Identifier: MIT */

#include "hv_fb_mirror.h"

#ifdef HV_FB_MIRROR_HOST_TEST
#include <string.h>
#else
#include "string.h"
#endif

static bool range_ok(u64 base, u32 stride, u32 rows, u64 *end)
{
    u64 size = (u64)stride * rows;

    if (!base || base > ~0ULL - size)
        return false;
    *end = base + size;
    return true;
}

bool hv_fb_mirror_configure(struct hv_fb_mirror *m, u64 src, u32 src_width, u32 src_height,
                            u32 src_stride, u64 dst, u32 dst_width, u32 dst_height,
                            u32 dst_stride, u32 dst_depth, u32 rows_per_tick,
                            hv_fb_mirror_sync_fn sync_src)
{
    u64 src_end, dst_end;
    u32 depth = dst_depth & 0xff;

    if (!m)
        return false;
    memset(m, 0, sizeof(*m));

    if (!src_width || !src_height || !dst_width || !dst_height)
        return false;
    if (src_width > 0xffff || src_height > 0xffff || dst_width > 0xffff || dst_height > 0xffff)
        return false;
    if ((src_stride & 3) || (dst_stride & 3) || (src & 3) || (dst & 3))
        return false;
    if (src_stride < src_width * 4u || dst_stride < dst_width * 4u)
        return false;
    if (depth != 30 && depth != 32)
        return false;
    if (!range_ok(src, src_stride, src_height, &src_end) ||
        !range_ok(dst, dst_stride, dst_height, &dst_end))
        return false;
    if (src < dst_end && dst < src_end)
        return false; /* the two surfaces must not overlap */

    if (!rows_per_tick)
        rows_per_tick = HV_FB_MIRROR_DEFAULT_ROWS;
    if (rows_per_tick > HV_FB_MIRROR_MAX_ROWS_PER_TICK)
        rows_per_tick = HV_FB_MIRROR_MAX_ROWS_PER_TICK;

    m->src = src;
    m->src_stride = src_stride;
    m->dst = dst;
    m->dst_stride = dst_stride;
    m->width = src_width < dst_width ? src_width : dst_width;
    m->height = src_height < dst_height ? src_height : dst_height;
    m->dst_30bit = depth == 30;
    m->rows_per_tick = rows_per_tick > m->height ? m->height : rows_per_tick;
    m->sync_src = sync_src;
    m->enabled = true;
    return true;
}

void hv_fb_mirror_disable(struct hv_fb_mirror *m)
{
    if (m)
        m->enabled = false;
}

void hv_fb_mirror_tick(struct hv_fb_mirror *m)
{
    if (!m || !m->enabled)
        return;

    for (u32 n = 0; n < m->rows_per_tick; n++) {
        u32 y = m->next_row;
        const u32 *s = (const u32 *)(uintptr_t)(m->src + (u64)y * m->src_stride);
        u32 *d = (u32 *)(uintptr_t)(m->dst + (u64)y * m->dst_stride);

        /* The guest writes its framebuffer through a non-cacheable mapping. */
        if (m->sync_src)
            m->sync_src((void *)s, (size_t)m->width * 4u);
        if (m->dst_30bit) {
            for (u32 x = 0; x < m->width; x++)
                d[x] = hv_fb_mirror_pixel30(s[x]);
        } else {
            memcpy(d, s, (size_t)m->width * 4u);
        }

        if (++m->next_row >= m->height) {
            m->next_row = 0;
            m->frames++;
        }
    }
}
