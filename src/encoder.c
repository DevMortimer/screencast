#include "encoder.h"
#include "composite.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#ifdef __APPLE__
#include <libavutil/hwcontext.h>
#include <CoreVideo/CoreVideo.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One recording's encoder state.  Kept out of the header: the platform mains
 * talk to this module through its functions, and the accessors below are the
 * only reads of this state from outside.
 */
struct EncoderCtx {
    /* Output muxer */
    AVFormatContext  *fmt_ctx;

    /* ── Video ─────────────────────── */
    AVCodecContext   *vid_enc;
    AVStream         *vid_stream;
    AVFrame          *vid_frame;     /* YUV420P at canvas size */
    int               canvas_w, canvas_h;
    int64_t           vid_pts;       /* in microseconds from t0 */

    /* ── Audio ─────────────────────── */
    AVCodecContext   *aud_enc;
    AVStream         *aud_stream;
    AVFrame          *aud_frame;     /* FLTP stereo, 1024 samples */
    AVAudioFifo      *aud_fifo;
    SwrContext       *swr;
    AVChannelLayout   aud_in_layout; /* source layout stamped onto raw frames */
    int64_t           aud_pts;       /* running sample counter */
    int               aud_anchored;  /* aud_pts has been placed on the timeline */

    /* ── Pixel-format conversion (libswscale) ──────── */
    struct SwsContext *sws_screen;   /* screen_pix_fmt  → RGBA canvas */
    struct SwsContext *sws_cam_raw;  /* cam_pix_fmt → RGBA at cam size */
    struct SwsContext *sws_cam_main; /* RGBA cam crop → full canvas */
    struct SwsContext *sws_cam_scale;/* RGBA square crop → overlay size */
    struct SwsContext *sws_to_yuv;  /* canvas RGBA → YUV420P */

    /* ── Webcam overlay geometry ────────────── */
    int overlay_size;   /* side length of the square overlay (px) */
    int overlay_x;      /* top-left x of overlay on canvas */
    int overlay_y;      /* top-left y of overlay on canvas */
    int64_t cam_overlay_seq; /* last webcam frame scaled into cam_overlay */
    float *corner_mask; /* overlay_size² floats */

    /* ── Scratch RGBA buffers ──────────────── */
    uint8_t *canvas_rgba;  /* canvas_w * canvas_h * 4 */
    uint8_t *cam_rgba;     /* cam_src_w * cam_src_h * 4 */
    uint8_t *cam_overlay;  /* overlay_size² * 4 */
    int cam_src_w, cam_src_h, cam_crop_size;
    int cam_main_x, cam_main_y, cam_main_w, cam_main_h;
    enum AVPixelFormat cam_pix_fmt; /* format the cam sws chain was built for */

    /* ── Hardware video path (macOS) ──────────── */
    /* When hw_frames is set the encoder takes CVPixelBuffers by reference and
       none of the swscale chain above is built. */
    AVBufferRef      *hw_device;
    AVBufferRef      *hw_frames;

    /* ── Thread safety ─────────────────── */
    pthread_mutex_t write_mutex;

    /* ── Timing anchor ─────────────────── */
    int64_t t0;          /* av_gettime_relative() at start of recording */
    int64_t last_key_pts;/* PTS of the last forced keyframe (µs), -1 if none */
    int header_written;
};

/* Wall-clock spacing between forced keyframes. */
#define KEYFRAME_INTERVAL_US (2 * 1000000LL)

/* VideoToolbox constant-quality target, 0-100.  65 is visually clean on screen
   content — text stays crisp — without spending the bits that the top of the
   range costs for detail a screen recording does not contain.  Quality is
   bits, and bits are encode time — SCREENCAST_VT_QUALITY dials it. */
static int vt_quality(void)
{
    const char *e = getenv("SCREENCAST_VT_QUALITY");
    if (!e || !e[0]) return 65;
    int v = atoi(e);
    if (v < 1 || v > 100) {
        fprintf(stderr, "encoder: SCREENCAST_VT_QUALITY=%s out of range "
                        "(1-100) — using 65\n", e);
        return 65;
    }
    return v;
}

/* ── logging ──────────────────────────────────────────────── */

static void log_err(const char *label, int ret)
{
    char buf[128];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "encoder: %s: %s\n", label, buf);
}

/* ── drain all pending packets from an encoder ────────────── */

static int drain_encoder(EncoderCtx *enc, AVCodecContext *ctx, AVStream *st)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return AVERROR(ENOMEM);
    int ret;

    while ((ret = avcodec_receive_packet(ctx, pkt)) == 0) {
        av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
        pkt->stream_index = st->index;

        pthread_mutex_lock(&enc->write_mutex);
        int wr = av_interleaved_write_frame(enc->fmt_ctx, pkt);
        pthread_mutex_unlock(&enc->write_mutex);

        av_packet_unref(pkt);
        if (wr < 0) { av_packet_free(&pkt); fprintf(stderr, "encoder: write error %d\n", wr); return wr; }
    }
    av_packet_free(&pkt);
    return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : ret;
}

/* ── video encoder setup ──────────────────────────────────── */

