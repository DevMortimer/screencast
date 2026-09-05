#include <stdio.h>
#include "presenter_geometry.h"

static int failures;

#define CHECK(actual, expected, message) do {                              \
    int got_ = (actual);                                                   \
    int want_ = (expected);                                                \
    if (got_ != want_) {                                                   \
        fprintf(stderr, "FAIL: %s (got %d, want %d)\n",                  \
                message, got_, want_);                                     \
        failures++;                                                        \
    }                                                                      \
} while (0)

static void test_corner_drag(void)
{
    CHECK(presenter_corner_after_drag(PRESENTER_TOP_LEFT, 40, 40, 60),
          PRESENTER_TOP_LEFT, "small drag keeps corner");
    CHECK(presenter_corner_after_drag(PRESENTER_TOP_LEFT, 80, 0, 60),
          PRESENTER_TOP_RIGHT, "right drag selects right corner");
    CHECK(presenter_corner_after_drag(PRESENTER_TOP_RIGHT, -80, 0, 60),
          PRESENTER_TOP_LEFT, "left drag selects left corner");
    CHECK(presenter_corner_after_drag(PRESENTER_TOP_LEFT, 0, 80, 60),
          PRESENTER_BOTTOM_LEFT, "down drag selects bottom corner");
    CHECK(presenter_corner_after_drag(PRESENTER_BOTTOM_RIGHT, -80, -80, 60),
          PRESENTER_TOP_LEFT, "diagonal drag selects opposite corner");
}

static void test_square_resize(void)
{
    CHECK(presenter_resize_size(360, PRESENTER_EDGE_RIGHT, 50, 0, 160, 720),
          410, "right edge grows to the right");
    CHECK(presenter_resize_size(360, PRESENTER_EDGE_LEFT, 50, 0, 160, 720),
          310, "left edge shrinks when dragged right");
    CHECK(presenter_resize_size(360, PRESENTER_EDGE_TOP, 0, -50, 160, 720),
          410, "top edge grows upward");
    CHECK(presenter_resize_size(360, PRESENTER_EDGE_BOTTOM, 0, 50, 160, 720),
          410, "bottom edge grows downward");
    CHECK(presenter_resize_size(360,
                                PRESENTER_EDGE_RIGHT | PRESENTER_EDGE_BOTTOM,
                                25, 70, 160, 720),
          430, "corner uses the dominant drag axis");
    CHECK(presenter_resize_size(180, PRESENTER_EDGE_LEFT, 80, 0, 160, 720),
          160, "resize clamps to minimum");
    CHECK(presenter_resize_size(700, PRESENTER_EDGE_RIGHT, 80, 0, 160, 720),
          720, "resize clamps to maximum");
}

int main(void)
{
    test_corner_drag();
    test_square_resize();

    if (failures) {
        fprintf(stderr, "presenter geometry: %d checks failed\n", failures);
        return 1;
    }
    puts("presenter geometry: all checks passed");
    return 0;
}
