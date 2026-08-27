// overlay_geometry.h — pure presenter-window resize geometry
#ifndef OVERLAY_GEOMETRY_H
#define OVERLAY_GEOMETRY_H

typedef enum {
    OVERLAY_RESIZE_LEFT   = 1 << 0,
    OVERLAY_RESIZE_RIGHT  = 1 << 1,
    OVERLAY_RESIZE_BOTTOM = 1 << 2,
    OVERLAY_RESIZE_TOP    = 1 << 3,
} OverlayResizeEdges;

/* Cocoa window coordinates grow upward.  Therefore dragging the top edge up
   grows height, while dragging the bottom edge down grows height. */
static inline double overlay_resize_width(double start_width, double aspect,
                                          OverlayResizeEdges edges,
                                          double dx, double dy)
{
    double horizontal = start_width;
    if (edges & OVERLAY_RESIZE_RIGHT) horizontal = start_width + dx;
    if (edges & OVERLAY_RESIZE_LEFT)  horizontal = start_width - dx;

    double vertical = start_width;
    if (edges & OVERLAY_RESIZE_TOP)    vertical = start_width + dy * aspect;
    if (edges & OVERLAY_RESIZE_BOTTOM) vertical = start_width - dy * aspect;

    if ((edges & (OVERLAY_RESIZE_LEFT | OVERLAY_RESIZE_RIGHT)) &&
        (edges & (OVERLAY_RESIZE_TOP | OVERLAY_RESIZE_BOTTOM)))
        return (dx >= 0 ? dx : -dx) >=
               ((dy >= 0 ? dy : -dy) * aspect) ? horizontal : vertical;
    if (edges & (OVERLAY_RESIZE_LEFT | OVERLAY_RESIZE_RIGHT)) return horizontal;
    return vertical;
}

#endif
