#define _GNU_SOURCE
#include "wlcap.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <wayland-client.h>
#include <libavutil/pixfmt.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_OUTPUTS 16

typedef struct {
    struct wl_output *wl;
    char             *name;
} OutputRef;

struct WlCap {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_shm        *shm;
    struct zwlr_screencopy_manager_v1 *mgr;

    OutputRef outputs[MAX_OUTPUTS];
    int       n_outputs;

    struct wl_output *target; /* output we capture */
    int overlay_cursor;
    int fps;

    /* shm buffer (reused every grab) */
    struct wl_buffer *buffer;
    void  *data;
    size_t data_size;
    int    buf_w, buf_h, buf_stride;
    uint32_t shm_format;

    /* per-frame state */
    int frame_ready;
    int frame_failed;
    int y_invert;

    struct timespec next_deadline;
    int have_deadline;
};

/* ── wl_shm format → AVPixelFormat ─────────────────────────── */

static int shm_to_av(uint32_t fmt)
{
    switch (fmt) {
    case WL_SHM_FORMAT_ARGB8888: return AV_PIX_FMT_BGRA;
    case WL_SHM_FORMAT_XRGB8888: return AV_PIX_FMT_BGR0;
    case WL_SHM_FORMAT_ABGR8888: return AV_PIX_FMT_RGBA;
    case WL_SHM_FORMAT_XBGR8888: return AV_PIX_FMT_RGB0;
    case WL_SHM_FORMAT_RGBA8888: return AV_PIX_FMT_ABGR;
    case WL_SHM_FORMAT_RGBX8888: return AV_PIX_FMT_0BGR;
    case WL_SHM_FORMAT_BGRA8888: return AV_PIX_FMT_ARGB;
    case WL_SHM_FORMAT_BGRX8888: return AV_PIX_FMT_0RGB;
    default:
        fprintf(stderr,
                "wlcap: unexpected wl_shm format 0x%x; assuming XRGB8888\n",
                fmt);
        return AV_PIX_FMT_BGR0;
    }
}

/* ── wl_output listener (only the name matters to us) ──────── */

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t sp, const char *make,
                            const char *model, int32_t transform)
{ (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sp;(void)make;(void)model;(void)transform; }
static void output_mode(void *d, struct wl_output *o, uint32_t f, int32_t w,
                        int32_t h, int32_t r)
{ (void)d;(void)o;(void)f;(void)w;(void)h;(void)r; }
static void output_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t s)
{ (void)d;(void)o;(void)s; }

static void output_name(void *data, struct wl_output *o, const char *name)
{
    (void)o;
    OutputRef *ref = data;
    free(ref->name);
    ref->name = name ? strdup(name) : NULL;
}
static void output_description(void *d, struct wl_output *o, const char *desc)
{ (void)d;(void)o;(void)desc; }

static const struct wl_output_listener output_listener = {
    .geometry    = output_geometry,
    .mode        = output_mode,
    .done        = output_done,
    .scale       = output_scale,
    .name        = output_name,
    .description = output_description,
};

/* ── registry ──────────────────────────────────────────────── */

static void registry_global(void *data, struct wl_registry *reg, uint32_t id,
                            const char *iface, uint32_t version)
{
    WlCap *c = data;

    if (strcmp(iface, wl_shm_interface.name) == 0) {
        c->shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        uint32_t v = version < 3 ? version : 3;
        c->mgr = wl_registry_bind(reg, id,
                                  &zwlr_screencopy_manager_v1_interface, v);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (c->n_outputs >= MAX_OUTPUTS) return;
        uint32_t v = version < 4 ? version : 4; /* v4 exposes the name event */
        OutputRef *ref = &c->outputs[c->n_outputs];
        ref->wl = wl_registry_bind(reg, id, &wl_output_interface, v);
        ref->name = NULL;
        if (v >= 4)
            wl_output_add_listener(ref->wl, &output_listener, ref);
        c->n_outputs++;
    }
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t id)
{ (void)d;(void)r;(void)id; }

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ── shm buffer management ─────────────────────────────────── */

static int create_shm_buffer(WlCap *c, uint32_t format, int w, int h, int stride)
{
    size_t size = (size_t)stride * h;

    if (c->buffer && c->buf_w == w && c->buf_h == h &&
        c->buf_stride == stride && c->shm_format == format)
        return 0; /* reuse */

    if (c->buffer) {
        wl_buffer_destroy(c->buffer);
        c->buffer = NULL;
    }
    if (c->data) {
        munmap(c->data, c->data_size);
        c->data = NULL;
    }

    int fd = memfd_create("screencast-shm", MFD_CLOEXEC);
    if (fd < 0) { perror("wlcap: memfd_create"); return -1; }
    if (ftruncate(fd, (off_t)size) < 0) {
        perror("wlcap: ftruncate");
        close(fd);
        return -1;
    }

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("wlcap: mmap");
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, (int32_t)size);
    c->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (!c->buffer) {
        munmap(ptr, size);
        fprintf(stderr, "wlcap: wl_shm_pool_create_buffer failed\n");
        return -1;
    }

    c->data       = ptr;
    c->data_size  = size;
    c->buf_w      = w;
    c->buf_h      = h;
    c->buf_stride = stride;
    c->shm_format = format;
    return 0;
}

/* ── screencopy frame listener ─────────────────────────────── */

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *f,
                        uint32_t format, uint32_t w, uint32_t h, uint32_t stride)
{
    (void)f;
    WlCap *c = data;
    if (create_shm_buffer(c, format, (int)w, (int)h, (int)stride) < 0)
        c->frame_failed = 1;
}

