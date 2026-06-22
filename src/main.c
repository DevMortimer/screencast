#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/shape.h>
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
#define WEBCAM_DEV  "auto"
#define AUDIO_DEV   "default"

/* ── shared state ──────────────────────────────────────────── */

/*
 * Latest decoded webcam frame.  Updated by the webcam thread, read by the
 * record thread.  Protected by s_cam_mutex.
 */
static pthread_mutex_t s_cam_mutex  = PTHREAD_MUTEX_INITIALIZER;
static AVFrame        *s_cam_latest = NULL;
static atomic_llong    s_cam_seq    = 0;

/* Signal between main and capture threads: 1 while file is open. */
static atomic_int s_rec_open = 0;

static int recording_interrupted(void *opaque)
{
    (void)opaque;
    return !atomic_load(&g_running) || !atomic_load(&g_recording);
}

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
    char capture_path[512];
    char final_path[512];
} RecCtx;

static RecCtx s_rec;

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
} RecIndicator;

static RecIndicator s_rec_indicator;

/* ── live recording indicator ─────────────────────────────── */

static void indicator_close(void)
{
    if (!s_rec_indicator.dpy) return;

    if (s_rec_indicator.gc)
        XFreeGC(s_rec_indicator.dpy, s_rec_indicator.gc);
    if (s_rec_indicator.win)
        XDestroyWindow(s_rec_indicator.dpy, s_rec_indicator.win);
    XCloseDisplay(s_rec_indicator.dpy);
    memset(&s_rec_indicator, 0, sizeof(s_rec_indicator));
}

static int indicator_open(int mon_x, int mon_y, int canvas_w, int canvas_h)
{
    (void)canvas_h;

    indicator_close();

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return -1;

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int diameter = 24;
    int x = mon_x + canvas_w - diameter - 12;
    int y = mon_y + 12;

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.background_pixel = BlackPixel(dpy, screen);
    attrs.event_mask = ExposureMask;

    Window win = XCreateWindow(dpy, root, x, y,
                               (unsigned int)diameter,
                               (unsigned int)diameter,
                               0, CopyFromParent, InputOutput,
                               CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &attrs);
    if (!win) {
        XCloseDisplay(dpy);
        return -1;
    }

    int shape_event = 0, shape_error = 0;
    if (XShapeQueryExtension(dpy, &shape_event, &shape_error)) {
        Pixmap mask = XCreatePixmap(dpy, win,
                                    (unsigned int)diameter,
                                    (unsigned int)diameter, 1);
        GC mask_gc = XCreateGC(dpy, mask, 0, NULL);
        XSetForeground(dpy, mask_gc, 0);
        XFillRectangle(dpy, mask, mask_gc, 0, 0,
                       (unsigned int)diameter,
                       (unsigned int)diameter);
        XSetForeground(dpy, mask_gc, 1);
        XFillArc(dpy, mask, mask_gc, 0, 0,
                 (unsigned int)diameter,
                 (unsigned int)diameter, 0, 360 * 64);
        XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);
#ifdef ShapeInput
        XSetForeground(dpy, mask_gc, 0);
        XFillRectangle(dpy, mask, mask_gc, 0, 0,
                       (unsigned int)diameter,
                       (unsigned int)diameter);
        XShapeCombineMask(dpy, win, ShapeInput, 0, 0, mask, ShapeSet);
#endif
        XFreeGC(dpy, mask_gc);
        XFreePixmap(dpy, mask);
    }

    GC gc = XCreateGC(dpy, win, 0, NULL);
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor red, exact;
    unsigned long red_pixel = 0xff0000;
    if (XAllocNamedColor(dpy, cmap, "red", &red, &exact))
        red_pixel = red.pixel;

    XSetForeground(dpy, gc, red_pixel);
    XFillArc(dpy, win, gc, 0, 0,
             (unsigned int)diameter,
             (unsigned int)diameter, 0, 360 * 64);
    XMapRaised(dpy, win);
    XFlush(dpy);

    s_rec_indicator.dpy = dpy;
    s_rec_indicator.win = win;
    s_rec_indicator.gc = gc;
    return 0;
}

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

static void make_output_paths(char *final_buf, size_t final_n,
                              char *capture_buf, size_t capture_n)
{
    time_t t       = time(NULL);
    struct tm *tm  = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    snprintf(final_buf, final_n, "%s/screencast_%s.mp4", getenv("HOME"), ts);
    snprintf(capture_buf, capture_n, "%s/screencast_%s_capture.mp4",
             getenv("HOME"), ts);
}

static const char *get_webcam_device(void)
{
    const char *dev = getenv("SCREENCAST_WEBCAM_DEV");
    return (dev && dev[0]) ? dev : WEBCAM_DEV;
}

static const char *env_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value && value[0]) ? value : fallback;
}

static int wait_for_child(pid_t pid, int *status, const char *label)
{
    for (;;) {
        pid_t ret = waitpid(pid, status, 0);
        if (ret == pid)
            return 0;

        if (ret < 0 && errno == EINTR)
            continue;

        if (ret < 0)
            fprintf(stderr, "main: wait %s: %s\n", label, strerror(errno));
        else
            fprintf(stderr, "main: wait %s: unexpected pid %ld\n",
                    label, (long)ret);
        return -1;
    }
}

