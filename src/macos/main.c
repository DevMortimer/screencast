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
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>

#include "control.h"
#include "encoder.h"
#include "mixer.h"
#include "sck_capture.h"
#include "avf_camera.h"
#include "avf_mic.h"

/* ── configuration ─────────────────────────────────────────── */

#define FPS         30
#define WEBCAM_DEV  "auto"

/* ── shared state ──────────────────────────────────────────── */

/*
 * Latest decoded webcam frame.  Updated by the AVFoundation capture callback,
 * read by the record thread.  Protected by s_cam_mutex.
 */
static pthread_mutex_t s_cam_mutex  = PTHREAD_MUTEX_INITIALIZER;
static AVFrame        *s_cam_latest = NULL;
static atomic_llong    s_cam_seq    = 0;

/* Signal between main and capture threads: 1 while file is open. */
static atomic_int s_rec_open = 0;

/* ── recording bundle ──────────────────────────────────────── */

typedef struct {
    SckCapture *sck;           /* screen + desktop audio */
    AvfCamera  *avf_cam;       /* webcam (always on, no arbiter) */
    AvfMic     *avf_mic;       /* microphone */
    EncoderCtx enc;
    MixerCtx  *mixer;          /* mixes mic + desktop into one track */
    int canvas_w, canvas_h;
    int cam_w, cam_h;
    enum AVPixelFormat cam_fmt;
    int cam_active;            /* 1 when webcam compositing is engaged */
    int has_mic;
    int has_desktop;
    int has_aud;               /* has_mic || has_desktop */
    long mic_frames;           /* diagnostics: frames handed to the mixer */
    long desk_frames;
    char output_path[512];     /* ~/Movies/screencast_YYYYMMDD_HHMMSS.mp4 */
} RecCtx;

static RecCtx s_rec;

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

static void cam_frame_cb(void *user, AVFrame *frame)
{
    (void)user;
    pthread_mutex_lock(&s_cam_mutex);
    if (s_cam_latest) av_frame_free(&s_cam_latest);
    s_cam_latest = frame;
    atomic_fetch_add(&s_cam_seq, 1);
    pthread_mutex_unlock(&s_cam_mutex);
}

/* ── mixed audio delivered to the encoder ──────────────────── */

static void mixer_sink_encode(void *user, AVFrame *mixed)
{
    encoder_feed_audio((EncoderCtx *)user, mixed);
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
        AVFrame *f = avf_mic_read(rec->avf_mic);
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
        mixer_feed(rec->mixer, MIX_SRC_MIC, f,
                   MIX_SAMPLE_RATE,
                   &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
                   AV_SAMPLE_FMT_FLTP);
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

    int cam_w_hint = 0, cam_h_hint = 0, cam_fps_hint = 0;
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

    atomic_store(&s_rec_open, 1);
    return 0;
}

/* ── record loop: runs until g_recording -> 0 ──────────────── */

static void recording_loop(void)
{
    pthread_t mic_tid = 0;

    if (s_rec.has_mic)
        pthread_create(&mic_tid, NULL, mic_thread, &s_rec);

    while (atomic_load(&g_running) && atomic_load(&g_recording)) {
        RecordMode mode = atomic_load(&g_mode);

        /* Toggle webcam compositing based on mode */
        int want_cam = (mode == MODE_WEBCAM || mode == MODE_BOTH) && s_rec.avf_cam;
        if (want_cam && !s_rec.cam_active) {
            encoder_set_webcam(&s_rec.enc, s_rec.cam_w, s_rec.cam_h,
                               s_rec.cam_fmt);
            s_rec.cam_active = 1;
        } else if (!want_cam && s_rec.cam_active) {
            encoder_clear_webcam(&s_rec.enc);
            s_rec.cam_active = 0;
        }

        /* Grab screen frame (blocking with timeout — unblocks ~500ms so
           the stop condition is checked promptly). */
        AVFrame *screen = sck_capture_grab_video(s_rec.sck);
        if (!screen) {
            if (!atomic_load(&g_running) || !atomic_load(&g_recording))
                break;
            continue;
        }

        /* Grab desktop audio (non-blocking) */
        if (s_rec.has_desktop && s_rec.mixer) {
            AVFrame *desk_audio = sck_capture_grab_audio(s_rec.sck);
            if (desk_audio) {
                s_rec.desk_frames++;
                mixer_feed(s_rec.mixer, MIX_SRC_DESKTOP, desk_audio,
                           MIX_SAMPLE_RATE,
                           &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
                           AV_SAMPLE_FMT_FLTP);
                av_frame_free(&desk_audio);
            }
        }

        /* Get latest webcam frame */
        AVFrame *cam = NULL;
        int64_t cam_seq = -1;
        if (want_cam) {
            pthread_mutex_lock(&s_cam_mutex);
            if (s_cam_latest) {
                cam = av_frame_alloc();
                av_frame_ref(cam, s_cam_latest);
                cam_seq = atomic_load(&s_cam_seq);
            }
            pthread_mutex_unlock(&s_cam_mutex);
        }

        encoder_write_video(&s_rec.enc, mode, screen, cam, cam_seq);

        av_frame_free(&screen);
        if (cam) av_frame_free(&cam);
    }

    atomic_store(&s_rec_open, 0);
    if (mic_tid) pthread_join(mic_tid, NULL);
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

    /* Clear any remaining webcam frame */
    pthread_mutex_lock(&s_cam_mutex);
    if (s_cam_latest) { av_frame_free(&s_cam_latest); s_cam_latest = NULL; }
    pthread_mutex_unlock(&s_cam_mutex);

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
