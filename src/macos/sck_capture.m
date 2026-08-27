// sck_capture.m — ScreenCaptureKit display + system audio capture backend
//
// Wraps SCShareableContent → SCContentFilter → SCStream with a synchronous
// grab interface.  Frames arrive on an SCK-managed dispatch queue; the stream
// output delegate pushes video into a bounded circular buffer (signalled via
// dispatch semaphore) and accumulates audio samples in per-channel plane
// buffers.  System audio is normalised to 48 kHz stereo FLTP — the mixer's
// canonical format — before it is queued.

#import "sck_capture.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <AppKit/AppKit.h>
#import <Accelerate/Accelerate.h>
#import <libswresample/swresample.h>
#import <libavutil/channel_layout.h>
#import <libavutil/error.h>
#import <libavutil/samplefmt.h>
#import <math.h>
#import <pthread.h>
#import <stdatomic.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

/* ── constants ─────────────────────────────────────────────── */

/*
 * Queue capacity, ~270 ms at 24 fps.  Deliberately shallow: a backlog is
 * dropped, not drained.  Encoding queued frames back-to-back after a stall
 * would add a GPU burst at exactly the moment the system is recovering from
 * one — the headroom rule (ADR 0007) says the recorder yields instead.
 */
#define VIDEO_QUEUE_DEPTH   8
#define AUDIO_INIT_SAMPLES  (1024 * 8)

/*
 * At most this many canonical samples of desktop audio may accumulate before
 * capture drops the newest.  The desktop thread drains every ~10 ms, so the
 * bound is only ever reached while the downstream pipeline is stalled;
 * without it a long stall balloons RAM on a machine already under memory
 * pressure.  The timeline stays anchored at the head — dropping newest, never
 * oldest — and the mixer covers the hole with silence.
 */
#define AUDIO_MAX_SAMPLES   (48000 * 2)

/* ── tuning ────────────────────────────────────────────────── */

/*
 * Session frame rate.  24 fps default: a talking head and a shared screen
 * read perfectly at 24, and every frame saved is GPU time the rest of the
 * system keeps when a burst arrives (see CONTEXT.md: Headroom).
 */
int sck_capture_fps(void)
{
    const char *e = getenv("SCREENCAST_FPS");
    if (!e || !e[0]) return 24;
    int v = atoi(e);
    if (v < 10 || v > 60) {
        fprintf(stderr, "sck_capture: SCREENCAST_FPS=%s out of range (10-60) — "
                        "using 24\n", e);
        return 24;
    }
    return v;
}

/*
 * Ceiling on the captured canvas's longer edge, in pixels.  Default 1440:
 * still oversamples a 1440x900-point desktop, at ~40% of the pixels the old
 * 1920 cap cost — the pixel count multiplies the price of every stage after
 * it, so this is the headroom dial.
 */
static int capture_cap(void)
{
    const char *e = getenv("SCREENCAST_CAPTURE_CAP");
    if (!e || !e[0]) return 1440;
    int v = atoi(e);
    if (v < 640 || v > 4096) {
        fprintf(stderr, "sck_capture: SCREENCAST_CAPTURE_CAP=%s out of range "
                        "(640-4096) — using 1440\n", e);
        return 1440;
    }
    return v;
}

/* How long the stream may go without delivering a single video callback before
   we say so on stderr.  Deliberately a warning and not a verdict: SCK is
   allowed to go quiet while the display is static, and ending a recording of a
   still slide would be a worse failure than the one this watches for. */
#define SCK_STALL_WARN_US   (5 * 1000000LL)

/* ── opaque struct ─────────────────────────────────────────── */

struct SckCapture {
    CVPixelBufferRef        video_queue[VIDEO_QUEUE_DEPTH];
    int64_t                 video_pts[VIDEO_QUEUE_DEPTH];  /* host-clock µs */
    int                     video_head;
    int                     video_count;
    dispatch_semaphore_t    video_sem;

    /* Signalled once as soon as the stream delivers anything at all.  Distinct
       from video_sem: open() needs to know the stream is live, which is a
       question about the stream, not about the recording — frames arriving
       before the session is anchored are still proof it works. */
    dispatch_semaphore_t    ready_sem;
    int                     ready_signalled;

    uint8_t                *audio_planes[2];
    int                     audio_nb_samples;
    int                     audio_plane_cap;
    int                     audio_channels;
    int                     audio_sample_rate;
    enum AVSampleFormat     audio_fmt;
    int64_t                 audio_head_pts;   /* host-clock µs of sample 0 */

    /* Negotiated source format, read from the first audio buffer's ASBD.
       The stream follows the output device, so this can be float or integer
       PCM at the device's own rate; swr converts it to the canonical
       48 kHz stereo FLTP that audio_planes[] actually holds. */
    int                     src_sample_rate;
    int                     src_channels;
    enum AVSampleFormat     src_fmt;
    SwrContext              *swr;

    /* Session timeline.  t0_us is the host-clock reading that PTS 0 maps to;
       until session_started is set, arriving buffers are dropped rather than
       queued, so nothing captured during startup enters the recording. */
    int64_t                 t0_us;
    int                     session_started;

    void                   *stream;       /* CFBridgingRetain of SCStream */
    dispatch_queue_t        sample_queue;  /* retained — must outlive local scope */
    dispatch_queue_t        audio_queue;   /* separate: video memcpy must not
                                              back up audio delivery */
    void                   *helper;        /* _SckStreamHelper — retained (stream may hold weak ref) */
    int                     width;         /* pixels */
    int                     height;        /* pixels */
    double                  scale;         /* pixels per point actually captured */
    pthread_mutex_t         lock;
    SckCaptureInfo          info;
    int                     stopped;

