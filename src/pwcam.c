#define _GNU_SOURCE /* spa/utils/string.h uses newlocale()/LC_ALL_MASK */
#include "pwcam.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/utils/result.h>

#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>

/*
 * The raw pixel formats we accept from PipeWire.  Webcams commonly expose
 * planar YUV (NV12/I420) rather than packed RGB, so both are offered; the
 * frame copy below is plane-aware.  Whatever the camera picks, the existing
 * encoder swscale stage converts it into the overlay.  (Encoded formats like
 * MJPEG are not offered — we only take raw video.)
 */
static enum AVPixelFormat spa_to_av(uint32_t spa_fmt)
{
    switch (spa_fmt) {
    case SPA_VIDEO_FORMAT_NV12: return AV_PIX_FMT_NV12;
    case SPA_VIDEO_FORMAT_I420: return AV_PIX_FMT_YUV420P;
    case SPA_VIDEO_FORMAT_YUY2: return AV_PIX_FMT_YUYV422;
    case SPA_VIDEO_FORMAT_UYVY: return AV_PIX_FMT_UYVY422;
    case SPA_VIDEO_FORMAT_RGB:  return AV_PIX_FMT_RGB24;
    case SPA_VIDEO_FORMAT_BGR:  return AV_PIX_FMT_BGR24;
    case SPA_VIDEO_FORMAT_RGBx: return AV_PIX_FMT_RGB0;
    case SPA_VIDEO_FORMAT_BGRx: return AV_PIX_FMT_BGR0;
    case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
    case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
    default:                    return AV_PIX_FMT_NONE;
    }
}

struct PwCam {
    struct pw_thread_loop *loop;
    struct pw_stream      *stream;
    struct spa_hook        listener;

    int                    width, height;
    enum AVPixelFormat     av_fmt;

    /* Written on the loop thread, read under the loop lock during open. */
    int                    format_ready;
    int                    error;

    PwCamFrameFn           on_frame;
    void                  *user;
};

/* ── stream callbacks (run on the PipeWire loop thread) ─────── */

