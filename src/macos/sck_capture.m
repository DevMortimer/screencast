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
#import <Accelerate/Accelerate.h>
#import <pthread.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

/* ── constants ─────────────────────────────────────────────── */

#define VIDEO_QUEUE_DEPTH   8
#define AUDIO_INIT_SAMPLES  (1024 * 8)

/* ── opaque struct ─────────────────────────────────────────── */

struct SckCapture {
    AVFrame                *video_queue[VIDEO_QUEUE_DEPTH];
    int                     video_head;
    int                     video_count;
    dispatch_semaphore_t    video_sem;

    uint8_t                *audio_planes[2];
    int                     audio_nb_samples;
    int                     audio_plane_cap;
    int                     audio_channels;
    int                     audio_sample_rate;
    enum AVSampleFormat     audio_fmt;

    void                   *stream;       /* CFBridgingRetain of SCStream */
    int                     width;
    int                     height;
    pthread_mutex_t         lock;
    SckCaptureInfo          info;
    int                     stopped;
};

/* ── helpers ───────────────────────────────────────────────── */

static void log_error(const char *msg, OSStatus code)
{
    fprintf(stderr, "sck_capture: %s (osstatus %d)\n", msg, (int)code);
}

static AVFrame *copy_bgra_pixel_buffer(CVPixelBufferRef pb)
{
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);

    int       w      = (int)CVPixelBufferGetWidth(pb);
    int       h      = (int)CVPixelBufferGetHeight(pb);
    int       stride = (int)CVPixelBufferGetBytesPerRow(pb);
    uint8_t  *src    = (uint8_t *)CVPixelBufferGetBaseAddress(pb);

    AVFrame *frame = av_frame_alloc();
    if (!frame) { CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); return NULL; }

    frame->format       = AV_PIX_FMT_BGRA;
    frame->width        = w;
    frame->height       = h;
    frame->linesize[0]  = stride;

    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        return NULL;
    }

    for (int y = 0; y < h; y++)
        memcpy(frame->data[0] + y * (size_t)stride,
               src           + y * (size_t)stride, (size_t)stride);

    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return frame;
}

