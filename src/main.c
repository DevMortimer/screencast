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
#include "pwcam.h"
#include "encoder.h"
#include "composite.h"
#include "mixer.h"
#include "arbiter.h"

/* ── configuration ─────────────────────────────────────────── */

#define FPS         30
#define WEBCAM_DEV  "auto"
#define AUDIO_DEV   "default"
/* Default sink's monitor — desktop audio, resolved server-side by Pulse/PipeWire. */
#define DESKTOP_DEV "@DEFAULT_MONITOR@"

/*
 * Which output to capture.  SCREENCAST_OUTPUT wins; otherwise ask niri for the
 * focused output so we record the monitor the user is actually on.  Returns
 * NULL if neither is available, and wlcap falls back to the first output.
 */
static const char *screen_output(void)
{
    static char buf[128];

    const char *env = getenv("SCREENCAST_OUTPUT");
    if (env && *env) return env;

    FILE *fp = popen("niri msg --json focused-output 2>/dev/null", "r");
    if (!fp) return NULL;

    char json[1024];
    size_t n = fread(json, 1, sizeof(json) - 1, fp);
    pclose(fp);
    if (n == 0) return NULL;
    json[n] = '\0';

    /* Pull the value of the first "name":"…" pair. */
    const char *key = strstr(json, "\"name\"");
    if (!key) return NULL;
    const char *colon = strchr(key, ':');
    if (!colon) return NULL;
    const char *q1 = strchr(colon, '"');
    if (!q1) return NULL;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return NULL;

    size_t len = (size_t)(q2 - q1 - 1);
    if (len == 0 || len >= sizeof(buf)) return NULL;
    memcpy(buf, q1 + 1, len);
    buf[len] = '\0';
    return buf;
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
    PwCam     *pwcam;      /* webcam via PipeWire; NULL when the node is not held */
    CaptureCtx mic_cap;    /* microphone (default source) */
    CaptureCtx desk_cap;   /* desktop audio (default sink's monitor) */
    EncoderCtx enc;
    MixerCtx  *mixer;      /* mixes mic + desktop into one track */
    int canvas_w, canvas_h; /* captured output size */
    int cam_w, cam_h;
    enum AVPixelFormat cam_fmt; /* webcam frame format negotiated by PipeWire */
    int has_cam;           /* 1 while the camera node is held (pwcam != NULL) */
    int has_mic;
    int has_desktop;
    int has_aud;           /* has_mic || has_desktop */
    /* Cooperative-capture arbiter + camera-node acquisition bookkeeping. */
    ArbiterState arb;
    RecordMode   last_requested;  /* mode the previous loop tick saw */
    int64_t      next_cam_retry;  /* av_gettime_relative() deadline for a retry */
    char capture_path[512];
    char final_path[512];
} RecCtx;

/* Bounded auto-recovery cadence: how often a still-pending webcam request
 * re-attempts to acquire the camera node.  A few seconds, never per-frame. */
#define CAM_RETRY_US 3000000LL

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

/* One-line "[REC] <label>: <rate> Hz, <ch>, <fmt>" for an opened audio source;
 * dev, when non-NULL, is appended in parentheses (the desktop monitor name). */
