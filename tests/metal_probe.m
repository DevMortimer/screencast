// metal_probe.m — exercise the Metal compositor without the capture stack
//
// The compositor's shader is compiled from source at runtime, so a mistake in
// it cannot be caught by building the project — it surfaces the first time a
// recording starts.  This drives the compositor directly with synthetic NV12
// buffers standing in for the screen and the webcam, renders both compositing
// modes, and encodes the results through the same hardware path the recorder
// uses.
//
//   make probe && ./build/metal_probe
//
// Success is "probe: PASS" and a /tmp/metal_probe.mp4 showing a colour ramp
// with a rounded webcam inset (a grey wedge) in the bottom right.

#import "metal_compositor.h"

#import <CoreVideo/CoreVideo.h>
#import <libavcodec/avcodec.h>
#import <libavformat/avformat.h>
#import <libavutil/hwcontext.h>
#import <libavutil/opt.h>
#import <stdio.h>

#define W       1280
#define H       720
#define CAM_W   1920
#define CAM_H   1080
#define FPS     30
#define NFRAMES 60

static int fail(const char *what, int ret)
{
    char buf[256] = "";
    if (ret) av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "probe: FAIL — %s%s%s\n", what, ret ? ": " : "", buf);
    return 1;
}

static CVPixelBufferRef make_nv12(int w, int h)
{
    NSDictionary *attrs = @{
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
        (id)kCVPixelBufferMetalCompatibilityKey  : @YES,
    };
    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(kCFAllocatorDefault, w, h,
                        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                        (__bridge CFDictionaryRef)attrs, &pb);
    return pb;
}

/* Horizontal ramp with a drifting hue, so a wrong plane or stride is obvious. */
static void fill_nv12(CVPixelBufferRef pb, int i, int bright)
{
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *y  = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    size_t   ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t   us = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    size_t   w  = CVPixelBufferGetWidth(pb);
    size_t   h  = CVPixelBufferGetHeight(pb);

    for (size_t r = 0; r < h; r++)
        for (size_t c = 0; c < w; c++)
            y[r * ys + c] = (uint8_t)(16 + ((c * 200 / w + (bright ? 40 : 0)
                                             + i * 3) % 200));
    for (size_t r = 0; r < h / 2; r++)
        for (size_t c = 0; c + 1 < w; c += 2) {
            uv[r * us + c]     = (uint8_t)(bright ? 90 : 160);
            uv[r * us + c + 1] = (uint8_t)(bright ? 200 : 100);
        }
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

static void release_pixbuf(void *opaque, uint8_t *data)
{
    (void)data;
    CVPixelBufferRelease((CVPixelBufferRef)opaque);
}

int main(void)
{
    int ret;

    int overlay = W / 4;
    if (overlay > 480) overlay = 480;

    MetalCompositor *mc = metal_compositor_create(W, H, overlay,
                                                  W - overlay - 20,
                                                  H - overlay - 20);
    if (!mc) return fail("metal_compositor_create", 0);
    fprintf(stderr, "probe: compositor created (shader compiled)\n");

    /* ── hardware encode path, as proven by vt_hwframe_probe ── */
    AVBufferRef *dev = NULL;
    ret = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                 NULL, NULL, 0);
    if (ret < 0) return fail("av_hwdevice_ctx_create", ret);

    AVBufferRef *frames = av_hwframe_ctx_alloc(dev);
    AVHWFramesContext *fc = (AVHWFramesContext *)frames->data;
    fc->format    = AV_PIX_FMT_VIDEOTOOLBOX;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width     = W;
    fc->height    = H;
    ret = av_hwframe_ctx_init(frames);
    if (ret < 0) return fail("av_hwframe_ctx_init", ret);

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_videotoolbox");
    AVFormatContext *fmt = NULL;
    avformat_alloc_output_context2(&fmt, NULL, NULL, "/tmp/metal_probe.mp4");
    AVStream *st = avformat_new_stream(fmt, NULL);
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->width         = W;
    enc->height        = H;
    enc->pix_fmt       = AV_PIX_FMT_VIDEOTOOLBOX;
    enc->time_base     = (AVRational){1, 1000000};
    enc->max_b_frames  = 0;
    enc->colorspace    = AVCOL_SPC_BT709;
    enc->color_primaries = AVCOL_PRI_BT709;
    enc->color_trc     = AVCOL_TRC_BT709;
    enc->hw_frames_ctx = av_buffer_ref(frames);
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(enc->priv_data, "realtime", "1", 0);

    ret = avcodec_open2(enc, codec, NULL);
    if (ret < 0) return fail("avcodec_open2", ret);
    avcodec_parameters_from_context(st->codecpar, enc);
    st->time_base = enc->time_base;
    avio_open(&fmt->pb, "/tmp/metal_probe.mp4", AVIO_FLAG_WRITE);
    avformat_write_header(fmt, NULL);

    CVPixelBufferRef screen = make_nv12(W, H);
    CVPixelBufferRef cam    = make_nv12(CAM_W, CAM_H);
    if (!screen || !cam) return fail("CVPixelBufferCreate", 0);

    AVPacket *pkt = av_packet_alloc();
    int rendered = 0;

    for (int i = 0; i < NFRAMES; i++) {
        fill_nv12(screen, i, 0);
        fill_nv12(cam, i, 1);

        /* Alternate the two compositing modes so both shader paths run. */
        int mode = (i < NFRAMES / 2) ? 3 : 2;

        void *out = metal_compositor_render(mc, mode, screen, cam);
        if (!out) {
            fprintf(stderr, "probe: render returned NULL on frame %d "
                            "(mode %d)\n", i, mode);
            return 1;
        }
        rendered++;

        AVFrame *f = av_frame_alloc();
        f->format        = AV_PIX_FMT_VIDEOTOOLBOX;
        f->width         = W;
        f->height        = H;
        f->data[3]       = (uint8_t *)out;
        f->hw_frames_ctx = av_buffer_ref(frames);
        f->buf[0] = av_buffer_create((uint8_t *)out, sizeof(out),
                                     release_pixbuf, out, 0);
        f->pts = (int64_t)i * 1000000 / FPS;

        ret = avcodec_send_frame(enc, f);
        if (ret < 0) return fail("avcodec_send_frame", ret);
        av_frame_free(&f);

        while ((ret = avcodec_receive_packet(enc, pkt)) == 0) {
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
    }

    avcodec_send_frame(enc, NULL);
    while (avcodec_receive_packet(enc, pkt) == 0) {
        av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
        pkt->stream_index = st->index;
        av_interleaved_write_frame(fmt, pkt);
        av_packet_unref(pkt);
    }
    av_write_trailer(fmt);

    fprintf(stderr,
            "probe: PASS — %d frames composited and encoded\n"
            "probe: wrote /tmp/metal_probe.mp4\n"
            "probe: expect a colour ramp; first half has a rounded inset in\n"
            "probe: the bottom right, second half is the inset source alone\n",
            rendered);

    CVPixelBufferRelease(screen);
    CVPixelBufferRelease(cam);
    av_packet_free(&pkt);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    avcodec_free_context(&enc);
    av_buffer_unref(&frames);
    av_buffer_unref(&dev);
    metal_compositor_destroy(mc);
    return 0;
}
