// avf_camera.m — AVFoundation webcam capture (Objective-C)
#import "avf_camera.h"

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Accelerate/Accelerate.h>
#import <libavutil/pixdesc.h>

#pragma mark - Private Objective-C delegate

/*
 * Thin Objective-C class that receives AVCaptureVideoDataOutput frames and
 * forwards them to the C callback, with a semaphore for the first frame so
 * avf_camera_open can return synchronously with negotiated info.
 */
@interface AvfCameraHelper : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) struct AvfCamera *cam; // weak ref (owned by caller)
@end

#pragma mark - C struct (opaque)

struct AvfCamera {
    /* Bridged Objective-C objects — stored as retained void* so the C struct
     * works cleanly under ARC without needing __strong in a struct. */
    void *session;          // AVCaptureSession (retained)
    void *input;            // AVCaptureDeviceInput (retained)
    void *output;           // AVCaptureVideoDataOutput (retained)
    void *helper;           // AvfCameraHelper (retained)
    dispatch_queue_t sessionQueue; // serial queue for session operations
    dispatch_queue_t captureQueue; // queue for frame delivery

    /* Signalled from the capture callback after the first frame arrives. */
    dispatch_semaphore_t firstFrameSem;

    /* Negotiated geometry, written once on the first frame. */
    int                width, height;
    enum AVPixelFormat av_fmt;

    /* Flags set under the capture queue. */
    volatile int       firstFrameReceived;
    volatile int       captureError;

    /* Session timeline — see avf_camera_start_session(). */
    volatile int64_t   t0_us;
    volatile int       session_started;

    /* User-facing callback. */
    AvfCameraFrameFn   on_frame;
    void              *user;
};

#pragma mark - Delegate implementation

@implementation AvfCameraHelper

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection
{
    (void)output;
    (void)connection;

    struct AvfCamera *cam = self.cam;
    if (!cam) return;

    /* Grab the pixel buffer.  It is handed on retained and untouched: the
       compositor samples it as a Metal texture straight from its IOSurface,
       so there is nothing to lock, convert or copy here. */
    CVPixelBufferRef cvbuf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!cvbuf) return;

    OSType fmt = CVPixelBufferGetPixelFormatType(cvbuf);
    if (fmt != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
        fmt != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            fprintf(stderr, "avf_camera: expected NV12, got '%.4s' "
                    "— frames will be dropped\n", (const char *)&fmt);
        });
        return;
    }

    /* Populate negotiated info from the first frame. */
    if (!cam->firstFrameReceived) {
        cam->width  = (int)CVPixelBufferGetWidth(cvbuf);
        cam->height = (int)CVPixelBufferGetHeight(cvbuf);
        cam->av_fmt = AV_PIX_FMT_NV12;
    }

    /* Signal first-frame semaphore. */
    if (!cam->firstFrameReceived) {
        cam->firstFrameReceived = 1;
        dispatch_semaphore_signal(cam->firstFrameSem);
    }

    /* Deliver the frame (ownership passes to the callback). */
    CMTime cmpts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    int64_t pts = CMTIME_IS_VALID(cmpts)
        ? CMTimeConvertScale(cmpts, 1000000,
                             kCMTimeRoundingMethod_Default).value
        : 0;

    if (!cam->session_started || pts < cam->t0_us) return;

    if (cam->on_frame)
        cam->on_frame(cam->user, (void *)CFRetain(cvbuf), pts - cam->t0_us);
}

@end

#pragma mark - Camera device selection

void avf_camera_start_session(AvfCamera *cam, int64_t t0_us)
{
    if (!cam) return;
    cam->t0_us           = t0_us;
    cam->session_started = 1;
}

void avf_camera_release_frame(void *pixbuf)
{
    if (pixbuf) CVPixelBufferRelease((CVPixelBufferRef)pixbuf);
}

/* Find an AVCaptureDevice matching `target`.
   - NULL / "" / "auto"  → default video camera
   - Otherwise tries uniqueID first, then case-insensitive name substring
   Returns nil if no match. */