#ifdef __APPLE__
/*
 * Build a VideoToolbox hardware frame context so the encoder can take
 * CVPixelBuffers by reference.  Capture, compositing and encoding then all
 * operate on the same GPU memory and nothing is copied through the CPU.
 *
 * Returns 0 on success.  A failure here is not fatal to the recording — the
 * caller falls back to the software path — so it only warns.
 */
static int setup_hw_frames(EncoderCtx *enc, int w, int h)
{
    int ret = av_hwdevice_ctx_create(&enc->hw_device,
                                     AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                     NULL, NULL, 0);
    if (ret < 0) { log_err("av_hwdevice_ctx_create", ret); return ret; }

    enc->hw_frames = av_hwframe_ctx_alloc(enc->hw_device);
    if (!enc->hw_frames) return AVERROR(ENOMEM);

    AVHWFramesContext *fc = (AVHWFramesContext *)enc->hw_frames->data;
    fc->format    = AV_PIX_FMT_VIDEOTOOLBOX;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width     = w;
    fc->height    = h;

    ret = av_hwframe_ctx_init(enc->hw_frames);
    if (ret < 0) {
        log_err("av_hwframe_ctx_init", ret);
        av_buffer_unref(&enc->hw_frames);
        return ret;
    }
    return 0;
}
#endif

int encoder_is_hardware(const EncoderCtx *enc)
{
    return enc && enc->hw_frames != NULL;
}

int encoder_header_written(const EncoderCtx *enc)
{
    return enc && enc->header_written;
}

int64_t encoder_audio_pts_us(const EncoderCtx *enc)
{
    if (!enc || !enc->aud_enc || enc->aud_enc->sample_rate <= 0)
        return -1;
    return av_rescale_q(enc->aud_pts,
                        (AVRational){1, enc->aud_enc->sample_rate},
                        (AVRational){1, 1000000});
}

static int setup_video(EncoderCtx *enc, int w, int h, int fps)
{
#ifdef __APPLE__
    const char *codec_name = "h264_videotoolbox";
#else
    const char *codec_name = "h264_nvenc";
#endif
    const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        fprintf(stderr, "encoder: %s not found\n", codec_name);
        return -1;
    }

    enc->vid_stream = avformat_new_stream(enc->fmt_ctx, NULL);
    if (!enc->vid_stream) return AVERROR(ENOMEM);

    enc->vid_enc = avcodec_alloc_context3(codec);
    if (!enc->vid_enc) return AVERROR(ENOMEM);

    enc->vid_enc->codec_id     = AV_CODEC_ID_H264;
    enc->vid_enc->width        = w;
    enc->vid_enc->height       = h;
    enc->vid_enc->time_base    = (AVRational){1, 1000000};
    enc->vid_enc->pix_fmt      = AV_PIX_FMT_YUV420P;

#ifdef __APPLE__
    if (setup_hw_frames(enc, w, h) == 0) {
        enc->vid_enc->pix_fmt       = AV_PIX_FMT_VIDEOTOOLBOX;
        enc->vid_enc->hw_frames_ctx = av_buffer_ref(enc->hw_frames);
    } else {
        fprintf(stderr, "encoder: hardware frames unavailable — "
                        "falling back to software frames\n");
    }
