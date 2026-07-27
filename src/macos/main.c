#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
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
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>

#include "control.h"
#include "encoder.h"
#include "mixer.h"
#include "sck_capture.h"
#include "avf_camera.h"
#include "avf_mic.h"
#include "metal_compositor.h"
#include <CoreVideo/CoreVideo.h>

/* ── configuration ─────────────────────────────────────────── */

#define FPS         30
#define WEBCAM_DEV  "auto"

/* ── shared state ──────────────────────────────────────────── */

/*
 * Recent webcam frames, kept with their timestamps.
 *
 * In the webcam modes the camera *is* the video clock: one output frame per
 * camera frame, stamped with that frame's own capture time.  So this ring only
 * ever has to hold the frames that arrived while the loop was busy encoding the
 * previous one — the depth is slack, not history.
 *
 * The screen used to be the clock, and the pairing ran the other way: hold a
 * screen frame back and wait for the camera to catch up with it.  That wait
 * cost a quarter second of latency per frame and, worse, made the picture
 * hostage to a source that stops entirely — see the record loop.
 */
#define CAM_RING_DEPTH   24              /* ~800 ms at 30 fps */

static pthread_mutex_t s_cam_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cam_cond  = PTHREAD_COND_INITIALIZER;
static void           *s_cam_ring[CAM_RING_DEPTH];   /* CVPixelBufferRef */
static int64_t         s_cam_ring_pts[CAM_RING_DEPTH];
static int             s_cam_ring_head;
static int             s_cam_ring_count;

/*
 * Recent screen frames, most recent last.
 *
 * Deep enough to cover how far the camera runs behind the display — the loop
 * pairs a camera frame with the screen frame captured alongside it, and the
 * screen frame it wants is therefore always a little older than the newest one
 * in hand.  Six frames is ~200 ms at 30 fps, comfortably past any camera's lag.
 */
#define SCREEN_HOLD 6

static void    *s_scr_ring[SCREEN_HOLD];   /* CVPixelBufferRef */
static int64_t  s_scr_pts[SCREEN_HOLD];
static int      s_scr_head;
static int      s_scr_count;

/*
 * Correction for cameras that timestamp on arrival rather than on exposure.
 *
 * USB cameras generally stamp a buffer when the host receives it, by which
 * point the image is already however old the sensor readout and transport
 * made it.  There is no way to interrogate a UVC device for that figure, so
 * it is a manual constant: record a hand clap, find the offset between the
 * sound and the picture, and set it once for that camera.
 */
static int64_t s_cam_offset_us;

/* Signal between main and capture threads: 1 while file is open. */
static atomic_int s_rec_open = 0;

/* ── recording bundle ──────────────────────────────────────── */

typedef struct {
    SckCapture *sck;           /* screen + desktop audio */
    AvfCamera  *avf_cam;       /* webcam (always on, no arbiter) */
    AvfMic     *avf_mic;       /* microphone */
    EncoderCtx enc;
    MixerCtx  *mixer;          /* mixes mic + desktop into one track */
    MetalCompositor *comp;     /* GPU compositing; NULL in display-only mode */
    int canvas_w, canvas_h;
    int cam_w, cam_h;
    enum AVPixelFormat cam_fmt;
    int cam_active;            /* 1 when webcam compositing is engaged */
    int has_mic;
    int has_desktop;
    int has_aud;               /* has_mic || has_desktop */
    long mic_frames;           /* diagnostics: frames handed to the mixer */
    long desk_frames;
    int64_t t0_us;             /* session clock origin (host clock) */
    atomic_llong audio_anchor_us; /* timeline position of the first audio sample */
    char output_path[512];     /* ~/Movies/screencast_YYYYMMDD_HHMMSS.mp4 */
} RecCtx;

static RecCtx s_rec;

/*
 * Record where the audio track begins on the session timeline.
 *
 * Whichever source delivers first wins, and the value never moves afterwards:
 * the encoder anchors the track once and then counts samples, so a later
 * update would be a discontinuity rather than a correction.
 */
static void note_audio_anchor(int64_t pts_us)
{
    int64_t unset = -1;
    if (pts_us < 0) pts_us = 0;
    atomic_compare_exchange_strong(&s_rec.audio_anchor_us, &unset, pts_us);
}

/* ── helper: build timestamped output path ─────────────────── */

static void make_output_path(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    snprintf(buf, n, "%s/Movies/screencast_%s.mp4", getenv("HOME"), ts);
}

