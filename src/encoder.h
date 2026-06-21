#pragma once
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <pthread.h>

typedef struct {
    /* Output muxer */
    AVFormatContext  *fmt_ctx;

    /* ── Video ────────────────────────────────────── */
    AVCodecContext   *vid_enc;
    AVStream         *vid_stream;
    AVFrame          *vid_frame;     /* YUV420P at canvas size */
    int               canvas_w, canvas_h;
    int64_t           vid_pts;       /* in microseconds from t0 */

    /* ── Audio ────────────────────────────────────── */
    AVCodecContext   *aud_enc;
    AVStream         *aud_stream;
    AVFrame          *aud_frame;     /* FLTP stereo, 1024 samples */
    AVAudioFifo      *aud_fifo;
    SwrContext       *swr;
    AVChannelLayout   aud_in_layout; /* source layout stamped onto raw frames */
    int64_t           aud_pts;       /* running sample counter */

    /* ── Pixel-format conversion (libswscale) ─────── */
    struct SwsContext *sws_screen;   /* screen_pix_fmt  → RGBA canvas */
    struct SwsContext *sws_cam_raw;  /* cam_pix_fmt → RGBA at cam size */
    struct SwsContext *sws_cam_main; /* RGBA cam crop → full canvas */
    struct SwsContext *sws_cam_scale;/* RGBA square crop → overlay size */
    struct SwsContext *sws_to_yuv;  /* canvas RGBA → YUV420P */

    /* ── Webcam overlay geometry ──────────────────── */
    int overlay_size;   /* side length of the square overlay (px) */
    int overlay_x;      /* top-left x of overlay on canvas */
    int overlay_y;      /* top-left y of overlay on canvas */
    int64_t cam_overlay_seq; /* last webcam frame scaled into cam_overlay */
    float *corner_mask; /* overlay_size² floats */

    /* ── Scratch RGBA buffers ─────────────────────── */
    uint8_t *canvas_rgba;  /* canvas_w * canvas_h * 4 */
    uint8_t *cam_rgba;     /* cam_src_w * cam_src_h * 4 */
    uint8_t *cam_overlay;  /* overlay_size² * 4 */
    int cam_src_w, cam_src_h, cam_crop_size;
    int cam_main_x, cam_main_y, cam_main_w, cam_main_h;

    /* ── Thread safety ────────────────────────────── */
    pthread_mutex_t write_mutex;

    /* ── Timing anchor ────────────────────────────── */
    int64_t t0;          /* av_gettime_relative() at start of recording */
    int header_written;
} EncoderCtx;

/*
 * Open the output MP4 at `path`.  cam_src_w/h == 0 means no webcam.
 * audio_ch_layout is the layout reported by the ALSA capture context.
 */
int  encoder_open(EncoderCtx *enc, const char *path,
                  int canvas_w, int canvas_h, int fps,
                  enum AVPixelFormat screen_pix_fmt,
                  int cam_src_w, int cam_src_h,
                  enum AVPixelFormat cam_pix_fmt,
                  int audio_sample_rate,
                  const AVChannelLayout *audio_ch_layout,
                  enum AVSampleFormat audio_sample_fmt);

/*
 * Composite + encode one video frame.
 * mode 1 = display only, 2 = webcam only, 3 = both.
 * cam_frame may be NULL when no webcam is open or no frame arrived yet.
 */
int  encoder_write_video(EncoderCtx *enc, int mode,
                          AVFrame *screen_frame, AVFrame *cam_frame,
                          int64_t cam_seq);

/* Resample raw audio into FIFO; encodes full 1024-sample chunks. */
int  encoder_feed_audio(EncoderCtx *enc, AVFrame *raw_frame);

/* Flush encoders, write MP4 trailer. */
int  encoder_flush(EncoderCtx *enc);

/* Free all resources. */
void encoder_free(EncoderCtx *enc);
