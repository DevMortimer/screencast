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
 * and captures system audio (any Linear PCM the output device negotiates,
 * normalised to 48 kHz stereo FLTP before it is queued).
 *
 * The header is pure C — callers need not know about Objective-C or Apple
 * frameworks.
 */

typedef struct SckCapture SckCapture; // opaque

typedef struct {
    int                width;          // pixels
    int                height;         // pixels
    /*
     * Pixels captured per point of display geometry (see SCREENCAST_SCALE).
     * width/height are already in pixels; this is here so that callers placing
     * overlays can size them in points and have them occupy the same fraction
     * of the frame whatever scale the capture ended up at.
     */
    double             scale;
    enum AVPixelFormat pix_fmt;       // BGRA expected
    int                sample_rate;   // audio sample rate, 0 if no audio
    int                channels;      // audio channels, 0 if no audio
    enum AVSampleFormat sample_fmt;   // audio sample format
} SckCaptureInfo;

/*
 * Bootstrap the NSApplication shared instance.
 *
 * Call early in main() when the process may have been launched from a
 * terminal multiplexer (Zellij, tmux, etc.) where AppKit is not
 * automatically initialised.  Without this call macOS may not offer
 * Presenter Overlay in Control Center, because ScreenCaptureKit and
 * AVFoundation need the NSApplication infrastructure to register the
 * process as a camera + screen client.
 *
 * Safe to call multiple times — only the first call does anything.
 */
void sck_bootstrap_app(void);

/*
 * Pump one iteration of the AppKit run loop.
 *
 * Call this periodically during recording so that AppKit can process
 * system messages (including Presenter Overlay registration and state
 * changes).  The main thread blocks for seconds at a time inside the
 * capture loop, so without this help the system may never recognise
 * this process as a live screen+camera client.
 */
void sck_pump_run_loop(void);

/*
 * Current reading of the session clock, in microseconds.
 *
 * This is CMClockGetHostTimeClock() — the same time domain that stamps every
 * ScreenCaptureKit and AVFoundation sample buffer.  Every subsystem derives its
 * PTS from this one clock, so screen, webcam, microphone and desktop audio all
 * land on a single timeline regardless of how long each took to start or how
 * far behind the encoder falls.
 */
int64_t sck_host_time_us(void);

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
 * Anchor the session timeline at `t0_us` (a sck_host_time_us() reading).
 *
 * Capture starts before the recording does — the stream must already be
 * running for sck_capture_open() to confirm a first frame, and the webcam and
 * microphone take a further few hundred milliseconds to come up.  Everything
 * captured during that window is discarded here, so no source can enter the
 * recording carrying a head start.  Until this is called, buffers are still
 * queued but no PTS is meaningful.
 */
void sck_capture_start_session(SckCapture *c, int64_t t0_us);

/*
 * Blocks until the next video frame is available.  Returns a retained
 * CVPixelBufferRef in NV12, as void * to keep this header free of Apple
 * types.  The caller owns the reference and must hand it to
 * sck_capture_release_frame().
 *
 * The buffer is the one ScreenCaptureKit produced, not a copy of it: it stays
 * in GPU memory the whole way to the encoder.
 *
 * *pts_us receives the frame's presentation timestamp relative to the session
 * start — taken from the sample buffer, so it reports when the frame was
 * *captured*, not when it was dequeued.  A backlogged queue therefore delays
 * delivery without corrupting the timeline.
 *
 * Returns NULL on error or after sck_capture_close() has been called.
 */
void *sck_capture_grab_video(SckCapture *c, int64_t *pts_us);

/*
 * Same, but returns NULL immediately when no frame is waiting.
 *
 * The webcam modes cannot block on the screen: ScreenCaptureKit stops
 * delivering entirely while the display is static, and a loop that waits for it
 * would hold the camera at whatever frame it had when the screen went still.
 * They drain the screen queue with this instead and take their cadence from the
 * camera.
 */
void *sck_capture_try_grab_video(SckCapture *c, int64_t *pts_us);

/*
 * Has the stream died underneath us?
 *
 * ScreenCaptureKit can retire a stream at any time — a second capture client,
 * a display reconfiguration, the system recorder claiming the display.  When it
 * does, the only symptom on this side is that frames stop arriving, which is
 * indistinguishable from a screen that simply is not changing.  A caller that
 * holds the last frame it received will therefore go on recording a frozen
 * screen, at full frame rate, with live audio over the top, and nothing in the
 * finished file says anything went wrong.
 *
 * Callers that composite the screen should poll this and stop rather than write
 * a recording they cannot use.  Returns non-zero once the stream has stopped
 * for any reason other than sck_capture_close().
 */
int sck_capture_failed(SckCapture *c);

/* Release a buffer returned by either grab_video variant. */
void sck_capture_release_frame(void *pixbuf);

/*
 * Non-blocking read of the accumulated audio samples.  Returns a heap-allocated
 * AVFrame in the canonical mix format (48 kHz stereo FLTP), or NULL if no
 * audio data is queued.
 *
 * *pts_us receives the session-relative timestamp of the *first* sample in the
 * returned frame.
 *
 * The caller owns the frame and must av_frame_free() it.
 */
AVFrame *sck_capture_grab_audio(SckCapture *c, int64_t *pts_us);

/*
 * Stops capture, drains queues, and frees all resources.  Any thread blocked
 * in grab_video will wake and receive NULL.  Safe to call with NULL.
 */
void sck_capture_close(SckCapture *c);

#endif /* SCK_CAPTURE_H */