/* ── webcam frame callback (AVFoundation delivery queue) ───── */

static void cam_frame_cb(void *user, void *pixbuf, int64_t pts_us)
{
    (void)user;
    pthread_mutex_lock(&s_cam_mutex);

    if (s_cam_ring_count >= CAM_RING_DEPTH) {
        avf_camera_release_frame(s_cam_ring[s_cam_ring_head]);
        s_cam_ring[s_cam_ring_head] = NULL;
        s_cam_ring_head = (s_cam_ring_head + 1) % CAM_RING_DEPTH;
        s_cam_ring_count--;
    }

    int tail = (s_cam_ring_head + s_cam_ring_count) % CAM_RING_DEPTH;
    s_cam_ring[tail]     = pixbuf;   /* ownership taken */
    s_cam_ring_pts[tail] = pts_us - s_cam_offset_us;
    s_cam_ring_count++;

    pthread_cond_signal(&s_cam_cond);
    pthread_mutex_unlock(&s_cam_mutex);
}

static void cam_ring_clear(void)
{
    pthread_mutex_lock(&s_cam_mutex);
    for (int i = 0; i < CAM_RING_DEPTH; i++) {
        avf_camera_release_frame(s_cam_ring[i]);
        s_cam_ring[i] = NULL;
    }
    s_cam_ring_head = s_cam_ring_count = 0;
    pthread_mutex_unlock(&s_cam_mutex);
}

/* How long the record loop waits on the camera before giving up on this tick
   and letting the screen carry the picture instead. */
#define CAM_WAIT_MS 250

/*
 * Take the newest webcam frame captured after `after_us`, waiting up to
 * CAM_WAIT_MS for one to arrive.  Ownership of the returned buffer passes to
 * the caller; every frame older than it is released here.
 *
 * *Newest*, not oldest: this is the elastic-video rule applied to the camera.
 * When the loop keeps up there is exactly one new frame per call and the choice
 * is moot; when it falls behind, encoding the backlog in order would only sink
 * it further, so the intervening frames are dropped and the timeline stays
 * honest — the returned frame carries its own capture time, which is what the
 * output is stamped with.
 */
static void *cam_take_latest(int64_t after_us, int64_t *out_pts)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += CAM_WAIT_MS * 1000000L;
    deadline.tv_sec  += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;

    void *out = NULL;
    pthread_mutex_lock(&s_cam_mutex);

    for (;;) {
        int newest = s_cam_ring_count > 0
            ? (s_cam_ring_head + s_cam_ring_count - 1) % CAM_RING_DEPTH : -1;
        if (newest >= 0 && s_cam_ring_pts[newest] > after_us) {
            out = s_cam_ring[newest];
            if (out_pts) *out_pts = s_cam_ring_pts[newest];
            s_cam_ring[newest] = NULL;
            /* Everything still in the ring predates the frame being taken. */
            for (int i = 0; i < s_cam_ring_count - 1; i++) {
                int idx = (s_cam_ring_head + i) % CAM_RING_DEPTH;
                avf_camera_release_frame(s_cam_ring[idx]);
                s_cam_ring[idx] = NULL;
            }
            s_cam_ring_head = s_cam_ring_count = 0;
            break;
        }

        if (!atomic_load(&g_running) || !atomic_load(&s_rec_open)) break;
        if (pthread_cond_timedwait(&s_cam_cond, &s_cam_mutex, &deadline)
                == ETIMEDOUT)
            break;
    }

    pthread_mutex_unlock(&s_cam_mutex);
    return out;
}

/* ── screen frame ring ─────────────────────────────────────── */

/* Takes ownership of `pixbuf`, evicting the oldest frame when full. */
static void screen_ring_push(void *pixbuf, int64_t pts_us)
{
    if (s_scr_count >= SCREEN_HOLD) {
        sck_capture_release_frame(s_scr_ring[s_scr_head]);
        s_scr_ring[s_scr_head] = NULL;
        s_scr_head = (s_scr_head + 1) % SCREEN_HOLD;
        s_scr_count--;
    }
    int tail = (s_scr_head + s_scr_count) % SCREEN_HOLD;
    s_scr_ring[tail] = pixbuf;
    s_scr_pts[tail]  = pts_us;
    s_scr_count++;
}