#endif
    enc->vid_enc->gop_size     = fps * 2;
    enc->vid_enc->max_b_frames = 0;
    enc->vid_enc->profile      = AV_PROFILE_H264_HIGH;
    /* Level auto.  A hardcoded level has to be revised every time the capture
       resolution changes — 4.0 caps out around 1080p and a Retina-scale canvas
       exceeds it outright. */
    enc->vid_enc->level        = 0;
    enc->vid_enc->color_primaries = AVCOL_PRI_BT709;
    enc->vid_enc->color_trc       = AVCOL_TRC_BT709;
    enc->vid_enc->colorspace      = AVCOL_SPC_BT709;
    enc->vid_enc->color_range     = AVCOL_RANGE_MPEG;

    if (enc->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc->vid_enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /*
     * NVENC real-time capture: high-quality intermediate with cheap GPU
     * rate control.  The final file is rendered after recording stops.
     * VideoToolbox (macOS) uses its own defaults — none of these apply.
     */
#ifndef __APPLE__
    const char *preset = getenv("SCREENCAST_NVENC_CAPTURE_PRESET");
    const char *qp     = getenv("SCREENCAST_NVENC_CAPTURE_QP");
    if (!preset || !preset[0]) preset = "p3";
    if (!qp     || !qp[0])     qp     = "12";

    av_opt_set(enc->vid_enc->priv_data, "preset",  preset,   0);
    av_opt_set(enc->vid_enc->priv_data, "tune",    "hq",     0);
    av_opt_set(enc->vid_enc->priv_data, "profile", "high",   0);
    av_opt_set(enc->vid_enc->priv_data, "rc",      "constqp",0);
    av_opt_set(enc->vid_enc->priv_data, "qp",      qp,       0);
    av_opt_set(enc->vid_enc->priv_data, "surfaces","16",     0);
#else
    /*
     * VideoToolbox: real-time hardware encoding.  prio_speed keeps per-frame
     * latency down so the capture queue does not back up under load.
     *
     * power_efficient defaults OFF (SCREENCAST_VT_POWER_EFFICIENT=1 restores
     * it): the full encoder finishes each frame sooner, freeing the GPU while
     * the rest of the system is under load — the headroom rule (ADR 0007).
     * The low-power path exists for battery sessions on a fanless machine
     * when nothing is competing for the GPU.
     */
    const char *pe = getenv("SCREENCAST_VT_POWER_EFFICIENT");
    const char *power_efficient = (pe && pe[0] && pe[0] != '0') ? "1" : "0";
    av_opt_set(enc->vid_enc->priv_data, "realtime",        "1", 0);
    av_opt_set(enc->vid_enc->priv_data, "power_efficient", power_efficient, 0);
    av_opt_set(enc->vid_enc->priv_data, "prio_speed",      "1", 0);

    /*
     * Rate control.  Setting none at all left the codec on libavcodec's
     * generic default bit rate, which is not a considered figure for a screen
     * capture at any resolution.
     *
     * A screencast is the case constant quality exists for: a still terminal
     * costs almost nothing, and the bits go to the moments something actually
     * moves.  A fixed bit rate pays the same for a motionless frame as for a
     * scroll, which is the opposite of what this pipeline is built around.
     *
     * VideoToolbox only offers constant quality on Apple silicon —
     * libavcodec rejects AV_CODEC_FLAG_QSCALE outright on x86_64 — so Intel
     * gets an average bit rate derived from the canvas instead, at roughly
     * the bits per pixel that quality lands on for screen content.
     */
#if defined(__aarch64__)
    enc->vid_enc->flags         |= AV_CODEC_FLAG_QSCALE;
    enc->vid_enc->global_quality = vt_quality() * FF_QP2LAMBDA;
#else
    enc->vid_enc->bit_rate = (int64_t)w * h * fps / 7;
#endif
#endif

    int ret = avcodec_open2(enc->vid_enc, codec, NULL);
    if (ret < 0) { log_err("avcodec_open2 (video)", ret); return ret; }

    avcodec_parameters_from_context(enc->vid_stream->codecpar, enc->vid_enc);
    enc->vid_stream->time_base = enc->vid_enc->time_base;

    /* The hardware path composites into CVPixelBuffers, so the software
       scratch frame and the whole swscale chain are never used. */
    if (encoder_is_hardware(enc)) return 0;

    enc->vid_frame         = av_frame_alloc();
    enc->vid_frame->format = AV_PIX_FMT_YUV420P;
    enc->vid_frame->width  = w;
    enc->vid_frame->height = h;
    av_frame_get_buffer(enc->vid_frame, 0);
    return 0;
}

/* ── audio encoder setup ──────────────────────────────────── */

static int setup_audio(EncoderCtx *enc, int sample_rate,
                        const AVChannelLayout *in_ch,
                        enum AVSampleFormat in_fmt)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) { fprintf(stderr, "encoder: AAC encoder not found\n"); return -1; }

    enc->aud_stream = avformat_new_stream(enc->fmt_ctx, NULL);
    if (!enc->aud_stream) return AVERROR(ENOMEM);

    enc->aud_enc = avcodec_alloc_context3(codec);
    if (!enc->aud_enc) return AVERROR(ENOMEM);

    enc->aud_enc->sample_rate = sample_rate;
    enc->aud_enc->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    enc->aud_enc->bit_rate    = 256000;
    /* Audio PTS uses sample count; time_base = 1/sample_rate */
    enc->aud_enc->time_base   = (AVRational){1, sample_rate};
    av_channel_layout_default(&enc->aud_enc->ch_layout, 2); /* stereo out */

    if (enc->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc->aud_enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(enc->aud_enc, codec, NULL);
    if (ret < 0) { log_err("avcodec_open2 (audio)", ret); return ret; }

    avcodec_parameters_from_context(enc->aud_stream->codecpar, enc->aud_enc);
    enc->aud_stream->time_base = enc->aud_enc->time_base;

    /*
     * Pulse/ALSA frames often arrive with AV_CHANNEL_ORDER_UNSPEC.  Configure
     * swr with a native layout that has the same channel count as the capture
     * stream, then up/down-mix to stereo AAC output.
     */
    int in_channels = (in_ch && in_ch->nb_channels > 0) ? in_ch->nb_channels : 2;
    AVChannelLayout in_layout;
    AVChannelLayout stereo_layout;
    av_channel_layout_default(&in_layout, in_channels);
    av_channel_layout_default(&stereo_layout, 2);
    ret = swr_alloc_set_opts2(
        &enc->swr,
        &stereo_layout, AV_SAMPLE_FMT_FLTP, sample_rate,
        &in_layout,     in_fmt,             sample_rate,
        0, NULL);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&stereo_layout);
    if (ret < 0) { log_err("swr_alloc_set_opts2", ret); return ret; }

    /*
     * swr_convert_frame detects the input sample format from the frame at
     * call time.  We just need to initialise the resampler so the internal
     * state is ready.
     */
    swr_init(enc->swr);

    /* FIFO: stereo FLTP, pre-allocated for 1 second */
    enc->aud_fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, sample_rate);
    if (!enc->aud_fifo) return AVERROR(ENOMEM);

    /* AAC always consumes exactly frame_size (1024) samples per frame */
    int frame_size = enc->aud_enc->frame_size;
    enc->aud_frame = av_frame_alloc();
    enc->aud_frame->format      = AV_SAMPLE_FMT_FLTP;
    enc->aud_frame->nb_samples  = frame_size;
    enc->aud_frame->sample_rate = sample_rate;
    av_channel_layout_default(&enc->aud_frame->ch_layout, 2);
    av_frame_get_buffer(enc->aud_frame, 0);
    return 0;
}

