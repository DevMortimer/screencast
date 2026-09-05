#define _GNU_SOURCE
#include "presenter.h"
#include "presenter_geometry.h"
#include "composite.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <wayland-client.h>

#define DEFAULT_SIZE 360
#define MIN_SIZE     160
#define MAX_SIZE     720
#define MARGIN        20
#define RESIZE_EDGE   16
#define MOVE_THRESHOLD 60
#define BUFFER_COUNT   3

struct Presenter;

typedef struct PresenterBuffer {
    struct PresenterBuffer *next;
    struct Presenter       *owner;
    struct wl_buffer       *wl;
    uint8_t                *data;
    size_t                  bytes;
    int                     size;
    int                     busy;
    int                     stale;
} PresenterBuffer;

typedef struct PresenterOutput {
    struct PresenterOutput *next;
    struct Presenter       *owner;
    struct wl_output       *wl;
    uint32_t                global_name;
    char                    name[128];
} PresenterOutput;

struct Presenter {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    atomic_int running;
    int event_fd;
    int init_done;
    int init_ok;

    char *output_name;
    PresenterCorner corner;
    int size;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    PresenterOutput *outputs;
    PresenterBuffer *buffers;
    int configured;

    AVFrame *pending;
    AVFrame *current;
    struct SwsContext *sws;
    uint8_t *scaled;
    int scaled_w;
    int scaled_h;
    int scaled_stride;
    float *mask;
    int render_size;

    int pointer_x;
    int pointer_y;
    int drag_start_x;
    int drag_start_y;
    int drag_last_x;
    int drag_last_y;
    int drag_start_size;
    unsigned drag_edges;
    int dragging;
};

/* ── persisted corner and size ─────────────────────────────── */

static int preferences_path(char *buf, size_t size)
{
    const char *config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (config && config[0])
        return snprintf(buf, size, "%s/screencast/presenter", config) < (int)size;
    if (home && home[0])
        return snprintf(buf, size, "%s/.config/screencast/presenter", home) <
               (int)size;
    return 0;
}

static void load_preferences(Presenter *p)
{
    p->corner = PRESENTER_BOTTOM_RIGHT;
    p->size = DEFAULT_SIZE;

    char path[512];
    if (!preferences_path(path, sizeof(path))) return;
    FILE *file = fopen(path, "r");
    if (!file) return;

    int corner, size;
    if (fscanf(file, "%d %d", &corner, &size) == 2) {
        if (corner >= PRESENTER_TOP_LEFT && corner <= PRESENTER_BOTTOM_RIGHT)
            p->corner = (PresenterCorner)corner;
        if (size >= MIN_SIZE && size <= MAX_SIZE)
            p->size = size;
    }
    fclose(file);
}

static void save_preferences(Presenter *p)
{
    char path[512];
    if (!preferences_path(path, sizeof(path))) return;

    char *slash = strrchr(path, '/');
    if (!slash) return;
    *slash = '\0';
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return;
    *slash = '/';

    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "%d %d\n", p->corner, p->size);
    fclose(file);
}

/* ── wl_shm buffers ───────────────────────────────────────── */

static void unlink_buffer(Presenter *p, PresenterBuffer *buffer)
{
    PresenterBuffer **link = &p->buffers;
    while (*link && *link != buffer) link = &(*link)->next;
    if (*link) *link = buffer->next;
}

static void destroy_buffer(PresenterBuffer *buffer)
{
    if (!buffer) return;
    Presenter *p = buffer->owner;
    unlink_buffer(p, buffer);
    if (buffer->wl) wl_buffer_destroy(buffer->wl);
    if (buffer->data && buffer->data != MAP_FAILED)
        munmap(buffer->data, buffer->bytes);
    free(buffer);
}

static void buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    PresenterBuffer *buffer = data;
    buffer->busy = 0;
    if (buffer->stale) destroy_buffer(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static PresenterBuffer *create_buffer(Presenter *p, int size)
{
    int stride = size * 4;
    size_t bytes = (size_t)stride * size;
    int fd = memfd_create("screencast-presenter", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) < 0) {
        if (fd >= 0) close(fd);
        return NULL;
    }

    uint8_t *data = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(p->shm, fd, (int)bytes);
    struct wl_buffer *wl = wl_shm_pool_create_buffer(
        pool, 0, size, size, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!wl) {
        munmap(data, bytes);
        return NULL;
    }

    PresenterBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        wl_buffer_destroy(wl);
        munmap(data, bytes);
        return NULL;
    }
    buffer->owner = p;
    buffer->wl = wl;
    buffer->data = data;
    buffer->bytes = bytes;
    buffer->size = size;
    buffer->next = p->buffers;
    p->buffers = buffer;
    wl_buffer_add_listener(wl, &buffer_listener, buffer);
    return buffer;
}

