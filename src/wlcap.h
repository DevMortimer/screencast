#pragma once

/*
 * Wayland screen capture via the wlr-screencopy-unstable-v1 protocol.
 *
 * Replaces the old X11/x11grab screen source.  Produces raw 32-bit frames in a
 * client wl_shm buffer; the pixel format is reported as an AVPixelFormat value
 * (see wlcap.c) so the encoder's swscale stage can consume it directly.
 *
 * Kept free of FFmpeg headers on purpose: capture.c bridges the raw buffer into
 * an AVFrame.
 */

typedef struct WlCap WlCap;

typedef struct {
    int width;       /* output width in pixels                       */
    int height;      /* output height in pixels                      */
    int stride;      /* bytes per row of the shm buffer              */
    int av_pix_fmt;  /* enum AVPixelFormat value of the shm buffer   */
    int y_invert;    /* 1 if compositor reports the frame y-inverted */
} WlCapInfo;

/*
 * Connect to the Wayland display, pick an output (by name, else the first),
 * and perform one capture to learn the geometry/format.  overlay_cursor != 0
 * composites the cursor into the frame.  fps caps the grab rate.
 * Fills *info and returns an opaque handle, or NULL on failure.
 */
WlCap *wlcap_open(const char *output_name, int overlay_cursor, int fps,
                  WlCapInfo *info);

/* Pointer to the mapped pixel buffer.  Stable for the handle's lifetime. */
void *wlcap_buffer(WlCap *c);

/* Grab one frame into the shm buffer (paced to fps).  0 on success, -1 fail. */
int wlcap_grab(WlCap *c);

void wlcap_close(WlCap *c);