    /* Liveness.  ScreenCaptureKit can retire a stream at any time — a second
       capture client, a display reconfiguration, the system recorder claiming
       the display — and when it does the only symptom is that frames stop
       arriving.  `failed` is set from the stream delegate; last_video_cb_us
       catches a stream that goes quiet without saying anything. */
    atomic_int              failed;
    atomic_llong            last_video_cb_us;
    int                     stall_warned;
    int                     drop_warned;   /* backlog warning said once per episode */

    /* diagnostics (SCREENCAST_DEBUG=1) — each counter is touched by exactly
       one serial queue, so plain increments are safe */
    int                     debug;
    long                    video_cb;
    long                    audio_cb;
    long                    video_idle;    /* frames skipped: nothing changed */
    long                    video_dropped; /* frames evicted: queue overflow */
};

/* ── helpers ───────────────────────────────────────────────── */

static void log_error(const char *msg, OSStatus code)
{
    fprintf(stderr, "sck_capture: %s (osstatus %d)\n", msg, (int)code);
}

static void log_error_av(const char *msg, int ret)
{
    char buf[128];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "sck_capture: %s: %s\n", msg, buf);
}

/* Derive an AVSampleFormat + channel count from the ASBD.
 *
 * The negotiated format is the output device's, not ours: Float32 on the
 * built-in audio path, signed integer (16- or 32-bit) on many USB/HDMI sinks
 * — the same split the microphone backend hit with the Fifine.  Sample size
 * is read from the description, never assumed; libswresample then converts
 * whatever this admits to the canonical 48 kHz stereo FLTP. */
static int asbd_to_avfmt(const AudioStreamBasicDescription *asbd,
                         enum AVSampleFormat *out_fmt,
                         int *out_channels)
{
    if (asbd->mFormatID != kAudioFormatLinearPCM) {
        fprintf(stderr, "sck_capture: unsupported format (not Linear PCM)\n");
        return -1;
    }

    int ch = (int)asbd->mChannelsPerFrame;
    if (ch < 1) ch = 1;
    if (ch > 2) {
        fprintf(stderr, "sck_capture: >2 channel system audio not expected (%d)\n", ch);
        ch = 2;
    }

    int planar = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved);

    if (asbd->mFormatFlags & kAudioFormatFlagIsFloat) {
        if (asbd->mBitsPerChannel != 32) {
            fprintf(stderr, "sck_capture: unsupported float bit depth (%d)\n",
                    (int)asbd->mBitsPerChannel);
            return -1;
        }
        *out_channels = ch;
        *out_fmt = planar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
        return 0;
    }

    if (asbd->mFormatFlags & kAudioFormatFlagIsSignedInteger) {
        /* 24-bit audio usually arrives as 24 bits aligned high inside a
           32-bit word — that is S32 data with a limited range, and swr
           scales it correctly.  True 24-bit packed frames (3 bytes per
           sample) have no native AVSampleFormat and are rejected loudly
           rather than misread. */
        if (asbd->mBitsPerChannel == 24 && asbd->mBytesPerFrame / ch == 4) {
            *out_channels = ch;
            *out_fmt = planar ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S32;
            return 0;
        }
        switch (asbd->mBitsPerChannel) {
        case 16:
            *out_channels = ch;
            *out_fmt = planar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
            return 0;
        case 32:
            *out_channels = ch;
            *out_fmt = planar ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S32;
            return 0;
        default:
            fprintf(stderr, "sck_capture: unsupported integer bit depth (%d)\n",
                    (int)asbd->mBitsPerChannel);
            return -1;
        }
    }

    fprintf(stderr, "sck_capture: unsupported format (not Float or Signed Integer PCM)\n");
    return -1;
}

void sck_bootstrap_app(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSApplication *app = [NSApplication sharedApplication];

        /*
         * Register this process with the Window Server.
         *
         * Launched from the app bundle (the normal presenter path — main.c
         * hands off through LaunchServices), registration already happened
         * at launch and LSUIElement in the Info.plist pins the Accessory
         * policy; re-asserting Regular here would flash a Dock icon.
         *
         * Launched as a bare binary — display-only recording, or the
         * fallback when the bundle is not installed — the process starts
         * Prohibited, so walk it to Accessory the long way round: the
         * intermediate Regular forces the full Window Server registration
         * that a direct Prohibited→Accessory transition can skip on newer
         * macOS versions.
         */
        if ([[NSBundle mainBundle] bundleIdentifier] == nil) {
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        }

        /*
         * Pump the run loop once so AppKit can finish its initialisation
         * and register the process with the Window Server.  Without this
         * the change to Accessory may not take full effect because the
         * main thread never yields to AppKit's event processing.
         */
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
    });
}

void sck_pump_run_loop(void)
{
    /*
     * CFRunLoopRunInMode services sources and timers, but it does not run
     * NSApplication's event dispatcher.  That left the presenter window
     * visible while mouse events sat in AppKit's queue, producing a beachball
     * cursor and making both drag and resize appear dead.
     *
     * Pull the queued events through NSApplication explicitly.  Keep the
     * deadline short so the recording worker and status menu remain separate
     * from this main-thread UI pump.
     */
    NSApplication *app = [NSApplication sharedApplication];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:0.010];
    for (;;) {
        NSEvent *event = [app nextEventMatchingMask:NSEventMaskAny
                                          untilDate:deadline
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
        if (!event) break;
        [app sendEvent:event];
    }
}

int64_t sck_host_time_us(void)
{
    CMTime now = CMClockGetTime(CMClockGetHostTimeClock());
    if (!CMTIME_IS_VALID(now)) return 0;
    return CMTimeConvertScale(now, 1000000, kCMTimeRoundingMethod_Default).value;
}