static void on_state_changed(void *data, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
    (void)old;
    struct PwCam *pw = data;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "pwcam: stream error: %s\n", error ? error : "?");
        pw->error = 1;
        pw_thread_loop_signal(pw->loop, false);
    }
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct PwCam *pw = data;
    struct spa_video_info info;

    if (param == NULL || id != SPA_PARAM_Format)
        return;

    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_video ||
        info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0)
        return;

    enum AVPixelFormat fmt = spa_to_av(info.info.raw.format);
    if (fmt == AV_PIX_FMT_NONE ||
        info.info.raw.size.width == 0 || info.info.raw.size.height == 0) {
        fprintf(stderr, "pwcam: unusable negotiated format\n");
        pw->error = 1;
        pw_thread_loop_signal(pw->loop, false);
        return;
    }

    pw->av_fmt = fmt;
    pw->width  = (int)info.info.raw.size.width;
    pw->height = (int)info.info.raw.size.height;

    /* Tell the stream what buffers we can consume: a single contiguous,
     * CPU-mappable block (MemFd/MemPtr) big enough for the whole frame.  V4L2
     * sources refuse to stream until this is negotiated. */
    int stride = av_image_get_linesize(fmt, pw->width, 0);
    int size   = av_image_get_buffer_size(fmt, pw->width, pw->height, 1);
    uint8_t bb[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(bb, sizeof(bb));
    const struct spa_pod *bufs[1];
    bufs[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers,  SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
        SPA_PARAM_BUFFERS_blocks,   SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,     SPA_POD_Int(size),
        SPA_PARAM_BUFFERS_stride,   SPA_POD_Int(stride),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
            (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    pw_stream_update_params(pw->stream, bufs, 1);

    pw->format_ready = 1;
    fprintf(stderr, "pwcam: webcam using %dx%d %s\n",
            pw->width, pw->height, av_get_pix_fmt_name(fmt));
    pw_thread_loop_signal(pw->loop, false);
}

static void on_process(void *data)
{
    struct PwCam *pw = data;
    struct pw_buffer *b;

    if ((b = pw_stream_dequeue_buffer(pw->stream)) == NULL)
        return;

    struct spa_buffer *buf = b->buffer;
    struct spa_data   *d0  = &buf->datas[0];

    if (!pw->format_ready || d0->data == NULL || d0->chunk->size == 0) {
        pw_stream_queue_buffer(pw->stream, b);
        return;
    }

    AVFrame *f = av_frame_alloc();
    if (!f) {
        pw_stream_queue_buffer(pw->stream, b);
        return;
    }
    f->format = pw->av_fmt;
    f->width  = pw->width;
    f->height = pw->height;
    if (av_frame_get_buffer(f, 32) < 0) {
        av_frame_free(&f);
        pw_stream_queue_buffer(pw->stream, b);
        return;
    }

    /* Build source plane pointers/strides, then copy respecting dst padding.
     * PipeWire may hand each plane its own data block, or (typical for V4L2
     * mmap) all planes packed into a single contiguous block. */
    uint8_t *src_data[4] = { 0 };
    int      src_ls[4]   = { 0 };
    int n_planes = av_pix_fmt_count_planes(pw->av_fmt);

    if ((int)buf->n_datas >= n_planes) {
        for (int i = 0; i < n_planes; i++) {
            struct spa_data *d = &buf->datas[i];
            src_data[i] = (uint8_t *)d->data + d->chunk->offset;
            src_ls[i]   = d->chunk->stride > 0
                          ? d->chunk->stride
                          : av_image_get_linesize(pw->av_fmt, pw->width, i);
        }
    } else {
        /* One block: derive plane offsets/strides from the geometry. */
        uint8_t *base = (uint8_t *)d0->data + d0->chunk->offset;
        av_image_fill_arrays(src_data, src_ls, base, pw->av_fmt,
                             pw->width, pw->height, 1);
    }

    av_image_copy(f->data, f->linesize,
                  (const uint8_t * const *)src_data, src_ls,
                  pw->av_fmt, pw->width, pw->height);

    pw_stream_queue_buffer(pw->stream, b);

    pw->on_frame(pw->user, f); /* ownership handed to the callback */
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process       = on_process,
};

/* ── public ────────────────────────────────────────────────── */

static pthread_once_t s_pw_init_once = PTHREAD_ONCE_INIT;
static void pw_init_once(void) { pw_init(NULL, NULL); }

PwCam *pwcam_open(const char *target, int want_w, int want_h, int want_fps,
                  PwCamInfo *info, PwCamFrameFn on_frame, void *user)
{
    pthread_once(&s_pw_init_once, pw_init_once);

    int w   = want_w   > 0 ? want_w   : 1280;
    int h   = want_h   > 0 ? want_h   : 720;
    int fps = want_fps > 0 ? want_fps : 30;

    struct PwCam *pw = calloc(1, sizeof(*pw));
    if (!pw) return NULL;
    pw->on_frame = on_frame;
    pw->user     = user;
    pw->av_fmt   = AV_PIX_FMT_NONE;

    pw->loop = pw_thread_loop_new("pwcam", NULL);
    if (!pw->loop) { free(pw); return NULL; }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,     "Camera",
        NULL);
    if (props && target && target[0] && strcmp(target, "auto") != 0)
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);

    pw->stream = pw_stream_new_simple(pw_thread_loop_get_loop(pw->loop),
                                      "screencast-webcam", props,
                                      &stream_events, pw);
    if (!pw->stream) { pwcam_close(pw); return NULL; }

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(11,
            SPA_VIDEO_FORMAT_NV12,          /* preferred */
            SPA_VIDEO_FORMAT_NV12, SPA_VIDEO_FORMAT_I420,
            SPA_VIDEO_FORMAT_YUY2, SPA_VIDEO_FORMAT_UYVY,
            SPA_VIDEO_FORMAT_RGB,  SPA_VIDEO_FORMAT_BGR,
            SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE((uint32_t)w, (uint32_t)h),
            &SPA_RECTANGLE(1, 1),
            &SPA_RECTANGLE(4096, 4096)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION((uint32_t)fps, 1),
            &SPA_FRACTION(1, 1),
            &SPA_FRACTION(1000, 1)));

    if (pw_thread_loop_start(pw->loop) < 0) { pwcam_close(pw); return NULL; }

    pw_thread_loop_lock(pw->loop);
    int cret = pw_stream_connect(pw->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                 PW_STREAM_FLAG_AUTOCONNECT |
                                 PW_STREAM_FLAG_MAP_BUFFERS,
                                 params, 1);
    if (cret < 0) {
        pw_thread_loop_unlock(pw->loop);
        fprintf(stderr, "pwcam: connect failed: %s\n", spa_strerror(cret));
        pwcam_close(pw);
        return NULL;
    }

    /* Wait for the format to negotiate (or a camera to appear).  A missing
     * camera never signals, so the timed wait bounds the stall. */
    while (!pw->format_ready && !pw->error) {
        if (pw_thread_loop_timed_wait(pw->loop, 4) != 0)
            break; /* timeout */
    }
    int ok = pw->format_ready && !pw->error;
    pw_thread_loop_unlock(pw->loop);

    if (!ok) {
        fprintf(stderr, "pwcam: no camera stream available\n");
        pwcam_close(pw);
        return NULL;
    }

    info->width  = pw->width;
    info->height = pw->height;
    info->av_fmt = pw->av_fmt;
    return pw;
}

void pwcam_close(PwCam *pw)
{
    if (!pw) return;
    if (pw->loop)   pw_thread_loop_stop(pw->loop);
    if (pw->stream) pw_stream_destroy(pw->stream);
    if (pw->loop)   pw_thread_loop_destroy(pw->loop);
    free(pw);
}
