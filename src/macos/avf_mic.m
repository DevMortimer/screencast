// avf_mic.m — AVFoundation microphone capture backend
//
// Wraps AVCaptureSession + AVCaptureAudioDataOutput with a blocking read
// interface that delivers 48 kHz stereo FLTP — the canonical format expected
// by the mixer in mixer.c.
//
// Thread model
//   The AVCapture delegate callback runs on a private serial queue.  The
//   avf_mic_read caller (typically the main loop) blocks on a condition
//   variable.  A mutex protects the circular frame queue.
//
// Format conversion
//   AVFoundation delivers Float32 PCM (interleaved or non-interleaved,
//   device-native sample rate, 1-2 channels).  libswresample converts to
//   48 kHz stereo FLTP on the callback thread before enqueueing.
//
// Resilience
//   If no microphone is present or permission is denied, avf_mic_open returns
//   NULL — the caller keeps going without mic audio.  Never abort/crash.

#import "avf_mic.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreMedia/CoreMedia.h>
#import <libswresample/swresample.h>
#import <libavutil/frame.h>
#import <libavutil/channel_layout.h>
#import <libavutil/error.h>
#import <libavutil/mem.h>
#import <pthread.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

/* ── constants ─────────────────────────────────────────────── */

#define QUEUE_CAP  64      /* circular-buffer depth (~1.3 s at 48 kHz / 1024 frames) */

/* ── private delegate interface ────────────────────────────── */

@interface _AvfMicDelegate : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate>
@property (nonatomic, unsafe_unretained) AvfMic *mic;
@end

/* ── opaque struct ─────────────────────────────────────────── */

struct AvfMic {
    /* AVFoundation objects */
    AVCaptureSession        *session;
    AVCaptureDeviceInput    *input;
    AVCaptureAudioDataOutput *output;
    _AvfMicDelegate         *delegate;

    /* source format (from first CMSampleBuffer) — immutable after init */
    int                     src_sample_rate;
    int                     src_channels;
    enum AVSampleFormat     src_fmt;    /* FLT (interleaved) or FLTP (planar) */

    /* resampler: src → 48 kHz stereo FLTP */
    SwrContext              *swr;

    /* circular frame queue */
    AVFrame                 *queue[QUEUE_CAP];
    int64_t                 queue_pts[QUEUE_CAP]; /* session-relative µs */
    int                     head;       /* dequeue position */
    int                     tail;       /* enqueue position */
    int                     count;

    /* Session timeline — see avf_mic_start_session(). */
    int64_t                 t0_us;
    int                     session_started;

    /* synchronisation */
    pthread_mutex_t         lock;
    pthread_cond_t          cond;

    /* One-shot format info returned to caller (48 kHz stereo FLTP) */
    AvfMicInfo              info;

    /* tear-down flag — signals avf_mic_read to stop waiting */
    int                     stopped;
};

/* ── helpers ───────────────────────────────────────────────── */

static void log_error(const char *msg, OSStatus code)
{
    fprintf(stderr, "avf_mic: %s (osstatus %d)\n", msg, (int)code);
}

static void log_error_av(const char *msg, int ret)
{
    char buf[128];
    av_strerror(ret, buf, sizeof(buf));
    fprintf(stderr, "avf_mic: %s: %s\n", msg, buf);
}

/* Derive an AVSampleFormat + channel count from the ASBD. */
static int asbd_to_avfmt(const AudioStreamBasicDescription *asbd,
                         enum AVSampleFormat *out_fmt,
                         int *out_channels)
{
    if (!(asbd->mFormatID == kAudioFormatLinearPCM &&
          asbd->mFormatFlags & kAudioFormatFlagIsFloat)) {
        fprintf(stderr, "avf_mic: unsupported format (not Float PCM)\n");
        return -1;
    }

    int ch = (int)asbd->mChannelsPerFrame;
    if (ch < 1) ch = 1;
    if (ch > 2) {
        fprintf(stderr, "avf_mic: >2 channel mic not expected (%d)\n", ch);
        ch = 2;
    }

    *out_channels = ch;
    *out_fmt      = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved)
                        ? AV_SAMPLE_FMT_FLTP
                        : AV_SAMPLE_FMT_FLT;
    return 0;
}

