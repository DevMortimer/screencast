#pragma once
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

/*
 * Webcam capture as a PipeWire client.
 *
 * Replaces the old raw-V4L2 open (exclusive `/dev/videoN`).  By consuming the
 * camera through PipeWire, the physical sensor stays owned by PipeWire and is
 * fanned out to every consumer, so screencast can record the webcam *while* a
 * meeting app is using the same camera.  See docs/adr/0002.
 *
 * Kept free of PipeWire headers on purpose: only pwcam.c talks to libpipewire;
 * callers see AVFrames.
 */

typedef struct PwCam PwCam;

/* Negotiated stream geometry, filled in by pwcam_open (mirrors WlCapInfo). */
typedef struct {
    int                width;
    int                height;
    enum AVPixelFormat av_fmt;
} PwCamInfo;

/*
 * Called from the PipeWire capture thread with each newly captured frame.
 * The AVFrame is heap-allocated and ownership passes to the callback, which
 * must eventually av_frame_free() it.
 */
typedef void (*PwCamFrameFn)(void *user, AVFrame *frame);

/*
 * Connect to PipeWire and start capturing the camera.  target selects a
 * specific PipeWire node (name or serial); NULL/empty/"auto" uses the system
 * default camera.  want_w/want_h/want_fps are negotiation *hints* (0 = default).
 *
 * Blocks until the stream format is negotiated (or a short timeout), writing
 * the negotiated geometry and pixel format into *info.  From then on, on_frame
 * is invoked for every frame until pwcam_close().
 *
 * Returns an opaque handle, or NULL if PipeWire or a camera node is unavailable
 * (the caller treats the webcam as best-effort and records without it).
 */
PwCam *pwcam_open(const char *target, int want_w, int want_h, int want_fps,
                  PwCamInfo *info, PwCamFrameFn on_frame, void *user);

/* Stop the capture thread and release the stream.  No callback fires after. */
void pwcam_close(PwCam *pw);
