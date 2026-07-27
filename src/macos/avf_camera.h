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
 *
 * pixbuf is a retained NV12 CVPixelBufferRef, passed as void * to keep this
 * header free of Apple types.  Ownership passes to the callback, which must
 * release it with avf_camera_release_frame().  It is the buffer AVFoundation
 * produced, so it stays in GPU memory for the compositor to sample.
 *
 * pts_us is the frame's timestamp on the session timeline, so callers can
 * match a camera frame against the screen frame it belongs beside rather than
 * assuming the most recent one is contemporaneous with it.
 */
typedef void (*AvfCameraFrameFn)(void *user, void *pixbuf, int64_t pts_us);

/* Release a buffer handed to an AvfCameraFrameFn. */
void avf_camera_release_frame(void *pixbuf);

/*
 * Open the default (or specified) camera and start capture, at the smallest
 * stream the device offers.
 *
 * There is no size to ask for because there is nothing to size.  The camera
 * is held open so macOS offers Presenter Overlay for this app; the system
 * renders the presenter from the device itself, and the frames delivered here
 * are discarded.
 *
 * target selects a specific camera: a device UID, name substring, or
 * NULL / "" / "auto" for the system default.
 *
 * Populates *info with the format actually delivered.
 * Returns an opaque handle, or NULL on failure (no camera, permission denied,
 * and so on).  The caller treats the camera as best-effort and records without
 * it on failure.
 */
AvfCamera *avf_camera_open(const char *target, AvfCameraInfo *info,
                           AvfCameraFrameFn on_frame, void *user);

/*
 * Anchor the session timeline and discard everything captured before it.
 * Cameras take several hundred milliseconds to start delivering, so frames
 * from that window belong to setup rather than to the recording.
 */
void avf_camera_start_session(AvfCamera *cam, int64_t t0_us);

/*
 * Stop capture and release all resources.  Blocks until the capture queue
 * has drained.  No callback fires after this returns.
 */
void avf_camera_close(AvfCamera *cam);

#endif /* AVF_CAMERA_H */
