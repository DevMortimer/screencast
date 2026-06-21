#pragma once
#include <stdatomic.h>

typedef enum {
    MODE_IDLE    = 0,
    MODE_DISPLAY = 1,
    MODE_WEBCAM  = 2,
    MODE_BOTH    = 3,
} RecordMode;

/* Updated atomically by the hotkey thread; read by the record loop. */
extern atomic_int g_mode;       /* RecordMode */
extern atomic_int g_recording;  /* 1 = encode loop is active */
extern atomic_int g_running;    /* 0 = exit requested (Win+Esc) */

int  hotkeys_init(void);
void hotkeys_run(void);         /* returns when g_running → 0 */
void hotkeys_cleanup(void);