static void screen_ring_clear(void)
{
    for (int i = 0; i < SCREEN_HOLD; i++) {
        sck_capture_release_frame(s_scr_ring[i]);
        s_scr_ring[i] = NULL;
    }
    s_scr_head = s_scr_count = 0;
}

/*
 * The screen frame captured closest in time to `target_us`, borrowed — the ring
 * keeps the reference, so the caller must not release it and must be done with
 * it before the next push.  NULL only when nothing has been captured yet.
 */
static void *screen_ring_nearest(int64_t target_us)
{
    void *best = NULL;
    int64_t best_dist = INT64_MAX;

    for (int i = 0; i < s_scr_count; i++) {
        int idx = (s_scr_head + i) % SCREEN_HOLD;
        int64_t d = s_scr_pts[idx] - target_us;
        if (d < 0) d = -d;
        if (d < best_dist) { best_dist = d; best = s_scr_ring[idx]; }
    }
    return best;
}

/* Newest screen frame in the ring, borrowed, with its timestamp. */
static void *screen_ring_newest(int64_t *out_pts)
{
    if (s_scr_count == 0) return NULL;
    int idx = (s_scr_head + s_scr_count - 1) % SCREEN_HOLD;
    if (out_pts) *out_pts = s_scr_pts[idx];
    return s_scr_ring[idx];
}

/* ── mixed audio delivered to the encoder ──────────────────── */

static void mixer_sink_encode(void *user, AVFrame *mixed)
{
    encoder_feed_audio((EncoderCtx *)user, mixed,
                       atomic_load(&s_rec.audio_anchor_us));
}

/* ── microphone audio thread ───────────────────────────────── */

/* Consecutive failed reads before a source is declared dead and dropped. */
#define AUDIO_MAX_FAILS 40

/*
 * Reads microphone frames in a dedicated thread and feeds them into the mixer.
 * avf_mic already outputs 48 kHz stereo FLTP — the mixer's canonical format —
 * so the internal resampler is a no-op.  If the mic device dies mid-recording,
 * we drop it from the mix after a backoff run so the mixed track keeps flowing.
 */
static void *mic_thread(void *arg)
{
    RecCtx *rec = arg;
    struct timespec backoff = { .tv_nsec = 100000000L }; /* 100 ms */
    int fails = 0;

    while (atomic_load(&g_running) && atomic_load(&s_rec_open)) {
        int64_t pts_us = 0;
        AVFrame *f = avf_mic_read(rec->avf_mic, &pts_us);
        if (!f) {
            /* NULL can mean timeout (still recording) or stop. */
            if (!atomic_load(&g_running) || !atomic_load(&s_rec_open))
                break;
            if (++fails >= AUDIO_MAX_FAILS) {
                fprintf(stderr, "main: microphone died — dropping from mix\n");
                mixer_drop_source(rec->mixer, MIX_SRC_MIC);
                break;
            }
            nanosleep(&backoff, NULL);
            continue;
        }
        fails = 0;
        rec->mic_frames++;
        note_audio_anchor(pts_us);
        mixer_feed(rec->mixer, MIX_SRC_MIC, f,
                   MIX_SAMPLE_RATE,
                   &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
                   AV_SAMPLE_FMT_FLTP, pts_us);
        av_frame_free(&f);
    }
    return NULL;
}

/* ── desktop audio thread ──────────────────────────────────── */

/*
 * Drains system audio from the ScreenCaptureKit stream.
 *
 * This has to be its own thread.  It used to run inline in the record loop,
 * pulling audio only when a video frame arrived — which was survivable when
 * every frame was delivered, but the capture layer now skips frames while the
 * screen is static.  On a talking-head screencast that is most of them, so
 * audio would sit in the capture buffer growing unboundedly and then arrive in
 * one lump whenever the picture finally changed.
 */
static void *desktop_audio_thread(void *arg)
{
    RecCtx *rec = arg;
    struct timespec tick = { .tv_nsec = 10000000L }; /* 10 ms */

    while (atomic_load(&g_running) && atomic_load(&s_rec_open)) {
        int64_t pts_us = 0;
        AVFrame *f = sck_capture_grab_audio(rec->sck, &pts_us);
        if (!f) { nanosleep(&tick, NULL); continue; }

        rec->desk_frames++;
        note_audio_anchor(pts_us);
        mixer_feed(rec->mixer, MIX_SRC_DESKTOP, f,
                   MIX_SAMPLE_RATE,
                   &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
                   AV_SAMPLE_FMT_FLTP, pts_us);
        av_frame_free(&f);
    }
    return NULL;
}

