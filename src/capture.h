#pragma once
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

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
} CaptureCtx;

/* Opens x11grab at display_name offset by (x_off, y_off), fixed w×h at fps. */
int  capture_screen_open(CaptureCtx *ctx, const char *display_name,
                          int x_off, int y_off,
                          int width, int height, int fps);

/* Opens v4l2; actual resolution written to *out_w, *out_h. */
int  capture_webcam_open(CaptureCtx *ctx, const char *device,
                          int *out_w, int *out_h);

/* Opens ALSA audio device. */
int  capture_audio_open(CaptureCtx *ctx, const char *device);

/* Read + decode one frame into ctx->frame. Returns 0 on success. */
int  capture_read(CaptureCtx *ctx);

void capture_free(CaptureCtx *ctx);
