#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>
#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>

#include "hotkeys.h"
#include "capture.h"
#include "encoder.h"
#include "composite.h"

/* ── configuration ─────────────────────────────────────────── */

#define FPS         30
#define SCREEN_DEV  ":0.0"
#define WEBCAM_DEV  "/dev/video0"
#define AUDIO_DEV   "default"

/* ── shared state ──────────────────────────────────────────── */

/*
 * Latest decoded webcam frame.  Updated by the webcam thread, read by the
 * record thread.  Protected by s_cam_mutex.
 */
static pthread_mutex_t s_cam_mutex  = PTHREAD_MUTEX_INITIALIZER;
static AVFrame        *s_cam_latest = NULL;

/* Signal between main and capture threads: 1 while file is open. */
static atomic_int s_rec_open = 0;

/* ── recording bundle ──────────────────────────────────────── */

typedef struct {
    CaptureCtx screen_cap;
    CaptureCtx cam_cap;
    CaptureCtx aud_cap;
    EncoderCtx enc;
    int mon_x, mon_y;       /* active monitor origin */
    int canvas_w, canvas_h; /* active monitor size   */
    int cam_w, cam_h;
    int has_cam;
    int has_aud;
} RecCtx;

static RecCtx s_rec;

/* ── helper: find which monitor the pointer is on ──────────── */

static void get_active_monitor(int *out_x, int *out_y, int *out_w, int *out_h)
{
    *out_x = 0; *out_y = 0; *out_w = 1920; *out_h = 1080;

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return;

    Screen *scr = DefaultScreenOfDisplay(dpy);
    *out_w = WidthOfScreen(scr);
    *out_h = HeightOfScreen(scr);

    Window root = DefaultRootWindow(dpy);
    Window d1, d2;
    int ptr_x, ptr_y, d3, d4;
    unsigned int d5;
    XQueryPointer(dpy, root, &d1, &d2, &ptr_x, &ptr_y, &d3, &d4, &d5);

    int n;
    XineramaScreenInfo *screens = XineramaQueryScreens(dpy, &n);
    if (screens) {
        for (int i = 0; i < n; i++) {
            int sx = screens[i].x_org, sy = screens[i].y_org;
            int sw = screens[i].width, sh = screens[i].height;
            if (ptr_x >= sx && ptr_x < sx + sw &&
                ptr_y >= sy && ptr_y < sy + sh) {
                *out_x = sx; *out_y = sy;
                *out_w = sw; *out_h = sh;
                break;
            }
        }
        XFree(screens);
    }
    XCloseDisplay(dpy);
}

/* ── helper: build timestamped output path ─────────────────── */

static void make_output_path(char *buf, size_t n)
{
    time_t t       = time(NULL);
    struct tm *tm  = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    snprintf(buf, n, "%s/screencast_%s.mp4", getenv("HOME"), ts);
}

/* ── webcam thread ─────────────────────────────────────────── */

static void *webcam_thread(void *arg)
{
    CaptureCtx *cap = (CaptureCtx *)arg;
    while (atomic_load(&g_running) && atomic_load(&s_rec_open)) {
        if (capture_read(cap) < 0) continue;

        AVFrame *copy = av_frame_alloc();
        av_frame_ref(copy, cap->frame);

        pthread_mutex_lock(&s_cam_mutex);
        if (s_cam_latest) av_frame_free(&s_cam_latest);
        s_cam_latest = copy;
        pthread_mutex_unlock(&s_cam_mutex);
    }
    return NULL;
}

/* ── audio thread ──────────────────────────────────────────── */

typedef struct { CaptureCtx *cap; EncoderCtx *enc; } AudioArg;

static void *audio_thread(void *arg)
{
    AudioArg *a = (AudioArg *)arg;
    while (atomic_load(&g_running) && atomic_load(&s_rec_open)) {
        if (capture_read(a->cap) < 0) continue;
        encoder_feed_audio(a->enc, a->cap->frame);
    }
    return NULL;
}

/* ── open one recording session ────────────────────────────── */

static int recording_open(void)
{
    get_active_monitor(&s_rec.mon_x, &s_rec.mon_y,
                       &s_rec.canvas_w, &s_rec.canvas_h);

    char out_path[512];
    make_output_path(out_path, sizeof(out_path));
    printf("[REC] Opening: %s  monitor=%dx%d+%d+%d\n",
           out_path, s_rec.canvas_w, s_rec.canvas_h, s_rec.mon_x, s_rec.mon_y);

    if (capture_screen_open(&s_rec.screen_cap, SCREEN_DEV,
                             s_rec.mon_x, s_rec.mon_y,
                             s_rec.canvas_w, s_rec.canvas_h, FPS) < 0) {
        fprintf(stderr, "main: screen capture failed\n");
        return -1;
    }

    /* Webcam is optional — continue without it if missing */
    s_rec.has_cam = 0;
    s_rec.cam_w   = 0;
    s_rec.cam_h   = 0;
    if (capture_webcam_open(&s_rec.cam_cap, WEBCAM_DEV,
                             &s_rec.cam_w, &s_rec.cam_h) == 0) {
        s_rec.has_cam = 1;
    } else {
        fprintf(stderr, "main: webcam not available — display+audio only\n");
        capture_free(&s_rec.cam_cap);
    }

    s_rec.has_aud = 0;
    if (capture_audio_open(&s_rec.aud_cap, AUDIO_DEV) == 0) {
        s_rec.has_aud = 1;
    } else {
        fprintf(stderr, "main: audio not available — video only\n");
        capture_free(&s_rec.aud_cap);
    }

    enum AVPixelFormat cam_fmt = s_rec.has_cam
                                 ? s_rec.cam_cap.pix_fmt
                                 : AV_PIX_FMT_NONE;
    int audio_sr = s_rec.has_aud ? s_rec.aud_cap.sample_rate : 0;
    const AVChannelLayout *audio_ch = s_rec.has_aud ? &s_rec.aud_cap.ch_layout : NULL;
    enum AVSampleFormat audio_fmt = s_rec.has_aud ? s_rec.aud_cap.sample_fmt : AV_SAMPLE_FMT_NONE;

    if (encoder_open(&s_rec.enc, out_path,
                     s_rec.canvas_w, s_rec.canvas_h, FPS,
                     s_rec.screen_cap.pix_fmt,
                     s_rec.cam_w, s_rec.cam_h, cam_fmt,
                     audio_sr, audio_ch, audio_fmt) < 0) {
        fprintf(stderr, "main: encoder open failed\n");
        capture_free(&s_rec.screen_cap);
        if (s_rec.has_cam) capture_free(&s_rec.cam_cap);
        if (s_rec.has_aud) capture_free(&s_rec.aud_cap);
        return -1;
    }

    atomic_store(&s_rec_open, 1);
    return 0;
}

