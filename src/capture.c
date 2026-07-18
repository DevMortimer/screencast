#include "capture.h"
#include "wlcap.h"
#include <libavdevice/avdevice.h>
#include <libavutil/opt.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── internal helpers ──────────────────────────────────────── */

static void preserve_interrupt(CaptureCtx *ctx,
                               CaptureInterruptFn *fn,
                               void **opaque)
{
    *fn = ctx->should_interrupt;
    *opaque = ctx->interrupt_opaque;
}

static void restore_interrupt(CaptureCtx *ctx,
                              CaptureInterruptFn fn,
                              void *opaque)
{
    ctx->should_interrupt = fn;
    ctx->interrupt_opaque = opaque;
}

static int capture_interrupted(void *opaque)
{
    CaptureCtx *ctx = (CaptureCtx *)opaque;
    return ctx && ctx->should_interrupt &&
           ctx->should_interrupt(ctx->interrupt_opaque);
}

static void attach_interrupt(CaptureCtx *ctx)
{
    if (!ctx->fmt_ctx || !ctx->should_interrupt) return;

    ctx->fmt_ctx->interrupt_callback.callback = capture_interrupted;
    ctx->fmt_ctx->interrupt_callback.opaque = ctx;
}

static int open_input(CaptureCtx *ctx, const char *fmt_name,
                       const char *device, AVDictionary **opts)
{
    const AVInputFormat *fmt = av_find_input_format(fmt_name);
    if (!fmt) {
        fprintf(stderr, "capture: format '%s' not found\n", fmt_name);
        return -1;
    }

    if (ctx->should_interrupt) {
        ctx->fmt_ctx = avformat_alloc_context();
        if (!ctx->fmt_ctx) return AVERROR(ENOMEM);
        attach_interrupt(ctx);
    }

    int ret = avformat_open_input(&ctx->fmt_ctx, device, fmt, opts);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "capture: cannot open '%s' as '%s': %s\n",
                device, fmt_name, errbuf);
        avformat_close_input(&ctx->fmt_ctx);
        return ret;
    }
    attach_interrupt(ctx);
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

static int webcam_try_device(CaptureCtx *ctx, const char *device,
                              int *out_w, int *out_h)
{
    CaptureInterruptFn fn;
    void *opaque;
    preserve_interrupt(ctx, &fn, &opaque);
    memset(ctx, 0, sizeof(*ctx));
    restore_interrupt(ctx, fn, opaque);

    const char *env_fmt  = getenv("SCREENCAST_CAM_FORMAT");
    const char *env_fps  = getenv("SCREENCAST_CAM_FPS");
    const char *env_size = getenv("SCREENCAST_CAM_SIZE");
    const char *fmt      = (env_fmt  && env_fmt[0])  ? env_fmt  : "nv12";
    const char *fps      = (env_fps  && env_fps[0])  ? env_fps  : "30";
    const char *size     = (env_size && env_size[0]) ? env_size : "1920x1080";

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "input_format", fmt, 0);
    av_dict_set(&opts, "framerate",    fps, 0);
    av_dict_set(&opts, "video_size",   size, 0);

    if (open_input(ctx, "video4linux2", device, &opts) < 0) {
        av_dict_free(&opts);
        capture_free(ctx);

        fprintf(stderr,
                "capture: requested webcam mode failed "
                "(format=%s size=%s fps=%s), falling back\n",
                fmt, size, fps);

        opts = NULL;
        av_dict_set(&opts, "input_format", "mjpeg", 0);
        av_dict_set(&opts, "framerate",    "30",    0);
        if (!env_size || !env_size[0])
            av_dict_set(&opts, "video_size", "1920x1080", 0);

        if (open_input(ctx, "video4linux2", device, &opts) < 0) {
            av_dict_free(&opts);
            capture_free(ctx);

            opts = NULL;
            if (open_input(ctx, "video4linux2", device, &opts) < 0) {
                av_dict_free(&opts);
                capture_free(ctx);
                return -1;
            }
        }
    }

    if (open_decoder(ctx, AVMEDIA_TYPE_VIDEO) < 0) {
        capture_free(ctx);
        return -1;
    }

    ctx->width   = ctx->dec_ctx->width;
    ctx->height  = ctx->dec_ctx->height;
    ctx->pix_fmt = ctx->dec_ctx->pix_fmt;
    *out_w = ctx->width;
    *out_h = ctx->height;
    fprintf(stderr, "capture: webcam using %s (%dx%d)\n",
            device, ctx->width, ctx->height);
    return 0;
}