/* ── AVCapture delegate ────────────────────────────────────── */

@implementation _AvfMicDelegate

- (void)captureOutput:(AVCaptureAudioDataOutput *)output
  didOutputSampleBuffer:(CMSampleBufferRef)sbuf
         fromConnection:(AVCaptureConnection *)connection
{
    (void)output;
    (void)connection;

    AvfMic *m = self.mic;
    if (!m || m->stopped) return;

    /* ---- source format (one-time init) ---- */
    if (!m->swr) {
        CMAudioFormatDescriptionRef fmtDesc =
            CMSampleBufferGetFormatDescription(sbuf);
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

        m->src_sample_rate = src_rate;
        m->src_channels    = src_channels;
        m->src_fmt         = src_fmt;

        /* Build resampler: src -> 48 kHz stereo FLTP */
        AVChannelLayout out_layout, src_layout;
        av_channel_layout_default(&out_layout, 2);
        av_channel_layout_default(&src_layout, m->src_channels);

        SwrContext *swr = NULL;
        int ret = swr_alloc_set_opts2(&swr,
                     &out_layout, AV_SAMPLE_FMT_FLTP, 48000,
                     &src_layout, m->src_fmt,          m->src_sample_rate,
                     0, NULL);
        av_channel_layout_uninit(&out_layout);
        av_channel_layout_uninit(&src_layout);
        if (ret < 0 || !swr || swr_init(swr) < 0) {
            log_error_av("swr init", ret < 0 ? ret : AVERROR_EXTERNAL);
            if (swr) swr_free(&swr);
            return;
        }
        m->swr = swr;
    }

    /* ---- extract audio data ---- */
    CMBlockBufferRef blockBuf = NULL;
    AudioBufferList *bufList  = NULL;

    size_t bufListSize;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sbuf, &bufListSize, NULL, 0,
        kCFAllocatorSystemDefault, kCFAllocatorSystemDefault,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
        &blockBuf);

    if (!bufListSize) {
        if (blockBuf) CFRelease(blockBuf);
        return;
    }

    if (m->src_channels <= 0) { CFRelease(blockBuf); return; }

    /* We need bufListSize bytes — use alloca for the stack-friendly case. */
    bufList = alloca(bufListSize);
    memset(bufList, 0, bufListSize);
    bufList->mNumberBuffers = (UInt32)m->src_channels;

    OSStatus err = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sbuf, NULL, bufList, bufListSize,
        kCFAllocatorSystemDefault, kCFAllocatorSystemDefault,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
        &blockBuf);
    if (err != noErr) {
        if (blockBuf) CFRelease(blockBuf);
        log_error("CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", err);
        return;
    }
    /* blockBuf is now retained; release when done. */

    /* Number of samples from the first non-empty buffer. */
    size_t nSamples = 0;
    for (UInt32 i = 0; i < bufList->mNumberBuffers; i++) {
        AudioBuffer *ab = &bufList->mBuffers[i];
        if (ab->mData && ab->mDataByteSize > 0) {
            size_t s = ab->mDataByteSize / (sizeof(float));
            if (nSamples == 0 || s < nSamples) nSamples = s;
        }
    }
    if (nSamples == 0) {
        CFRelease(blockBuf);
        return;
    }

    /* ---- wrap into an AVFrame ---- */
    AVFrame *src_frame = av_frame_alloc();
    if (!src_frame) { CFRelease(blockBuf); return; }

    src_frame->format      = m->src_fmt;
    src_frame->sample_rate = m->src_sample_rate;
    av_channel_layout_default(&src_frame->ch_layout, m->src_channels);
    src_frame->nb_samples  = (int)nSamples;

    /* Allocate internal buffer and copy data. */
    if (av_frame_get_buffer(src_frame, 0) < 0) {
        av_frame_free(&src_frame);
        CFRelease(blockBuf);
        return;
    }

    if (m->src_fmt == AV_SAMPLE_FMT_FLT) {
        /* Interleaved: single buffer, L,R,L,R,... */
        AudioBuffer *ab = &bufList->mBuffers[0];
        memcpy(src_frame->data[0], ab->mData,
               (size_t)nSamples * sizeof(float) * (size_t)m->src_channels);
    } else {
        /* Planar non-interleaved: each channel in its own AudioBuffer. */
        for (int ch = 0; ch < m->src_channels && ch < (int)bufList->mNumberBuffers; ch++) {
            AudioBuffer *ab = &bufList->mBuffers[ch];
            size_t copy = (size_t)nSamples * sizeof(float);
            if (copy > ab->mDataByteSize) copy = ab->mDataByteSize;
            memcpy(src_frame->data[ch], ab->mData, copy);
        }
    }
    CFRelease(blockBuf);

    /* ---- resample to 48 kHz stereo FLTP ---- */
    AVFrame *dst = av_frame_alloc();
    if (!dst) { av_frame_free(&src_frame); return; }
    dst->format      = AV_SAMPLE_FMT_FLTP;
    dst->sample_rate = 48000;
    av_channel_layout_default(&dst->ch_layout, 2);

    int ret = swr_convert_frame(m->swr, dst, src_frame);
    if (ret == AVERROR_INPUT_CHANGED) {
        /* Parameters shifted (e.g. sample rate changed mid-stream). */
        av_frame_unref(dst);
        dst->format      = AV_SAMPLE_FMT_FLTP;
        dst->sample_rate = 48000;
        av_channel_layout_default(&dst->ch_layout, 2);
        ret = swr_convert_frame(m->swr, dst, src_frame);
    }
    av_frame_free(&src_frame);
    if (ret < 0) {
        log_error_av("swr_convert_frame", ret);
        av_frame_free(&dst);
        return;
    }
    if (dst->nb_samples <= 0) {
        av_frame_free(&dst);
        return;
    }

    /* ---- enqueue ---- */
    CMTime cmpts = CMSampleBufferGetPresentationTimeStamp(sbuf);
    int64_t pts = CMTIME_IS_VALID(cmpts)
        ? CMTimeConvertScale(cmpts, 1000000,
                             kCMTimeRoundingMethod_Default).value
        : 0;

    pthread_mutex_lock(&m->lock);

    /* Samples captured while the rest of the recording was still starting up
       are not part of it. */
    if (!m->session_started || pts < m->t0_us) {
        pthread_mutex_unlock(&m->lock);
        av_frame_free(&dst);
        return;
    }

    if (m->count >= QUEUE_CAP) {
        /* Queue full — drop oldest frame to stay real-time. */
        av_frame_free(&m->queue[m->head]);
        m->head = (m->head + 1) % QUEUE_CAP;
        m->count--;
    }

    m->queue[m->tail]     = dst;
    m->queue_pts[m->tail] = pts - m->t0_us;
    m->tail               = (m->tail + 1) % QUEUE_CAP;
    m->count++;

    pthread_cond_signal(&m->cond);
    pthread_mutex_unlock(&m->lock);
}

