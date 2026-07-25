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

    /* User-facing callback. */
    AvfCameraFrameFn   on_frame;
    void              *user;
};

#pragma mark - vImage helpers

/* Build a YpCbCr->ARGB conversion matrix for video-range NV12. */
static void nv12_matrix_init(vImage_YpCbCrToARGB *matrix, bool isFullRange)
{
    vImage_YpCbCrPixelRange range;
    if (isFullRange) {
        range = (vImage_YpCbCrPixelRange){ .Yp_bias = 0, .CbCr_bias = 128,
            .YpRangeMax = 255, .CbCrRangeMax = 255,
            .YpMax = 255, .YpMin = 0, .CbCrMax = 255, .CbCrMin = 0 };
    } else {
        range = (vImage_YpCbCrPixelRange){ .Yp_bias = 16, .CbCr_bias = 128,
            .YpRangeMax = 235, .CbCrRangeMax = 240,
            .YpMax = 235, .YpMin = 16, .CbCrMax = 240, .CbCrMin = 16 };
    }
    vImageConvert_YpCbCrToARGB_GenerateConversion(
        kvImage_YpCbCrToARGBMatrix_ITU_R_601_4, &range, matrix,
        kvImage420Yp8_CbCr8, kvImageARGB8888, kvImageNoFlags);
}

/* Convert a single NV12 CVPixelBuffer into an AVFrame (BGRA).
   Returns 0 on success, non-zero on failure. */