/* ── SwsContext setup ─────────────────────────────────────── */

/* Build the canvas scaling chain (screen → RGBA, RGBA → YUV420P).  The webcam
 * chain is built separately and lazily by encoder_set_webcam(). */
static int setup_sws(EncoderCtx *enc, enum AVPixelFormat screen_fmt)
{
    int cw = enc->canvas_w, ch = enc->canvas_h;
    int screen_flags = SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    int yuv_flags = SWS_BICUBIC | SWS_ACCURATE_RND |
                    SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;

    enc->sws_screen = sws_getContext(
        cw, ch, screen_fmt,
        cw, ch, AV_PIX_FMT_RGBA,
        screen_flags, NULL, NULL, NULL);

    enc->sws_to_yuv = sws_getContext(
        cw, ch, AV_PIX_FMT_RGBA,
        cw, ch, AV_PIX_FMT_YUV420P,
        yuv_flags, NULL, NULL, NULL);

    if (!enc->sws_screen || !enc->sws_to_yuv) {
        fprintf(stderr, "encoder: sws_getContext failed\n");
        return -1;
    }
    return 0;
}

/* ── dynamic webcam compositing chain ─────────────────────── */

/* Drop the webcam scaling chain, leaving the overlay buffer/mask (which depend
 * only on the canvas) intact for a later re-engage. */
static void free_cam_sws(EncoderCtx *enc)
{
    sws_freeContext(enc->sws_cam_raw);   enc->sws_cam_raw   = NULL;
    sws_freeContext(enc->sws_cam_main);  enc->sws_cam_main  = NULL;
    sws_freeContext(enc->sws_cam_scale); enc->sws_cam_scale = NULL;
    free(enc->cam_rgba);                 enc->cam_rgba      = NULL;
    enc->cam_src_w = enc->cam_src_h = 0;
    enc->cam_crop_size   = 0;
    enc->cam_pix_fmt     = AV_PIX_FMT_NONE;
    enc->cam_overlay_seq = -1;
}

int encoder_set_webcam(EncoderCtx *enc, int cam_src_w, int cam_src_h,
                       enum AVPixelFormat cam_pix_fmt)
{
    if (cam_src_w <= 0 || cam_src_h <= 0) return -1;

    /* Idempotent for an unchanged camera: keep the existing chain so the record
     * loop can call this every frame while the webcam is active. */
    if (enc->sws_cam_raw &&
        enc->cam_src_w == cam_src_w && enc->cam_src_h == cam_src_h &&
        enc->cam_pix_fmt == cam_pix_fmt)
        return 0;

    free_cam_sws(enc);   /* a re-acquired camera may negotiate a new geometry */

    int cw = enc->canvas_w, ch = enc->canvas_h;
    enc->cam_src_w     = cam_src_w;
    enc->cam_src_h     = cam_src_h;
    enc->cam_pix_fmt   = cam_pix_fmt;
    /* Largest centre-crop square that fits inside the webcam frame */
    enc->cam_crop_size = (cam_src_w < cam_src_h) ? cam_src_w : cam_src_h;

    if ((int64_t)cam_src_w * ch > (int64_t)cw * cam_src_h) {
        enc->cam_main_h = cam_src_h;
        enc->cam_main_w = (int)(((int64_t)cam_src_h * cw) / ch);
    } else {
        enc->cam_main_w = cam_src_w;
        enc->cam_main_h = (int)(((int64_t)cam_src_w * ch) / cw);
    }
    if (enc->cam_main_w < 1) enc->cam_main_w = 1;
    if (enc->cam_main_h < 1) enc->cam_main_h = 1;
    enc->cam_main_x = (cam_src_w - enc->cam_main_w) / 2;
    enc->cam_main_y = (cam_src_h - enc->cam_main_h) / 2;

    int screen_flags = SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    /* Webcam is secondary quality — use fast bilinear scaling (not Lanczos)
       to keep the encode pipeline tight and audio in sync. */
    int cam_full_flags = SWS_BILINEAR | SWS_ACCURATE_RND |
                         SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
    int cam_overlay_flags = SWS_BILINEAR | SWS_ACCURATE_RND |
                            SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;

    enc->sws_cam_raw = sws_getContext(
        cam_src_w, cam_src_h, cam_pix_fmt,
        cam_src_w, cam_src_h, AV_PIX_FMT_RGBA,
        screen_flags, NULL, NULL, NULL);