/* Presentation timestamp of a sample buffer, in host-clock microseconds.
   SCK and AVFoundation both stamp against CMClockGetHostTimeClock(), so this
   is directly comparable with sck_host_time_us(). */
static int64_t sample_pts_us(CMSampleBufferRef sb)
{
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
    if (!CMTIME_IS_VALID(pts)) return sck_host_time_us();
    return CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_Default).value;
}

/*
 * How many pixels to capture per point of display geometry.
 *
 * SCDisplay.frame is in *points*; SCStreamConfiguration.width/height are in
 * pixels.  Configuring the stream straight from the frame therefore asks a
 * Retina panel for a downscaled capture — a 2560x1600 display reporting
 * 1440x900 points was recorded at 56% of its linear resolution, and text is the
 * first thing that costs.
 *
 * The default is measured, not assumed: the ratio between the current mode's
 * pixel width and its point width.  Be clear about what that measures.  It is
 * the *backing framebuffer* macOS composites into, which on a scaled HiDPI
 * mode is larger than the panel — a MacBook Air running 1440x900 points
 * reports 2880x1800 here, and the display resamples that down to its own
 * 2560x1664.  So this captures everything the window server drew, at the cost
 * of ~22% more pixels than the screen can actually show.  Measuring is still
 * right (a display set to an unscaled mode reports its true 1:1 or 2:1 ratio
 * rather than a guessed one), but it is not the panel's resolution and should
 * not be described as such.
 *
 * Capturing all of it, though, is not worth what it costs.  Every pixel is
 * paid for again at each stage downstream — the encode, the memory bandwidth,
 * the file — and on a machine where other apps are also encoding (a video
 * call, a simulator) that standing cost is headroom the rest of the system
 * cannot use.  So the default is capped at 1440 on the longer edge, which
 * still oversamples a 1440x900-point desktop at ~40% of the pixels of the old
 * 1920 cap.  See ADR 0007; SCREENCAST_CAPTURE_CAP dials it.
 *
 * Two bounds therefore apply, and the log says which one bit: the measured
 * native ratio, and the cap.  The result is the smaller — never more than the
 * framebuffer holds, because upscaling costs encode bandwidth and memory
 * without adding any detail, and never more than the cap.
 */

static double display_capture_scale(CGDirectDisplayID did,
                                    int pt_w, int pt_h,
                                    int cap_max,
                                    char *bound_by, size_t bound_by_n)
{
    double native = 1.0;

    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did);
    if (mode) {
        size_t px = CGDisplayModeGetPixelWidth(mode);
        size_t pt = CGDisplayModeGetWidth(mode);
        if (px > 0 && pt > 0) native = (double)px / (double)pt;
        CGDisplayModeRelease(mode);
    }
    if (native < 1.0) native = 1.0;

    /* The cap is expressed in pixels, so turn it into the scale that would
       produce it.  On a display already wider than the cap in points this is
       below 1 — a deliberate downscale, not a clamp. */
    int    long_pt   = pt_w > pt_h ? pt_w : pt_h;
    double cap_scale = long_pt > 0
                       ? (double)cap_max / (double)long_pt
                       : native;

    if (native < cap_scale)
        snprintf(bound_by, bound_by_n, "native");
    else
        snprintf(bound_by, bound_by_n, "%d px cap", cap_max);
    return native < cap_scale ? native : cap_scale;
}

/* Points → pixels, rounded to an even count.  NV12 subsamples chroma 2x2, so
   an odd dimension has no valid chroma plane. */
static int scaled_even(int points, double scale)
{
    long v = lround((double)points * scale);
    if (v < 2) v = 2;
    return (int)(v & ~1L);
}

/* Did anything on screen actually change?  SCK re-delivers the previous surface
   with status Idle when the display is static — encoding those is pure waste,
   and on a talking-head screencast they are the majority of frames. */
static BOOL frame_is_complete(CMSampleBufferRef sb)
{
    CFArrayRef attachments =
        CMSampleBufferGetSampleAttachmentsArray(sb, /*create*/ false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return YES;

    CFDictionaryRef info = CFArrayGetValueAtIndex(attachments, 0);
    if (!info) return YES;

    CFNumberRef statusRef =
        CFDictionaryGetValue(info, (__bridge CFStringRef)SCStreamFrameInfoStatus);
    if (!statusRef) return YES;

    int status = 0;
    CFNumberGetValue(statusRef, kCFNumberIntType, &status);
    return status == SCFrameStatusComplete;
}

/* ── SCStreamOutput delegate (SDK 27+) ─────────────────────── */

@interface _SckStreamHelper : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) SckCapture *capture;
@end

