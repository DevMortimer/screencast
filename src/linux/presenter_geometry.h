#pragma once
#include <stdlib.h>

typedef enum {
    PRESENTER_TOP_LEFT = 0,
    PRESENTER_TOP_RIGHT,
    PRESENTER_BOTTOM_LEFT,
    PRESENTER_BOTTOM_RIGHT,
} PresenterCorner;

typedef enum {
    PRESENTER_EDGE_NONE   = 0,
    PRESENTER_EDGE_TOP    = 1 << 0,
    PRESENTER_EDGE_RIGHT  = 1 << 1,
    PRESENTER_EDGE_BOTTOM = 1 << 2,
    PRESENTER_EDGE_LEFT   = 1 << 3,
} PresenterEdges;

static inline int presenter_corner_is_right(PresenterCorner corner)
{
    return corner == PRESENTER_TOP_RIGHT || corner == PRESENTER_BOTTOM_RIGHT;
}

static inline int presenter_corner_is_bottom(PresenterCorner corner)
{
    return corner == PRESENTER_BOTTOM_LEFT || corner == PRESENTER_BOTTOM_RIGHT;
}

static inline PresenterCorner presenter_corner_make(int right, int bottom)
{
    if (bottom)
        return right ? PRESENTER_BOTTOM_RIGHT : PRESENTER_BOTTOM_LEFT;
    return right ? PRESENTER_TOP_RIGHT : PRESENTER_TOP_LEFT;
}

/* A move gesture changes only the axes whose drag exceeds the threshold. */
static inline PresenterCorner presenter_corner_after_drag(
    PresenterCorner start, int dx, int dy, int threshold)
{
    int right = presenter_corner_is_right(start);
    int bottom = presenter_corner_is_bottom(start);

    if (dx >= threshold) right = 1;
    if (dx <= -threshold) right = 0;
    if (dy >= threshold) bottom = 1;
    if (dy <= -threshold) bottom = 0;
    return presenter_corner_make(right, bottom);
}

/* Keep the presenter square.  At a corner, the dominant axis controls size. */
static inline int presenter_resize_size(int start_size, unsigned edges,
                                        int dx, int dy,
                                        int min_size, int max_size)
{
    int horizontal = 0;
    int vertical = 0;
    int has_horizontal = 0;
    int has_vertical = 0;

    if (edges & PRESENTER_EDGE_LEFT) {
        horizontal = -dx;
        has_horizontal = 1;
    } else if (edges & PRESENTER_EDGE_RIGHT) {
        horizontal = dx;
        has_horizontal = 1;
    }

    if (edges & PRESENTER_EDGE_TOP) {
        vertical = -dy;
        has_vertical = 1;
    } else if (edges & PRESENTER_EDGE_BOTTOM) {
        vertical = dy;
        has_vertical = 1;
    }

    int delta = has_horizontal ? horizontal : vertical;
    if (has_horizontal && has_vertical && abs(vertical) > abs(horizontal))
        delta = vertical;

    int size = start_size + delta;
    if (size < min_size) size = min_size;
    if (size > max_size) size = max_size;
    return size;
}
