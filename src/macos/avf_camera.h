// avf_camera.h — AVFoundation webcam capture
#ifndef AVF_CAMERA_H
#define AVF_CAMERA_H

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

/*
 * AVFoundation webcam capture backend for macOS.
 *
 * Replaces the Linux PipeWire pwcam.c with the same callback-based delivery
 * pattern.  Uses AVCaptureSession + AVCaptureVideoDataOutput under the hood.
 * Frames are delivered on a background dispatch queue.
 *
 * The header is pure C — callers need not know about Objective-C or Apple
 * frameworks.
 */

typedef struct AvfCamera AvfCamera; // opaque

/* Negotiated stream geometry, filled in by avf_camera_open. */
typedef struct {
    int                width;
    int                height;
    enum AVPixelFormat pix_fmt;
} AvfCameraInfo;

/*
 * Called from the capture queue with each newly captured frame.
 * The AVFrame is heap-allocated and ownership passes to the callback, which
 * must eventually av_frame_free() it.
 */
typedef void (*AvfCameraFrameFn)(void *user, AVFrame *frame);

/*
 * Open the default (or specified) camera and start capture.
 *
 * target selects a specific camera: a device UID, name substring, or
 * NULL / "" / "auto" for the system default.
 * want_w / want_h / want_fps are negotiation hints; 0 = let the system choose.
 *
 * Populates *info with the actual negotiated format.
 * Returns an opaque handle, or NULL on failure (no camera, permission denied,
 * format negotiation failure, etc.).  The caller treats the webcam as
 * best-effort and records without it on failure.
 */
AvfCamera *avf_camera_open(const char *target, int want_w, int want_h,
                           int want_fps, AvfCameraInfo *info,
                           AvfCameraFrameFn on_frame, void *user);

/*
 * Stop capture and release all resources.  Blocks until the capture queue
 * has drained.  No callback fires after this returns.
 */
void avf_camera_close(AvfCamera *cam);

#endif /* AVF_CAMERA_H */