static AVCaptureDevice *find_device(const char *target)
{
    if (!target || target[0] == '\0' || strcmp(target, "auto") == 0) {
        AVCaptureDevice *def = [AVCaptureDevice
                                 defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (!def)
            def = [AVCaptureDevice
                    defaultDeviceWithMediaType:AVMediaTypeMuxed]; // external
        return def;
    }

    NSString *str = [NSString stringWithUTF8String:target];
    if (!str) return nil;

    /* Try unique ID first. */
    AVCaptureDevice *dev =
        [AVCaptureDevice deviceWithUniqueID:str];
    if (dev) return dev;

    /* Fallback: search by localized name (substring, case-insensitive). */
    for (AVCaptureDevice *d in [AVCaptureDevice
                                  devicesWithMediaType:AVMediaTypeVideo]) {
        if ([d.localizedName rangeOfString:str
                                   options:NSCaseInsensitiveSearch]
              .location != NSNotFound)
            return d;
    }
    for (AVCaptureDevice *d in [AVCaptureDevice
                                  devicesWithMediaType:AVMediaTypeMuxed]) {
        if ([d.localizedName rangeOfString:str
                                   options:NSCaseInsensitiveSearch]
              .location != NSNotFound)
            return d;
    }
    return nil;
}

/* Is this format delivered as raw pixels rather than compressed frames?
   MJPEG is how UVC cameras expose their highest resolutions, but every frame
   then costs a decode — for a 30-minute recording that is a decode session
   running the entire time, mostly to produce pixels that get scaled down into
   a 480px overlay. */
static BOOL format_is_raw(AVCaptureDeviceFormat *fmt)
{
    FourCharCode sub =
        CMFormatDescriptionGetMediaSubType(fmt.formatDescription);
    return sub == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
           sub == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange  ||
           sub == kCVPixelFormatType_422YpCbCr8                   ||
           sub == kCVPixelFormatType_422YpCbCr8_yuvs              ||
           sub == kCVPixelFormatType_32BGRA;
}

/* Try to match a device format for the given dimensions and frame rate.
   Returns the closest match by area (not just exact), or nil if no formats
   at all.  When want_w/want_h are 0 the device default is left alone. */
static AVCaptureDeviceFormat *match_format(AVCaptureDevice *dev,
                                            int want_w, int want_h,
                                            int want_fps)
{
    if (want_w <= 0 || want_h <= 0) return nil;

    int64_t want_area = (int64_t)want_w * want_h;
    AVCaptureDeviceFormat *best = nil;
    int64_t best_diff = INT64_MAX;

    for (AVCaptureDeviceFormat *fmt in dev.formats) {
        CMVideoDimensions dims =
            CMVideoFormatDescriptionGetDimensions(
                fmt.formatDescription);
        if (dims.width < 320 || dims.height < 240) continue;
        if (want_fps > 0) {
            BOOL supported = NO;
            for (AVFrameRateRange *r in
                 fmt.videoSupportedFrameRateRanges) {
                if (want_fps >= (int)r.minFrameRate &&
                    want_fps <= (int)r.maxFrameRate) {
                    supported = YES;
                    break;
                }
            }
            if (!supported) continue;
        }

        /* Prefer exact match; otherwise closest area (smaller is better
           for screencast overlays — we scale down anyway). */
        if ((int)dims.width == want_w && (int)dims.height == want_h) {
            if (format_is_raw(fmt)) {
                best = fmt;
                break; /* exact match in a raw format wins immediately */
            }
            /* Right size but compressed — hold it as a candidate and keep
               looking for the same size in a raw format. */
            if (!best || !format_is_raw(best)) { best = fmt; best_diff = 0; }
            continue;
        }
        int64_t area = (int64_t)dims.width * dims.height;
        int64_t diff = llabs(area - want_area);
        /* Slight bias toward smaller-than-requested: add 5% penalty when
           the format is larger, so we prefer to scale down rather than up. */
        if (area > want_area) diff = diff * 105 / 100;
        /* Compressed formats cost a hardware decode on every frame for the
           whole recording; make one worth a substantial size advantage before
           it beats a raw format. */
        if (!format_is_raw(fmt)) diff = diff * 2 + want_area / 2;
        if (diff < best_diff) {
            best_diff = diff;
            best = fmt;
        }
    }
    return best;
}

/* Lock the device for configuration, set the format and/or frame rate. */
static int configure_device(AVCaptureDevice *dev,
                             int want_w, int want_h, int want_fps)
{
    NSError *err = nil;
    if (![dev lockForConfiguration:&err]) {
        fprintf(stderr, "avf_camera: lockForConfiguration failed: %s\n",
                err.localizedDescription.UTF8String);
        return -1;
    }

    if (want_w > 0 && want_h > 0) {
        AVCaptureDeviceFormat *match = match_format(dev, want_w, want_h, want_fps);
        if (match) {
            CMVideoDimensions dims =
                CMVideoFormatDescriptionGetDimensions(match.formatDescription);
            fprintf(stderr, "avf_camera: matched format %dx%d (requested %dx%d)\n",
                    dims.width, dims.height, want_w, want_h);
            dev.activeFormat = match;
        } else {
            fprintf(stderr, "avf_camera: no matching format for %dx%d, "
                    "keeping device default\n", want_w, want_h);
        }
    }

    if (want_fps > 0) {
        /* Find the frame-rate range that contains want_fps and pin to its
           exact CMTime values — AVFoundation compares rational times exactly
           and will throw if we supply a slightly different 1/fps value. */
        AVCaptureDeviceFormat *active = dev.activeFormat;
        for (AVFrameRateRange *r in active.videoSupportedFrameRateRanges) {
            if (want_fps >= (int)r.minFrameRate &&
                want_fps <= (int)r.maxFrameRate) {
                dev.activeVideoMinFrameDuration = r.minFrameDuration;
                dev.activeVideoMaxFrameDuration = r.minFrameDuration;
                break;
            }
        }
    }

    [dev unlockForConfiguration];
    return 0;
}

#pragma mark - Public C API

AvfCamera *avf_camera_open(const char *target, int want_w, int want_h,
                           int want_fps, AvfCameraInfo *info,
                           AvfCameraFrameFn on_frame, void *user)
{
    /* --- authorization --- */
    AVAuthorizationStatus auth =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (auth == AVAuthorizationStatusDenied ||
        auth == AVAuthorizationStatusRestricted) {
        fprintf(stderr, "avf_camera: camera access denied/restricted\n");
        return NULL;
    }
    if (auth == AVAuthorizationStatusNotDetermined) {
        /* Synchronously request.  This blocks briefly and shows the
           system permission dialog. */
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block BOOL granted = NO;
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL g) {
            granted = g;
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        dispatch_release(sem);
        if (!granted) {
            fprintf(stderr, "avf_camera: camera access denied by user\n");
            return NULL;
        }
    }

    /* --- device --- */
    AVCaptureDevice *dev = find_device(target);
    if (!dev) {
        fprintf(stderr, "avf_camera: no camera found\n");
        return NULL;
    }

    /* --- struct --- */
    struct AvfCamera *cam = calloc(1, sizeof(*cam));
    if (!cam) return NULL;
    cam->on_frame = on_frame;
    cam->user     = user;

    cam->firstFrameSem = dispatch_semaphore_create(0);

    /* Serial queue for session operations. */
    cam->sessionQueue = dispatch_queue_create(
        "com.screencast.avf-camera.session",
        DISPATCH_QUEUE_SERIAL);

    /* Serial queue for frame delivery (AVCapture requires this). */
    cam->captureQueue = dispatch_queue_create(
        "com.screencast.avf-camera.capture",
        DISPATCH_QUEUE_SERIAL);

    /* --- session --- */
    AVCaptureSession *session = [[AVCaptureSession alloc] init];
    cam->session = (__bridge_retained void *)session;

    /* --- configure device format --- */
    configure_device(dev, want_w, want_h, want_fps);

    /* --- input --- */
    NSError *err = nil;
    AVCaptureDeviceInput *input =
        [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
    if (!input) {
        fprintf(stderr, "avf_camera: input error: %s\n",
                err.localizedDescription.UTF8String);
        avf_camera_close(cam);
        return NULL;
    }
    cam->input = (__bridge_retained void *)input;

    if (![session canAddInput:input]) {
        fprintf(stderr, "avf_camera: cannot add input\n");
        avf_camera_close(cam);
        return NULL;
    }
    [session addInput:input];

    /* --- output --- */
    AVCaptureVideoDataOutput *output =
        [[AVCaptureVideoDataOutput alloc] init];
    cam->output = (__bridge_retained void *)output;

    /* Request NV12, which is what UVC cameras deliver natively and what the
       compositor's shader samples.  Asking for BGRA instead would make
       AVFoundation convert every frame for no benefit, since the pixels are
       going to the GPU either way.  Buffers must be Metal-compatible so they
       can be bound as textures without a copy. */
    output.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    output.alwaysDiscardsLateVideoFrames = YES;

    /* --- delegate --- */
    AvfCameraHelper *helper = [[AvfCameraHelper alloc] init];
    helper.cam = cam;
    cam->helper = (__bridge_retained void *)helper;

    [output setSampleBufferDelegate:helper queue:cam->captureQueue];

    if (![session canAddOutput:output]) {
        fprintf(stderr, "avf_camera: cannot add output\n");
        avf_camera_close(cam);
        return NULL;
    }
    [session addOutput:output];

    /* --- start --- */
    dispatch_sync(cam->sessionQueue, ^{
        [session startRunning];
    });

    /* --- wait for first frame (4-second timeout) --- */
    int64_t timeout_ns = 4LL * NSEC_PER_SEC;
    long sig = dispatch_semaphore_wait(
        cam->firstFrameSem,
        dispatch_time(DISPATCH_TIME_NOW, timeout_ns));

    if (sig != 0 || !cam->firstFrameReceived) {
        fprintf(stderr, "avf_camera: no frames within 4s timeout\n");
        avf_camera_close(cam);
        return NULL;
    }

    /* --- populate info --- */
    info->width  = cam->width;
    info->height = cam->height;
    info->pix_fmt = cam->av_fmt;

    fprintf(stderr, "avf_camera: webcam using %dx%d %s\n",
            cam->width, cam->height,
            av_get_pix_fmt_name(cam->av_fmt));

    return cam;
}

void avf_camera_close(AvfCamera *cam)
{
    if (!cam) return;

    /* Stop the session on its queue, then drain the capture queue so no
       more callbacks fire. */
    void *session = cam->session;
    if (session) {
        AVCaptureSession *s = (__bridge AVCaptureSession *)session;
        dispatch_sync(cam->sessionQueue, ^{
            [s stopRunning];
        });
    }

    /* Barrier on the capture queue ensures pending callbacks complete. */
    if (cam->captureQueue)
        dispatch_sync(cam->captureQueue, ^{});

    /* Release bridged Objective-C objects. */
    if (cam->helper) {
        AvfCameraHelper *h = (__bridge_transfer AvfCameraHelper *)cam->helper;
        h.cam = nil; // break the weak back-reference
        cam->helper = NULL;
    }
    if (cam->output)
        (__bridge_transfer AVCaptureVideoDataOutput *)cam->output;
    if (cam->input)
        (__bridge_transfer AVCaptureDeviceInput *)cam->input;
    if (cam->session)
        (__bridge_transfer AVCaptureSession *)cam->session;

    if (cam->sessionQueue)
        dispatch_release(cam->sessionQueue);
    if (cam->captureQueue)
        dispatch_release(cam->captureQueue);
    if (cam->firstFrameSem)
        dispatch_release(cam->firstFrameSem);

    free(cam);
}
