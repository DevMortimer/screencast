// sck_capture.m — ScreenCaptureKit display + system audio capture backend
//
// Wraps SCShareableContent → SCContentFilter → SCStream with a synchronous
// grab interface.  Frames arrive on an SCK-managed dispatch queue; the stream
// output delegate pushes video into a bounded circular buffer (signalled via
// dispatch semaphore) and accumulates audio samples in per-channel plane
// buffers.

#import "sck_capture.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <AppKit/AppKit.h>
#import <Accelerate/Accelerate.h>
#import <pthread.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

/* ── constants ─────────────────────────────────────────────── */

/* ~1 s at 30 fps.  Deep enough to absorb the record loop briefly holding a
   frame back while it waits for the matching webcam frame to arrive. */
#define VIDEO_QUEUE_DEPTH   32
#define AUDIO_INIT_SAMPLES  (1024 * 8)
#define SCK_TARGET_FPS      30

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
    int                     width;
    int                     height;
    pthread_mutex_t         lock;
    SckCaptureInfo          info;
    int                     stopped;

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

@interface _SckStreamHelper : NSObject <SCStreamOutput>
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
            /* Encoder is behind.  Video is the elastic resource: drop the
               oldest frame rather than stall capture or delay a timestamp. */
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

            int ch = (int)asbd->mChannelsPerFrame;
            if (ch < 1) ch = 1;
            if (ch > 2) ch = 2;

            c->audio_channels    = ch;
            c->audio_sample_rate = (asbd->mSampleRate > 0) ? (int)asbd->mSampleRate : 48000;
            c->audio_fmt = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved)
                               ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;

            c->audio_plane_cap = AUDIO_INIT_SAMPLES * (int)sizeof(float);
            for (int i = 0; i < c->audio_channels; i++) {
                c->audio_planes[i] = (uint8_t *)malloc((size_t)c->audio_plane_cap);
                if (!c->audio_planes[i]) {
                    fprintf(stderr, "sck_capture: audio plane alloc failed\n");
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

        /* nSamples counts frames *per channel*.  For interleaved input a single
         * buffer holds every channel, so the byte count must be divided by the
         * channel count as well. */
        size_t nSamples = 0;
        if (c->audio_fmt == AV_SAMPLE_FMT_FLT) {
            AudioBuffer *ab = &bufList->mBuffers[0];
            if (ab->mData && c->audio_channels > 0)
                nSamples = ab->mDataByteSize /
                           (sizeof(float) * (size_t)c->audio_channels);
        } else {
            for (UInt32 i = 0; i < bufList->mNumberBuffers; i++) {
                AudioBuffer *ab = &bufList->mBuffers[i];
                if (!ab->mData || ab->mDataByteSize == 0) continue;
                size_t s = ab->mDataByteSize / sizeof(float);
                if (nSamples == 0 || s < nSamples) nSamples = s;
            }
        }

        if (nSamples == 0) { CFRelease(blockBuf); return; }

        int64_t apts = sample_pts_us(sampleBuffer);

        pthread_mutex_lock(&c->lock);
        /* Desktop audio flows from the moment the stream starts, which is well
           before the recording is anchored.  Without this guard those samples
           are handed over as the first audio of the recording and the whole
           track leads the video by the startup duration. */
        if (c->stopped || !c->session_started || apts < c->t0_us) {
            pthread_mutex_unlock(&c->lock);
            CFRelease(blockBuf);
            return;
        }

        if (c->audio_nb_samples == 0)
            c->audio_head_pts = apts - c->t0_us;

        int new_total = c->audio_nb_samples + (int)nSamples;
        /* Interleaved input packs every channel into plane 0, so it needs
         * channels× the bytes a planar frame count implies. */
        int bytes_per_frame = (int)sizeof(float) *
            (c->audio_fmt == AV_SAMPLE_FMT_FLT ? c->audio_channels : 1);
        if (new_total * bytes_per_frame > c->audio_plane_cap) {
            int new_cap = c->audio_plane_cap * 2;
            while (new_total * bytes_per_frame > new_cap) new_cap *= 2;
            for (int i = 0; i < c->audio_channels; i++) {
                uint8_t *p = (uint8_t *)realloc(c->audio_planes[i], (size_t)new_cap);
                if (!p) {
                    pthread_mutex_unlock(&c->lock);
                    CFRelease(blockBuf);
                    return;
                }
                c->audio_planes[i] = p;
            }
            c->audio_plane_cap = new_cap;
        }

        if (c->audio_fmt == AV_SAMPLE_FMT_FLT) {
            AudioBuffer *ab = &bufList->mBuffers[0];
            float *dst = (float *)c->audio_planes[0];
            memcpy(dst + c->audio_nb_samples * c->audio_channels,
                   ab->mData,
                   nSamples * sizeof(float) * (size_t)c->audio_channels);
        } else {
            for (int ch = 0; ch < c->audio_channels && ch < (int)bufList->mNumberBuffers; ch++) {
                AudioBuffer *ab = &bufList->mBuffers[ch];
                float *dst = (float *)c->audio_planes[ch];
                size_t copy = nSamples * sizeof(float);
                if (copy > ab->mDataByteSize) copy = ab->mDataByteSize;
                memcpy(dst + c->audio_nb_samples, ab->mData, copy);
            }
        }
        c->audio_nb_samples = new_total;

        pthread_mutex_unlock(&c->lock);
        CFRelease(blockBuf);
    }
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
        c->width  = (int)CGRectGetWidth(db);
        c->height = (int)CGRectGetHeight(db);
        fprintf(stderr, "[REC] Display: id %u, %dx%d at (%d,%d)\n",
                (unsigned)display.displayID, c->width, c->height,
                (int)CGRectGetMinX(db), (int)CGRectGetMinY(db));
        if (c->width <= 0 || c->height <= 0) {
            fprintf(stderr, "sck_capture: invalid display dimensions\n");
            break;
        }

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
           120 Hz — and every one of those frames was being converted and
           encoded, doubling or quadrupling the cost of a 30 fps recording. */
        config.minimumFrameInterval = CMTimeMake(1, SCK_TARGET_FPS);

        /* ── stream ── */
        SCStream *stream = [[SCStream alloc] initWithFilter:filter
                                              configuration:config
                                                   delegate:nil];
        if (!stream) {
            fprintf(stderr, "sck_capture: could not create stream\n");
            break;
        }
        c->stream = (void *)CFBridgingRetain(stream);

        /* ── stream output delegate ── */
        _SckStreamHelper *helper = [[_SckStreamHelper alloc] init];
        helper.capture = c;
        c->helper = (void *)CFBridgingRetain(helper); /* retain for struct lifetime */

        dispatch_queue_t sq = dispatch_queue_create(
            "screencast.sck.samples", DISPATCH_QUEUE_SERIAL);
        if (!sq) {
            fprintf(stderr, "sck_capture: could not create sample queue\n");
            break;
        }
        c->sample_queue = sq;   /* prevent ARC release after scope exit */

        /* Audio gets its own queue: a shared serial queue would serialise
         * every audio buffer behind a full-frame BGRA memcpy at 30 fps. */
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
            info->width   = c->width;
            info->height  = c->height;
            info->pix_fmt = AV_PIX_FMT_BGRA;
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

void *sck_capture_grab_video(SckCapture *c, int64_t *pts_us)
{
    if (!c) return NULL;

    /* Timed wait — allows the caller to re-check g_recording/g_running
       periodically (every ~500 ms) rather than blocking forever. */
    intptr_t sig = dispatch_semaphore_wait(c->video_sem,
                        dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC));
    if (sig != 0) return NULL;  /* timeout — caller should retry or check stop */

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
    pthread_mutex_unlock(&c->lock);
    return pb;   /* ownership passes to the caller */
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

    if (c->helper) {
        CFBridgingRelease(c->helper);
        c->helper = NULL;
    }

    for (int i = 0; i < VIDEO_QUEUE_DEPTH; i++)
        if (c->video_queue[i]) CVPixelBufferRelease(c->video_queue[i]);

    for (int i = 0; i < 2; i++)
        free(c->audio_planes[i]);

    pthread_mutex_destroy(&c->lock);
    c->sample_queue = nil;  /* release dispatch queues before freeing struct */
    c->audio_queue  = nil;
    free(c);
}