@implementation _SckStreamHelper
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    (void)stream;
    SckCapture *c = _capture;
    if (!c || c->stopped) return;

    if (type == SCStreamOutputTypeScreen) {
        c->video_cb++;
        /* Proof of life, recorded before any filtering below can discard this
           frame — an idle screen still delivers callbacks, a dead stream does
           not, and that is the difference worth measuring. */
        atomic_store(&c->last_video_cb_us, (long long)sck_host_time_us());
        if (c->debug && (c->video_cb == 1 || c->video_cb % 100 == 0))
            fprintf(stderr, "sck: video callback #%ld\n", c->video_cb);

        /* Report the stream as live before any filtering below can discard
           this frame.  Touched only from the serial sample queue. */
        if (!c->ready_signalled) {
            c->ready_signalled = 1;
            dispatch_semaphore_signal(c->ready_sem);
        }

        /* Nothing changed on screen — don't pay to re-encode an identical
           image.  Wall-clock PTS means the previous frame simply displays
           longer, which is exactly right for a VFR MP4. */
        if (!frame_is_complete(sampleBuffer)) { c->video_idle++; return; }

        int64_t pts = sample_pts_us(sampleBuffer);

        CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
        if (!pb) return;

        /* The buffer is retained and queued as-is.  It is IOSurface-backed and
           already in the format the encoder wants, so there is nothing to
           convert and no reason to copy it out of GPU memory. */
        pthread_mutex_lock(&c->lock);
        /* Before the session is anchored the recording has not begun; and a
           frame captured ahead of t0 belongs to the startup window. */
        if (c->stopped || !c->session_started || pts < c->t0_us) {
            pthread_mutex_unlock(&c->lock);
            return;
        }

        if (c->video_count >= VIDEO_QUEUE_DEPTH) {
            /* Downstream is behind.  Video is the elastic resource: drop the
               oldest frame rather than stall capture, delay a timestamp, or
               let the backlog grow into a burst.  Warn once per episode so a
               real recording says when it happened. */
            if (!c->drop_warned) {
                c->drop_warned = 1;
                fprintf(stderr, "sck_capture: capture falling behind — "
                                "dropping frames while the system is under "
                                "load\n");
            }
            CVPixelBufferRelease(c->video_queue[c->video_head]);
            c->video_queue[c->video_head] = NULL;
            c->video_head = (c->video_head + 1) % VIDEO_QUEUE_DEPTH;
            c->video_count--;
            c->video_dropped++;
        }

        int tail = (c->video_head + c->video_count) % VIDEO_QUEUE_DEPTH;
        c->video_queue[tail] = (CVPixelBufferRef)CFRetain(pb);
        c->video_pts[tail]   = pts - c->t0_us;
        c->video_count++;
        pthread_mutex_unlock(&c->lock);
        dispatch_semaphore_signal(c->video_sem);

    } else if (type == SCStreamOutputTypeAudio) {
        c->audio_cb++;
        if (c->debug && (c->audio_cb == 1 || c->audio_cb % 100 == 0))
            fprintf(stderr, "sck: audio callback #%ld\n", c->audio_cb);

        if (c->audio_sample_rate == 0) {
            CMAudioFormatDescriptionRef fmtDesc =
                CMSampleBufferGetFormatDescription(sampleBuffer);
            if (!fmtDesc) return;

            const AudioStreamBasicDescription *asbd =
                CMAudioFormatDescriptionGetStreamBasicDescription(fmtDesc);
            if (!asbd) return;

            enum AVSampleFormat src_fmt;
            int src_channels;
            if (asbd_to_avfmt(asbd, &src_fmt, &src_channels) < 0)
                return;

            int src_rate = (int)asbd->mSampleRate;
            if (src_rate <= 0) src_rate = 48000;

            /*
             * Normalise to the mixer's canonical 48 kHz stereo FLTP here,
             * on the capture path.
             *
             * The negotiated format is the output device's — float or
             * integer PCM, planar or interleaved, at the device's rate.
             * Reading it as float32 regardless (as this code used to) turns
             * every integer sample into garbage the mixer's clamp pins at
             * full scale, drowning the microphone completely.  Normalising
             * here means the frames handed to the mixer are always the
             * format the desktop thread tells it they are, exactly as the
             * microphone backend does in its own callback.
             */
            AVChannelLayout out_layout, src_layout;
            av_channel_layout_default(&out_layout, 2);
            av_channel_layout_default(&src_layout, src_channels);
            SwrContext *swr = NULL;
            int ret = swr_alloc_set_opts2(&swr,
                         &out_layout, AV_SAMPLE_FMT_FLTP, 48000,
                         &src_layout, src_fmt,         src_rate,
                         0, NULL);
            av_channel_layout_uninit(&out_layout);
            av_channel_layout_uninit(&src_layout);
            if (ret < 0 || !swr || swr_init(swr) < 0) {
                log_error_av("swr init", ret < 0 ? ret : AVERROR_EXTERNAL);
                if (swr) swr_free(&swr);
                return;
            }
            c->swr = swr;

            c->src_sample_rate = src_rate;
            c->src_channels    = src_channels;
            c->src_fmt         = src_fmt;

            /* What grab_audio reports from here on: always canonical. */
            c->audio_channels    = 2;
            c->audio_sample_rate = 48000;
            c->audio_fmt         = AV_SAMPLE_FMT_FLTP;

            printf("[REC] Desktop audio: %d Hz, %d channel%s, %s\n",
                   src_rate, src_channels, src_channels == 1 ? "" : "s",
                   av_get_sample_fmt_name(src_fmt));

            c->audio_plane_cap = AUDIO_INIT_SAMPLES * (int)sizeof(float);
            for (int i = 0; i < c->audio_channels; i++) {
                c->audio_planes[i] = (uint8_t *)malloc((size_t)c->audio_plane_cap);
                if (!c->audio_planes[i]) {
                    fprintf(stderr, "sck_capture: audio plane alloc failed\n");
                    for (int j = 0; j < c->audio_channels; j++) {
                        free(c->audio_planes[j]);
                        c->audio_planes[j] = NULL;
                    }
                    c->audio_sample_rate = 0;   /* retry the one-time init */
                    return;
                }
            }
        }

        CMBlockBufferRef blockBuf = NULL;
        AudioBufferList *bufList  = NULL;

        /* Sizing call.  Passing NULL for bufferListOut asks only for the
         * required size — no block buffer is retained, so blockBufferOut stays
         * NULL.  Testing it here would discard every audio buffer, which is
         * precisely what used to happen. */
        size_t bufListSize = 0;
        OSStatus err = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, &bufListSize, NULL, 0,
            kCFAllocatorSystemDefault, kCFAllocatorSystemDefault,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            NULL);
        if (err != noErr || bufListSize == 0) {
            if (c->debug)
                fprintf(stderr, "sck: audio buffer list sizing failed (%d, %zu)\n",
                        (int)err, bufListSize);
            return;
        }

        bufList = (AudioBufferList *)alloca(bufListSize);
        memset(bufList, 0, bufListSize);

        err = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, NULL, bufList, bufListSize,
            kCFAllocatorSystemDefault, kCFAllocatorSystemDefault,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            &blockBuf);
        if (err != noErr) {
            if (blockBuf) CFRelease(blockBuf);
            log_error("CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", err);
            return;
        }
        if (!blockBuf) return;

        /* nSamples counts frames *per channel*, sized from the format the
         * ASBD actually declared — never assumed. */
        int elem_bytes = av_get_bytes_per_sample(c->src_fmt);
        if (elem_bytes <= 0) { CFRelease(blockBuf); return; }

        size_t nSamples = 0;
        if (av_sample_fmt_is_planar(c->src_fmt)) {
            for (UInt32 i = 0; i < bufList->mNumberBuffers; i++) {
                AudioBuffer *ab = &bufList->mBuffers[i];
                if (ab->mData && ab->mDataByteSize > 0) {
                    size_t s = ab->mDataByteSize / (size_t)elem_bytes;
                    if (nSamples == 0 || s < nSamples) nSamples = s;
                }
            }
        } else {
            /* Interleaved: a single buffer holds every channel. */
            AudioBuffer *ab = &bufList->mBuffers[0];
            if (ab->mData && ab->mDataByteSize > 0)
                nSamples = ab->mDataByteSize /
                           ((size_t)elem_bytes * (size_t)c->src_channels);
        }
        if (nSamples == 0) { CFRelease(blockBuf); return; }

        /* Wrap the raw bytes in a frame that describes them truthfully. */
        AVFrame *src = av_frame_alloc();
        if (!src) { CFRelease(blockBuf); return; }
        src->format      = c->src_fmt;
        src->sample_rate = c->src_sample_rate;
        src->nb_samples  = (int)nSamples;
        av_channel_layout_default(&src->ch_layout, c->src_channels);
        if (av_frame_get_buffer(src, 0) < 0) {
            av_frame_free(&src);
            CFRelease(blockBuf);
            return;
        }

        if (av_sample_fmt_is_planar(c->src_fmt)) {
            for (int ch = 0; ch < c->src_channels && ch < (int)bufList->mNumberBuffers; ch++) {
                AudioBuffer *ab = &bufList->mBuffers[ch];
                size_t copy = (size_t)nSamples * (size_t)elem_bytes;
                if (copy > ab->mDataByteSize) copy = ab->mDataByteSize;
                memcpy(src->data[ch], ab->mData, copy);
            }
        } else {
            AudioBuffer *ab = &bufList->mBuffers[0];
            memcpy(src->data[0], ab->mData,
                   (size_t)nSamples * (size_t)elem_bytes * (size_t)c->src_channels);
        }
        CFRelease(blockBuf);

        /* Convert to the canonical format before anything can misread it. */
        AVFrame *dst = av_frame_alloc();
        if (!dst) { av_frame_free(&src); return; }
        dst->format      = AV_SAMPLE_FMT_FLTP;
        dst->sample_rate = 48000;
        av_channel_layout_default(&dst->ch_layout, 2);

        int ret = swr_convert_frame(c->swr, dst, src);
        if (ret == AVERROR_INPUT_CHANGED) {
            /* Device renegotiated mid-stream; retry once. */
            av_frame_unref(dst);
            dst->format      = AV_SAMPLE_FMT_FLTP;
            dst->sample_rate = 48000;
            av_channel_layout_default(&dst->ch_layout, 2);
            ret = swr_convert_frame(c->swr, dst, src);
        }
        av_frame_free(&src);
        if (ret < 0) {
            log_error_av("swr_convert_frame", ret);
            av_frame_free(&dst);
            return;
        }
        if (dst->nb_samples <= 0) { av_frame_free(&dst); return; }

        int64_t apts = sample_pts_us(sampleBuffer);

        pthread_mutex_lock(&c->lock);
        /* Desktop audio flows from the moment the stream starts, which is well
           before the recording is anchored.  Without this guard those samples
           are handed over as the first audio of the recording and the whole
           track leads the video by the startup duration. */
        if (c->stopped || !c->session_started || apts < c->t0_us) {
            pthread_mutex_unlock(&c->lock);
            av_frame_free(&dst);
            return;
        }

        if (c->audio_nb_samples == 0)
            c->audio_head_pts = apts - c->t0_us;

        int new_total = c->audio_nb_samples + dst->nb_samples;
        if (new_total > AUDIO_MAX_SAMPLES) {
            /* Downstream is stalled — a video encode or write is holding the
               shared path.  Drop the newest samples rather than balloon RAM:
               the head stays anchored and the mixer covers the hole with
               silence (see CONTEXT.md: Silence padding). */
            if (c->debug)
                fprintf(stderr, "sck: desktop audio backlog — dropping %d "
                                "samples\n", new_total - AUDIO_MAX_SAMPLES);
            pthread_mutex_unlock(&c->lock);
            av_frame_free(&dst);
            return;
        }
        if (new_total * (int)sizeof(float) > c->audio_plane_cap) {
            int new_cap = c->audio_plane_cap * 2;
            while (new_total * (int)sizeof(float) > new_cap) new_cap *= 2;
            for (int i = 0; i < c->audio_channels; i++) {
                uint8_t *p = (uint8_t *)realloc(c->audio_planes[i], (size_t)new_cap);
                if (!p) {
                    pthread_mutex_unlock(&c->lock);
                    av_frame_free(&dst);
                    return;
                }
                c->audio_planes[i] = p;
            }
            c->audio_plane_cap = new_cap;
        }

        for (int ch = 0; ch < c->audio_channels; ch++)
            memcpy(c->audio_planes[ch] + (size_t)c->audio_nb_samples * sizeof(float),
                   dst->data[ch], (size_t)dst->nb_samples * sizeof(float));
        c->audio_nb_samples = new_total;
        av_frame_free(&dst);

        pthread_mutex_unlock(&c->lock);
    }
}

