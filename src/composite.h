#pragma once
#include <stdint.h>

/*
 * Populate a float alpha mask for an (w × h) image with rounded corners.
 * Corner pixels at distance > radius from the corner center get alpha 0.
 * A 1-pixel anti-alias transition band is applied at the boundary.
 * mask must hold w * h floats (caller allocates).
 */
void composite_build_mask(float *mask, int w, int h, int radius);

/*
 * Alpha-blend src RGBA (src_w × src_h) onto dst RGBA (dst_w × dst_h)
 * at pixel offset (ox, oy). mask[i] ∈ [0,1] scales src pixel alpha.
 * Pass mask=NULL for full alpha (no corner rounding).
 */
void composite_blend(
    uint8_t       *dst, int dst_w, int dst_h,
    const uint8_t *src, int src_w, int src_h,
    int ox, int oy,
    const float *mask);
