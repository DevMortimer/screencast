// sck_capture.h — ScreenCaptureKit display + system audio capture
#ifndef SCK_CAPTURE_H
#define SCK_CAPTURE_H

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

/*
 * ScreenCaptureKit display + system audio capture backend for macOS.
 *
 * Bridges the async SCK stream model into a synchronous grab interface.
 * Captures the main display at native resolution with BGRA pixel format
 * and captures system audio (Float32 PCM).
 *
 * The header is pure C — callers need not know about Objective-C or Apple
 * frameworks.
 */

typedef struct SckCapture SckCapture; // opaque

typedef struct {
    int                width;
    int                height;
    enum AVPixelFormat pix_fmt;       // BGRA expected
    int                sample_rate;   // audio sample rate, 0 if no audio
    int                channels;      // audio channels, 0 if no audio
    enum AVSampleFormat sample_fmt;   // audio sample format
} SckCaptureInfo;

/*
 * Opens capture on the default display.  Populates info with display geometry
 * and audio stream parameters.  Blocks until the SCK stream is running and
 * the first video frame has arrived.
 *
 * Returns a handle, or NULL on failure (no display, permission denied, stream
 * startup timeout).
 */
SckCapture *sck_capture_open(SckCaptureInfo *info);

/*
 * Blocks until the next video frame is available.  Returns a heap-allocated
 * AVFrame in BGRA pixel format.  The caller owns the frame and must
 * av_frame_free() it.
 *
 * Returns NULL on error or after sck_capture_close() has been called.
 */
AVFrame *sck_capture_grab_video(SckCapture *c);

/*
 * Non-blocking read of the latest audio samples.  Returns a heap-allocated
 * AVFrame in the format reported by info (typically FLTP planar float at
 * 48 kHz), or NULL if no audio data is queued.
 *
 * The caller owns the frame and must av_frame_free() it.
 */
AVFrame *sck_capture_grab_audio(SckCapture *c);

/*
 * Stops capture, drains queues, and frees all resources.  Any thread blocked
 * in grab_video will wake and receive NULL.  Safe to call with NULL.
 */
void sck_capture_close(SckCapture *c);

#endif /* SCK_CAPTURE_H */