static int child_exited_successfully(int status)
{
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void log_child_status(const char *label, int status)
{
    if (WIFEXITED(status)) {
        fprintf(stderr, "main: %s exited with status %d\n",
                label, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "main: %s terminated by signal %d",
                label, WTERMSIG(status));
#ifdef WCOREDUMP
        if (WCOREDUMP(status))
            fprintf(stderr, " (core dumped)");
#endif
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "main: %s ended with unexpected wait status 0x%x\n",
                label, status);
    }
}

static void send_render_notification(const char *summary, const char *body)
{
    pid_t pid = fork();
    if (pid < 0) return;

    if (pid == 0) {
        execlp("notify-send", "notify-send",
               "-a", "screencast", summary, body, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    wait_for_child(pid, &status, "notify-send");
}

static int final_video_is_valid(const char *final_path)
{
    struct stat st;
    if (stat(final_path, &st) < 0 || st.st_size <= 0)
        return 0;

    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input(&fmt, final_path, NULL, NULL);
    if (ret < 0)
        return 0;

    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        avformat_close_input(&fmt);
        return 0;
    }

    int valid = 0;
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters *par = fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
            par->codec_id != AV_CODEC_ID_NONE &&
            par->width > 0 && par->height > 0) {
            valid = 1;
            break;
        }
    }

    avformat_close_input(&fmt);
    return valid;
}

static int run_final_render(const char *capture_path, const char *final_path)
{
    const char *preset   = env_default("SCREENCAST_NVENC_FINAL_PRESET", "p7");
    const char *cq       = env_default("SCREENCAST_NVENC_FINAL_CQ", "16");
    const char *lookahead= env_default("SCREENCAST_NVENC_FINAL_LOOKAHEAD", "32");
    const char *aq       = env_default("SCREENCAST_NVENC_FINAL_AQ", "10");

    printf("[REC] Rendering final: %s\n", final_path);

    pid_t pid = fork();
    if (pid < 0) {
        perror("main: fork ffmpeg");
        return -1;
    }

    if (pid == 0) {
        execlp("ffmpeg", "ffmpeg",
               "-hide_banner", "-loglevel", "error", "-y",
               "-hwaccel", "cuda",
               "-hwaccel_output_format", "cuda",
               "-i", capture_path,
               "-map", "0",
               "-c:v", "h264_nvenc",
               "-preset", preset,
               "-tune", "hq",
               "-profile:v", "high",
               "-rc", "vbr",
               "-cq:v", cq,
               "-b:v", "0",
               "-rc-lookahead:v", lookahead,
               "-spatial-aq:v", "1",
               "-temporal-aq:v", "1",
               "-aq-strength:v", aq,
               "-multipass:v", "fullres",
               "-bf:v", "3",
               "-b_ref_mode:v", "middle",
               "-colorspace", "bt709",
               "-color_primaries", "bt709",
               "-color_trc", "bt709",
               "-c:a", "copy",
               "-movflags", "+faststart",
               final_path,
               (char *)NULL);
        perror("main: exec ffmpeg");
        _exit(127);
    }

    int status = 0;
    if (wait_for_child(pid, &status, "ffmpeg") < 0)
        return -1;

    if (!child_exited_successfully(status)) {
        log_child_status("ffmpeg", status);
        if (final_video_is_valid(final_path)) {
            fprintf(stderr,
                    "main: ffmpeg did not report success but final video validates: %s\n",
                    final_path);
            return 0;
        }

        fprintf(stderr,
                "main: final render failed; intermediate kept at %s\n",
                capture_path);
        return -1;
    }

    return 0;
}