/* ── record loop: runs until g_recording → 0 ──────────────── */

static void recording_loop(void)
{
    pthread_t cam_tid = 0, aud_tid = 0;
    AudioArg aud_arg  = { &s_rec.aud_cap, &s_rec.enc };

    if (s_rec.has_cam)
        pthread_create(&cam_tid, NULL, webcam_thread, &s_rec.cam_cap);
    if (s_rec.has_aud)
        pthread_create(&aud_tid, NULL, audio_thread, &aud_arg);

    while (atomic_load(&g_running) && atomic_load(&g_recording)) {
        /* av_read_frame blocks ≈1/fps seconds — acceptable stop latency */
        if (capture_read(&s_rec.screen_cap) < 0) continue;

        int mode = atomic_load(&g_mode);

        /* Grab a reference to the latest webcam frame if needed */
        AVFrame *cam_copy = NULL;
        if ((mode == MODE_WEBCAM || mode == MODE_BOTH) && s_rec.has_cam) {
            pthread_mutex_lock(&s_cam_mutex);
            if (s_cam_latest) {
                cam_copy = av_frame_alloc();
                av_frame_ref(cam_copy, s_cam_latest);
            }
            pthread_mutex_unlock(&s_cam_mutex);
        }

        encoder_write_video(&s_rec.enc, mode,
                            s_rec.screen_cap.frame, cam_copy);

        if (cam_copy) av_frame_free(&cam_copy);
    }

    atomic_store(&s_rec_open, 0);
    if (cam_tid) pthread_join(cam_tid, NULL);
    if (aud_tid) pthread_join(aud_tid, NULL);
}

/* ── close / flush one recording session ───────────────────── */

static void recording_close(void)
{
    /* Give capture threads up to 50 ms to drain their current read */
    struct timespec ts = { .tv_nsec = 50000000L };
    nanosleep(&ts, NULL);

    encoder_flush(&s_rec.enc);
    encoder_free(&s_rec.enc);
    capture_free(&s_rec.screen_cap);
    if (s_rec.has_cam) capture_free(&s_rec.cam_cap);
    if (s_rec.has_aud) capture_free(&s_rec.aud_cap);

    pthread_mutex_lock(&s_cam_mutex);
    if (s_cam_latest) { av_frame_free(&s_cam_latest); s_cam_latest = NULL; }
    pthread_mutex_unlock(&s_cam_mutex);

    printf("[REC] Stopped.\n");
}

/* ── hotkey thread wrapper ─────────────────────────────────── */

static void *hotkey_thread(void *arg)
{
    (void)arg;
    hotkeys_run();
    return NULL;
}

/* ── entry point ───────────────────────────────────────────── */

static void sig_stop(int sig)
{
    (void)sig;
    atomic_store(&g_recording, 0);
    atomic_store(&g_running,   0);
}

int main(void)
{
    signal(SIGINT,  sig_stop);
    signal(SIGTERM, sig_stop);

    av_log_set_level(AV_LOG_ERROR);

    if (hotkeys_init() < 0) return 1;

    pthread_t hk_tid;
    pthread_create(&hk_tid, NULL, hotkey_thread, NULL);

    struct timespec poll = { .tv_nsec = 20000000L }; /* 20 ms */

    while (atomic_load(&g_running)) {
        if (atomic_load(&g_recording)) {
            /*
             * Open a fresh MP4, run the encode loop until g_recording drops,
             * then flush and close.  If the user presses another mode key
             * during close, the next iteration opens a new file.
             */
            if (recording_open() == 0) {
                printf("[REC] Mode %d active.\n", atomic_load(&g_mode));
                recording_loop();
            } else {
                atomic_store(&g_recording, 0);
            }
            recording_close();
        } else {
            nanosleep(&poll, NULL);
        }
    }

    /* Make sure we close any session left open by a rapid Win+Esc */
    if (atomic_load(&s_rec_open)) {
        atomic_store(&g_recording, 0);
        recording_close();
    }

    pthread_join(hk_tid, NULL);
    hotkeys_cleanup();
    printf("[INFO] Exited cleanly.\n");
    return 0;
}