static AVFrame *convert_nv12_to_bgra(CVPixelBufferRef pb)
{
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);

    int       w        = (int)CVPixelBufferGetWidth(pb);
    int       h        = (int)CVPixelBufferGetHeight(pb);
    uint8_t  *y_plane  = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    int       y_stride = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    uint8_t  *uv_plane = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    int       uv_stride= (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 1);

    uint8_t *bgra_buf = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    if (!bgra_buf) {
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        return NULL;
    }

    vImage_YpCbCrToARGB conv;
    vImage_YpCbCrPixelRange range = {
        .Yp_bias = 16, .CbCr_bias = 128,
        .YpRangeMax = 235, .CbCrRangeMax = 240,
        .YpMax = 235, .YpMin = 16,
        .CbCrMax = 240, .CbCrMin = 16
    };
    vImageConvert_YpCbCrToARGB_GenerateConversion(
        kvImage_YpCbCrToARGBMatrix_ITU_R_601_4,
        &range, &conv,
        kvImage420Yp8_Cb8_Cr8,
        kvImageARGB8888, kvImageNoFlags);

    vImage_Buffer srcY  = { y_plane,  (vImagePixelCount)h, (vImagePixelCount)w, (size_t)y_stride };
    vImage_Buffer srcUV = { uv_plane, (vImagePixelCount)h, (vImagePixelCount)w, (size_t)uv_stride };
    vImage_Buffer dst   = { bgra_buf, (vImagePixelCount)h, (vImagePixelCount)w, (size_t)w * 4 };

    vImage_Error err = vImageConvert_420Yp8_CbCr8ToARGB8888(
        &srcY, &srcUV, &dst, &conv, NULL, 0, kvImageNoFlags);

    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);

    if (err != kvImageNoError) {
        fprintf(stderr, "sck_capture: vImage NV12->BGRA failed (%zd)\n", err);
        free(bgra_buf);
        return NULL;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) { free(bgra_buf); return NULL; }

    frame->format       = AV_PIX_FMT_BGRA;
    frame->width        = w;
    frame->height       = h;
    frame->linesize[0]  = w * 4;

    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        free(bgra_buf);
        return NULL;
    }

    memcpy(frame->data[0], bgra_buf, (size_t)w * (size_t)h * 4);
    free(bgra_buf);
    return frame;
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

    static int dbg_frame = 0;
    dbg_frame++;
    if (dbg_frame <= 3 || dbg_frame % 60 == 0)
        fprintf(stderr, "sck: callback #%d type=%ld\n", dbg_frame, (long)type);

    if (type == SCStreamOutputTypeScreen) {
        CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
        if (!pb) return;

        AVFrame *frame = NULL;
        OSType fmt = CVPixelBufferGetPixelFormatType(pb);

        if (fmt == kCVPixelFormatType_32BGRA)
            frame = copy_bgra_pixel_buffer(pb);
        else if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                 fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
            frame = convert_nv12_to_bgra(pb);
        else {
            fprintf(stderr, "sck_capture: unexpected pixel format %c%c%c%c\n",
                    (char)(fmt >> 24), (char)(fmt >> 16),
                    (char)(fmt >> 8),  (char)fmt);
            return;
        }

        if (!frame) return;

        pthread_mutex_lock(&c->lock);
        if (c->stopped) {
            av_frame_free(&frame);
            pthread_mutex_unlock(&c->lock);
            return;
        }

        if (c->video_count >= VIDEO_QUEUE_DEPTH) {
            av_frame_free(&c->video_queue[c->video_head]);
            c->video_head = (c->video_head + 1) % VIDEO_QUEUE_DEPTH;
            c->video_count--;
        }

        int tail = (c->video_head + c->video_count) % VIDEO_QUEUE_DEPTH;
        c->video_queue[tail] = frame;
        c->video_count++;
        pthread_mutex_unlock(&c->lock);
        dispatch_semaphore_signal(c->video_sem);

    } else if (type == SCStreamOutputTypeAudio) {
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

        size_t bufListSize;
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, &bufListSize, NULL, 0,
            kCFAllocatorSystemDefault, kCFAllocatorSystemDefault,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            &blockBuf);

        if (!bufListSize || !blockBuf) return;

        size_t allocSize = offsetof(AudioBufferList, mBuffers) +
            sizeof(AudioBuffer) * (size_t)(c->audio_channels > 0 ? c->audio_channels : 2);
        bufList = (AudioBufferList *)alloca(bufListSize > 0 ? bufListSize : allocSize);
        memset(bufList, 0, bufListSize);
        bufList->mNumberBuffers = (UInt32)c->audio_channels;

        OSStatus err = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, NULL, bufList, bufListSize,
            kCFAllocatorSystemDefault, kCFAllocatorSystemDefault,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            &blockBuf);
        if (err != noErr) {
            if (blockBuf) CFRelease(blockBuf);
            log_error("CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", err);
            return;
        }

        size_t nSamples = 0;
        for (UInt32 i = 0; i < bufList->mNumberBuffers; i++) {
            AudioBuffer *ab = &bufList->mBuffers[i];
            if (ab->mData && ab->mDataByteSize > 0) {
                size_t s = ab->mDataByteSize / sizeof(float);
                if (nSamples == 0 || s < nSamples) nSamples = s;
            }
        }

        if (nSamples == 0) { CFRelease(blockBuf); return; }

        pthread_mutex_lock(&c->lock);
        if (c->stopped) {
            pthread_mutex_unlock(&c->lock);
            CFRelease(blockBuf);
            return;
        }

        int new_total = c->audio_nb_samples + (int)nSamples;
        if (new_total * (int)sizeof(float) > c->audio_plane_cap) {
            int new_cap = c->audio_plane_cap * 2;
            while (new_total * (int)sizeof(float) > new_cap) new_cap *= 2;
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

    pthread_mutex_init(&c->lock, NULL);
    c->video_sem = dispatch_semaphore_create(0);
    if (!c->video_sem) { free(c); return NULL; }

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

        /* ── pick first display ── */
        SCDisplay *display = [content.displays firstObject];
        if (!display) {
            fprintf(stderr, "sck_capture: no display found\n");
            break;
        }

        CGRect db = display.frame;
        c->width  = (int)CGRectGetWidth(db);
        c->height = (int)CGRectGetHeight(db);
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
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.capturesAudio = YES;
        config.excludesCurrentProcessAudio = YES;
        config.queueDepth = 4;
        config.showsCursor = YES;

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

        dispatch_queue_t sq = dispatch_queue_create(
            "screencast.sck.samples", DISPATCH_QUEUE_SERIAL);
        if (!sq) {
            fprintf(stderr, "sck_capture: could not create sample queue\n");
            break;
        }

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
                  sampleHandlerQueue:sq error:&addErr]) {
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

        /* ── wait for first video frame (5 s timeout) ── */
        intptr_t sig = dispatch_semaphore_wait(c->video_sem,
                                dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC));
        if (sig != 0) {
            fprintf(stderr, "sck_capture: timed out waiting for first frame\n");
            break;
        }
        dispatch_semaphore_signal(c->video_sem); /* keep frame in queue */

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

AVFrame *sck_capture_grab_video(SckCapture *c)
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

    AVFrame *frame = c->video_queue[c->video_head];
    c->video_queue[c->video_head] = NULL;
    c->video_head = (c->video_head + 1) % VIDEO_QUEUE_DEPTH;
    c->video_count--;
    pthread_mutex_unlock(&c->lock);
    return frame;
}

AVFrame *sck_capture_grab_audio(SckCapture *c)
{
    if (!c) return NULL;
    pthread_mutex_lock(&c->lock);

    if (c->stopped || c->audio_nb_samples <= 0 || c->audio_channels <= 0) {
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }

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

    for (int i = 0; i < VIDEO_QUEUE_DEPTH; i++)
        av_frame_free(&c->video_queue[i]);

    for (int i = 0; i < 2; i++)
        free(c->audio_planes[i]);

    pthread_mutex_destroy(&c->lock);
    free(c);
}