static int webcam_try_glob(CaptureCtx *ctx, const char *pattern,
                            int *out_w, int *out_h)
{
    glob_t matches;
    memset(&matches, 0, sizeof(matches));

    if (glob(pattern, 0, NULL, &matches) != 0) {
        globfree(&matches);
        return -1;
    }

    for (size_t i = 0; i < matches.gl_pathc; i++) {
        if (webcam_try_device(ctx, matches.gl_pathv[i], out_w, out_h) == 0) {
            globfree(&matches);
            return 0;
        }
    }

    globfree(&matches);
    return -1;
}

/* ── public ────────────────────────────────────────────────── */

void capture_set_interrupt(CaptureCtx *ctx, CaptureInterruptFn fn,
                           void *opaque)
{
    if (!ctx) return;

    ctx->should_interrupt = fn;
    ctx->interrupt_opaque = opaque;
    attach_interrupt(ctx);
}

int capture_screen_open(CaptureCtx *ctx, const char *output_name, int fps)
{
    CaptureInterruptFn fn;
    void *opaque;
    preserve_interrupt(ctx, &fn, &opaque);
    memset(ctx, 0, sizeof(*ctx));
    restore_interrupt(ctx, fn, opaque);

    /* draw the cursor into the recording by default, like x11grab did */
    const char *env_cur = getenv("SCREENCAST_DRAW_MOUSE");
    int overlay_cursor = (!env_cur || env_cur[0] != '0');

    WlCapInfo info;
    WlCap *wl = wlcap_open(output_name, overlay_cursor, fps, &info);
    if (!wl) return -1;

    ctx->wl_backend = wl;
    ctx->width      = info.width;
    ctx->height     = info.height;
    ctx->pix_fmt    = (enum AVPixelFormat)info.av_pix_fmt;

    ctx->frame = av_frame_alloc();
    if (!ctx->frame) { wlcap_close(wl); ctx->wl_backend = NULL; return -1; }
    ctx->frame->format = ctx->pix_fmt;
    ctx->frame->width  = info.width;
    ctx->frame->height = info.height;

    uint8_t *base = wlcap_buffer(wl);
    if (info.y_invert) {
        /* Present the frame flipped via a negative stride so swscale reads it
         * top-to-bottom without an extra copy. */
        ctx->frame->data[0]     = base + (size_t)info.stride * (info.height - 1);
        ctx->frame->linesize[0] = -info.stride;
    } else {
        ctx->frame->data[0]     = base;
        ctx->frame->linesize[0] = info.stride;
    }
    return 0;
}

int capture_webcam_open(CaptureCtx *ctx, const char *device,
                         int *out_w, int *out_h)
{
    avdevice_register_all();

    if (device && device[0] && strcmp(device, "auto") != 0) {
        return webcam_try_device(ctx, device, out_w, out_h);
    }

    if (webcam_try_glob(ctx, "/dev/v4l/by-id/*-video-index0",
                        out_w, out_h) == 0)
        return 0;
    if (webcam_try_glob(ctx, "/dev/v4l/by-path/*-video-index0",
                        out_w, out_h) == 0)
        return 0;

    for (int i = 0; i < 64; i++) {
        char path[32];
        struct stat st;
        snprintf(path, sizeof(path), "/dev/video%d", i);
        if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode))
            continue;
        if (webcam_try_device(ctx, path, out_w, out_h) == 0)
            return 0;
    }

    return -1;
}

int capture_audio_open(CaptureCtx *ctx, const char *device)
{
    avdevice_register_all();
    CaptureInterruptFn fn;
    void *opaque;
    preserve_interrupt(ctx, &fn, &opaque);
    memset(ctx, 0, sizeof(*ctx));
    restore_interrupt(ctx, fn, opaque);

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
    if (ctx->wl_backend) {
        if (capture_interrupted(ctx))
            return AVERROR_EXIT;
        /* Grabs into the shm buffer already aliased by ctx->frame->data[0]. */
        return wlcap_grab(ctx->wl_backend) < 0 ? AVERROR(EIO) : 0;
    }

    for (;;) {
        if (capture_interrupted(ctx))
            return AVERROR_EXIT;

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
    CaptureInterruptFn fn;
    void *opaque;
    preserve_interrupt(ctx, &fn, &opaque);
    if (ctx->wl_backend) {
        /* frame->data[0] aliases the wlcap shm buffer — don't let av_frame_free
         * touch it (it holds no AVBufferRef, so this is just tidiness). */
        if (ctx->frame) {
            ctx->frame->data[0]     = NULL;
            ctx->frame->linesize[0] = 0;
        }
        wlcap_close(ctx->wl_backend);
        ctx->wl_backend = NULL;
    }
    av_frame_free(&ctx->frame);
    av_packet_free(&ctx->pkt);
    avcodec_free_context(&ctx->dec_ctx);
    avformat_close_input(&ctx->fmt_ctx);
    av_channel_layout_uninit(&ctx->ch_layout);
    memset(ctx, 0, sizeof(*ctx));
    restore_interrupt(ctx, fn, opaque);
}