/* ── open one recording session ────────────────────────────── */

static int recording_open(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    make_output_path(s_rec.output_path, sizeof(s_rec.output_path));

    /* Open screen capture + desktop audio */
    SckCaptureInfo sck_info;
    s_rec.sck = sck_capture_open(&sck_info);
    if (!s_rec.sck) {
        fprintf(stderr, "main: screen capture failed\n");
        return -1;
    }
    s_rec.canvas_w = sck_info.width;
    s_rec.canvas_h = sck_info.height;
    s_rec.has_desktop = (sck_info.sample_rate > 0 && sck_info.channels > 0);

    /* Respect SCREENCAST_DESKTOP_AUDIO=0 */
    const char *desktop_env = getenv("SCREENCAST_DESKTOP_AUDIO");
    if (desktop_env && strcmp(desktop_env, "0") == 0)
        s_rec.has_desktop = 0;

    printf("[REC] Output: %s\n", s_rec.output_path);
    printf("[REC] Capture: %dx%d BGRA\n", s_rec.canvas_w, s_rec.canvas_h);

    /* Open webcam (always on — AVFoundation fans out, no arbiter needed) */
    s_rec.cam_active = 0;
    s_rec.cam_w = 0;
    s_rec.cam_h = 0;
    s_rec.cam_fmt = AV_PIX_FMT_NONE;

    /*
     * Default webcam: 1920x1080.
     *
     * This is the largest size external UVC cameras deliver as raw NV12 —
     * above it they switch to MJPEG, which costs a JPEG decode on every single
     * frame of the recording.  1080p is generous for the ≤480px overlay in
     * `both` mode and adequate when the webcam fills the canvas, and it stays
     * in the pixel format the pipeline wants, so it needs no decode at all.
     *
     * The format is fixed for the whole session rather than varied per mode:
     * changing it means restarting the capture device, which would blank the
     * camera and break the timeline every time the mode hotkey is pressed.
     *
     * Override with SCREENCAST_CAM_SIZE=WxH.  Asking for 3840x2160 will
     * select MJPEG and pay for the decode.
     */
    int cam_w_hint = 1920, cam_h_hint = 1080, cam_fps_hint = 30;
    const char *size = getenv("SCREENCAST_CAM_SIZE");
    if (size && size[0]) sscanf(size, "%dx%d", &cam_w_hint, &cam_h_hint);
    const char *fps = getenv("SCREENCAST_CAM_FPS");
    if (fps && fps[0]) cam_fps_hint = atoi(fps);

    /* NOTE: SCREENCAST_WEBCAM_DEV is intentionally ignored on macOS —
       AVFoundation fans out natively so camera selection isn't needed. */
    AvfCameraInfo cam_info;
    s_rec.avf_cam = avf_camera_open(WEBCAM_DEV, cam_w_hint, cam_h_hint,
                                    cam_fps_hint, &cam_info,
                                    cam_frame_cb, NULL);
    if (s_rec.avf_cam) {
        s_rec.cam_w = cam_info.width;
        s_rec.cam_h = cam_info.height;
        s_rec.cam_fmt = cam_info.pix_fmt;
        printf("[REC] Webcam: %dx%d %s\n", cam_info.width, cam_info.height,
               av_get_pix_fmt_name(cam_info.pix_fmt));
    } else {
        fprintf(stderr, "main: webcam not available — recording without it\n");
    }

    /* Open microphone */
    s_rec.has_mic = 0;
    AvfMicInfo mic_info;
    s_rec.avf_mic = avf_mic_open(&mic_info);
    if (s_rec.avf_mic) {
        s_rec.has_mic = 1;
        printf("[REC] Microphone: %d Hz, %d channel%s, %s\n",
               mic_info.sample_rate, mic_info.ch_layout.nb_channels,
               mic_info.ch_layout.nb_channels == 1 ? "" : "s",
               av_get_sample_fmt_name(mic_info.sample_fmt));
    } else {
        fprintf(stderr, "main: microphone not available\n");
    }

    s_rec.has_aud = s_rec.has_mic || s_rec.has_desktop;

    /* Build mixer */
    if (s_rec.has_aud) {
        int active[MIX_SRC_COUNT] = {
            [MIX_SRC_MIC]     = s_rec.has_mic,
            [MIX_SRC_DESKTOP] = s_rec.has_desktop,
        };
        s_rec.mixer = mixer_create(active, mixer_sink_encode, &s_rec.enc);
        if (!s_rec.mixer) {
            fprintf(stderr, "main: mixer init failed — recording video only\n");
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

    /* Open encoder (VideoToolbox on macOS, via encoder.c's __APPLE__ ifdef) */
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

    int enc_ret = encoder_open(&s_rec.enc, s_rec.output_path,
                     s_rec.canvas_w, s_rec.canvas_h, FPS,
                     AV_PIX_FMT_BGRA,  /* SCK delivers BGRA */
                     audio_sr, audio_ch, audio_fmt);
    if (s_rec.has_aud) av_channel_layout_uninit(&mix_ch);

    if (enc_ret < 0) {
        fprintf(stderr, "main: encoder open failed\n");
        if (s_rec.avf_mic) avf_mic_close(s_rec.avf_mic);
        if (s_rec.avf_cam) avf_camera_close(s_rec.avf_cam);
        sck_capture_close(s_rec.sck);
        if (s_rec.mixer) { mixer_destroy(s_rec.mixer); s_rec.mixer = NULL; }
        return -1;
    }

    /*
     * Anchor the session timeline.  Everything above — the SCK stream, the
     * webcam, the microphone — has been running and buffering for as long as
     * it took to get here, and those buffers are not part of the recording.
     * Fixing t0 once, after the last source is live, is what stops any one
     * track from entering with a head start on the others.
     */
    /*
     * Overlay geometry: a quarter of the canvas width, capped at 480 points,
     * inset 20 points from the bottom-right corner.
     *
     * The cap and the inset are defined in points and scaled with the capture,
     * so the camera keeps the same apparent size and margin however many pixels
     * per point the display turned out to have.  Left in raw pixels they would
     * halve on a Retina panel, which is the whole difference between an overlay
     * that reads and one that does not.
     */
    double scale = sck_info.scale > 0.0 ? sck_info.scale : 1.0;
    int overlay_cap = (int)lround(480.0 * scale);
    int inset       = (int)lround(20.0 * scale);
    int overlay = s_rec.canvas_w / 4;
    if (overlay > overlay_cap) overlay = overlay_cap;
    s_rec.comp = metal_compositor_create(s_rec.canvas_w, s_rec.canvas_h,
                                         overlay,
                                         s_rec.canvas_w - overlay - inset,
                                         s_rec.canvas_h - overlay - inset);
    if (!s_rec.comp)
        fprintf(stderr, "main: compositor unavailable — "
                        "webcam overlay disabled\n");

    const char *cam_off = getenv("SCREENCAST_CAM_OFFSET_MS");
    s_cam_offset_us = (cam_off && cam_off[0]) ? atoll(cam_off) * 1000LL : 0;
    if (s_cam_offset_us)
        fprintf(stderr, "[REC] Webcam offset: %lldms\n",
                (long long)(s_cam_offset_us / 1000));

    cam_ring_clear();
    screen_ring_clear();
    s_rec.t0_us = sck_host_time_us();
    atomic_store(&s_rec.audio_anchor_us, -1);
    sck_capture_start_session(s_rec.sck, s_rec.t0_us);
    if (s_rec.avf_mic) avf_mic_start_session(s_rec.avf_mic, s_rec.t0_us);
    if (s_rec.avf_cam) avf_camera_start_session(s_rec.avf_cam, s_rec.t0_us);

    atomic_store(&s_rec_open, 1);
    return 0;
}

/* ── record loop: runs until g_recording -> 0 ──────────────── */

static void recording_loop(void)
{
    pthread_t mic_tid = 0;
    pthread_t desk_tid = 0;
    int debug = getenv("SCREENCAST_DEBUG") != NULL;
    int64_t next_report = 10 * 1000000LL;
    long frames_encoded = 0;

    if (s_rec.has_mic)
        pthread_create(&mic_tid, NULL, mic_thread, &s_rec);
    if (s_rec.has_desktop && s_rec.mixer)
        pthread_create(&desk_tid, NULL, desktop_audio_thread, &s_rec);

    /*
     * Which source clocks the output.
     *
     * Display-only recording is clocked by the screen: ScreenCaptureKit stops
     * delivering while the picture is static, nothing is encoded, and variable
     * frame rate carries the still image for as long as it lasts.  That is the
     * cheapest possible recording of a screen that is not moving.
     *
     * The webcam modes cannot use that clock, because in those modes the screen
     * going still says nothing about whether the *picture* is still.  Waiting on
     * it froze the overlay — and in webcam mode the entire frame — for as long
     * as the display happened not to change, which on a talking-head screencast
     * is most of the recording.  So the camera clocks them instead: one output
     * frame per camera frame, paired with the screen frame captured alongside
     * it, at the camera's own timestamp.
     */
    int64_t last_cam_pts  = INT64_MIN;   /* newest camera frame consumed */
    int64_t last_emit_pts = INT64_MIN;   /* keeps output PTS strictly rising */
    int     was_cam_mode  = -1;

    while (atomic_load(&g_running) && atomic_load(&g_recording)) {
        RecordMode mode = atomic_load(&g_mode);

        /* The compositor is built once for the session, so switching modes is
           just a different draw — nothing to set up or tear down. */
        int want_cam = (mode == MODE_WEBCAM || mode == MODE_BOTH) && s_rec.avf_cam;

        /* Whatever the other clock buffered while it was not driving is stale
           by the time the mode switches back. */
        if (want_cam != was_cam_mode) {
            screen_ring_clear();
            last_cam_pts = INT64_MIN;
            was_cam_mode = want_cam;
        }

        void   *screen     = NULL;   /* borrowed from the ring, or owned below */
        void   *own_screen = NULL;   /* set when this iteration owns `screen` */
        void   *cam        = NULL;
        int64_t emit_pts   = -1;

        if (want_cam) {
            /* Keep the screen queue drained so it never backs up behind the
               camera's cadence, and so the pairing has history to choose from. */
            for (;;) {
                int64_t pts = -1;
                void *s = sck_capture_try_grab_video(s_rec.sck, &pts);
                if (!s) break;
                screen_ring_push(s, pts);
            }

            int64_t cam_pts = -1;
            cam = cam_take_latest(last_cam_pts, &cam_pts);
            if (cam) {
                last_cam_pts = cam_pts;
                emit_pts     = cam_pts;
                screen       = screen_ring_nearest(cam_pts);
            } else {
                /* Camera stalled.  Don't let it take the screen down with it:
                   fall back to the newest screen frame for this tick. */
                if (!atomic_load(&g_running) || !atomic_load(&g_recording))
                    break;
                screen = screen_ring_newest(&emit_pts);
                if (!screen) continue;
            }

            /* Mode 3 draws the screen underneath, so it needs one. */
            if (!screen && mode == MODE_BOTH) {
                if (cam) avf_camera_release_frame(cam);
                continue;
            }
        } else {
            /* Blocking with timeout — unblocks ~500 ms so the stop condition
               and a mode switch are both noticed promptly. */
            int64_t pts = -1;
            own_screen = sck_capture_grab_video(s_rec.sck, &pts);
            if (!own_screen) {
                if (!atomic_load(&g_running) || !atomic_load(&g_recording))
                    break;
                continue;
            }
            screen   = own_screen;
            emit_pts = pts;
        }

        /* The encoder counts on a strictly rising timeline, and two clocks feed
           it here — a mode switch or a camera hiccup can hand back a timestamp
           that has already been used. */
        if (emit_pts <= last_emit_pts) {
            if (cam) avf_camera_release_frame(cam);
            if (own_screen) sck_capture_release_frame(own_screen);
            continue;
        }
        last_emit_pts = emit_pts;

        /*
         * Display mode composites nothing, so the capture buffer goes to the
         * encoder untouched — no GPU pass, no CPU pass, no copy at all.  The
         * other modes need one Metal pass to draw the webcam in.
         */
        void *encode_buf = screen;
        void *composited = NULL;
        if (mode != MODE_DISPLAY && s_rec.comp) {
            composited = metal_compositor_render(s_rec.comp, mode, screen, cam);
            if (composited) encode_buf = composited;
        }

        if (encode_buf) {
            encoder_write_pixbuf(&s_rec.enc, encode_buf, emit_pts);
            frames_encoded++;
        }

        if (composited) metal_compositor_release_frame(composited);
        if (cam) avf_camera_release_frame(cam);
        if (own_screen) sck_capture_release_frame(own_screen);

        int64_t screen_pts = emit_pts;
        /*
         * Sync telemetry.  Both tracks are stamped from the same session clock,
         * so a healthy recording holds this delta near zero for its whole
         * duration; a delta that grows means one track is slipping and is worth
         * catching here rather than by watching the finished video.
         */
        if (debug && screen_pts >= next_report) {
            int64_t aud_us = 0;
            if (s_rec.enc.aud_enc && s_rec.enc.aud_enc->sample_rate > 0)
                aud_us = av_rescale_q(s_rec.enc.aud_pts,
                            (AVRational){1, s_rec.enc.aud_enc->sample_rate},
                            (AVRational){1, 1000000});
            fprintf(stderr,
                    "[SYNC] t=%llds video_pts=%lldms audio_pts=%lldms "
                    "delta=%+lldms frames=%ld\n",
                    (long long)(screen_pts / 1000000),
                    (long long)(screen_pts / 1000),
                    (long long)(aud_us / 1000),
                    (long long)((aud_us - screen_pts) / 1000),
                    frames_encoded);
            next_report += 10 * 1000000LL;
        }
    }

    atomic_store(&s_rec_open, 0);
    /* Wake the camera wait so it notices the session is over. */
    pthread_mutex_lock(&s_cam_mutex);
    pthread_cond_broadcast(&s_cam_cond);
    pthread_mutex_unlock(&s_cam_mutex);

    screen_ring_clear();
    if (mic_tid)  pthread_join(mic_tid, NULL);
    if (desk_tid) pthread_join(desk_tid, NULL);
}

/* ── close / flush one recording session ───────────────────── */

static void recording_close(void)
{
    /* Brief drain for threads */
    struct timespec ts = { .tv_nsec = 50000000L };
    nanosleep(&ts, NULL);

    /* Report what actually reached the mix, not what we hoped for at open. */
    if (s_rec.mixer) {
        int mic_live  = mixer_source_live(s_rec.mixer, MIX_SRC_MIC);
        int desk_live = mixer_source_live(s_rec.mixer, MIX_SRC_DESKTOP);
        fprintf(stderr, "[REC] Audio delivered: mic %s (%ld frames), "
                        "desktop %s (%ld frames)\n",
                mic_live  ? "yes" : "NO", s_rec.mic_frames,
                desk_live ? "yes" : "NO", s_rec.desk_frames);
        if (s_rec.has_mic && !mic_live)
            fprintf(stderr, "[REC] Hint: microphone produced no samples — check "
                            "System Settings > Privacy & Security > Microphone\n");
        if (s_rec.has_desktop && !desk_live)
            fprintf(stderr, "[REC] Hint: system audio produced no samples — the "
                            "recording keeps the remaining sources\n");
    }

    encoder_flush(&s_rec.enc);
    encoder_free(&s_rec.enc);

    if (s_rec.mixer) { mixer_destroy(s_rec.mixer); s_rec.mixer = NULL; }
    if (s_rec.comp)  { metal_compositor_destroy(s_rec.comp); s_rec.comp = NULL; }

    if (s_rec.avf_mic) { avf_mic_close(s_rec.avf_mic); s_rec.avf_mic = NULL; }
    if (s_rec.avf_cam) { avf_camera_close(s_rec.avf_cam); s_rec.avf_cam = NULL; }
    sck_capture_close(s_rec.sck);
    s_rec.sck = NULL;

    /* Clear any remaining webcam frames */
    cam_ring_clear();

    printf("[REC] Capture stopped.\n");
    printf("[REC] Output: %s\n", s_rec.output_path);
    control_notify("Screencast ready", s_rec.output_path);

    /* No final render — single-pass VideoToolbox encode writes directly to
     * the output path. */
}

/* ── entry point ───────────────────────────────────────────── */

static void sig_stop(int sig)
{
    (void)sig;
    atomic_store(&g_recording, 0);
    atomic_store(&g_running, 0);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <display|webcam|both|stop>\n"
        "\n"
        "  display   record the screen + audio (mic + desktop)\n"
        "  webcam    record the webcam + audio (mic + desktop)\n"
        "  both      record screen + webcam overlay + audio (mic + desktop)\n"
        "  stop      stop the running recorder\n"
        "\n"
        "The first record command starts a background daemon; later commands\n"
        "switch its mode over the control socket.  Bind these to skhd keys:\n"
        "  shift + cmd - d : screencast display\n"
        "  shift + cmd - w : screencast webcam\n"
        "  shift + cmd - b : screencast both\n"
        "  shift + cmd - escape : screencast stop\n",
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
