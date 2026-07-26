// vt_hwframe_probe.m — does h264_videotoolbox accept CVPixelBuffers?
//
// The zero-copy capture path depends on handing VideoToolbox a CVPixelBuffer
// by reference instead of a software AVFrame, and on doing it two different
// ways: buffers allocated from libav's own hardware frame pool, and buffers
// that came from somewhere else entirely (ScreenCaptureKit, a Metal render
// target) and are merely wrapped.  This probe exercises both against the real
// encoder and writes a file, so a failure shows up here rather than halfway
// through rewriting the pipeline.
//
// Build and run:
//   make probe && ./build/vt_hwframe_probe
//
// Success is "probe: PASS" plus a playable /tmp/vt_probe.mp4.

#import <CoreVideo/CoreVideo.h>
#import <libavcodec/avcodec.h>
#import <libavformat/avformat.h>
#import <libavutil/hwcontext.h>
#import <libavutil/hwcontext_videotoolbox.h>
#import <libavutil/imgutils.h>
#import <libavutil/opt.h>
#import <stdio.h>

#define W       1280
#define H       720
#define FPS     30
#define NFRAMES 60

static int fail(const char *what, int ret)
{
    char buf[256] = "";
    if (ret) av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "probe: FAIL — %s%s%s\n", what, ret ? ": " : "", buf);
    return 1;
}

/* Paint a moving band so the output is obviously not garbage. */
static void paint_nv12(CVPixelBufferRef pb, int i)
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
            y[r * ys + c] = (uint8_t)(16 + ((c + i * 8) % 220));
    for (size_t r = 0; r < h / 2; r++)
        for (size_t c = 0; c < w; c++)
            uv[r * us + c] = 128;

    CVPixelBufferUnlockBaseAddress(pb, 0);
}

/* Release callback for a wrapped, externally-owned CVPixelBuffer. */
static void release_pixbuf(void *opaque, uint8_t *data)
{
    (void)data;
    CVPixelBufferRelease((CVPixelBufferRef)opaque);
}

int main(void)
{
    int ret;

    /* ── hardware device + frame pool ── */
    AVBufferRef *dev = NULL;
    ret = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                 NULL, NULL, 0);
    if (ret < 0) return fail("av_hwdevice_ctx_create", ret);
    fprintf(stderr, "probe: hwdevice created\n");

    AVBufferRef *frames = av_hwframe_ctx_alloc(dev);
    if (!frames) return fail("av_hwframe_ctx_alloc", 0);

    AVHWFramesContext *fc = (AVHWFramesContext *)frames->data;
    fc->format            = AV_PIX_FMT_VIDEOTOOLBOX;
    fc->sw_format         = AV_PIX_FMT_NV12;
    fc->width             = W;
    fc->height            = H;
    fc->initial_pool_size = 8;
    ret = av_hwframe_ctx_init(frames);
    if (ret < 0) return fail("av_hwframe_ctx_init", ret);
    fprintf(stderr, "probe: hwframe pool created (nv12 %dx%d)\n", W, H);

    /* ── encoder ── */
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_videotoolbox");
    if (!codec) return fail("h264_videotoolbox not found", 0);

    AVFormatContext *fmt = NULL;
    ret = avformat_alloc_output_context2(&fmt, NULL, NULL, "/tmp/vt_probe.mp4");
    if (ret < 0) return fail("avformat_alloc_output_context2", ret);

    AVStream *st = avformat_new_stream(fmt, NULL);
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->width         = W;
    enc->height        = H;
    enc->pix_fmt       = AV_PIX_FMT_VIDEOTOOLBOX;   /* <- the thing under test */
    enc->time_base     = (AVRational){1, 1000000};
    enc->max_b_frames  = 0;
    enc->hw_frames_ctx = av_buffer_ref(frames);
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(enc->priv_data, "realtime", "1", 0);

    ret = avcodec_open2(enc, codec, NULL);
    if (ret < 0) return fail("avcodec_open2 (hw pix_fmt)", ret);
    fprintf(stderr, "probe: encoder opened with AV_PIX_FMT_VIDEOTOOLBOX\n");

    avcodec_parameters_from_context(st->codecpar, enc);
    st->time_base = enc->time_base;
    ret = avio_open(&fmt->pb, "/tmp/vt_probe.mp4", AVIO_FLAG_WRITE);
    if (ret < 0) return fail("avio_open", ret);
    ret = avformat_write_header(fmt, NULL);
    if (ret < 0) return fail("avformat_write_header", ret);

    /* CVPixelBuffer attributes matching what a capture source hands over. */
    NSDictionary *attrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey :
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferWidthKey           : @(W),
        (id)kCVPixelBufferHeightKey          : @(H),
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };

    AVPacket *pkt = av_packet_alloc();
    int pool_frames = 0, wrapped_frames = 0;

    for (int i = 0; i < NFRAMES; i++) {
        AVFrame *f = av_frame_alloc();

        if (i % 2 == 0) {
            /* Path A: allocate from libav's hardware pool. */
            ret = av_hwframe_get_buffer(frames, f, 0);
            if (ret < 0) return fail("av_hwframe_get_buffer", ret);
            paint_nv12((CVPixelBufferRef)f->data[3], i);
            pool_frames++;
        } else {
            /* Path B: wrap a CVPixelBuffer we created ourselves — this is what
               ScreenCaptureKit and a Metal render target will hand us. */
            CVPixelBufferRef pb = NULL;
            CVReturn cv = CVPixelBufferCreate(kCFAllocatorDefault, W, H,
                              kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                              (__bridge CFDictionaryRef)attrs, &pb);
            if (cv != kCVReturnSuccess || !pb) {
                fprintf(stderr, "probe: CVPixelBufferCreate failed (%d)\n", cv);
                return 1;
            }
            paint_nv12(pb, i);

            f->format        = AV_PIX_FMT_VIDEOTOOLBOX;
            f->width         = W;
            f->height        = H;
            f->data[3]       = (uint8_t *)pb;
            f->hw_frames_ctx = av_buffer_ref(frames);
            f->buf[0]        = av_buffer_create((uint8_t *)pb, sizeof(pb),
                                                release_pixbuf, pb, 0);
            if (!f->buf[0]) return fail("av_buffer_create", 0);
            wrapped_frames++;
        }

        f->pts = (int64_t)i * 1000000 / FPS;

        ret = avcodec_send_frame(enc, f);
        if (ret < 0) {
            fprintf(stderr, "probe: send_frame failed on %s frame %d\n",
                    (i % 2 == 0) ? "pooled" : "wrapped", i);
            return fail("avcodec_send_frame", ret);
        }
        av_frame_free(&f);

        while ((ret = avcodec_receive_packet(enc, pkt)) == 0) {
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            return fail("avcodec_receive_packet", ret);
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
            "probe: PASS — %d pooled + %d wrapped frames encoded\n"
            "probe: wrote /tmp/vt_probe.mp4 (play it: a moving grey band)\n",
            pool_frames, wrapped_frames);

    av_packet_free(&pkt);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    avcodec_free_context(&enc);
    av_buffer_unref(&frames);
    av_buffer_unref(&dev);
    return 0;
}
