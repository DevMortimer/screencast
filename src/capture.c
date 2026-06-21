#include "capture.h"
#include <libavdevice/avdevice.h>
#include <libavutil/opt.h>
#include <stdio.h>
#include <string.h>

/* ── internal helpers ──────────────────────────────────────── */

static int open_input(CaptureCtx *ctx, const char *fmt_name,
                       const char *device, AVDictionary **opts)
{
    const AVInputFormat *fmt = av_find_input_format(fmt_name);
    if (!fmt) {
        fprintf(stderr, "capture: format '%s' not found\n", fmt_name);
        return -1;
    }
    int ret = avformat_open_input(&ctx->fmt_ctx, device, fmt, opts);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "capture: cannot open '%s' as '%s': %s\n",
                device, fmt_name, errbuf);
        return ret;
    }
    av_dict_free(opts);

    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "capture: find_stream_info failed\n");
        return ret;
    }
    return 0;
}

static int open_decoder(CaptureCtx *ctx, enum AVMediaType mtype)
{
    int idx = av_find_best_stream(ctx->fmt_ctx, mtype, -1, -1, NULL, 0);
    if (idx < 0) {
        fprintf(stderr, "capture: no %s stream\n",
                av_get_media_type_string(mtype));
        return idx;
    }
    ctx->stream_idx = idx;
    AVStream *st = ctx->fmt_ctx->streams[idx];

    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        fprintf(stderr, "capture: no decoder for codec %d\n",
                st->codecpar->codec_id);
        return -1;
    }

    ctx->dec_ctx = avcodec_alloc_context3(dec);
    if (!ctx->dec_ctx) return AVERROR(ENOMEM);

    avcodec_parameters_to_context(ctx->dec_ctx, st->codecpar);

    int ret = avcodec_open2(ctx->dec_ctx, dec, NULL);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "capture: avcodec_open2 failed: %s\n", errbuf);
        return ret;
    }

    ctx->frame = av_frame_alloc();
    ctx->pkt   = av_packet_alloc();
    if (!ctx->frame || !ctx->pkt) return AVERROR(ENOMEM);
    return 0;
}

/* ── public ────────────────────────────────────────────────── */

int capture_screen_open(CaptureCtx *ctx, const char *display_name,
                         int x_off, int y_off,
                         int width, int height, int fps)
{
    avdevice_register_all();
    memset(ctx, 0, sizeof(*ctx));

    /* x11grab device string: ":0.0+x_off,y_off" */
    char dev[64];
    snprintf(dev, sizeof(dev), "%s+%d,%d", display_name, x_off, y_off);

    AVDictionary *opts = NULL;
    char size_str[32], fps_str[8];
    snprintf(size_str, sizeof(size_str), "%dx%d", width, height);
    snprintf(fps_str,  sizeof(fps_str),  "%d",    fps);
    av_dict_set(&opts, "video_size", size_str, 0);
    av_dict_set(&opts, "framerate",  fps_str,  0);
    av_dict_set(&opts, "draw_mouse", "1",      0);
    av_dict_set(&opts, "rtbufsize",  "100M",   0);

    if (open_input(ctx, "x11grab", dev, &opts) < 0) return -1;
    if (open_decoder(ctx, AVMEDIA_TYPE_VIDEO) < 0) return -1;

    ctx->width   = ctx->dec_ctx->width;
    ctx->height  = ctx->dec_ctx->height;
    ctx->pix_fmt = ctx->dec_ctx->pix_fmt;
    return 0;
}

int capture_webcam_open(CaptureCtx *ctx, const char *device,
                         int *out_w, int *out_h)
{
    avdevice_register_all();
    memset(ctx, 0, sizeof(*ctx));

    AVDictionary *opts = NULL;
    /* Ask for a reasonable default; fall back to device capability */
    av_dict_set(&opts, "input_format", "mjpeg", 0);
    av_dict_set(&opts, "framerate",    "30",    0);

    if (open_input(ctx, "video4linux2", device, &opts) < 0) {
        /* Retry without format hint */
        av_dict_free(&opts);
        opts = NULL;
        if (open_input(ctx, "video4linux2", device, &opts) < 0) return -1;
    }
    if (open_decoder(ctx, AVMEDIA_TYPE_VIDEO) < 0) return -1;

    ctx->width   = ctx->dec_ctx->width;
    ctx->height  = ctx->dec_ctx->height;
    ctx->pix_fmt = ctx->dec_ctx->pix_fmt;
    *out_w = ctx->width;
    *out_h = ctx->height;
    return 0;
}

int capture_audio_open(CaptureCtx *ctx, const char *device)
{
    avdevice_register_all();
    memset(ctx, 0, sizeof(*ctx));

    /* Try PulseAudio/PipeWire first — honours the user's default source (mic). */
    AVDictionary *opts = NULL;
    int ok = (open_input(ctx, "pulse", "default", &opts) == 0 &&
              open_decoder(ctx, AVMEDIA_TYPE_AUDIO) == 0);

    if (!ok) {
        av_dict_free(&opts);
        capture_free(ctx);

        opts = NULL;
        av_dict_set(&opts, "sample_rate", "44100", 0);
        av_dict_set(&opts, "channels",    "2",     0);
        if (open_input(ctx, "alsa", device, &opts) < 0) {
            av_dict_free(&opts);
            return -1;
        }
        if (open_decoder(ctx, AVMEDIA_TYPE_AUDIO) < 0) return -1;
    }

    ctx->sample_rate = ctx->dec_ctx->sample_rate;
    ctx->sample_fmt  = ctx->dec_ctx->sample_fmt;
    av_channel_layout_copy(&ctx->ch_layout, &ctx->dec_ctx->ch_layout);
    return 0;
}

int capture_read(CaptureCtx *ctx)
{
    for (;;) {
        int ret = av_read_frame(ctx->fmt_ctx, ctx->pkt);
        if (ret < 0) return ret;

        if (ctx->pkt->stream_index != ctx->stream_idx) {
            av_packet_unref(ctx->pkt);
            continue;
        }

        ret = avcodec_send_packet(ctx->dec_ctx, ctx->pkt);
        av_packet_unref(ctx->pkt);
        if (ret < 0) continue; /* corrupt packet — skip */

        ret = avcodec_receive_frame(ctx->dec_ctx, ctx->frame);
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) return ret;
        return 0;
    }
}

void capture_free(CaptureCtx *ctx)
{
    if (!ctx) return;
    av_frame_free(&ctx->frame);
    av_packet_free(&ctx->pkt);
    avcodec_free_context(&ctx->dec_ctx);
    avformat_close_input(&ctx->fmt_ctx);
    av_channel_layout_uninit(&ctx->ch_layout);
    memset(ctx, 0, sizeof(*ctx));
}