static int rebuild_buffers(Presenter *p)
{
    PresenterBuffer *buffer = p->buffers;
    while (buffer) {
        PresenterBuffer *next = buffer->next;
        buffer->stale = 1;
        if (!buffer->busy) destroy_buffer(buffer);
        buffer = next;
    }

    int made = 0;
    for (int i = 0; i < BUFFER_COUNT; i++)
        if (create_buffer(p, p->size)) made++;
    return made > 0 ? 0 : -1;
}

static PresenterBuffer *free_buffer(Presenter *p)
{
    for (PresenterBuffer *buffer = p->buffers; buffer; buffer = buffer->next)
        if (!buffer->busy && !buffer->stale && buffer->size == p->size)
            return buffer;
    return NULL;
}

/* ── frame conversion and drawing ─────────────────────────── */

static int prepare_renderer(Presenter *p, const AVFrame *frame)
{
    int size = p->size;
    double source_aspect = (double)frame->width / frame->height;
    int scaled_w = size;
    int scaled_h = size;
    if (source_aspect > 1.0)
        scaled_w = (int)ceil(size * source_aspect);
    else
        scaled_h = (int)ceil(size / source_aspect);

    p->sws = sws_getCachedContext(
        p->sws, frame->width, frame->height, frame->format,
        scaled_w, scaled_h, AV_PIX_FMT_BGRA,
        SWS_BILINEAR | SWS_ACCURATE_RND, NULL, NULL, NULL);
    if (!p->sws) return -1;

    if (p->scaled_w != scaled_w || p->scaled_h != scaled_h) {
        free(p->scaled);
        p->scaled_stride = scaled_w * 4;
        p->scaled = malloc((size_t)p->scaled_stride * scaled_h);
        if (!p->scaled) return -1;
        p->scaled_w = scaled_w;
        p->scaled_h = scaled_h;
    }

    if (p->render_size != size) {
        free(p->mask);
        p->mask = malloc((size_t)size * size * sizeof(*p->mask));
        if (!p->mask) return -1;
        composite_build_mask(p->mask, size, size, size / 8);
        p->render_size = size;
    }
    return 0;
}

static void render_frame(Presenter *p, const AVFrame *frame)
{
    if (!p->configured || !frame) return;
    PresenterBuffer *buffer = free_buffer(p);
    if (!buffer || prepare_renderer(p, frame) < 0) return;

    uint8_t *dst_data[4] = { p->scaled, NULL, NULL, NULL };
    int dst_linesize[4] = { p->scaled_stride, 0, 0, 0 };
    if (sws_scale(p->sws,
                  (const uint8_t * const *)frame->data, frame->linesize,
                  0, frame->height, dst_data, dst_linesize) <= 0)
        return;

    int crop_x = (p->scaled_w - p->size) / 2;
    int crop_y = (p->scaled_h - p->size) / 2;
    int dst_stride = p->size * 4;
    for (int y = 0; y < p->size; y++) {
        const uint8_t *src = p->scaled +
            (size_t)(crop_y + y) * p->scaled_stride + crop_x * 4;
        uint8_t *dst = buffer->data + (size_t)y * dst_stride;
        for (int x = 0; x < p->size; x++) {
            unsigned alpha = (unsigned)(p->mask[y * p->size + x] * 255.0f);
            /* Wayland ARGB8888 is BGRA in little-endian memory and requires
               premultiplied color channels. */
            dst[x * 4 + 0] = (uint8_t)(src[x * 4 + 0] * alpha / 255);
            dst[x * 4 + 1] = (uint8_t)(src[x * 4 + 1] * alpha / 255);
            dst[x * 4 + 2] = (uint8_t)(src[x * 4 + 2] * alpha / 255);
            dst[x * 4 + 3] = (uint8_t)alpha;
        }
    }

    buffer->busy = 1;
    wl_surface_attach(p->surface, buffer->wl, 0, 0);
    wl_surface_damage_buffer(p->surface, 0, 0, p->size, p->size);
    wl_surface_commit(p->surface);
}