static void frame_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f,
                               uint32_t fmt, uint32_t w, uint32_t h)
{ (void)d;(void)f;(void)fmt;(void)w;(void)h; /* shm path only */ }

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    WlCap *c = data;
    if (c->frame_failed || !c->buffer) {
        c->frame_failed = 1;
        return;
    }
    zwlr_screencopy_frame_v1_copy(f, c->buffer);
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *f,
                        uint32_t flags)
{
    (void)f;
    WlCap *c = data;
    c->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) ? 1 : 0;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *f,
                        uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
    (void)f;(void)sec_hi;(void)sec_lo;(void)nsec;
    WlCap *c = data;
    c->frame_ready = 1;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    (void)f;
    WlCap *c = data;
    c->frame_failed = 1;
}

static void frame_damage(void *d, struct zwlr_screencopy_frame_v1 *f,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)d;(void)f;(void)x;(void)y;(void)w;(void)h; }

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer       = frame_buffer,
    .flags        = frame_flags,
    .ready        = frame_ready,
    .failed       = frame_failed,
    .damage       = frame_damage,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done  = frame_buffer_done,
};

/* ── fps pacing ────────────────────────────────────────────── */

static void pace(WlCap *c)
{
    if (c->fps <= 0) return;

    long interval_ns = 1000000000L / c->fps;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!c->have_deadline) {
        c->next_deadline = now;
        c->have_deadline = 1;
    }

    long diff = (c->next_deadline.tv_sec - now.tv_sec) * 1000000000L +
                (c->next_deadline.tv_nsec - now.tv_nsec);
    if (diff > 0) {
        struct timespec sleep = { diff / 1000000000L, diff % 1000000000L };
        nanosleep(&sleep, NULL);
    } else if (diff < -interval_ns) {
        /* Fell behind; reset the cadence to avoid a burst of catch-up frames. */
        c->next_deadline = now;
    }

    c->next_deadline.tv_nsec += interval_ns;
    while (c->next_deadline.tv_nsec >= 1000000000L) {
        c->next_deadline.tv_nsec -= 1000000000L;
        c->next_deadline.tv_sec  += 1;
    }
}

/* ── public ────────────────────────────────────────────────── */

int wlcap_grab(WlCap *c)
{
    pace(c);

    c->frame_ready  = 0;
    c->frame_failed = 0;

    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(c->mgr, c->overlay_cursor,
                                                  c->target);
    if (!frame) return -1;
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, c);

    while (!c->frame_ready && !c->frame_failed) {
        if (wl_display_dispatch(c->display) < 0) {
            c->frame_failed = 1;
            break;
        }
    }

    zwlr_screencopy_frame_v1_destroy(frame);
    return c->frame_failed ? -1 : 0;
}

void *wlcap_buffer(WlCap *c) { return c ? c->data : NULL; }

WlCap *wlcap_open(const char *output_name, int overlay_cursor, int fps,
                  WlCapInfo *info)
{
    WlCap *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->overlay_cursor = overlay_cursor;
    c->fps = fps;

    c->display = wl_display_connect(NULL);
    if (!c->display) {
        fprintf(stderr, "wlcap: cannot connect to Wayland display "
                        "(is WAYLAND_DISPLAY set?)\n");
        free(c);
        return NULL;
    }

    c->registry = wl_display_get_registry(c->display);
    wl_registry_add_listener(c->registry, &registry_listener, c);
    wl_display_roundtrip(c->display); /* enumerate globals */
    wl_display_roundtrip(c->display); /* deliver wl_output name events */

    if (!c->shm || !c->mgr || c->n_outputs == 0) {
        fprintf(stderr, "wlcap: compositor lacks wlr-screencopy or wl_shm "
                        "(shm=%p mgr=%p outputs=%d)\n",
                (void *)c->shm, (void *)c->mgr, c->n_outputs);
        wlcap_close(c);
        return NULL;
    }

    /* Pick the output: match SCREENCAST_OUTPUT by name, else the first. */
    c->target = c->outputs[0].wl;
    if (output_name && output_name[0]) {
        for (int i = 0; i < c->n_outputs; i++) {
            if (c->outputs[i].name &&
                strcmp(c->outputs[i].name, output_name) == 0) {
                c->target = c->outputs[i].wl;
                break;
            }
        }
        if (c->target == c->outputs[0].wl &&
            (!c->outputs[0].name ||
             strcmp(c->outputs[0].name, output_name) != 0))
            fprintf(stderr, "wlcap: output '%s' not found; using first output\n",
                    output_name);
    }

    /* One capture to learn geometry, format and orientation. */
    if (wlcap_grab(c) < 0 || !c->data) {
        fprintf(stderr, "wlcap: initial capture failed\n");
        wlcap_close(c);
        return NULL;
    }

    if (info) {
        info->width      = c->buf_w;
        info->height     = c->buf_h;
        info->stride     = c->buf_stride;
        info->av_pix_fmt = shm_to_av(c->shm_format);
        info->y_invert   = c->y_invert;
    }
    return c;
}

void wlcap_close(WlCap *c)
{
    if (!c) return;

    if (c->buffer) wl_buffer_destroy(c->buffer);
    if (c->data)   munmap(c->data, c->data_size);
    if (c->mgr)    zwlr_screencopy_manager_v1_destroy(c->mgr);
    for (int i = 0; i < c->n_outputs; i++) {
        free(c->outputs[i].name);
        if (c->outputs[i].wl) wl_output_destroy(c->outputs[i].wl);
    }
    if (c->shm)      wl_shm_destroy(c->shm);
    if (c->registry) wl_registry_destroy(c->registry);
    if (c->display)  wl_display_disconnect(c->display);
    free(c);
}