static int nv12_cvpixelbuffer_to_avframe(CVPixelBufferRef cv,
                                           AVFrame *avf)
{
    size_t w = CVPixelBufferGetWidth(cv);
    size_t h = CVPixelBufferGetHeight(cv);

    /* NV12 planes */
    uint8_t *srcY   = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(cv, 0);
    uint8_t *srcUV  = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(cv, 1);
    size_t   strideY  = CVPixelBufferGetBytesPerRowOfPlane(cv, 0);
    size_t   strideUV = CVPixelBufferGetBytesPerRowOfPlane(cv, 1);

    if (!srcY || !srcUV) return -1;

    /* Prepare vImage source buffers.  Y is WxH, UV is Wx(H/2) for 4:2:0. */
    vImage_Buffer vy  = { srcY,  h,      w,     strideY };
    vImage_Buffer vuv = { srcUV, h / 2,  w,     strideUV };

    /* Prepare destination (BGRA).  vImageConvert_NV12toBGRA produces BGRA
     * pixel data and the matrix accounts for the channel order internally. */
    vImage_Buffer vdst = { avf->data[0], h, w, (size_t)avf->linesize[0] };

    /* Build the matrix once per call (it's small/cheap).  Detect video-
     * vs full-range from the pixel format type. */
    OSType fmt = CVPixelBufferGetPixelFormatType(cv);
    vImage_YpCbCrToARGB matrix;
    nv12_matrix_init(&matrix,
        fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);

    /* vImageConvert_420Yp8_CbCr8ToARGB8888 with BGRA permute map. */
    uint8_t bgra_map[4] = { 3, 2, 1, 0 };  /* swap R↔B for BGRA output */
    vImage_Error err = vImageConvert_420Yp8_CbCr8ToARGB8888(
        &vy, &vuv, &vdst, &matrix, bgra_map, 255, kvImageNoFlags);
    return (err == kvImageNoError) ? 0 : -1;
}

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

    /* Grab the pixel buffer. */
    CVPixelBufferRef cvbuf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!cvbuf) return;

    /* Lock the pixel buffer for reading. */
    if (CVPixelBufferLockBaseAddress(cvbuf, kCVPixelBufferLock_ReadOnly) != 0)
        return;

    size_t w = CVPixelBufferGetWidth(cvbuf);
    size_t h = CVPixelBufferGetHeight(cvbuf);
    OSType fmt = CVPixelBufferGetPixelFormatType(cvbuf);

    /* Populate negotiated info from the first frame. */
    if (!cam->firstFrameReceived) {
        cam->width  = (int)w;
        cam->height = (int)h;
        /* We always deliver BGRA to callers. */
        cam->av_fmt = AV_PIX_FMT_BGRA;
    }

    /* Allocate an AVFrame. */
    AVFrame *avf = av_frame_alloc();
    if (!avf) {
        CVPixelBufferUnlockBaseAddress(cvbuf, kCVPixelBufferLock_ReadOnly);
        return;
    }
    avf->width  = (int)w;
    avf->height = (int)h;
    avf->format = AV_PIX_FMT_BGRA;

    if (av_frame_get_buffer(avf, 32) < 0) {
        av_frame_free(&avf);
        CVPixelBufferUnlockBaseAddress(cvbuf, kCVPixelBufferLock_ReadOnly);
        return;
    }

    /* Copy / convert pixel data. */
    int copy_ok = 0;
    if (fmt == kCVPixelFormatType_32BGRA ||
        fmt == kCVPixelFormatType_32ABGR /* rare, handle anyway */) {
        /* Direct BGRA copy (single plane). */
        uint8_t *src   = (uint8_t *)CVPixelBufferGetBaseAddress(cvbuf);
        size_t   stride = CVPixelBufferGetBytesPerRow(cvbuf);
        uint8_t *dst   = avf->data[0];
        int      dst_ls = avf->linesize[0];
        int      row_bytes = (int)(w * 4); /* 4 bytes per pixel for BGRA */
        if (dst_ls == (int)stride && row_bytes == dst_ls) {
            /* Fast path: contiguous. */
            memcpy(dst, src, (size_t)(h * stride));
        } else {
            /* Slower row-by-row copy with stride adjustment. */
            for (size_t y = 0; y < h; y++)
                memcpy(dst + y * dst_ls, src + y * stride,
                       (size_t)(row_bytes < dst_ls ? row_bytes : dst_ls));
        }
        copy_ok = 1;
    } else if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
               fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        /* NV12 -> BGRA via vImage. */
        copy_ok = (nv12_cvpixelbuffer_to_avframe(cvbuf, avf) == 0);
    } else {
        /* Unknown format — log a warning and skip the frame.
         * kCVPixelFormatType_32BGRA (requested via videoSettings) and the
         * two NV12 variants (most common camera-native formats) are
         * handled above.  Anything else is unexpected. */
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            fprintf(stderr, "avf_camera: unsupported pixel format '%.4s' "
                    "— frames will be dropped\n", (const char *)&fmt);
        });
    }

    CVPixelBufferUnlockBaseAddress(cvbuf, kCVPixelBufferLock_ReadOnly);

    if (!copy_ok) {
        av_frame_free(&avf);
        return;
    }

    /* Signal first-frame semaphore. */
    if (!cam->firstFrameReceived) {
        cam->firstFrameReceived = 1;
        dispatch_semaphore_signal(cam->firstFrameSem);
    }

    /* Deliver the frame (ownership passes to the callback). */
    if (cam->on_frame)
        cam->on_frame(cam->user, avf);
    else
        av_frame_free(&avf);
}

@end

#pragma mark - Camera device selection

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

/* Try to match a device format for the given dimensions and frame rate.
   Returns the first exact match, or nil. */
static AVCaptureDeviceFormat *match_format(AVCaptureDevice *dev,
                                            int want_w, int want_h,
                                            int want_fps)
{
    if (want_w <= 0 || want_h <= 0) return nil;

    for (AVCaptureDeviceFormat *fmt in dev.formats) {
        CMVideoDimensions dims =
            CMVideoFormatDescriptionGetDimensions(
                fmt.formatDescription);
        if ((int)dims.width != want_w || (int)dims.height != want_h)
            continue;
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
        return fmt;
    }
    return nil;
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

    AVCaptureDeviceFormat *match = match_format(dev, want_w, want_h, want_fps);
    if (match) {
        dev.activeFormat = match;
        if (want_fps > 0) {
            /* Set the minimum and maximum to the same value to pin it. */
            dev.activeVideoMinFrameDuration =
                CMTimeMake(1, want_fps);
            dev.activeVideoMaxFrameDuration =
                CMTimeMake(1, want_fps);
        }
    }
    /* If no exact match, leave the default format (will be reported
       from the first received frame). */

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

    /* Request BGRA (32-bit) pixel format.  AVCaptureVideoDataOutput converts
       from the sensor's native format when possible. */
    output.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_32BGRA)
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
