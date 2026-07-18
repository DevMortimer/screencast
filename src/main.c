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

#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>

#include "control.h"
#include "capture.h"
#include "encoder.h"
#include "composite.h"

/* ── configuration ─────────────────────────────────────────── */

#define FPS         30
#define WEBCAM_DEV  "auto"
#define AUDIO_DEV   "default"

/* Empty/NULL selects the first Wayland output. */
static const char *screen_output(void)
{
    const char *o = getenv("SCREENCAST_OUTPUT");
    return (o && *o) ? o : NULL;
}

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
    int canvas_w, canvas_h; /* captured output size */
    int cam_w, cam_h;
    int has_cam;
    int has_aud;
    char capture_path[512];
    char final_path[512];
} RecCtx;

static RecCtx s_rec;

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
    int wait_ok = (wait_for_child(pid, &status, "ffmpeg") == 0);

    if (!wait_ok || !child_exited_successfully(status)) {
        if (wait_ok)
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

        /* The parent may run with SIGCHLD=SIG_IGN (inherited from the desktop
         * launcher) which causes waitpid() to return ECHILD immediately instead
         * of blocking.  Reset to SIG_DFL so we can properly wait for ffmpeg. */
        signal(SIGCHLD, SIG_DFL);

        int ret = run_final_render(capture_path, final_path);
        if (ret == 0) {
            const char *keep = getenv("SCREENCAST_KEEP_CAPTURE");
            if (!keep || !keep[0])
                unlink(capture_path);

            control_notify("Screencast ready", final_path);
        } else {
            control_notify("Screencast render failed", capture_path);
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
    s_rec.capture_path[0] = '\0';
    s_rec.final_path[0] = '\0';

    make_output_paths(s_rec.final_path, sizeof(s_rec.final_path),
                      s_rec.capture_path, sizeof(s_rec.capture_path));

    capture_set_interrupt(&s_rec.screen_cap, recording_interrupted, NULL);
    capture_set_interrupt(&s_rec.cam_cap, recording_interrupted, NULL);
    capture_set_interrupt(&s_rec.aud_cap, recording_interrupted, NULL);

    if (capture_screen_open(&s_rec.screen_cap, screen_output(), FPS) < 0) {
        fprintf(stderr, "main: screen capture failed\n");
        return -1;
    }
    s_rec.canvas_w = s_rec.screen_cap.width;
    s_rec.canvas_h = s_rec.screen_cap.height;

    printf("[REC] Capture: %s  output=%dx%d\n",
           s_rec.capture_path, s_rec.canvas_w, s_rec.canvas_h);
    printf("[REC] Final:   %s\n", s_rec.final_path);

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
             * on the screen source here limits webcam-only output to the
             * screen capture rate even though the screen frame is ignored.
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
            /* screen grab blocks ≈1/fps seconds — acceptable stop latency */
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

/* ── entry point ───────────────────────────────────────────── */

static void sig_stop(int sig)
{
    (void)sig;
    atomic_store(&g_recording, 0);
    atomic_store(&g_running,   0);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <display|webcam|both|stop>\n"
        "\n"
        "  display   record the screen + microphone\n"
        "  webcam    record the webcam + microphone\n"
        "  both      record screen + webcam overlay + microphone\n"
        "  stop      stop the running recorder and render the final file\n"
        "\n"
        "The first record command starts a background daemon; later commands\n"
        "switch its mode over the control socket.  Bind these to compositor\n"
        "keys (niri): Mod+Shift+D / W / B to record, Mod+Escape to stop.\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "display";

    /* If a daemon is already running, this invocation is just a controller. */
    if (control_client_send(cmd) == 0)
        return 0;

    /* No daemon running. */
    if (control_is_stop(cmd))
        return 0; /* nothing to stop */

    int mode = control_parse_mode(cmd);
    if (mode < 0) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT,  sig_stop);
    signal(SIGTERM, sig_stop);

    av_log_set_level(AV_LOG_ERROR);

    atomic_store(&g_mode,      mode);
    atomic_store(&g_recording, 1);
    atomic_store(&g_running,   1);

    if (control_server_start() < 0)
        return 1;

    control_notify("Screencast recording", control_mode_label(mode));

    struct timespec poll = { .tv_nsec = 20000000L }; /* 20 ms */

    while (atomic_load(&g_running)) {
        if (atomic_load(&g_recording)) {
            /*
             * Open a fresh MP4, run the encode loop until g_recording drops,
             * then flush and close.  Mode changes are applied live inside the
             * loop, so one file spans the whole daemon lifetime.
             */
            if (recording_open() == 0) {
                printf("[REC] Mode %d active.\n", atomic_load(&g_mode));
                recording_loop();
            } else {
                /* Capture could not start — don't spin re-opening it. */
                atomic_store(&g_recording, 0);
                atomic_store(&g_running, 0);
            }
            recording_close();
        } else {
            nanosleep(&poll, NULL);
        }
    }

    /* Make sure we close any session left open by a rapid stop */
    if (atomic_load(&s_rec_open)) {
        atomic_store(&g_recording, 0);
        recording_close();
    }

    control_notify("Screencast stopped", NULL);
    control_server_stop();
    printf("[INFO] Exited cleanly.\n");
    return 0;
}
