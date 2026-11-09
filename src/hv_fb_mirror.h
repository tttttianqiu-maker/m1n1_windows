/* SPDX-License-Identifier: MIT */

#ifndef HV_FB_MIRROR_H
#define HV_FB_MIRROR_H

/*
 * Copies the guest's virtual framebuffer (XRGB8888 in guest RAM) into the
 * boot framebuffer that iBoot left scanning out on the internal panel, a few
 * rows per hypervisor tick. No DCP calls: the display controller keeps
 * showing the same boot surface (the M3 DCP firmware rejects a new guest
 * surface, EXP-20261105-006), only its pixels change.
 *
 * The boot framebuffer is either XRGB8888 (depth 32) or x2r10g10b10 (depth
 * 30, what iBoot uses on Apple Silicon panels; see fb.c rgb2pixel_30()).
 */

#ifdef HV_FB_MIRROR_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include "types.h"
#endif

#define HV_FB_MIRROR_MAX_ROWS_PER_TICK 64u
#define HV_FB_MIRROR_DEFAULT_ROWS      4u

/* Called on each source row before it is read (cache maintenance); may be NULL. */
typedef void (*hv_fb_mirror_sync_fn)(void *addr, size_t length);

struct hv_fb_mirror {
    bool enabled;
    u64 src;
    u32 src_stride;
    u64 dst;
    u32 dst_stride;
    u32 width;
    u32 height;
    bool dst_30bit;
    u32 rows_per_tick;
    u32 next_row;
    u64 frames;
    hv_fb_mirror_sync_fn sync_src;
};

static inline u32 hv_fb_mirror_pixel30(u32 xrgb)
{
    u32 r = (xrgb >> 16) & 0xff, g = (xrgb >> 8) & 0xff, b = xrgb & 0xff;

    /* 8 -> 10 bits with the top bits replicated, so white stays full white */
    r = (r << 2) | (r >> 6);
    g = (g << 2) | (g >> 6);
    b = (b << 2) | (b >> 6);
    return (r << 20) | (g << 10) | b;
}

/*
 * src/dst are addresses the caller can dereference (identity-mapped PAs in
 * m1n1). The copied area is the overlap of both geometries, top-left aligned.
 * Returns false (and leaves the mirror disabled) on any inconsistent input.
 */
bool hv_fb_mirror_configure(struct hv_fb_mirror *m, u64 src, u32 src_width, u32 src_height,
                            u32 src_stride, u64 dst, u32 dst_width, u32 dst_height,
                            u32 dst_stride, u32 dst_depth, u32 rows_per_tick,
                            hv_fb_mirror_sync_fn sync_src);
void hv_fb_mirror_disable(struct hv_fb_mirror *m);
void hv_fb_mirror_tick(struct hv_fb_mirror *m);

#endif
