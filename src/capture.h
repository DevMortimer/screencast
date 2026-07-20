#pragma once
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

typedef int (*CaptureInterruptFn)(void *opaque);

typedef struct {
    AVFormatContext    *fmt_ctx;
    AVCodecContext     *dec_ctx;
    int                 stream_idx;
    AVFrame            *frame;
    AVPacket           *pkt;
    /* Decoded-frame properties */
    enum AVPixelFormat  pix_fmt;    /* video */
    enum AVSampleFormat sample_fmt; /* audio */
    int width, height;
    int sample_rate;
    AVChannelLayout ch_layout;
    CaptureInterruptFn should_interrupt;
    void               *interrupt_opaque;
    /* Non-NULL for the Wayland screen source (wlcap); NULL for libav sources. */
    void               *wl_backend;
} CaptureCtx;

/* Installs a callback used to interrupt blocking FFmpeg device reads. */
void capture_set_interrupt(CaptureCtx *ctx, CaptureInterruptFn fn,
                           void *opaque);

/*
 * Opens the Wayland screen source (wlr-screencopy) for the given output name
 * (NULL/empty selects the first output), capped at fps.  The captured output's
 * size and pixel format are written back into ctx (width/height/pix_fmt).
 */
int  capture_screen_open(CaptureCtx *ctx, const char *output_name, int fps);

/* Opens v4l2; actual resolution written to *out_w, *out_h. */
int  capture_webcam_open(CaptureCtx *ctx, const char *device,
                          int *out_w, int *out_h);

/*
 * Opens the default microphone source (PulseAudio/PipeWire "default", with an
 * ALSA fallback on `device`).
 */
int  capture_audio_open(CaptureCtx *ctx, const char *device);

/*
 * Opens a named PulseAudio/PipeWire source for desktop audio, e.g.
 * "@DEFAULT_MONITOR@" (the default sink's monitor).  PulseAudio/PipeWire only.
 */
int  capture_audio_open_monitor(CaptureCtx *ctx, const char *source);

/* Read + decode one frame into ctx->frame. Returns 0 on success. */
int  capture_read(CaptureCtx *ctx);

void capture_free(CaptureCtx *ctx);