static int render_final_video(const char *capture_path, const char *final_path)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("main: fork render worker");
        return -1;
    }

    if (pid == 0) {
        if (setsid() < 0)
            perror("main: setsid render worker");

        int ret = run_final_render(capture_path, final_path);
        if (ret == 0) {
            const char *keep = getenv("SCREENCAST_KEEP_CAPTURE");
            if (!keep || !keep[0])
                unlink(capture_path);

            send_render_notification("Screencast ready", final_path);
        } else {
            send_render_notification("Screencast render failed", capture_path);
        }

        _exit(ret == 0 ? 0 : 1);
    }

    printf("[REC] Final render running in background (pid %ld): %s\n",
           (long)pid, final_path);
    return 0;
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
        atomic_fetch_add(&s_cam_seq, 1);
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
    s_rec.capture_path[0] = '\0';
    s_rec.final_path[0] = '\0';

    make_output_paths(s_rec.final_path, sizeof(s_rec.final_path),
                      s_rec.capture_path, sizeof(s_rec.capture_path));
    printf("[REC] Capture: %s  monitor=%dx%d+%d+%d\n",
           s_rec.capture_path, s_rec.canvas_w, s_rec.canvas_h,
           s_rec.mon_x, s_rec.mon_y);
    printf("[REC] Final:   %s\n", s_rec.final_path);

    capture_set_interrupt(&s_rec.screen_cap, recording_interrupted, NULL);
    capture_set_interrupt(&s_rec.cam_cap, recording_interrupted, NULL);
    capture_set_interrupt(&s_rec.aud_cap, recording_interrupted, NULL);

    if (indicator_open(s_rec.mon_x, s_rec.mon_y,
                       s_rec.canvas_w, s_rec.canvas_h) < 0)
        fprintf(stderr, "main: live recording indicator unavailable\n");

    if (capture_screen_open(&s_rec.screen_cap, SCREEN_DEV,
                             s_rec.mon_x, s_rec.mon_y,
                             s_rec.canvas_w, s_rec.canvas_h, FPS) < 0) {
        fprintf(stderr, "main: screen capture failed\n");
        indicator_close();
        return -1;
    }

    /* Webcam is optional — continue without it if missing */
    s_rec.has_cam = 0;
    s_rec.cam_w   = 0;
    s_rec.cam_h   = 0;
    if (capture_webcam_open(&s_rec.cam_cap, get_webcam_device(),
                             &s_rec.cam_w, &s_rec.cam_h) == 0) {
        s_rec.has_cam = 1;
    } else {
        fprintf(stderr, "main: webcam not available — display+audio only\n");
        capture_free(&s_rec.cam_cap);
    }

    s_rec.has_aud = 0;
    if (capture_audio_open(&s_rec.aud_cap, AUDIO_DEV) == 0) {
        s_rec.has_aud = 1;
        printf("[REC] Audio source: %d Hz, %d channel%s, %s\n",
               s_rec.aud_cap.sample_rate,
               s_rec.aud_cap.ch_layout.nb_channels,
               s_rec.aud_cap.ch_layout.nb_channels == 1 ? "" : "s",
               av_get_sample_fmt_name(s_rec.aud_cap.sample_fmt));
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

    if (encoder_open(&s_rec.enc, s_rec.capture_path,
                     s_rec.canvas_w, s_rec.canvas_h, FPS,
                     s_rec.screen_cap.pix_fmt,
                     s_rec.cam_w, s_rec.cam_h, cam_fmt,
                     audio_sr, audio_ch, audio_fmt) < 0) {
        fprintf(stderr, "main: encoder open failed\n");
        indicator_close();
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
    int64_t last_webcam_seq = -1;
    struct timespec cam_poll = { .tv_nsec = 1000000L };

    if (s_rec.has_cam)
        pthread_create(&cam_tid, NULL, webcam_thread, &s_rec.cam_cap);
    if (s_rec.has_aud)
        pthread_create(&aud_tid, NULL, audio_thread, &aud_arg);

    while (atomic_load(&g_running) && atomic_load(&g_recording)) {
        int mode = atomic_load(&g_mode);
        AVFrame *cam_copy = NULL;
        int64_t cam_seq = -1;

        if (mode == MODE_WEBCAM && s_rec.has_cam) {
            /*
             * Webcam-only recordings should be paced by the camera.  Waiting
             * on x11grab here limits webcam-only output to the screen capture
             * rate even though the screen frame is ignored.
             */
            while (atomic_load(&g_running) && atomic_load(&g_recording)) {
                pthread_mutex_lock(&s_cam_mutex);
                int64_t latest_seq = atomic_load(&s_cam_seq);
                if (s_cam_latest && latest_seq != last_webcam_seq) {
                    cam_copy = av_frame_alloc();
                    av_frame_ref(cam_copy, s_cam_latest);
                    cam_seq = latest_seq;
                    last_webcam_seq = latest_seq;
                }
                pthread_mutex_unlock(&s_cam_mutex);

                if (cam_copy)
                    break;
                nanosleep(&cam_poll, NULL);
            }

            if (!cam_copy)
                continue;
        } else {
            /* av_read_frame blocks ≈1/fps seconds — acceptable stop latency */
            if (capture_read(&s_rec.screen_cap) < 0) continue;

            if (mode == MODE_BOTH && s_rec.has_cam) {
                pthread_mutex_lock(&s_cam_mutex);
                if (s_cam_latest) {
                    cam_copy = av_frame_alloc();
                    av_frame_ref(cam_copy, s_cam_latest);
                    cam_seq = atomic_load(&s_cam_seq);
                }
                pthread_mutex_unlock(&s_cam_mutex);
            }
        }

        encoder_write_video(&s_rec.enc, mode,
                            s_rec.screen_cap.frame, cam_copy, cam_seq);

        if (cam_copy) av_frame_free(&cam_copy);
    }

    atomic_store(&s_rec_open, 0);
    if (cam_tid) pthread_join(cam_tid, NULL);
    if (aud_tid) pthread_join(aud_tid, NULL);
}

/* ── close / flush one recording session ───────────────────── */

static void recording_close(void)
{
    int should_render = s_rec.enc.header_written &&
                        s_rec.capture_path[0] &&
                        s_rec.final_path[0];

    indicator_close();

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

    printf("[REC] Capture stopped.\n");
    if (should_render)
        render_final_video(s_rec.capture_path, s_rec.final_path);
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