    enc->sws_cam_main = sws_getContext(
        enc->cam_main_w, enc->cam_main_h, AV_PIX_FMT_RGBA,
        cw, ch, AV_PIX_FMT_RGBA,
        cam_full_flags, NULL, NULL, NULL);

    int cs = enc->cam_crop_size;
    enc->sws_cam_scale = sws_getContext(
        cs, cs, AV_PIX_FMT_RGBA,
        enc->overlay_size, enc->overlay_size, AV_PIX_FMT_RGBA,
        cam_overlay_flags, NULL, NULL, NULL);

    enc->cam_rgba = malloc((size_t)cam_src_w * cam_src_h * 4);

    if (!enc->sws_cam_raw || !enc->sws_cam_main || !enc->sws_cam_scale ||
        !enc->cam_rgba) {
        fprintf(stderr, "encoder: webcam sws setup failed\n");
        free_cam_sws(enc);
        return -1;
    }
    return 0;
}

void encoder_clear_webcam(EncoderCtx *enc)
{
    free_cam_sws(enc);
}

/* Diagnostic: AAC frames handed to the audio encoder this session.  An empty
 * audio track is silently dropped by the mov muxer, so a zero here is the
 * difference between "no audio captured" and "audio captured but not muxed". */
static long s_aud_frames_encoded = 0;

/* ── public: encoder_open ─────────────────────────────────── */

EncoderCtx *encoder_open(const char *path,
                         int canvas_w, int canvas_h, int fps,
                         enum AVPixelFormat screen_pix_fmt,
                         int audio_sample_rate,
                         const AVChannelLayout *audio_ch_layout,
                         enum AVSampleFormat audio_sample_fmt)
{
    EncoderCtx *enc = calloc(1, sizeof(*enc));
    if (!enc) return NULL;
    /* Initialised up front so every failure path below can encoder_free(). */
    pthread_mutex_init(&enc->write_mutex, NULL);
    s_aud_frames_encoded = 0;
    enc->canvas_w = canvas_w;
    enc->canvas_h = canvas_h;

    /* Overlay: 25% of canvas width, capped at 480 px */
    enc->overlay_size = canvas_w / 4;
    if (enc->overlay_size > 480) enc->overlay_size = 480;
    enc->overlay_x = canvas_w  - enc->overlay_size - 20;
    enc->overlay_y = canvas_h - enc->overlay_size - 20;
    enc->cam_overlay_seq = -1;
    enc->last_key_pts = -1;
    enc->cam_pix_fmt = AV_PIX_FMT_NONE; /* no webcam engaged yet */

    int ret = avformat_alloc_output_context2(&enc->fmt_ctx, NULL, NULL, path);
    if (ret < 0) { log_err("avformat_alloc_output_context2", ret); goto fail; }

    if ((ret = setup_video(enc, canvas_w, canvas_h, fps)) < 0) goto fail;
    if (audio_sample_rate > 0) {
        if ((ret = setup_audio(enc, audio_sample_rate, audio_ch_layout, audio_sample_fmt)) < 0) goto fail;
        av_channel_layout_copy(&enc->aud_in_layout, audio_ch_layout);
    }
    if (!encoder_is_hardware(enc) &&
        (ret = setup_sws(enc, screen_pix_fmt)) < 0) goto fail;

    if (!(enc->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&enc->fmt_ctx->pb, path, AVIO_FLAG_WRITE);
        if (ret < 0) { log_err("avio_open", ret); goto fail; }
    }

    ret = avformat_write_header(enc->fmt_ctx, NULL);
    if (ret < 0) { log_err("avformat_write_header", ret); goto fail; }
    enc->header_written = 1;

    /* Scratch RGBA buffers — software compositing only. */
    if (encoder_is_hardware(enc))
        return enc;

    enc->canvas_rgba = malloc((size_t)canvas_w * canvas_h * 4);
    if (!enc->canvas_rgba) goto fail;

    /* The overlay buffer and rounded-corner mask depend only on the canvas, so
     * build them up front; they are reused whenever the webcam engages.  The
     * cam_rgba scratch depends on the camera geometry and is allocated lazily
     * by encoder_set_webcam(). */
    enc->cam_overlay = malloc((size_t)enc->overlay_size * enc->overlay_size * 4);
    enc->corner_mask = malloc(sizeof(float) *
                               (size_t)enc->overlay_size * enc->overlay_size);
    if (!enc->cam_overlay || !enc->corner_mask)
        goto fail;

    /* Pre-build the rounded-corner mask (radius = 1/8 of overlay size) */
    composite_build_mask(enc->corner_mask,
                         enc->overlay_size, enc->overlay_size,
                         enc->overlay_size / 8);

    enc->t0 = av_gettime_relative();
    return enc;

fail:
    if (enc->fmt_ctx && enc->fmt_ctx->pb)
        avio_closep(&enc->fmt_ctx->pb);
    encoder_free(enc);
    return NULL;
}

/* ── recording-indicator dot (Linux only) ─────────────────── */

