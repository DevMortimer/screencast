#pragma once
#include <stdatomic.h>

typedef enum {
    MODE_IDLE    = 0,
    MODE_DISPLAY = 1,
    MODE_WEBCAM  = 2,
    MODE_BOTH    = 3,
} RecordMode;

/* Updated by the control listener thread; read by the record loop. */
extern atomic_int g_mode;       /* RecordMode */
extern atomic_int g_recording;  /* 1 = encode loop is active */
extern atomic_int g_running;    /* 0 = exit requested (stop) */

/* Human-readable label for a mode, or NULL for MODE_IDLE. */
const char *control_mode_label(RecordMode mode);

/* Map a command word to a RecordMode, or -1 if it is not a record mode. */
int  control_parse_mode(const char *cmd);

/* 1 if the command word means "stop and exit". */
int  control_is_stop(const char *cmd);

/*
 * Try to hand `cmd` to an already-running screencast daemon over its control
 * socket.  Returns 0 if delivered, -1 if no daemon is listening.
 */
int  control_client_send(const char *cmd);

/*
 * Become the daemon: bind the control socket and spawn the listener thread.
 * Subsequent invocations reach this process via control_client_send().
 * Returns 0 on success, -1 on failure.
 */
int  control_server_start(void);
void control_server_stop(void);

/* Fire-and-wait desktop notification via notify-send. */
void control_notify(const char *summary, const char *body);