/*
 * The stream is gone.  Without this the loss is completely silent: SCK simply
 * stops calling back, the record loop goes on holding the last screen frame it
 * received, and the recording continues with a frozen picture for as long as
 * it runs.
 */
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    SckCapture *c = _capture;
    if (!c) return;
    if (c->stopped) return;   /* our own close() — this is the expected path */

    fprintf(stderr, "sck_capture: the system stopped the capture stream: %s\n",
            error ? [[error localizedDescription] UTF8String]
                  : "no reason given");
    /*
     * Name the likeliest cause, because the error rarely does.  Concurrent
     * ScreenCaptureKit streams are only reliable against different displays,
     * so a second app starting a capture of this one — a call sharing your
     * screen, another recorder — is the usual way this happens.
     */
    fprintf(stderr, "sck_capture: another app may have started capturing this "
                    "display; only one\n"
                    "            capture of a given display is reliable at a "
                    "time\n");
    atomic_store(&c->failed, 1);

    /* Wake anyone blocked on a frame that is never going to arrive. */
    dispatch_semaphore_signal(c->video_sem);
}
@end

/* ── public API ────────────────────────────────────────────── */

SckCapture *sck_capture_open(SckCaptureInfo *info)
{
    SckCapture *c = (SckCapture *)calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->debug = (getenv("SCREENCAST_DEBUG") != NULL);
    pthread_mutex_init(&c->lock, NULL);
    c->video_sem = dispatch_semaphore_create(0);
    c->ready_sem = dispatch_semaphore_create(0);
    if (!c->video_sem || !c->ready_sem) { free(c); return NULL; }

    /* do/while(0) + break for ARC-safe cleanup (goto cannot cross ObjC
       __strong variables inside blocks under ARC). */
    do {
        /* ── get shareable content ── */
        __block SCShareableContent *content = nil;
        __block NSError *contentError = nil;
        dispatch_semaphore_t contentSem = dispatch_semaphore_create(0);

        [SCShareableContent getShareableContentWithCompletionHandler:
            ^(SCShareableContent * _Nullable sc, NSError * _Nullable err) {
                content = sc;
                contentError = err;
                dispatch_semaphore_signal(contentSem);
            }];
        dispatch_semaphore_wait(contentSem, DISPATCH_TIME_FOREVER);

        if (contentError || !content) {
            fprintf(stderr, "sck_capture: getShareableContent failed: %s\n",
                    contentError ? [[contentError localizedDescription] UTF8String]
                                 : "no content");
            break;
        }

        /* ── pick the display the cursor is currently on ──
         *
         * CGEventGetLocation reports the cursor in the same global,
         * top-left-origin space that SCDisplay.frame uses.  The previous
         * implementation hit-tested [NSEvent mouseLocation], which is Cocoa
         * bottom-left-origin — mismatched spaces, so the test almost always
         * missed and silently fell through to a "largest display" heuristic.
         * There is no size-based fallback any more: main display, then first. */
        SCDisplay *display = nil;
        CGDirectDisplayID want = kCGNullDirectDisplay;

        CGEventRef ev = CGEventCreate(NULL);
        if (ev) {
            CGPoint cursor = CGEventGetLocation(ev);
            CFRelease(ev);
            CGDirectDisplayID hit[8];
            uint32_t nhit = 0;
            if (CGGetDisplaysWithPoint(cursor, 8, hit, &nhit) == kCGErrorSuccess
                && nhit > 0)
                want = hit[0];
        }
        if (want == kCGNullDirectDisplay)
            want = CGMainDisplayID();

        for (SCDisplay *d in content.displays)
            if (d.displayID == want) { display = d; break; }

        if (!display)
            display = [content.displays firstObject];
        if (!display) {
            fprintf(stderr, "sck_capture: no display found\n");
            break;
        }

        CGRect db = display.frame;
        int pt_w = (int)CGRectGetWidth(db);
        int pt_h = (int)CGRectGetHeight(db);
        if (pt_w <= 0 || pt_h <= 0) {
            fprintf(stderr, "sck_capture: invalid display dimensions\n");
            break;
        }

        char bound_by[32];
        int cap_max = capture_cap();
        c->scale  = display_capture_scale(display.displayID, pt_w, pt_h,
                                          cap_max, bound_by, sizeof(bound_by));
        c->width  = scaled_even(pt_w, c->scale);
        c->height = scaled_even(pt_h, c->scale);
        fprintf(stderr, "[REC] Display: id %u, %dx%d px "
                        "(%dx%d pt, scale %.2f, bound by %s) at (%d,%d)\n",
                (unsigned)display.displayID, c->width, c->height,
                pt_w, pt_h, c->scale, bound_by,
                (int)CGRectGetMinX(db), (int)CGRectGetMinY(db));

        /* ── content filter ── */
        SCContentFilter *filter =
            [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        if (!filter) {
            fprintf(stderr, "sck_capture: could not create content filter\n");
            break;
        }

        /* ── stream config ── */
        SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
        config.width       = c->width;
        config.height      = c->height;
        /* NV12 is what VideoToolbox encodes natively and what the compositor's
           shader reads, so the capture buffer needs no conversion at any point
           between ScreenCaptureKit and the encoder. */
        config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        config.colorMatrix = kCVImageBufferYCbCrMatrix_ITU_R_709_2;
        config.capturesAudio = YES;
        config.excludesCurrentProcessAudio = YES;
        config.sampleRate    = 48000;
        config.channelCount  = 2;
        config.queueDepth = 4;
        config.showsCursor = YES;
        /* Without this SCK delivers at the display's refresh rate — 60 Hz or
           120 Hz — and every one of those frames would be encoded, doubling
           or quadrupling the cost of the recording. */
        config.minimumFrameInterval = CMTimeMake(1, sck_capture_fps());

        /* ── stream output + stream delegate ── */
        /* Built before the stream, because the stream wants it at init: SCK
           holds the delegate weakly, so it is also retained for the lifetime of
           the struct. */
        _SckStreamHelper *helper = [[_SckStreamHelper alloc] init];
        helper.capture = c;
        c->helper = (void *)CFBridgingRetain(helper); /* retain for struct lifetime */

        /* ── stream ── */
        SCStream *stream = [[SCStream alloc] initWithFilter:filter
                                              configuration:config
                                                   delegate:helper];
        if (!stream) {
            fprintf(stderr, "sck_capture: could not create stream\n");
            break;
        }
        c->stream = (void *)CFBridgingRetain(stream);

        dispatch_queue_t sq = dispatch_queue_create(
            "screencast.sck.samples", DISPATCH_QUEUE_SERIAL);
        if (!sq) {
            fprintf(stderr, "sck_capture: could not create sample queue\n");
            break;
        }
        c->sample_queue = sq;   /* prevent ARC release after scope exit */

        /* Audio gets its own queue so video delivery cannot delay samples. */
        dispatch_queue_t aq = dispatch_queue_create(
            "screencast.sck.audio", DISPATCH_QUEUE_SERIAL);
        if (!aq) {
            fprintf(stderr, "sck_capture: could not create audio queue\n");
            break;
        }
        c->audio_queue = aq;

        NSError *addErr = nil;
        if (![stream addStreamOutput:helper
                                type:SCStreamOutputTypeScreen
                  sampleHandlerQueue:sq error:&addErr]) {
            fprintf(stderr, "sck_capture: addStreamOutput(screen) failed: %s\n",
                    addErr ? [[addErr localizedDescription] UTF8String] : "unknown");
            break;
        }
        addErr = nil;
        if (![stream addStreamOutput:helper
                                type:SCStreamOutputTypeAudio
                  sampleHandlerQueue:aq error:&addErr]) {
            fprintf(stderr, "sck_capture: addStreamOutput(audio) failed: %s\n",
                    addErr ? [[addErr localizedDescription] UTF8String] : "unknown");
            break;
        }

        /* ── start capture ── */
        __block NSError *startError = nil;
        dispatch_semaphore_t startSem = dispatch_semaphore_create(0);
        [stream startCaptureWithCompletionHandler:^(NSError * _Nullable err) {
            startError = err;
            dispatch_semaphore_signal(startSem);
        }];
        dispatch_semaphore_wait(startSem, DISPATCH_TIME_FOREVER);

        if (startError) {
            fprintf(stderr, "sck_capture: startCapture failed: %s\n",
                    [[startError localizedDescription] UTF8String]);
            break;
        }

        /* ── wait for the stream to prove it is delivering (5 s timeout) ──
         * The frame itself is not kept: the session is anchored later, and
         * everything captured before that point is startup, not recording. */
        intptr_t sig = dispatch_semaphore_wait(c->ready_sem,
                                dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC));
        if (sig != 0) {
            fprintf(stderr, "sck_capture: timed out waiting for first frame\n");
            break;
        }

        /* ── populate info ── */
        if (info) {
            info->width      = c->width;
            info->height     = c->height;
            info->display_id = (unsigned int)display.displayID;
            info->scale      = c->scale;
            info->pix_fmt = AV_PIX_FMT_NV12;
            if (c->audio_sample_rate > 0) {
                info->sample_rate = c->audio_sample_rate;
                info->channels    = c->audio_channels;
                info->sample_fmt  = c->audio_fmt;
            } else {
                info->sample_rate = 48000;
                info->channels    = 2;
                info->sample_fmt  = AV_SAMPLE_FMT_FLTP;
            }
            c->info = *info;
        }

        return c;

    } while (0);

    /* any break lands here */
    sck_capture_close(c);
    return NULL;
}

