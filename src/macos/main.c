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
#include <CoreVideo/CoreVideo.h>

/* ── configuration ─────────────────────────────────────────── */

#define FPS         30
#define WEBCAM_DEV  "auto"

/* ── shared state ──────────────────────────────────────────── */

/* Signal between main and capture threads: 1 while file is open. */
static atomic_int s_rec_open = 0;

/* ── recording bundle ──────────────────────────────────────── */

typedef struct {
    SckCapture *sck;           /* screen + desktop audio */
    AvfCamera  *avf_cam;       /* held open only to make Presenter Overlay
                                  available; its frames are never read */
    AvfMic     *avf_mic;       /* microphone */
    EncoderCtx enc;
    MixerCtx  *mixer;          /* mixes mic + desktop into one track */
    int canvas_w, canvas_h;
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

/*
 * Nothing here reads the camera.
 *
 * The device is opened so that macOS offers Presenter Overlay for this stream
 * — ScreenCaptureKit makes the effect available to an app that is capturing
 * the screen and using the camera at the same time — and once the overlay is
 * on, the system takes the camera and composites the presenter into the SCK
 * frames we already receive.  AVFoundation stops delivering a live camera
 * stream at that point anyway.
 *
 * So every frame that does arrive is released immediately.  Compositing an
 * overlay ourselves is what this replaced.
 */
static void cam_frame_cb(void *user, void *pixbuf, int64_t pts_us)
{
    (void)user; (void)pts_us;
    avf_camera_release_frame(pixbuf);
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

    /*
     * Open the camera, and read nothing from it.
     *
     * Its only job is to make this app a camera client while an SCK stream is
     * running, which is the condition macOS attaches Presenter Overlay to.
     * With that satisfied the overlay appears in the Video Effects menu, and
     * turning it on has ScreenCaptureKit composite the presenter into the
     * frames this process already receives.
     *
     * A camera we cannot open is not a failure: it costs the overlay, not the
     * recording.
     */
    AvfCameraInfo cam_info;
    s_rec.avf_cam = avf_camera_open(WEBCAM_DEV, 1920, 1080, 30, &cam_info,
                                    cam_frame_cb, NULL);
    if (s_rec.avf_cam) {
        printf("[REC] Camera held open for Presenter Overlay (%dx%d %s)\n",
               cam_info.width, cam_info.height,
               av_get_pix_fmt_name(cam_info.pix_fmt));
    } else {
        fprintf(stderr, "main: camera unavailable — recording the display "
                        "without Presenter Overlay\n");
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
     * The screen clocks the output, and nothing else does.
     *
     * ScreenCaptureKit stops delivering while the picture is static, so a still
     * display encodes nothing at all and variable frame rate carries the image
     * for as long as it lasts.  That is the cheapest a screen recording gets.
     *
     * It holds now because the presenter is no longer ours to draw.  When
     * Presenter Overlay is on, the frames arriving here already have the
     * presenter composited into them by the system, and they keep arriving
     * because the presenter is moving even when the screen is not — the cost
     * of the overlay is paid only while it is switched on.
     */
    int64_t last_emit_pts = INT64_MIN;   /* keeps output PTS strictly rising */

    while (atomic_load(&g_running) && atomic_load(&g_recording)) {
        /*
         * A stream that stopped and a display that is not changing look
         * identical from here — both are simply "no new frame".  Left alone we
         * would go on holding the last frame we received and record a frozen
         * screen with live audio over it, and nothing in the finished file
         * would say so.  Better to end here, where it is still finalised and
         * the reason can be said out loud.
         */
        if (sck_capture_failed(s_rec.sck)) {
            fprintf(stderr, "screencast: screen capture stopped — ending the "
                            "recording rather than writing a frozen screen\n");
            atomic_store(&g_recording, 0);
            break;
        }

        /* Blocking with timeout — unblocks ~500 ms so the stop condition is
           noticed promptly even while the display is perfectly still. */
        int64_t pts = -1;
        void *screen = sck_capture_grab_video(s_rec.sck, &pts);
        if (!screen) {
            if (!atomic_load(&g_running) || !atomic_load(&g_recording))
                break;
            continue;
        }

        /* The encoder counts on a strictly rising timeline. */
        if (pts <= last_emit_pts) {
            sck_capture_release_frame(screen);
            continue;
        }
        last_emit_pts = pts;

        /* Nothing is composited, so the capture buffer goes to the encoder
           untouched — no GPU pass, no CPU pass, no copy at all. */
        encoder_write_pixbuf(&s_rec.enc, screen, pts);
        frames_encoded++;
        sck_capture_release_frame(screen);

        int64_t screen_pts = pts;
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

    if (s_rec.avf_mic) { avf_mic_close(s_rec.avf_mic); s_rec.avf_mic = NULL; }
    if (s_rec.avf_cam) { avf_camera_close(s_rec.avf_cam); s_rec.avf_cam = NULL; }
    sck_capture_close(s_rec.sck);
    s_rec.sck = NULL;

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
        "usage: %s [stop]\n"
        "\n"
        "  (no argument)  record the display + audio (mic + desktop)\n"
        "  stop           stop the running recorder\n"
        "\n"
        "To appear in the recording, turn on Presenter Overlay from the Video\n"
        "Effects menu in Control Center while a recording is running; macOS\n"
        "composites you into the capture itself.  Bind these to skhd keys:\n"
        "  shift + cmd - s : screencast\n"
        "  cmd - escape    : screencast stop\n",
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
                printf("[REC] Recording.\n");
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
