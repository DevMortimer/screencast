#pragma once
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>

/*
 * Encoder: one MP4 out of captured frames and mixed audio.
 *
 * The webcam is *not* a stream — it is composited onto the canvas — so it is
 * engaged and released dynamically via encoder_set_webcam() /
 * encoder_clear_webcam() rather than being wired up at open.
 *
 * The context is opaque: the platform mains read nothing inside it.  State
 * they used to reach for (has the header been written, where does the audio
 * track sit on the timeline) is exposed through accessors so the module's
 * invariants stay the module's business.
 */
typedef struct EncoderCtx EncoderCtx;

/*
 * Open the output MP4 at `path`.  The output stream set is fixed for the
 * whole recording: the canvas video stream plus, when audio_sample_rate > 0,
 * one audio stream.
 * audio_ch_layout is the layout reported by the capture context.
 *
 * Returns a new context, or NULL on failure (nothing to free).
 */
EncoderCtx *encoder_open(const char *path,
                         int canvas_w, int canvas_h, int fps,
                         enum AVPixelFormat screen_pix_fmt,
                         int audio_sample_rate,
                         const AVChannelLayout *audio_ch_layout,
                         enum AVSampleFormat audio_sample_fmt);

/*
 * Engage the webcam compositing path for a camera stream of the given geometry
 * and pixel format, (re)building the cam scaling chain lazily.  Idempotent: a
 * repeat call with unchanged geometry/format is a no-op, so the record loop can
 * call it every frame while the webcam is active.  A call with different
 * geometry rebuilds the chain (a re-acquired camera may negotiate a new size).
 * Returns 0 on success, negative on failure (compositing stays off).
 */
int  encoder_set_webcam(EncoderCtx *enc, int cam_src_w, int cam_src_h,
                        enum AVPixelFormat cam_pix_fmt);

/*
 * Tear down the webcam compositing path.  After this, encoder_write_video
 * composites the display/canvas only, until encoder_set_webcam() re-engages.
 * Safe to call when no webcam is engaged.
 */
void encoder_clear_webcam(EncoderCtx *enc);

/*
 * Composite + encode one video frame.
 * mode 1 = display only, 2 = webcam only, 3 = both.
 * cam_frame may be NULL when no webcam is open or no frame arrived yet.
 *
 * pts_us is the frame's capture time in microseconds on the session timeline.
 * Pass -1 to fall back to stamping wall-clock time at encode, which is what
 * the Linux path still does until its capture layer carries timestamps.
 * Prefer a real capture timestamp: a wall-clock stamp silently absorbs any
 * delay between capture and encode, so the video slides later against the
 * audio exactly when the machine is too loaded to keep up.
 */
int  encoder_write_video(EncoderCtx *enc, int mode,
                          AVFrame *screen_frame, AVFrame *cam_frame,
                          int64_t cam_seq, int64_t pts_us);

/*
 * Encode one already-composited frame held in a CVPixelBuffer (passed as
 * void * so this header stays platform-neutral).  Only valid when the encoder
 * was opened with the hardware path available; the buffer is handed to
 * VideoToolbox by reference, so its pixels are never read by the CPU.
 *
 * The caller retains ownership of the buffer and may release it as soon as
 * this returns — the encoder takes its own reference.
 *
 * pts_us is the frame's capture time on the session timeline.
 */
int  encoder_write_pixbuf(EncoderCtx *enc, void *pixbuf, int64_t pts_us);

/* Was the encoder opened on the hardware path? */
int  encoder_is_hardware(const EncoderCtx *enc);

/* 1 once the MP4 header is in the file.  A recording that never reached this
   point has nothing worth rendering. */
int  encoder_header_written(const EncoderCtx *enc);

/*
 * Where the audio track sits on the session timeline, in microseconds, or -1
 * when the encoder has no audio track.  The track anchors once — on the
 * first fed frame's PTS — and then counts samples, so this is the position a
 * [SYNC]-style check compares against the video PTS.
 */
int64_t encoder_audio_pts_us(const EncoderCtx *enc);

/*
 * Resample raw audio into FIFO; encodes full 1024-sample chunks.
 *
 * pts_us places the first sample of the very first frame on the session
 * timeline; later calls ignore it and continue counting samples, which is
 * drift-free as long as no samples are lost.  Pass -1 to anchor at zero.
 */
int  encoder_feed_audio(EncoderCtx *enc, AVFrame *raw_frame, int64_t pts_us);

/* Flush encoders, write MP4 trailer. */
int  encoder_flush(EncoderCtx *enc);

/* Free the context (NULL-safe). */
void encoder_free(EncoderCtx *enc);
