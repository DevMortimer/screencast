#include "encoder.h"
#include "composite.h"
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        if (wr < 0) { av_packet_free(&pkt); return wr; }
    }
    av_packet_free(&pkt);
    return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : ret;
}

/* ── video encoder setup ──────────────────────────────────── */

static int setup_video(EncoderCtx *enc, int w, int h, int fps)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        fprintf(stderr, "encoder: h264_nvenc not found; NVIDIA GPU encoding is required\n");
        return -1;
    }

    enc->vid_stream = avformat_new_stream(enc->fmt_ctx, NULL);
    if (!enc->vid_stream) return AVERROR(ENOMEM);

    enc->vid_enc = avcodec_alloc_context3(codec);
    if (!enc->vid_enc) return AVERROR(ENOMEM);

    enc->vid_enc->codec_id     = AV_CODEC_ID_H264;
    enc->vid_enc->width        = w;
    enc->vid_enc->height       = h;
    /* PTS is in microseconds; rescaled to stream time_base before muxing */
    enc->vid_enc->time_base    = (AVRational){1, 1000000};
    enc->vid_enc->pix_fmt      = AV_PIX_FMT_YUV420P;
    enc->vid_enc->gop_size     = fps * 2;
    enc->vid_enc->max_b_frames = 0;
    enc->vid_enc->profile      = AV_PROFILE_H264_HIGH;
    enc->vid_enc->color_primaries = AVCOL_PRI_BT709;
    enc->vid_enc->color_trc       = AVCOL_TRC_BT709;
    enc->vid_enc->colorspace      = AVCOL_SPC_BT709;
    enc->vid_enc->color_range     = AVCOL_RANGE_MPEG;

    if (enc->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc->vid_enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /*
     * Real-time capture writes a high-quality intermediate with cheap GPU
     * rate control. The final file is rendered after recording stops.
     */
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

    int ret = avcodec_open2(enc->vid_enc, codec, NULL);
    if (ret < 0) { log_err("avcodec_open2 (video)", ret); return ret; }

    avcodec_parameters_from_context(enc->vid_stream->codecpar, enc->vid_enc);
    enc->vid_stream->time_base = enc->vid_enc->time_base;

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

static int setup_sws(EncoderCtx *enc,
                      enum AVPixelFormat screen_fmt,
                      int cam_w, int cam_h,
                      enum AVPixelFormat cam_fmt)
{
    int cw = enc->canvas_w, ch = enc->canvas_h;
    int screen_flags = SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    int yuv_flags = SWS_BICUBIC | SWS_ACCURATE_RND |
                    SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
    int cam_full_flags = SWS_LANCZOS | SWS_ACCURATE_RND |
                         SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
    int cam_overlay_flags = SWS_BICUBIC | SWS_ACCURATE_RND |
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

    if (cam_w > 0 && cam_h > 0) {
        enc->sws_cam_raw = sws_getContext(
            cam_w, cam_h, cam_fmt,
            cam_w, cam_h, AV_PIX_FMT_RGBA,
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

        if (!enc->sws_cam_raw || !enc->sws_cam_main || !enc->sws_cam_scale) {
            fprintf(stderr, "encoder: sws_getContext (cam) failed\n");
            return -1;
        }
    }
    return 0;
}

/* ── public: encoder_open ─────────────────────────────────── */

int encoder_open(EncoderCtx *enc, const char *path,
                  int canvas_w, int canvas_h, int fps,
                  enum AVPixelFormat screen_pix_fmt,
                  int cam_src_w, int cam_src_h,
                  enum AVPixelFormat cam_pix_fmt,
                  int audio_sample_rate,
                  const AVChannelLayout *audio_ch_layout,
                  enum AVSampleFormat audio_sample_fmt)
{
    memset(enc, 0, sizeof(*enc));
    enc->canvas_w = canvas_w;
    enc->canvas_h = canvas_h;

    /* Overlay: 25% of canvas width, capped at 480 px */
    enc->overlay_size = canvas_w / 4;
    if (enc->overlay_size > 480) enc->overlay_size = 480;
    enc->overlay_x = canvas_w  - enc->overlay_size - 20;
    enc->overlay_y = canvas_h - enc->overlay_size - 20;
    enc->cam_overlay_seq = -1;

    enc->cam_src_w     = cam_src_w;
    enc->cam_src_h     = cam_src_h;
    /* Largest centre-crop square that fits inside the webcam frame */
    enc->cam_crop_size = (cam_src_w < cam_src_h) ? cam_src_w : cam_src_h;
    if (cam_src_w > 0 && cam_src_h > 0) {
        if ((int64_t)cam_src_w * canvas_h > (int64_t)canvas_w * cam_src_h) {
            enc->cam_main_h = cam_src_h;
            enc->cam_main_w = (int)(((int64_t)cam_src_h * canvas_w) / canvas_h);
        } else {
            enc->cam_main_w = cam_src_w;
            enc->cam_main_h = (int)(((int64_t)cam_src_w * canvas_h) / canvas_w);
        }
        if (enc->cam_main_w < 1) enc->cam_main_w = 1;
        if (enc->cam_main_h < 1) enc->cam_main_h = 1;
        enc->cam_main_x = (cam_src_w - enc->cam_main_w) / 2;
        enc->cam_main_y = (cam_src_h - enc->cam_main_h) / 2;
    }

    int ret = avformat_alloc_output_context2(&enc->fmt_ctx, NULL, NULL, path);
    if (ret < 0) { log_err("avformat_alloc_output_context2", ret); return ret; }

    if ((ret = setup_video(enc, canvas_w, canvas_h, fps)) < 0) return ret;
    if (audio_sample_rate > 0) {
        if ((ret = setup_audio(enc, audio_sample_rate, audio_ch_layout, audio_sample_fmt)) < 0) return ret;
        av_channel_layout_copy(&enc->aud_in_layout, audio_ch_layout);
    }
    if ((ret = setup_sws(enc, screen_pix_fmt,
                          cam_src_w, cam_src_h, cam_pix_fmt)) < 0) return ret;

    if (!(enc->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&enc->fmt_ctx->pb, path, AVIO_FLAG_WRITE);
        if (ret < 0) { log_err("avio_open", ret); return ret; }
    }

    ret = avformat_write_header(enc->fmt_ctx, NULL);
    if (ret < 0) { log_err("avformat_write_header", ret); return ret; }
    enc->header_written = 1;

    /* Scratch RGBA buffers */
    enc->canvas_rgba = malloc((size_t)canvas_w * canvas_h * 4);
    if (!enc->canvas_rgba) return AVERROR(ENOMEM);

    if (cam_src_w > 0) {
        enc->cam_rgba    = malloc((size_t)cam_src_w * cam_src_h * 4);
        enc->cam_overlay = malloc((size_t)enc->overlay_size * enc->overlay_size * 4);
        enc->corner_mask = malloc(sizeof(float) *
                                   (size_t)enc->overlay_size * enc->overlay_size);
        if (!enc->cam_rgba || !enc->cam_overlay || !enc->corner_mask)
            return AVERROR(ENOMEM);

        /* Pre-build the rounded-corner mask (radius = 1/8 of overlay size) */
        composite_build_mask(enc->corner_mask,
                             enc->overlay_size, enc->overlay_size,
                             enc->overlay_size / 8);
    }

    pthread_mutex_init(&enc->write_mutex, NULL);
    enc->t0 = av_gettime_relative();
    return 0;
}

/* ── recording-indicator dot ──────────────────────────────── */

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
                         int64_t cam_seq)
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
    draw_rec_dot(enc->canvas_rgba, cw, ch, cw - 24, 24, 10);

    /* ── 3. RGBA → YUV420P ── */
    {
        uint8_t *s[1]  = { enc->canvas_rgba };
        int      ls[1] = { cw * 4 };
        sws_scale(enc->sws_to_yuv,
                  (const uint8_t *const *)s, ls,
                  0, ch,
                  enc->vid_frame->data, enc->vid_frame->linesize);
    }

    /* ── 4. Stamp PTS (microseconds since t0) and encode ── */
    enc->vid_frame->pts = av_gettime_relative() - enc->t0;

    int ret = avcodec_send_frame(enc->vid_enc, enc->vid_frame);
    if (ret < 0) { log_err("avcodec_send_frame (video)", ret); return ret; }
    return drain_encoder(enc, enc->vid_enc, enc->vid_stream);
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
int encoder_feed_audio(EncoderCtx *enc, AVFrame *raw_frame)
{
    if (!enc->aud_enc) return 0;

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
        drain_encoder(enc, enc->aud_enc, enc->aud_stream);
    }
    return 0;
}

/* ── public: encoder_flush ────────────────────────────────── */

int encoder_flush(EncoderCtx *enc)
{
    if (!enc->fmt_ctx || !enc->header_written) return 0;

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

    free(enc->canvas_rgba);
    free(enc->cam_rgba);
    free(enc->cam_overlay);
    free(enc->corner_mask);

    pthread_mutex_destroy(&enc->write_mutex);
    memset(enc, 0, sizeof(*enc));
}