@end

/* ── public API ────────────────────────────────────────────── */

AvfMic *avf_mic_open(AvfMicInfo *info)
{
    /* ---- check mic availability ---- */
    AVCaptureDevice *device =
        [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
    if (!device) {
        fprintf(stderr, "avf_mic: no default microphone found\n");
        return NULL;
    }

    /* ---- permission check (macOS 10.14+) ---- */
#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 101400
    AVAuthorizationStatus auth =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (auth == AVAuthorizationStatusDenied ||
        auth == AVAuthorizationStatusRestricted) {
        fprintf(stderr, "avf_mic: microphone access denied\n");
        return NULL;
    }
    /* If not determined yet, we proceed — the system will prompt on first
     * connection.  If denied later, the delegate simply won't fire. */
#endif

    /* ---- allocate ---- */
    AvfMic *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->cond, NULL);

    /* ---- AVCaptureSession ---- */
    m->session = [[AVCaptureSession alloc] init];
    if (!m->session) goto fail;

    /* ---- device input ---- */
    NSError *error = nil;
    m->input = [[AVCaptureDeviceInput alloc] initWithDevice:device error:&error];
    if (!m->input) {
        fprintf(stderr, "avf_mic: could not create device input: %s\n",
                error ? [[error localizedDescription] UTF8String] : "unknown");
        goto fail;
    }

    if (![m->session canAddInput:m->input]) {
        fprintf(stderr, "avf_mic: session cannot add input\n");
        goto fail;
    }
    [m->session addInput:m->input];

    /* ---- audio data output ---- */
    m->output = [[AVCaptureAudioDataOutput alloc] init];
    if (!m->output) goto fail;

    if (![m->session canAddOutput:m->output]) {
        fprintf(stderr, "avf_mic: session cannot add output\n");
        goto fail;
    }
    [m->session addOutput:m->output];

    /* ---- delegate (retained by output) ---- */
    m->delegate = [[_AvfMicDelegate alloc] init];
    m->delegate.mic = m;

    dispatch_queue_t dq = dispatch_queue_create("avf_mic.callback",
                                                DISPATCH_QUEUE_SERIAL);
    [m->output setSampleBufferDelegate:m->delegate queue:dq];
#if !__has_feature(objc_arc)
    [dq release];
#endif

    /* ---- start ---- */
    [m->session startRunning];

    /* Populate info with the canonical mixer format — the caller doesn't
     * need the device-native format; they consume what we output. */
    if (info) {
        info->sample_rate = 48000;
        av_channel_layout_default(&info->ch_layout, 2);
        info->sample_fmt  = AV_SAMPLE_FMT_FLTP;
        /* Cache for querying after open. */
        m->info = *info;
    }

    return m;

fail:
    avf_mic_close(m);
    return NULL;
}