void sck_capture_start_session(SckCapture *c, int64_t t0_us)
{
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    c->t0_us           = t0_us;
    c->session_started = 1;
    /* Anything queued during startup predates the session — discard it. */
    for (int i = 0; i < VIDEO_QUEUE_DEPTH; i++) {
        if (c->video_queue[i]) CVPixelBufferRelease(c->video_queue[i]);
        c->video_queue[i] = NULL;
    }
    c->video_head = c->video_count = 0;
    c->audio_nb_samples = 0;
    pthread_mutex_unlock(&c->lock);
}

void sck_capture_release_frame(void *pixbuf)
{
    if (pixbuf) CVPixelBufferRelease((CVPixelBufferRef)pixbuf);
}

/* Pop the head of the video queue.  The caller has already claimed a slot on
   video_sem, so a frame is normally present; the queue can still be empty if
   close() signalled the semaphore to wake a waiter. */
static void *dequeue_video(SckCapture *c, int64_t *pts_us)
{
    pthread_mutex_lock(&c->lock);
    if (c->stopped || c->video_count == 0) {
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }

    CVPixelBufferRef pb = c->video_queue[c->video_head];
    if (pts_us) *pts_us = c->video_pts[c->video_head];
    c->video_queue[c->video_head] = NULL;
    c->video_head = (c->video_head + 1) % VIDEO_QUEUE_DEPTH;
    c->video_count--;
    if (c->video_count == 0) c->drop_warned = 0;  /* backlog episode over */
    pthread_mutex_unlock(&c->lock);
    return pb;   /* ownership passes to the caller */
}

