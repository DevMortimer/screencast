#include "composite.h"
#include <math.h>
#include <string.h>

/*
 * Rounded-corner mask builder.
 *
 * For each pixel we check whether it falls inside one of the four corner
 * squares (each radius×radius pixels).  Inside a corner square, we compute
 * the Euclidean distance from that corner's "arc centre" and map it to an
 * alpha value:
 *
 *   dist ≤ radius-1  →  alpha 1.0  (fully opaque)
 *   dist ≥ radius    →  alpha 0.0  (fully transparent)
 *   in between       →  linear blend  (1-pixel anti-alias seam)
 */
void composite_build_mask(float *mask, int w, int h, int radius)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float alpha = 1.0f;

            /* Identify which corner region this pixel falls into */
            int cx = -1, cy = -1;
            if (x < radius && y < radius) {
                cx = radius;  cy = radius;          /* top-left */
            } else if (x >= w - radius && y < radius) {
                cx = w - 1 - radius; cy = radius;   /* top-right */
            } else if (x < radius && y >= h - radius) {
                cx = radius; cy = h - 1 - radius;   /* bottom-left */
            } else if (x >= w - radius && y >= h - radius) {
                cx = w - 1 - radius; cy = h - 1 - radius; /* bottom-right */
            }

            if (cx >= 0) {
                float dist = sqrtf((float)(x - cx) * (x - cx) +
                                   (float)(y - cy) * (y - cy));
                /* Smooth step over the 1-pixel seam */
                alpha = (float)(radius) - dist;
                if (alpha < 0.0f) alpha = 0.0f;
                if (alpha > 1.0f) alpha = 1.0f;
            }

            mask[y * w + x] = alpha;
        }
    }
}

/*
 * Per-pixel RGBA alpha blend using src_alpha × mask_alpha.
 *
 * out_R = src_R * a + dst_R * (1 - a)   where a = src.alpha/255 * mask
 *
 * The destination alpha is forced to 255 (fully opaque) after blending,
 * which is what we need for the final YUV conversion.
 */
void composite_blend(
    uint8_t       *dst, int dst_w, int dst_h,
    const uint8_t *src, int src_w, int src_h,
    int ox, int oy,
    const float *mask)
{
    for (int sy = 0; sy < src_h; sy++) {
        int dy = oy + sy;
        if (dy < 0 || dy >= dst_h) continue;

        for (int sx = 0; sx < src_w; sx++) {
            int dx = ox + sx;
            if (dx < 0 || dx >= dst_w) continue;

            int si = (sy * src_w + sx) * 4;
            int di = (dy * dst_w + dx) * 4;

            float mask_a = mask ? mask[sy * src_w + sx] : 1.0f;
            float src_a  = (src[si + 3] / 255.0f) * mask_a;
            float dst_a  = 1.0f - src_a;

            dst[di + 0] = (uint8_t)(src[si + 0] * src_a + dst[di + 0] * dst_a);
            dst[di + 1] = (uint8_t)(src[si + 1] * src_a + dst[di + 1] * dst_a);
            dst[di + 2] = (uint8_t)(src[si + 2] * src_a + dst[di + 2] * dst_a);
            dst[di + 3] = 255;
        }
    }
}
