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
#include "audsrc.h"
#include "sck_capture.h"
#include "avf_camera.h"
#include "avf_mic.h"
#include "status_menu.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>

/* ── configuration ─────────────────────────────────────────── */

/* Frame rate comes from SCREENCAST_FPS via sck_capture_fps() — capture and
   encoder must agree, so there is one source. */
#define DEVICE_TARGET_CAP 512

/* ── shared state ──────────────────────────────────────────── */

/* Set from the command line: show the local camera overlay. */
static int s_want_camera;

/* Set from the command line: leave desktop audio out of the mix. */
static int s_mute_desktop;

/* Signal between main and capture threads: 1 while file is open. */
static atomic_int s_rec_open = 0;

/* ── recording bundle ──────────────────────────────────────── */

typedef struct {
    SckCapture *sck;           /* screen + desktop audio */
    AvfCamera  *avf_cam;       /* camera + borderless presenter window */
    AvfMic     *avf_mic;       /* microphone */
    EncoderCtx *enc;
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

/* The presenter window displays the AVCaptureSession directly.  The data
 * output remains only as the camera module's format/health seam, so release
 * each callback buffer immediately. */
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

/* ── audio source adapters (the shared worker owns the policy) ── */

/*
 * Read one microphone frame (the adapter the shared audio source worker
 * drives).  avf_mic already outputs 48 kHz stereo FLTP — the mixer's
 * canonical format — so the worker hands it through untouched.  A NULL is a
 * read timeout on a live session, which the worker counts toward its drop
 * policy; if the session ended, the running predicate stops the loop first.
 * The frame is owned by us: owned=1 tells the worker to free it after
 * feeding.
 */
static AudSrcRead mic_read(void *user)
{
    RecCtx *rec = (RecCtx *)user;
    int64_t pts_us = 0;
    AVFrame *f = avf_mic_read(rec->avf_mic, &pts_us);
    if (!f) return (AudSrcRead){ .result = AUDSRC_ERROR };
    rec->mic_frames++;
    note_audio_anchor(pts_us);
    return (AudSrcRead){
        .result = AUDSRC_FRAME, .frame = f, .owned = 1,
        .in_sample_rate = MIX_SAMPLE_RATE, .in_channels = MIX_CHANNELS,
        .in_fmt = AV_SAMPLE_FMT_FLTP, .pts_us = pts_us,
    };
}

/*
 * Read accumulated desktop audio from the ScreenCaptureKit stream.
 *
 * This has to be its own thread.  It used to run inline in the record loop,
 * pulling audio only when a video frame arrived — which was survivable when
 * every frame was delivered, but the capture layer now skips frames while the
 * screen is static.  On a talking-head screencast that is most of them, so
 * audio would sit in the capture buffer growing unboundedly and then arrive
 * in one lump whenever the picture finally changed.
 *
 * Nothing queued is AUDSRC_EMPTY, never a failure: desktop audio legitimately
 * has no samples while nothing plays, so the worker polls it on the short
 * cadence and never counts it toward the drop policy.
 */
static AudSrcRead desk_read(void *user)
{
    RecCtx *rec = (RecCtx *)user;
    int64_t pts_us = 0;
    AVFrame *f = sck_capture_grab_audio(rec->sck, &pts_us);
    if (!f) return (AudSrcRead){ .result = AUDSRC_EMPTY };
    rec->desk_frames++;
    note_audio_anchor(pts_us);
    return (AudSrcRead){
        .result = AUDSRC_FRAME, .frame = f, .owned = 1,
        .in_sample_rate = MIX_SAMPLE_RATE, .in_channels = MIX_CHANNELS,
        .in_fmt = AV_SAMPLE_FMT_FLTP, .pts_us = pts_us,
    };
}

/* The worker runs until the recording session is over. */
static int rec_running(void *user)
{
    (void)user;
    return atomic_load(&g_running) && atomic_load(&s_rec_open);
}

static void *worker_thread(void *arg)
{
    audsrc_run((const AudSrcWorker *)arg);
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
    s_rec.has_desktop = (sck_info.sample_rate > 0 && sck_info.channels > 0 &&
                          status_menu_desktop_audio_enabled());
    if (s_mute_desktop) {
        s_rec.has_desktop = 0;
        printf("[REC] Desktop audio muted by request.\n");
    }

    printf("[REC] Output: %s\n", s_rec.output_path);
    printf("[REC] Capture: %dx%d NV12\n", s_rec.canvas_w, s_rec.canvas_h);

    /* Presenter is the only command that opens the camera.  Its preview window
       is captured as part of the display: edge-to-edge video, no title bar,
       no border and no shadow.  A missing camera costs the overlay, not the
       recording. */
    if (s_want_camera) {
        char camera_target[DEVICE_TARGET_CAP];
        status_menu_camera_target(camera_target, sizeof(camera_target));
        AvfCameraInfo cam_info;
        s_rec.avf_cam = avf_camera_open(camera_target[0] ? camera_target : NULL,
                                        &cam_info,
                                        cam_frame_cb, NULL);
        if (s_rec.avf_cam) {
            printf("[REC] Camera: %dx%d %s\n",
                   cam_info.width, cam_info.height,
                   av_get_pix_fmt_name(cam_info.pix_fmt));
            if (avf_camera_show_overlay(s_rec.avf_cam,
                                        sck_info.display_id) == 0) {
                printf("[REC] Presenter window ready — drag it to any corner; "
                       "resize from an edge or corner.\n");
            } else {
                fprintf(stderr, "main: camera opened, but presenter window "
                                "could not be shown\n");
            }
        } else {
            fprintf(stderr, "main: camera unavailable — recording the display "
                            "without the presenter window\n");
        }
    }

    /* Open microphone */
    s_rec.has_mic = 0;
    char microphone_target[DEVICE_TARGET_CAP];
    status_menu_microphone_target(microphone_target, sizeof(microphone_target));
    AvfMicInfo mic_info;
    s_rec.avf_mic = avf_mic_open(microphone_target[0] ? microphone_target : NULL,
                                 &mic_info);
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

    s_rec.enc = encoder_open(s_rec.output_path,
                     s_rec.canvas_w, s_rec.canvas_h, sck_capture_fps(),
                     sck_info.pix_fmt, /* SCK delivers NV12 CVPixelBuffers */
                     audio_sr, audio_ch, audio_fmt);
    if (s_rec.has_aud) av_channel_layout_uninit(&mix_ch);

    if (!s_rec.enc) {
        fprintf(stderr, "main: encoder open failed\n");
        if (s_rec.avf_mic) {
            avf_mic_close(s_rec.avf_mic);
            s_rec.avf_mic = NULL;
        }
        if (s_rec.avf_cam) {
            avf_camera_close(s_rec.avf_cam);
            s_rec.avf_cam = NULL;
        }
        sck_capture_close(s_rec.sck);
        s_rec.sck = NULL;
        return -1;
    }

    /* Build the mixer once the encoder exists: the sink hands mixed audio
       straight to it, so the context must be live before any feed can drain. */
    s_rec.mixer = NULL;
    if (s_rec.has_aud) {
        int active[MIX_SRC_COUNT] = {
            [MIX_SRC_MIC]     = s_rec.has_mic,
            [MIX_SRC_DESKTOP] = s_rec.has_desktop,
        };
        s_rec.mixer = mixer_create(active, mixer_sink_encode, s_rec.enc);
        if (!s_rec.mixer) {
            fprintf(stderr, "main: mixer init failed — recording video only\n");
            s_rec.has_mic = s_rec.has_desktop = s_rec.has_aud = 0;
        }
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
    /* The feed-loop policy lives in the shared audio source worker; these
       configs drive it with the two macOS adapters above. */
    AudSrcWorker mic_worker = {
        .src = MIX_SRC_MIC, .mixer = s_rec.mixer, .label = "microphone",
        .max_fails = AUDSRC_MAX_FAILS,
        .error_backoff_ns = AUDSRC_ERROR_BACKOFF_NS,
        .empty_poll_ns    = AUDSRC_EMPTY_POLL_NS,
        .read = mic_read, .running = rec_running, .user = &s_rec,
    };
    AudSrcWorker desk_worker = {
        .src = MIX_SRC_DESKTOP, .mixer = s_rec.mixer, .label = "desktop",
        .max_fails = AUDSRC_MAX_FAILS,
        .error_backoff_ns = AUDSRC_ERROR_BACKOFF_NS,
        .empty_poll_ns    = AUDSRC_EMPTY_POLL_NS,
        .read = desk_read, .running = rec_running, .user = &s_rec,
    };
    int debug = getenv("SCREENCAST_DEBUG") != NULL;
    int64_t next_report = 10 * 1000000LL;
    long frames_encoded = 0;

    if (s_rec.has_mic)
        pthread_create(&mic_tid, NULL, worker_thread, &mic_worker);
    if (s_rec.has_desktop && s_rec.mixer)
        pthread_create(&desk_tid, NULL, worker_thread, &desk_worker);

    /* The screen clocks the output.  The presenter is an ordinary floating
       window in that screen capture, so its motion also causes SCK to deliver
       frames; there is still no second video clock or compositor here. */
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
        encoder_write_pixbuf(s_rec.enc, screen, pts);
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
            int64_t aud_us = encoder_audio_pts_us(s_rec.enc);
            if (aud_us < 0) aud_us = 0;
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

static void *recording_thread(void *unused)
{
    (void)unused;
    recording_loop();
    return NULL;
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

    encoder_flush(s_rec.enc);
    encoder_free(s_rec.enc);
    s_rec.enc = NULL;

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
        "usage: %s [presenter|stop] [mute]\n"
        "\n"
        "  (no argument)  record the display + audio (mic + desktop)\n"
        "  presenter      add a borderless camera window to that recording.\n"
        "                 Drag it while recording to snap it to any corner;\n"
        "                 resize it from any edge or corner.\n"
        "  mute           leave desktop audio out of the mix (mic only)\n"
        "  stop           stop the running recorder\n"
        "\n"
        "Bind these to skhd keys:\n"
        "  shift + cmd - s : screencast\n"
        "  shift + cmd - p : screencast presenter\n"
        "  cmd - escape    : screencast stop\n",
        argv0);
}

/* ── app-bundle handoff ────────────────────────────────────── */

/* A bare executable's main bundle has no identifier.  Presenter uses the app
   bundle so AppKit, TCC, and every launcher agree on one UI process. */
static int running_in_bundle(void)
{
    CFBundleRef b = CFBundleGetMainBundle();
    return b && CFBundleGetIdentifier(b) != NULL;
}

static int relaunch_as_bundle(void)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;

    char app[512];
    snprintf(app, sizeof(app), "%s/Applications/Screencast.app", home);
    if (access(app, X_OK) != 0) return -1; /* not installed */

    /*
     * open(1) routes through LaunchServices, which is the point: launchd
     * starts the app as its own responsible process, under the bundle's
     * identity, instead of this process inheriting whoever ran the command.
     * If the app is somehow already running, open activates it rather than
     * starting a second daemon — the control-socket check above already
     * made that window small.
     */
    char sh[600];
    snprintf(sh, sizeof(sh), "/usr/bin/open '%s' --args presenter%s", app,
             s_mute_desktop ? " mute" : "");
    return system(sh) == 0 ? 0 : -1;
}

/* The bundled daemon has no terminal.  Append to the same log the skhd
   bindings redirect to, so every launch context reads back from one place. */
static void log_to_file(void)
{
    /* Running the bundle's executable by hand for debugging keeps the tty. */
    if (isatty(STDERR_FILENO)) return;

    const char *home = getenv("HOME");
    if (!home || !home[0]) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/Library/Logs/screencast.log", home);
    if (freopen(path, "a", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
    if (freopen(path, "a", stdout))
        setvbuf(stdout, NULL, _IOLBF, 0);
}

int main(int argc, char **argv)
{
    /*
     * `mute` can accompany any record argument (`screencast mute`,
     * `screencast presenter mute`); whatever else is on the line is the
     * command itself.
     */
    const char *arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "mute"))
            s_mute_desktop = 1;
        else
            arg = argv[i];
    }

    /* A running daemon always receives the explicit CLI command semantics:
       a plain invocation is the display controller, while presenter asks the
       user to restart in presenter mode.  Preferences only affect a new start. */
    const char *control_cmd = arg ? arg : "display";
    const char *existing_cmd = (arg && !strcmp(arg, "presenter"))
        ? "display" : control_cmd;
    if (control_client_send(existing_cmd) == 0) {
        if (arg && !strcmp(arg, "presenter"))
            fprintf(stderr, "screencast: already recording — stop and run\n"
                            "            `screencast presenter` to start with "
                            "the camera window.\n");
        if (s_mute_desktop)
            fprintf(stderr, "screencast: already recording — `mute` applies "
                            "when a recording starts, not mid-recording.\n");
        return 0;
    }

    /* No daemon running.  A mode selected in the status menu is the default
       for a bare `screencast`; an explicit CLI mode always wins. */
    if (control_is_stop(control_cmd))
        return 0; /* nothing to stop */
    int want_camera = arg ? !strcmp(arg, "presenter")
                          : status_menu_preferred_presenter();
    const char *cmd = want_camera ? "display" : control_cmd;
    int mode = control_parse_mode(cmd);
    if (mode < 0) {
        usage(argv[0]);
        return 1;
    }

    /* The presenter is real AppKit UI.  Hand it to the installed accessory
       bundle so Terminal, skhd, and zellij all start the same identified UI
       process with stable camera and screen-recording permissions. */
    if (want_camera && !running_in_bundle()) {
        if (relaunch_as_bundle() == 0) {
            fprintf(stderr, "screencast: handing off to Screencast.app for "
                            "the presenter window "
                            "(log: ~/Library/Logs/screencast.log)\n");
            return 0;
        }
        fprintf(stderr, "screencast: Screencast.app not installed (run `make "
                        "install`) — trying the presenter window in this "
                        "process\n");
    }

    if (running_in_bundle())
        log_to_file();

    /*
     * Bootstrap NSApplication before touching any Apple framework, so the
     * process is registered with the window server before ScreenCaptureKit
     * or AVFoundation see it.
     */
    sck_bootstrap_app();
    status_menu_start();
    status_menu_set_recording(0, want_camera);

    signal(SIGINT,  sig_stop);
    signal(SIGTERM, sig_stop);

    av_log_set_level(AV_LOG_ERROR);

    s_want_camera = want_camera;
    atomic_store(&g_mode,      mode);
    atomic_store(&g_recording, 1);
    atomic_store(&g_running,   1);

    if (control_server_start() < 0)
        return 1;

    control_notify("Screencast recording",
                   want_camera ? "Display + camera + mic" : control_mode_label(mode));

    while (atomic_load(&g_running)) {
        if (atomic_load(&g_recording)) {
            if (recording_open() == 0) {
                status_menu_set_recording(1, s_want_camera);
                printf("[REC] Recording.\n");

                /* Encoding runs off the main thread.  AppKit keeps the main
                   thread so dragging and resizing remain fluid while frames
                   and audio are being recorded. */
                pthread_t record_tid;
                if (pthread_create(&record_tid, NULL,
                                   recording_thread, NULL) == 0) {
                    while (atomic_load(&s_rec_open))
                        sck_pump_run_loop();
                    pthread_join(record_tid, NULL);
                } else {
                    fprintf(stderr, "main: could not start recording worker — "
                                    "presenter interaction may be delayed\n");
                    recording_loop();
                }
            } else {
                /* Capture could not start — don't spin re-opening it. */
                atomic_store(&g_recording, 0);
                atomic_store(&g_running, 0);
            }
            recording_close();
            status_menu_set_recording(0, s_want_camera);
        } else {
            /* Keep accessory-app events responsive between sessions. */
            sck_pump_run_loop();
        }
    }

    /* Make sure we close any session left open by a rapid stop */
    if (atomic_load(&s_rec_open)) {
        atomic_store(&g_recording, 0);
        recording_close();
    }

    control_notify("Screencast stopped", NULL);
    status_menu_stop();
    control_server_stop();
    printf("[INFO] Exited cleanly.\n");
    return 0;
}