void *sck_capture_grab_video(SckCapture *c, int64_t *pts_us)
{
    if (!c) return NULL;

    /* Timed wait — allows the caller to re-check g_recording/g_running
       periodically (every ~500 ms) rather than blocking forever. */
    intptr_t sig = dispatch_semaphore_wait(c->video_sem,
                        dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC));
    if (sig != 0) return NULL;  /* timeout — caller should retry or check stop */

    return dequeue_video(c, pts_us);
}

int sck_capture_failed(SckCapture *c)
{
    if (!c) return 0;
    if (atomic_load(&c->failed)) return 1;

    /* A stream can also die without SCK saying anything, so note when the
       callbacks stop.  This only warns.  SCK is entitled to go quiet — the
       display being static is the ordinary reason — and ending a recording of
       a still screen would be a worse bug than the one being guarded here. */
    if (!c->ready_signalled || c->stall_warned) return 0;

    long long last = atomic_load(&c->last_video_cb_us);
    if (last > 0 && sck_host_time_us() - (int64_t)last > SCK_STALL_WARN_US) {
        c->stall_warned = 1;
        fprintf(stderr,
                "sck_capture: no video callback for %llds (callbacks %ld) — "
                "the screen may have stopped updating\n",
                (long long)(SCK_STALL_WARN_US / 1000000), c->video_cb);
    }
    return 0;
}