static void draw_transparent(Presenter *p)
{
    PresenterBuffer *buffer = free_buffer(p);
    if (!buffer) return;
    memset(buffer->data, 0, buffer->bytes);
    buffer->busy = 1;
    wl_surface_attach(p->surface, buffer->wl, 0, 0);
    wl_surface_damage_buffer(p->surface, 0, 0, p->size, p->size);
    wl_surface_commit(p->surface);
}

/* ── corner anchoring and pointer interaction ─────────────── */

static void apply_layout(Presenter *p, int resize)
{
    uint32_t anchor = presenter_corner_is_right(p->corner)
        ? ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
        : ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    anchor |= presenter_corner_is_bottom(p->corner)
        ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
        : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;

    zwlr_layer_surface_v1_set_anchor(p->layer_surface, anchor);
    zwlr_layer_surface_v1_set_margin(p->layer_surface,
                                     MARGIN, MARGIN, MARGIN, MARGIN);
    zwlr_layer_surface_v1_set_size(p->layer_surface, p->size, p->size);
    if (resize && rebuild_buffers(p) == 0) {
        p->render_size = 0;
        if (p->current) render_frame(p, p->current);
        else draw_transparent(p);
    }
    wl_surface_commit(p->surface);
}

static unsigned edges_at(Presenter *p, int x, int y)
{
    unsigned edges = PRESENTER_EDGE_NONE;
    if (x <= RESIZE_EDGE) edges |= PRESENTER_EDGE_LEFT;
    if (x >= p->size - RESIZE_EDGE) edges |= PRESENTER_EDGE_RIGHT;
    if (y <= RESIZE_EDGE) edges |= PRESENTER_EDGE_TOP;
    if (y >= p->size - RESIZE_EDGE) edges |= PRESENTER_EDGE_BOTTOM;
    return edges;
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer; (void)serial; (void)surface;
    Presenter *p = data;
    p->pointer_x = wl_fixed_to_int(x);
    p->pointer_y = wl_fixed_to_int(y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer; (void)time;
    Presenter *p = data;
    p->pointer_x = wl_fixed_to_int(x);
    p->pointer_y = wl_fixed_to_int(y);
    if (p->dragging) {
        p->drag_last_x = p->pointer_x;
        p->drag_last_y = p->pointer_y;
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer; (void)serial; (void)time;
    Presenter *p = data;
    if (button != BTN_LEFT) return;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        p->dragging = 1;
        p->drag_start_x = p->drag_last_x = p->pointer_x;
        p->drag_start_y = p->drag_last_y = p->pointer_y;
        p->drag_start_size = p->size;
        p->drag_edges = edges_at(p, p->pointer_x, p->pointer_y);
        return;
    }
    if (!p->dragging) return;

    int dx = p->drag_last_x - p->drag_start_x;
    int dy = p->drag_last_y - p->drag_start_y;
    if (p->drag_edges) {
        int size = presenter_resize_size(p->drag_start_size, p->drag_edges,
                                         dx, dy, MIN_SIZE, MAX_SIZE);
        if (size != p->size) {
            p->size = size;
            apply_layout(p, 1);
        }
    } else {
        PresenterCorner corner = presenter_corner_after_drag(
            p->corner, dx, dy, MOVE_THRESHOLD);
        if (corner != p->corner) {
            p->corner = corner;
            apply_layout(p, 0);
        }
    }
    p->dragging = 0;
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer; (void)time;
    Presenter *p = data;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    int delta = wl_fixed_to_double(value) < 0 ? 24 : -24;
    int size = p->size + delta;
    if (size < MIN_SIZE) size = MIN_SIZE;
    if (size > MAX_SIZE) size = MAX_SIZE;
    if (size != p->size) {
        p->size = size;
        apply_layout(p, 1);
    }
}

/* Pointer version 5 groups every event sequence with frame and can report
   axis metadata.  Even when the presenter does not use that metadata, every
   advertised opcode needs a listener: libwayland aborts on a null callback. */
static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t source)
{
    (void)data; (void)pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities)
{
    Presenter *p = data;
    int has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_pointer && !p->pointer) {
        p->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(p->pointer, &pointer_listener, p);
    } else if (!has_pointer && p->pointer) {
        wl_pointer_release(p->pointer);
        p->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ── output and registry discovery ────────────────────────── */

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)physical_width;
    (void)physical_height; (void)subpixel; (void)make; (void)model;
    (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)flags; (void)width; (void)height;
    (void)refresh;
}

