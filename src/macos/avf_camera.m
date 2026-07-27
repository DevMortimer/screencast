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

#pragma mark - Public C API

AvfCamera *avf_camera_open(const char *target, AvfCameraInfo *info,
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

    /*
     * The smallest stream the device will give us.
     *
     * Nothing here looks at these pixels.  The camera is open so that macOS
     * offers Presenter Overlay for this app, and when the overlay is on the
     * system takes the camera and renders the presenter from it directly, at
     * whatever quality it wants.  Every frame that reaches this process is
     * released on arrival, so paying an ISP to fill a 1080p buffer for the
     * length of a recording buys precisely nothing.
     *
     * The preset, not the device's activeFormat, is what decides this on
     * macOS: a session re-applies its preset on startRunning and silently
     * overwrites any format set beforehand, and the preset that would defer
     * to the device instead (InputPriority) is iOS-only.
     */
    if ([session canSetSessionPreset:AVCaptureSessionPresetLow])
        session.sessionPreset = AVCaptureSessionPresetLow;

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

    /*
     * NV12, and as small as the output will scale to.
     *
     * The session preset does not govern what this output delivers — with the
     * preset set to Low the device still handed over 1920x1080 — so the size
     * has to be stated on the output itself, which is the one place that does
     * decide it.  These pixels are discarded on arrival; the number of them is
     * pure cost.
     */
    output.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferWidthKey:  @(320),
        (id)kCVPixelBufferHeightKey: @(240),
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
