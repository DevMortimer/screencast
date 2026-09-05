#include <assert.h>
#include <stdio.h>
#include "control.h"

int main(void)
{
    assert(control_parse_mode("display") == MODE_DISPLAY);
    assert(control_parse_mode("webcam") == MODE_WEBCAM);
    assert(control_parse_mode("both") == MODE_BOTH);
#ifndef __APPLE__
    assert(control_parse_mode("presenter") == MODE_PRESENTER);
    assert(control_parse_mode("p") == MODE_PRESENTER);
    assert(control_mode_label(MODE_PRESENTER) != NULL);
#endif
    assert(control_parse_mode("not-a-mode") == -1);
    puts("control modes passed");
    return 0;
}