void *sck_capture_try_grab_video(SckCapture *c, int64_t *pts_us)
{
    if (!c) return NULL;

    if (dispatch_semaphore_wait(c->video_sem, DISPATCH_TIME_NOW) != 0)
        return NULL;   /* nothing queued */

    return dequeue_video(c, pts_us);
}

AVFrame *sck_capture_grab_audio(SckCapture *c, int64_t *pts_us)
{
    if (!c) return NULL;
    pthread_mutex_lock(&c->lock);

    if (c->stopped || c->audio_nb_samples <= 0 || c->audio_channels <= 0) {
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }

    if (pts_us) *pts_us = c->audio_head_pts;

    int nb_samples = c->audio_nb_samples;
    int channels   = c->audio_channels;

    AVFrame *frame = av_frame_alloc();
    if (!frame) { pthread_mutex_unlock(&c->lock); return NULL; }

    frame->format      = c->audio_fmt;
    frame->sample_rate = c->audio_sample_rate;
    frame->nb_samples  = nb_samples;
    av_channel_layout_default(&frame->ch_layout, channels);

    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }

    if (c->audio_fmt == AV_SAMPLE_FMT_FLT) {
        memcpy(frame->data[0], c->audio_planes[0],
               (size_t)nb_samples * sizeof(float) * (size_t)channels);
    } else {
        for (int ch = 0; ch < channels; ch++)
            memcpy(frame->data[ch], c->audio_planes[ch],
                   (size_t)nb_samples * sizeof(float));
    }

    c->audio_nb_samples = 0;
    pthread_mutex_unlock(&c->lock);
    return frame;
}

void sck_capture_close(SckCapture *c)
{
    if (!c) return;

    if (c->debug)
        fprintf(stderr, "sck: totals — video callbacks %ld (idle %ld, dropped %ld), "
                        "audio callbacks %ld\n",
                c->video_cb, c->video_idle, c->video_dropped, c->audio_cb);

    pthread_mutex_lock(&c->lock);
    c->stopped = 1;
    pthread_mutex_unlock(&c->lock);
    dispatch_semaphore_signal(c->video_sem);

    if (c->stream) {
        SCStream *stream = (__bridge SCStream *)c->stream;
        dispatch_semaphore_t stopSem = dispatch_semaphore_create(0);
        [stream stopCaptureWithCompletionHandler:^(NSError * _Nullable err) {
            if (err) fprintf(stderr, "sck_capture: stopCapture error: %s\n",
                             [[err localizedDescription] UTF8String]);
            dispatch_semaphore_signal(stopSem);
        }];
        dispatch_semaphore_wait(stopSem,
            dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC));
        CFBridgingRelease(c->stream);
        c->stream = NULL;
    }

    /* stopCapture's completion means no new samples will be scheduled.  Drain
       both serial delivery queues before releasing their delegate and the C
       state that callbacks reference. */
    if (c->sample_queue)
        dispatch_sync(c->sample_queue, ^{});
    if (c->audio_queue)
        dispatch_sync(c->audio_queue, ^{});

    if (c->helper) {
        CFBridgingRelease(c->helper);
        c->helper = NULL;
    }

    for (int i = 0; i < VIDEO_QUEUE_DEPTH; i++)
        if (c->video_queue[i]) CVPixelBufferRelease(c->video_queue[i]);

    for (int i = 0; i < 2; i++)
        free(c->audio_planes[i]);

    swr_free(&c->swr);

    pthread_mutex_destroy(&c->lock);
    c->sample_queue = nil;  /* release dispatch queues before freeing struct */
    c->audio_queue  = nil;
    free(c);
}
