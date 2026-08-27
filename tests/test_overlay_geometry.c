#include "overlay_geometry.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void assert_close(double actual, double expected)
{
    assert(fabs(actual - expected) < 0.0001);
}

int main(void)
{
    const double width = 400.0;
    const double aspect = 1.0;

    /* Cocoa Y grows upward: top dragged up and bottom dragged down both grow. */
    assert_close(overlay_resize_width(width, aspect, OVERLAY_RESIZE_TOP,
                                      0.0, 50.0), width + 50.0);
    assert_close(overlay_resize_width(width, aspect, OVERLAY_RESIZE_BOTTOM,
                                      0.0, -50.0), width + 50.0);
    assert_close(overlay_resize_width(width, aspect, OVERLAY_RESIZE_TOP,
                                      0.0, -50.0), width - 50.0);
    assert_close(overlay_resize_width(width, aspect, OVERLAY_RESIZE_BOTTOM,
                                      0.0, 50.0), width - 50.0);

    assert_close(overlay_resize_width(width, aspect, OVERLAY_RESIZE_RIGHT,
                                      50.0, 0.0), width + 50.0);
    assert_close(overlay_resize_width(width, aspect, OVERLAY_RESIZE_LEFT,
                                      -50.0, 0.0), width + 50.0);
    /* Every resize direction changes one square side by the same amount. */
    assert_close(overlay_resize_width(width, aspect,
                                      OVERLAY_RESIZE_RIGHT | OVERLAY_RESIZE_TOP,
                                      30.0, 30.0), width + 30.0);
    assert_close(overlay_resize_width(width, aspect,
                                      OVERLAY_RESIZE_LEFT | OVERLAY_RESIZE_BOTTOM,
                                      -30.0, -30.0), width + 30.0);
    puts("overlay geometry signs and square aspect ratio passed");
    return 0;
}