static void output_done(void *data, struct wl_output *output)
{
    (void)data; (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t scale)
{
    (void)data; (void)output; (void)scale;
}

static void output_name(void *data, struct wl_output *output, const char *name)
{
    (void)output;
    PresenterOutput *presenter_output = data;
    snprintf(presenter_output->name, sizeof(presenter_output->name), "%s", name);
}

static void output_description(void *data, struct wl_output *output,
                               const char *description)
{
    (void)data; (void)output; (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    Presenter *p = data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        uint32_t bind_version = version < 4 ? version : 4;
        p->compositor = wl_registry_bind(registry, name,
                                         &wl_compositor_interface, bind_version);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        p->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name) && !p->seat) {
        uint32_t bind_version = version < 5 ? version : 5;
        p->seat = wl_registry_bind(registry, name,
                                   &wl_seat_interface, bind_version);
        wl_seat_add_listener(p->seat, &seat_listener, p);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        PresenterOutput *output = calloc(1, sizeof(*output));
        if (!output) return;
        output->owner = p;
        output->global_name = name;
        uint32_t bind_version = version < 4 ? version : 4;
        output->wl = wl_registry_bind(registry, name,
                                      &wl_output_interface, bind_version);
        output->next = p->outputs;
        p->outputs = output;
        wl_output_add_listener(output->wl, &output_listener, output);
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        uint32_t bind_version = version < 4 ? version : 4;
        p->layer_shell = wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, bind_version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ── layer surface lifecycle ──────────────────────────────── */

static void layer_configure(void *data,
                            struct zwlr_layer_surface_v1 *layer_surface,
                            uint32_t serial, uint32_t width, uint32_t height)
{
    (void)width; (void)height;
    Presenter *p = data;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    if (!p->configured) {
        p->configured = 1;
        if (rebuild_buffers(p) == 0) draw_transparent(p);
    }
}

static void layer_closed(void *data,
                         struct zwlr_layer_surface_v1 *layer_surface)
{
    (void)layer_surface;
    Presenter *p = data;
    atomic_store(&p->running, 0);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static struct wl_output *selected_output(Presenter *p)
{
    if (!p->output_name || !p->output_name[0]) return NULL;
    for (PresenterOutput *output = p->outputs; output; output = output->next)
        if (!strcmp(output->name, p->output_name)) return output->wl;
    fprintf(stderr, "presenter: output '%s' not found; compositor will choose\n",
            p->output_name);
    return NULL;
}

static int setup_wayland(Presenter *p)
{
    p->display = wl_display_connect(NULL);
    if (!p->display) {
        fprintf(stderr, "presenter: cannot connect to Wayland display\n");
        return -1;
    }
    p->registry = wl_display_get_registry(p->display);
    wl_registry_add_listener(p->registry, &registry_listener, p);
    if (wl_display_roundtrip(p->display) < 0 ||
        wl_display_roundtrip(p->display) < 0)
        return -1;

    if (!p->compositor || !p->shm || !p->layer_shell) {
        fprintf(stderr, "presenter: compositor does not provide layer-shell\n");
        return -1;
    }

    p->surface = wl_compositor_create_surface(p->compositor);
    p->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        p->layer_shell, p->surface, selected_output(p),
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "screencast-presenter");
    if (!p->surface || !p->layer_surface) return -1;

    zwlr_layer_surface_v1_add_listener(p->layer_surface, &layer_listener, p);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        p->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_set_exclusive_zone(p->layer_surface, -1);
    apply_layout(p, 0);
    if (wl_display_roundtrip(p->display) < 0 || !p->configured) return -1;
    return 0;
}

static void cleanup_wayland(Presenter *p)
{
    while (p->buffers) destroy_buffer(p->buffers);
    if (p->pointer) wl_pointer_release(p->pointer);
    if (p->layer_surface) zwlr_layer_surface_v1_destroy(p->layer_surface);
    if (p->surface) wl_surface_destroy(p->surface);
    if (p->seat) wl_seat_release(p->seat);
    while (p->outputs) {
        PresenterOutput *output = p->outputs;
        p->outputs = output->next;
        wl_output_release(output->wl);
        free(output);
    }
    if (p->layer_shell) zwlr_layer_shell_v1_destroy(p->layer_shell);
    if (p->shm) wl_shm_destroy(p->shm);
    if (p->compositor) wl_compositor_destroy(p->compositor);
    if (p->registry) wl_registry_destroy(p->registry);
    if (p->display) wl_display_disconnect(p->display);

    sws_freeContext(p->sws);
    free(p->scaled);
    free(p->mask);
    av_frame_free(&p->current);
}

/* ── rendering thread and public interface ────────────────── */

static AVFrame *take_pending(Presenter *p)
{
    pthread_mutex_lock(&p->mutex);
    AVFrame *frame = p->pending;
    p->pending = NULL;
    pthread_mutex_unlock(&p->mutex);
    return frame;
}

static void signal_ready(Presenter *p, int ok)
{
    pthread_mutex_lock(&p->mutex);
    p->init_ok = ok;
    p->init_done = 1;
    pthread_cond_signal(&p->ready);
    pthread_mutex_unlock(&p->mutex);
}

static void *presenter_thread(void *data)
{
    Presenter *p = data;
    if (setup_wayland(p) < 0) {
        signal_ready(p, 0);
        cleanup_wayland(p);
        return NULL;
    }

    signal_ready(p, 1);
    int wayland_fd = wl_display_get_fd(p->display);
    struct pollfd fds[2] = {
        { .fd = wayland_fd, .events = POLLIN },
        { .fd = p->event_fd, .events = POLLIN },
    };

    while (atomic_load(&p->running)) {
        if (wl_display_dispatch_pending(p->display) < 0) break;
        wl_display_flush(p->display);
        int count = poll(fds, 2, 50);
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP)) break;
        if (fds[0].revents & POLLIN)
            if (wl_display_dispatch(p->display) < 0) break;
        if (fds[1].revents & POLLIN) {
            uint64_t value;
            while (read(p->event_fd, &value, sizeof(value)) < 0 &&
                   errno == EINTR) {}
            AVFrame *frame = take_pending(p);
            if (frame) {
                av_frame_free(&p->current);
                p->current = frame;
                render_frame(p, frame);
            }
        }
    }

    atomic_store(&p->running, 0);
    cleanup_wayland(p);
    return NULL;
}