static void report_audio_source(const char *label, const CaptureCtx *cap,
                                const char *dev)
{
    printf("[REC] %s: %d Hz, %d channel%s, %s",
           label, cap->sample_rate, cap->ch_layout.nb_channels,
           cap->ch_layout.nb_channels == 1 ? "" : "s",
           av_get_sample_fmt_name(cap->sample_fmt));
    if (dev) printf(" (%s)", dev);
    putchar('\n');
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

/* ── webcam frames (delivered by the PipeWire capture thread) ─ */

/*
 * Publish one webcam frame into the shared latest-frame slot the record loop
 * reads.  Ownership of `frame` passes to us; it replaces the previous latest.
 */
static void cam_frame_cb(void *user, AVFrame *frame)
{
    (void)user;
    pthread_mutex_lock(&s_cam_mutex);
    if (s_cam_latest) av_frame_free(&s_cam_latest);
    s_cam_latest = frame;
    atomic_fetch_add(&s_cam_seq, 1);
    pthread_mutex_unlock(&s_cam_mutex);
}

/* ── camera-node acquisition wrapper (thin: available/unavailable) ─ */

/*
 * Try to acquire the camera node.  Returns CAP_AVAILABLE (storing the handle
 * and negotiated geometry) or CAP_UNAVAILABLE if it is busy or absent — the two
 * are indistinguishable with try-open detection and resolve the same way.  If
 * the node is already held, reports CAP_AVAILABLE without reopening.  This is
 * the only place a real camera open happens; the arbiter never calls it.
 */
static Availability cam_acquire(void)
{
    if (s_rec.pwcam) return CAP_AVAILABLE;

    int cam_w_hint = 0, cam_h_hint = 0, cam_fps_hint = 0;
    const char *size = getenv("SCREENCAST_CAM_SIZE");
    if (size && size[0]) sscanf(size, "%dx%d", &cam_w_hint, &cam_h_hint);
    const char *fps = getenv("SCREENCAST_CAM_FPS");
    if (fps && fps[0]) cam_fps_hint = atoi(fps);

    PwCamInfo info;
    PwCam *pw = pwcam_open(get_webcam_device(),
                           cam_w_hint, cam_h_hint, cam_fps_hint,
                           &info, cam_frame_cb, NULL);
    if (!pw) return CAP_UNAVAILABLE;

    s_rec.pwcam   = pw;
    s_rec.cam_w   = info.width;
    s_rec.cam_h   = info.height;
    s_rec.cam_fmt = info.av_fmt;
    s_rec.has_cam = 1;
    return CAP_AVAILABLE;
}

/* Release the camera node (handing it back to any meeting) and drop any frame
 * still buffered, so a stale frame cannot be composited after a later re-open.
 * Idempotent. */
static void cam_release(void)
{
    if (!s_rec.pwcam) return;
    /* Stop the PipeWire thread first so no late cam_frame_cb races the free. */
    pwcam_close(s_rec.pwcam);
    s_rec.pwcam   = NULL;
    s_rec.has_cam = 0;
    pthread_mutex_lock(&s_cam_mutex);
    if (s_cam_latest) av_frame_free(&s_cam_latest);
    s_cam_latest = NULL;
    pthread_mutex_unlock(&s_cam_mutex);
}

/*
 * Report webcam availability for one record-loop tick, honouring the bounded
 * auto-recovery cadence.  When the webcam is not wanted, no probe happens (the
 * arbiter ignores the verdict and will release any held node).  When it is
 * wanted and already held, it is available.  When wanted but not held, an open
 * is attempted immediately as the desire first appears, then only every
 * CAM_RETRY_US — never per frame, so a declined request never busy-spins.
 */
static Availability webcam_verdict(RecordMode requested, int64_t now)
{
    if (!arbiter_wants_webcam(requested))
        return CAP_UNAVAILABLE;
    if (s_rec.pwcam)
        return CAP_AVAILABLE;

    /* Attempt immediately the moment the desire first appears (entering a
     * webcam-wanting mode from one that wasn't), then only every CAM_RETRY_US.
     * Toggling between two webcam-wanting modes keeps the existing backoff, so
     * a declined request never turns into a per-frame open. */
    if (!arbiter_wants_webcam(s_rec.last_requested))
        s_rec.next_cam_retry = now;
    if (now < s_rec.next_cam_retry)
        return CAP_UNAVAILABLE;           /* backing off between retries */
    s_rec.next_cam_retry = now + CAM_RETRY_US;
    return cam_acquire();
}

/*
 * Act on one arbiter plan: engage or release the webcam compositing to match
 * the plan's active set, and emit any state-change notifications through the
 * existing control_notify path.
 */
static void apply_plan(const ArbiterPlan *p)
{
    if (p->active.webcam) {
        if (encoder_set_webcam(&s_rec.enc,
                               s_rec.cam_w, s_rec.cam_h, s_rec.cam_fmt) < 0) {
            /* Could not build the compositing chain — hand the node back so the
             * next tick declines cleanly to display rather than a black frame. */
            cam_release();
        }
    } else if (s_rec.pwcam) {
        cam_release();
        encoder_clear_webcam(&s_rec.enc);
    }

    if (p->notes & ARB_NOTE_WEBCAM_UNAVAILABLE)
        control_notify("Screencast webcam unavailable",
                       "Camera in use — recording the display");
    if (p->notes & ARB_NOTE_WEBCAM_ENGAGED)
        control_notify("Screencast webcam engaged", NULL);
}

/* ── audio threads ─────────────────────────────────────────── */

/* Mixed audio is delivered here and handed to the encoder's audio path. */
static void mixer_sink_encode(void *user, AVFrame *mixed)
{
    encoder_feed_audio((EncoderCtx *)user, mixed);
}

typedef struct { CaptureCtx *cap; MixerCtx *mixer; MixSource src;
                 const char *label; } AudioArg;

/* Consecutive failed reads before a source is declared dead and dropped. */
#define AUDIO_MAX_FAILS 40

/*
 * One audio source's capture loop.  A source that dies mid-recording (device
 * busy or unplugged) is dropped cleanly rather than fought over: on a run of
 * persistent read errors we back off (no tight retry), and once the run passes
 * AUDIO_MAX_FAILS we drop the source from the mix and exit.  The mixed track
 * keeps flowing over whatever sources remain.
 */
static void *audio_thread(void *arg)
{
    AudioArg *a = (AudioArg *)arg;
    struct timespec backoff = { .tv_nsec = 100000000L }; /* 100 ms */
    int fails = 0;

    while (atomic_load(&g_running) && atomic_load(&s_rec_open)) {
        int ret = capture_read(a->cap);
        if (ret == AVERROR_EXIT)
            break;                       /* interrupted for a normal stop */
        if (ret < 0) {
            if (++fails >= AUDIO_MAX_FAILS) {
                fprintf(stderr, "main: %s audio source died — dropping it "
                                "from the mix\n", a->label);
                mixer_drop_source(a->mixer, a->src);
                break;
            }
            nanosleep(&backoff, NULL);   /* bounded backoff, never busy-spin */
            continue;
        }
        fails = 0;
        mixer_feed(a->mixer, a->src, a->cap->frame,
                   a->cap->sample_rate, &a->cap->ch_layout, a->cap->sample_fmt);
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
    capture_set_interrupt(&s_rec.mic_cap, recording_interrupted, NULL);
    capture_set_interrupt(&s_rec.desk_cap, recording_interrupted, NULL);

    if (capture_screen_open(&s_rec.screen_cap, screen_output(), FPS) < 0) {
        fprintf(stderr, "main: screen capture failed\n");
        return -1;
    }
    s_rec.canvas_w = s_rec.screen_cap.width;
    s_rec.canvas_h = s_rec.screen_cap.height;

    printf("[REC] Capture: %s  output=%dx%d\n",
           s_rec.capture_path, s_rec.canvas_w, s_rec.canvas_h);
    printf("[REC] Final:   %s\n", s_rec.final_path);

    /*
     * The webcam is engaged cooperatively and dynamically, not opened here.
     * display holds no camera node; webcam/both acquires it only when free,
     * and the record loop (via the arbiter) drives acquire/release/retry.
     */
    arbiter_init(&s_rec.arb);
    s_rec.has_cam        = 0;
    s_rec.pwcam          = NULL;
    s_rec.cam_w          = 0;
    s_rec.cam_h          = 0;
    s_rec.cam_fmt        = AV_PIX_FMT_NONE;
    s_rec.last_requested = MODE_IDLE;  /* forces an immediate first attempt */
    s_rec.next_cam_retry = 0;

    /* Microphone — the default source. Best-effort. */
    s_rec.has_mic = 0;
    if (capture_audio_open(&s_rec.mic_cap, AUDIO_DEV) == 0) {
        s_rec.has_mic = 1;
        report_audio_source("Microphone", &s_rec.mic_cap, NULL);
    } else {
        fprintf(stderr, "main: microphone not available\n");
        capture_free(&s_rec.mic_cap);
    }

    /* Desktop audio — the default sink's monitor. On by default; best-effort. */
    s_rec.has_desktop = 0;
    const char *desktop_env = getenv("SCREENCAST_DESKTOP_AUDIO");
    int want_desktop = !(desktop_env && strcmp(desktop_env, "0") == 0);
    if (want_desktop) {
        const char *desktop_dev = env_default("SCREENCAST_DESKTOP_DEV", DESKTOP_DEV);
        if (capture_audio_open_monitor(&s_rec.desk_cap, desktop_dev) == 0) {
            s_rec.has_desktop = 1;
            report_audio_source("Desktop audio", &s_rec.desk_cap, desktop_dev);
        } else {
            fprintf(stderr, "main: desktop audio not available (%s)\n", desktop_dev);
            capture_free(&s_rec.desk_cap);
        }
    }

    s_rec.has_aud = s_rec.has_mic || s_rec.has_desktop;

    /* Build the mixer over whichever sources opened. */
    s_rec.mixer = NULL;
    if (s_rec.has_aud) {
        int active[MIX_SRC_COUNT] = {
            [MIX_SRC_MIC]     = s_rec.has_mic,
            [MIX_SRC_DESKTOP] = s_rec.has_desktop,
        };
        s_rec.mixer = mixer_create(active, mixer_sink_encode, &s_rec.enc);
        if (!s_rec.mixer) {
            fprintf(stderr, "main: mixer init failed — recording video only\n");
            if (s_rec.has_mic)     capture_free(&s_rec.mic_cap);
            if (s_rec.has_desktop) capture_free(&s_rec.desk_cap);
            s_rec.has_mic = s_rec.has_desktop = s_rec.has_aud = 0;
        }
    }

    const char *audio_sources =
        !s_rec.has_aud                     ? "none (video only)"
        : s_rec.has_mic && s_rec.has_desktop ? "mic + desktop (mixed)"
        : s_rec.has_mic                    ? "mic only"
                                           : "desktop only";
    fprintf(stderr, "[REC] Audio: %s\n", audio_sources);
    control_notify("Screencast audio", audio_sources);

    /* The encoder is fed the mixer's canonical format, not any raw source. */
    AVChannelLayout mix_ch;
    int audio_sr = 0;
    const AVChannelLayout *audio_ch = NULL;
    enum AVSampleFormat audio_fmt = AV_SAMPLE_FMT_NONE;
    if (s_rec.has_aud) {
        audio_sr = MIX_SAMPLE_RATE;
        av_channel_layout_default(&mix_ch, MIX_CHANNELS);
        audio_ch = &mix_ch;
        audio_fmt = AV_SAMPLE_FMT_FLTP;
    }

    /* The webcam is not an output stream — it is composited onto the canvas and
     * engaged/released mid-recording via encoder_set_webcam/clear_webcam. */
    int enc_ret = encoder_open(&s_rec.enc, s_rec.capture_path,
                     s_rec.canvas_w, s_rec.canvas_h, FPS,
                     s_rec.screen_cap.pix_fmt,
                     audio_sr, audio_ch, audio_fmt);
    if (s_rec.has_aud) av_channel_layout_uninit(&mix_ch);

    if (enc_ret < 0) {
        fprintf(stderr, "main: encoder open failed\n");
        capture_free(&s_rec.screen_cap);
        cam_release();
        if (s_rec.has_mic)     capture_free(&s_rec.mic_cap);
        if (s_rec.has_desktop) capture_free(&s_rec.desk_cap);
        if (s_rec.mixer) { mixer_destroy(s_rec.mixer); s_rec.mixer = NULL; }
        return -1;
    }

    atomic_store(&s_rec_open, 1);
    return 0;
}

/* ── record loop: runs until g_recording → 0 ──────────────── */

static void recording_loop(void)
{
    pthread_t mic_tid = 0, desk_tid = 0;
    AudioArg mic_arg  = { &s_rec.mic_cap,  s_rec.mixer, MIX_SRC_MIC,     "microphone" };
    AudioArg desk_arg = { &s_rec.desk_cap, s_rec.mixer, MIX_SRC_DESKTOP, "desktop" };
    int64_t last_webcam_seq = -1;
    struct timespec cam_poll = { .tv_nsec = 1000000L };

    /* The webcam has no thread here: while the camera node is held, the
     * PipeWire capture thread pushes frames into s_cam_latest via cam_frame_cb.
     * The node is acquired/released dynamically below, driven by the arbiter. */
    if (s_rec.has_mic)
        pthread_create(&mic_tid, NULL, audio_thread, &mic_arg);
    if (s_rec.has_desktop)
        pthread_create(&desk_tid, NULL, audio_thread, &desk_arg);

    while (atomic_load(&g_running) && atomic_load(&g_recording)) {
        RecordMode requested = atomic_load(&g_mode);
        int64_t    now       = av_gettime_relative();

        /* Decide the effective plan from real availability, then make the
         * camera node / compositing match it. */
        CaptureAvail avail = {
            .webcam  = webcam_verdict(requested, now),
            .mic     = s_rec.has_mic     ? CAP_AVAILABLE : CAP_UNAVAILABLE,
            .desktop = s_rec.has_desktop ? CAP_AVAILABLE : CAP_UNAVAILABLE,
        };
        ArbiterPlan plan = arbiter_step(&s_rec.arb, requested, avail);
        apply_plan(&plan);
        s_rec.last_requested = requested;

        int mode = plan.effective;   /* what can actually be recorded now */
        AVFrame *cam_copy = NULL;
        int64_t cam_seq = -1;

        if (mode == MODE_WEBCAM && s_rec.pwcam) {
            /*
             * Webcam-only recordings should be paced by the camera.  Waiting
             * on the screen source here limits webcam-only output to the
             * screen capture rate even though the screen frame is ignored.
             * Re-check the requested mode so a switch away breaks out promptly.
             */
            while (atomic_load(&g_running) && atomic_load(&g_recording) &&
                   atomic_load(&g_mode) == (int)requested) {
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
                continue;   /* mode changed or stopping → re-evaluate the plan */
        } else {
            /* screen grab blocks ≈1/fps seconds — acceptable stop latency */
            if (capture_read(&s_rec.screen_cap) < 0) continue;

            if (mode == MODE_BOTH && s_rec.pwcam) {
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
    if (mic_tid)  pthread_join(mic_tid, NULL);
    if (desk_tid) pthread_join(desk_tid, NULL);
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
    if (s_rec.mixer) { mixer_destroy(s_rec.mixer); s_rec.mixer = NULL; }
    capture_free(&s_rec.screen_cap);
    /* Release the camera node if still held (stops the PipeWire thread first,
     * so no late cam_frame_cb races the free, and drops any buffered frame). */
    cam_release();
    if (s_rec.has_mic)     capture_free(&s_rec.mic_cap);
    if (s_rec.has_desktop) capture_free(&s_rec.desk_cap);

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
        "  display   record the screen + audio (mic + desktop)\n"
        "  webcam    record the webcam + audio (mic + desktop)\n"
        "  both      record screen + webcam overlay + audio (mic + desktop)\n"
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