void avf_mic_start_session(AvfMic *m, int64_t t0_us)
{
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->t0_us           = t0_us;
    m->session_started = 1;
    for (int i = 0; i < QUEUE_CAP; i++)
        av_frame_free(&m->queue[i]);
    m->head = m->tail = m->count = 0;
    pthread_mutex_unlock(&m->lock);
}

AVFrame *avf_mic_read(AvfMic *m, int64_t *pts_us)
{
    if (!m) return NULL;

    pthread_mutex_lock(&m->lock);

    while (m->count == 0 && !m->stopped) {
        /* Timed wait — unblocks every ~500ms so the caller (mic thread)
           can re-check s_rec_open and exit promptly on stop. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 500 * 1000000L;  /* 500 ms */
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec++;
        }
        int rc = pthread_cond_timedwait(&m->cond, &m->lock, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&m->lock);
            return NULL;  /* timeout — caller retries or checks stop */
        }
    }

    if (m->count == 0 || m->stopped) {
        pthread_mutex_unlock(&m->lock);
        return NULL;
    }

    AVFrame *frame = m->queue[m->head];
    if (pts_us) *pts_us = m->queue_pts[m->head];
    m->queue[m->head] = NULL;   /* ownership transferred to caller */
    m->head = (m->head + 1) % QUEUE_CAP;
    m->count--;

    pthread_mutex_unlock(&m->lock);
    return frame;
}

void avf_mic_close(AvfMic *m)
{
    if (!m) return;

    /* Signal any blocked avf_mic_read to wake up and return NULL. */
    pthread_mutex_lock(&m->lock);
    m->stopped = 1;
    pthread_cond_broadcast(&m->cond);
    pthread_mutex_unlock(&m->lock);

    /* Tear down AVFoundation — this must happen *after* signalling the cond
     * so the read thread can exit cleanly. */
    if (m->session) {
        [m->session stopRunning];
        if (m->input)  [m->session removeInput:m->input];
        if (m->output) [m->session removeOutput:m->output];
    }

    /* Delegate unregister: set delegate to nil on the output's queue to
     * prevent further callbacks, then release our reference. */
    if (m->output && m->delegate) {
        dispatch_queue_t dq = [m->output sampleBufferCallbackQueue];
        if (dq) {
            dispatch_sync(dq, ^{
                [m->output setSampleBufferDelegate:nil queue:NULL];
            });
        } else {
            [m->output setSampleBufferDelegate:nil queue:NULL];
        }
    }

#if !__has_feature(objc_arc)
    [m->delegate release];
    [m->output release];
    [m->input release];
    [m->session release];
#endif

    /* Drain the frame queue. */
    for (int i = 0; i < QUEUE_CAP; i++) {
        av_frame_free(&m->queue[i]);
    }

    swr_free(&m->swr);
    av_channel_layout_uninit(&m->info.ch_layout);

    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->cond);

    free(m);
}