Presenter *presenter_open(const char *output_name)
{
    Presenter *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    pthread_mutex_init(&p->mutex, NULL);
    pthread_cond_init(&p->ready, NULL);
    p->event_fd = eventfd(0, EFD_CLOEXEC);
    if (p->event_fd < 0) {
        presenter_close(p);
        return NULL;
    }
    if (output_name && output_name[0]) p->output_name = strdup(output_name);
    load_preferences(p);
    atomic_store(&p->running, 1);

    if (pthread_create(&p->thread, NULL, presenter_thread, p) != 0) {
        presenter_close(p);
        return NULL;
    }

    pthread_mutex_lock(&p->mutex);
    while (!p->init_done) pthread_cond_wait(&p->ready, &p->mutex);
    int ok = p->init_ok;
    pthread_mutex_unlock(&p->mutex);
    if (!ok) {
        pthread_join(p->thread, NULL);
        p->thread = 0;
        presenter_close(p);
        return NULL;
    }
    fprintf(stderr, "presenter: ready; drag to a corner, drag an edge or scroll to resize\n");
    return p;
}

void presenter_submit(Presenter *p, const AVFrame *frame)
{
    if (!p || !frame || !atomic_load(&p->running)) return;
    AVFrame *copy = av_frame_clone(frame);
    if (!copy) return;

    pthread_mutex_lock(&p->mutex);
    av_frame_free(&p->pending);
    p->pending = copy;
    pthread_mutex_unlock(&p->mutex);

    uint64_t one = 1;
    if (write(p->event_fd, &one, sizeof(one)) < 0 && errno != EAGAIN)
        (void)0;
}

void presenter_close(Presenter *p)
{
    if (!p) return;
    atomic_store(&p->running, 0);
    if (p->event_fd >= 0) {
        uint64_t one = 1;
        (void)write(p->event_fd, &one, sizeof(one));
    }
    if (p->thread) pthread_join(p->thread, NULL);
    save_preferences(p);

    pthread_mutex_lock(&p->mutex);
    av_frame_free(&p->pending);
    pthread_mutex_unlock(&p->mutex);
    if (p->event_fd >= 0) close(p->event_fd);
    free(p->output_name);
    pthread_cond_destroy(&p->ready);
    pthread_mutex_destroy(&p->mutex);
    free(p);
}