#ifndef __APPLE__
static void draw_rec_dot(uint8_t *rgba, int canvas_w, int canvas_h,
                          int cx, int cy, int r)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy > r*r) continue;
            int px = cx + dx, py = cy + dy;
            if (px < 0 || px >= canvas_w || py < 0 || py >= canvas_h) continue;
            uint8_t *p = rgba + ((size_t)py * canvas_w + px) * 4;
            p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255;
        }
    }
}
#endif

/* ── public: encoder_write_video ──────────────────────────── */

/*
 * Pipeline for one video frame:
 *
 *  mode 1 (display):  screen → RGBA canvas → YUV420P → H264
 *  mode 2 (webcam):   cam → RGBA → aspect crop → full canvas → YUV420P → H264
 *  mode 3 (both):     screen → RGBA canvas; cam overlay pipeline
 *                     blended in the bottom-right corner → YUV420P → H264
 */
int encoder_write_video(EncoderCtx *enc, int mode,
                         AVFrame *screen_frame, AVFrame *cam_frame,
                         int64_t cam_seq, int64_t pts_us)
{
    int cw = enc->canvas_w, ch = enc->canvas_h;

    /* ── 1. Build canvas RGBA ── */
    if (mode == 1 || mode == 3) {
        uint8_t *d[1]  = { enc->canvas_rgba };
        int      ls[1] = { cw * 4 };
        sws_scale(enc->sws_screen,
                  (const uint8_t *const *)screen_frame->data,
                  screen_frame->linesize,
                  0, ch, d, ls);
    } else {
        /* Webcam-only: black background */
        memset(enc->canvas_rgba, 0, (size_t)cw * ch * 4);
    }

    /* ── 2. Webcam path ── */
    if ((mode == 2 || mode == 3) && cam_frame && enc->cam_src_w > 0) {
        if (mode == 2 || cam_seq != enc->cam_overlay_seq) {
            /* 2a. Decode webcam pixel format → RGBA at native cam resolution */
            uint8_t *d[1]  = { enc->cam_rgba };
            int      ls[1] = { enc->cam_src_w * 4 };
            sws_scale(enc->sws_cam_raw,
                      (const uint8_t *const *)cam_frame->data,
                      cam_frame->linesize,
                      0, enc->cam_src_h, d, ls);
        }

        if (mode == 2) {
            uint8_t *src[1] = {
                enc->cam_rgba + ((size_t)enc->cam_main_y * enc->cam_src_w +
                                 enc->cam_main_x) * 4
            };
            int      sls[1] = { enc->cam_src_w * 4 };
            uint8_t *dst[1] = { enc->canvas_rgba };
            int      dls[1] = { cw * 4 };
            sws_scale(enc->sws_cam_main,
                      (const uint8_t *const *)src, sls,
                      0, enc->cam_main_h, dst, dls);
        } else {
            if (cam_seq != enc->cam_overlay_seq) {
                /* 2b. Centre-crop to a square and scale to overlay size. */
                int cs = enc->cam_crop_size;
                int ox = (enc->cam_src_w - cs) / 2;
                int oy = (enc->cam_src_h - cs) / 2;
                int ov = enc->overlay_size;
                uint8_t *src[1] = {
                    enc->cam_rgba + ((size_t)oy * enc->cam_src_w + ox) * 4
                };
                int      sls[1] = { enc->cam_src_w * 4 };
                uint8_t *dst[1] = { enc->cam_overlay };
                int      dls[1] = { ov * 4 };
                sws_scale(enc->sws_cam_scale,
                          (const uint8_t *const *)src, sls,
                          0, cs, dst, dls);

                /* 2c. Force alpha=255; swscale does not set it for RGBA output. */
                int n = enc->overlay_size * enc->overlay_size;
                for (int i = 0; i < n; i++)
                    enc->cam_overlay[i * 4 + 3] = 255;

                enc->cam_overlay_seq = cam_seq;
            }

            composite_blend(enc->canvas_rgba, cw, ch,
                            enc->cam_overlay,
                            enc->overlay_size, enc->overlay_size,
                            enc->overlay_x, enc->overlay_y,
                            enc->corner_mask);
        }
    }

    /* ── 2.5. Recording indicator: red dot top-right ── */
#ifndef __APPLE__
    draw_rec_dot(enc->canvas_rgba, cw, ch, cw - 24, 24, 10);
#endif

    /* ── 3. RGBA → YUV420P ── */
    {
        uint8_t *s[1]  = { enc->canvas_rgba };
        int      ls[1] = { cw * 4 };
        sws_scale(enc->sws_to_yuv,
                  (const uint8_t *const *)s, ls,
                  0, ch,
                  enc->vid_frame->data, enc->vid_frame->linesize);
    }

    /* ── 4. Stamp PTS (microseconds on the session timeline) and encode ── */
    if (pts_us >= 0) {
        /* Capture timestamp supplied by the capture layer.  This is the frame's
           real moment in time, so queueing delay and encoder backlog cannot
           shift it. */
        enc->vid_frame->pts = pts_us;
    } else {
        /* Legacy path: no capture timestamps available, so stamp now and
           anchor the first frame at zero. */
        if (!enc->vid_pts) {
            enc->t0 = av_gettime_relative();
            enc->vid_pts = 1;  /* mark first-frame handled */
        }
        enc->vid_frame->pts = av_gettime_relative() - enc->t0;
    }

    /*
     * Keyframes on a time interval, not a frame count.  Static screen content
     * is skipped at capture, so a GOP measured in frames can stretch across
     * minutes of real time and make the file nearly unseekable.
     */
    if (enc->last_key_pts < 0 ||
        enc->vid_frame->pts - enc->last_key_pts >= KEYFRAME_INTERVAL_US) {
        enc->vid_frame->pict_type = AV_PICTURE_TYPE_I;
        enc->last_key_pts = enc->vid_frame->pts;
    } else {
        enc->vid_frame->pict_type = AV_PICTURE_TYPE_NONE;
    }

    int ret = avcodec_send_frame(enc->vid_enc, enc->vid_frame);
    while (ret == AVERROR(EAGAIN)) {
        int dr = drain_encoder(enc, enc->vid_enc, enc->vid_stream);
        if (dr < 0) return dr;
        ret = avcodec_send_frame(enc->vid_enc, enc->vid_frame);
    }
    if (ret < 0) { log_err("avcodec_send_frame (video)", ret); return ret; }
    return drain_encoder(enc, enc->vid_enc, enc->vid_stream);
}

/* ── public: encoder_write_pixbuf ─────────────────────────── */

#ifdef __APPLE__
/* Balances the CFRetain taken when the buffer was wrapped. */
static void unref_pixbuf(void *opaque, uint8_t *data)
{
    (void)data;
    CFRelease((CFTypeRef)opaque);
}
#endif

int encoder_write_pixbuf(EncoderCtx *enc, void *pixbuf, int64_t pts_us)
{
#ifndef __APPLE__
    (void)enc; (void)pixbuf; (void)pts_us;
    return AVERROR(ENOSYS);
#else
    if (!enc || !pixbuf || !encoder_is_hardware(enc)) return AVERROR(EINVAL);

    AVFrame *frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);

    /* Wrap rather than copy: data[3] is where libav expects the
       CVPixelBufferRef, and buf[0] owns the reference that keeps it alive
       until the encoder is finished with it. */
    frame->format        = AV_PIX_FMT_VIDEOTOOLBOX;
    frame->width         = enc->canvas_w;
    frame->height        = enc->canvas_h;
    frame->data[3]       = (uint8_t *)pixbuf;
    frame->hw_frames_ctx = av_buffer_ref(enc->hw_frames);
    frame->buf[0]        = av_buffer_create((uint8_t *)pixbuf, sizeof(void *),
                                            unref_pixbuf,
                                            (void *)CFRetain(pixbuf), 0);
    if (!frame->hw_frames_ctx || !frame->buf[0]) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

    frame->pts = pts_us;

    if (enc->last_key_pts < 0 ||
        pts_us - enc->last_key_pts >= KEYFRAME_INTERVAL_US) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        enc->last_key_pts = pts_us;
    }

    int ret = avcodec_send_frame(enc->vid_enc, frame);
    while (ret == AVERROR(EAGAIN)) {
        int dr = drain_encoder(enc, enc->vid_enc, enc->vid_stream);
        if (dr < 0) { av_frame_free(&frame); return dr; }
        ret = avcodec_send_frame(enc->vid_enc, frame);
    }
    av_frame_free(&frame);

    if (ret < 0) { log_err("avcodec_send_frame (hw video)", ret); return ret; }
    return drain_encoder(enc, enc->vid_enc, enc->vid_stream);
#endif
}

/* ── public: encoder_feed_audio ───────────────────────────── */

/*
 * Audio synchronisation design:
 *
 *  1. Resample the raw capture frame (S16 / any layout) to stereo FLTP at
 *     the encoder's sample rate using libswresample.
 *  2. Push all converted samples into an AVAudioFifo.
 *  3. Drain the FIFO in exact 1024-sample slices (AAC frame_size).
 *     Each slice gets PTS = enc->aud_pts (in samples), then aud_pts += 1024.
 *
 *  Using sample-count PTS avoids drift that accumulates with wall-clock
 *  based assignment (jitter from capture scheduling).  The muxer rescales
 *  both audio (1/sample_rate) and video (1/1000000) time bases when
 *  interleaving, so they stay locked.
 */
int encoder_feed_audio(EncoderCtx *enc, AVFrame *raw_frame, int64_t pts_us)
{
    if (!enc->aud_enc) return 0;

    /* Place the audio track on the session timeline exactly once.  Audio and
       video are captured by independent subsystems that come up at different
       times; anchoring both at zero regardless is what makes the audio lead
       the picture by the whole startup duration. */
    if (!enc->aud_anchored) {
        enc->aud_anchored = 1;
        if (pts_us > 0)
            enc->aud_pts = av_rescale_q(pts_us, (AVRational){1, 1000000},
                                        (AVRational){1, enc->aud_enc->sample_rate});
    }

    /* Stamp a native layout with the captured channel count before resampling. */
    int in_channels = enc->aud_in_layout.nb_channels > 0
                      ? enc->aud_in_layout.nb_channels
                      : raw_frame->ch_layout.nb_channels;
    if (in_channels <= 0) in_channels = 2;
    av_channel_layout_uninit(&raw_frame->ch_layout);
    av_channel_layout_default(&raw_frame->ch_layout, in_channels);

    /* Resample to stereo FLTP (swr_convert_frame reads fmt from the frame) */
    AVFrame *res = av_frame_alloc();
    if (!res) return AVERROR(ENOMEM);

    res->format      = AV_SAMPLE_FMT_FLTP;
    res->sample_rate = enc->aud_enc->sample_rate;
    av_channel_layout_default(&res->ch_layout, 2);

    int ret = swr_convert_frame(enc->swr, res, raw_frame);
    if (ret == AVERROR_INPUT_CHANGED) {
        /* swr auto-reconfigured for new input format; retry once */
        av_frame_unref(res);
        res->format      = AV_SAMPLE_FMT_FLTP;
        res->sample_rate = enc->aud_enc->sample_rate;
        av_channel_layout_default(&res->ch_layout, 2);
        ret = swr_convert_frame(enc->swr, res, raw_frame);
    }
    if (ret < 0) { log_err("swr_convert_frame", ret); av_frame_free(&res); return ret; }

    ret = av_audio_fifo_write(enc->aud_fifo, (void **)res->data, res->nb_samples);
    if (ret < res->nb_samples) {
        av_frame_free(&res);
        return AVERROR(ENOMEM);
    }
    av_frame_free(&res);

    /* Drain complete 1024-sample frames */
    int frame_size = enc->aud_enc->frame_size;
    while (av_audio_fifo_size(enc->aud_fifo) >= frame_size) {
        av_frame_make_writable(enc->aud_frame);
        av_audio_fifo_read(enc->aud_fifo, (void **)enc->aud_frame->data, frame_size);
        enc->aud_frame->pts        = enc->aud_pts;
        enc->aud_frame->nb_samples = frame_size;
        enc->aud_pts += frame_size;

        ret = avcodec_send_frame(enc->aud_enc, enc->aud_frame);
        if (ret < 0) { log_err("avcodec_send_frame (audio)", ret); return ret; }
        s_aud_frames_encoded++;
        drain_encoder(enc, enc->aud_enc, enc->aud_stream);
    }
    return 0;
}

/* ── public: encoder_flush ────────────────────────────────── */

int encoder_flush(EncoderCtx *enc)
{
    if (!enc || !enc->fmt_ctx || !enc->header_written) return 0;

    if (getenv("SCREENCAST_DEBUG"))
        fprintf(stderr, "encoder: AAC frames encoded this session: %ld\n",
                s_aud_frames_encoded);

    /* Signal end-of-stream to both encoders */
    avcodec_send_frame(enc->vid_enc, NULL);
    drain_encoder(enc, enc->vid_enc, enc->vid_stream);

    if (enc->aud_enc) {
        int frame_size = enc->aud_enc->frame_size;
        while (av_audio_fifo_size(enc->aud_fifo) > 0) {
            int queued = av_audio_fifo_size(enc->aud_fifo);
            int take = queued < frame_size ? queued : frame_size;

            av_frame_make_writable(enc->aud_frame);
            av_samples_set_silence(enc->aud_frame->data, 0, frame_size, 2,
                                   AV_SAMPLE_FMT_FLTP);
            av_audio_fifo_read(enc->aud_fifo,
                               (void **)enc->aud_frame->data, take);
            enc->aud_frame->pts        = enc->aud_pts;
            enc->aud_frame->nb_samples = frame_size;
            enc->aud_pts += frame_size;

            avcodec_send_frame(enc->aud_enc, enc->aud_frame);
            drain_encoder(enc, enc->aud_enc, enc->aud_stream);
        }

        avcodec_send_frame(enc->aud_enc, NULL);
        drain_encoder(enc, enc->aud_enc, enc->aud_stream);
    }

    return av_write_trailer(enc->fmt_ctx);
}

/* ── public: encoder_free ─────────────────────────────────── */

void encoder_free(EncoderCtx *enc)
{
    if (!enc) return;

    if (enc->fmt_ctx && enc->header_written &&
        !(enc->fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&enc->fmt_ctx->pb);

    avformat_free_context(enc->fmt_ctx);
    avcodec_free_context(&enc->vid_enc);
    avcodec_free_context(&enc->aud_enc);
    av_frame_free(&enc->vid_frame);
    av_frame_free(&enc->aud_frame);
    av_audio_fifo_free(enc->aud_fifo);
    swr_free(&enc->swr);
    av_channel_layout_uninit(&enc->aud_in_layout);

    sws_freeContext(enc->sws_screen);
    sws_freeContext(enc->sws_cam_raw);
    sws_freeContext(enc->sws_cam_main);
    sws_freeContext(enc->sws_cam_scale);
    sws_freeContext(enc->sws_to_yuv);

    av_buffer_unref(&enc->hw_frames);
    av_buffer_unref(&enc->hw_device);

    free(enc->canvas_rgba);
    free(enc->cam_rgba);
    free(enc->cam_overlay);
    free(enc->corner_mask);

    pthread_mutex_destroy(&enc->write_mutex);
    free(enc);
}
