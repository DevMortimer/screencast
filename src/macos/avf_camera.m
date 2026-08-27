// avf_camera.m — AVFoundation webcam capture (Objective-C)
#import "avf_camera.h"
#import "overlay_geometry.h"

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Accelerate/Accelerate.h>
#import <libavutil/pixdesc.h>
#include <stdatomic.h>

#pragma mark - Borderless presenter window

static NSString *const OverlayAnchorKey = @"CameraOverlayAnchor";
static NSString *const OverlayWidthKey  = @"CameraOverlayWidth";
static const CGFloat kOverlayResizeHandle = 16.0;
static const CGFloat kOverlayMinWidth = 180.0;

typedef NS_ENUM(NSInteger, CameraOverlayAnchor) {
    CameraOverlayTopLeft = 0,
    CameraOverlayTopRight,
    CameraOverlayBottomLeft,
    CameraOverlayBottomRight,
};

@interface CameraOverlayPanel : NSPanel
@end

@implementation CameraOverlayPanel
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface CameraOverlayView : NSView {
    NSPoint _gestureStartPoint;
    NSRect _gestureStartFrame;
    OverlayResizeEdges _resizeEdges;
    BOOL _resizing;
    BOOL _moving;
}
@end

static OverlayResizeEdges overlay_resize_edges(NSPoint point, NSRect bounds)
{
    OverlayResizeEdges edges = 0;
    if (point.x <= kOverlayResizeHandle) edges |= OVERLAY_RESIZE_LEFT;
    if (point.x >= NSMaxX(bounds) - kOverlayResizeHandle)
        edges |= OVERLAY_RESIZE_RIGHT;
    if (point.y <= kOverlayResizeHandle) edges |= OVERLAY_RESIZE_BOTTOM;
    if (point.y >= NSMaxY(bounds) - kOverlayResizeHandle)
        edges |= OVERLAY_RESIZE_TOP;
    return edges;
}

@implementation CameraOverlayView
/* Borderless-window dragging and resizing are explicit.  Returning NO keeps
   AppKit from consuming the invisible resize hit regions before this view sees
   them. */
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }

- (void)mouseDown:(NSEvent *)event
{
    _resizeEdges = overlay_resize_edges([self convertPoint:event.locationInWindow
                                                    fromView:nil], self.bounds);
    _resizing = (_resizeEdges != 0);
    _moving = !_resizing;
    _gestureStartPoint = event.locationInWindow;
    _gestureStartFrame = self.window.frame;
}

- (void)mouseDragged:(NSEvent *)event
{
    NSPoint point = event.locationInWindow;
    CGFloat dx = point.x - _gestureStartPoint.x;
    CGFloat dy = point.y - _gestureStartPoint.y;

    if (_moving) {
        NSRect frame = _gestureStartFrame;
        frame.origin.x += dx;
        frame.origin.y += dy;
        [self.window setFrameOrigin:frame.origin];
        return;
    }
    if (!_resizing) return;
    /* Presenter geometry is intentionally square.  Keep the aspect fixed even
       if AppKit reports a fractional frame during a live resize. */
    const CGFloat aspect = 1.0;

    /* Width is calculated in a pure helper so Cocoa's upward Y-axis signs stay
       covered independently of camera and window permissions. */
    CGFloat width = overlay_resize_width(NSWidth(_gestureStartFrame), aspect,
                                          _resizeEdges, dx, dy);

    CGFloat maxWidth = MIN(720.0, NSWidth(self.window.screen.visibleFrame) * 0.55);
    maxWidth = MIN(maxWidth, NSHeight(self.window.screen.visibleFrame) * 0.55);
    width = MAX(kOverlayMinWidth, MIN(width, maxWidth));
    NSSize size = NSMakeSize(width, width);
    NSRect frame = _gestureStartFrame;
    if (_resizeEdges & OVERLAY_RESIZE_LEFT)
        frame.origin.x = NSMaxX(_gestureStartFrame) - size.width;
    if (_resizeEdges & OVERLAY_RESIZE_BOTTOM)
        frame.origin.y = NSMaxY(_gestureStartFrame) - size.height;
    frame.size = size;
    [self.window setFrame:frame display:YES];
}

- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    if (_resizing)
        [[NSUserDefaults standardUserDefaults]
            setDouble:NSWidth(self.window.frame) forKey:OverlayWidthKey];

    /* Both a body drag and a resize finish at the saved/nearest corner. */
    if ([self.window.delegate respondsToSelector:@selector(overlayDidFinishGesture)])
        [self.window.delegate performSelector:@selector(overlayDidFinishGesture)];
    _resizing = NO;
    _moving = NO;
    _resizeEdges = 0;
}
@end

@interface CameraOverlayController : NSObject <NSWindowDelegate> {
    NSPanel *_window;
    NSScreen *_targetScreen;
    AVCaptureVideoPreviewLayer *_previewLayer;
    CameraOverlayAnchor _anchor;
    BOOL _placing;
}
- (instancetype)initWithSession:(AVCaptureSession *)session
                          width:(int)cameraWidth
                         height:(int)cameraHeight
                      displayID:(unsigned int)displayID;
- (void)placeAtAnchor;
- (void)overlayDidFinishGesture;
- (void)updateSurfaceGeometry;
- (void)close;
@end

static NSScreen *screen_for_display_id(unsigned int displayID)
{
    for (NSScreen *screen in [NSScreen screens]) {
        NSNumber *number = screen.deviceDescription[@"NSScreenNumber"];
        if (number && number.unsignedIntValue == displayID)
            return screen;
    }
    return [NSScreen mainScreen] ?: [[NSScreen screens] firstObject];
}

@implementation CameraOverlayController

static const CGFloat kOverlayMargin = 18.0;

- (NSRect)anchoredFrameForSize:(NSSize)size screen:(NSScreen *)screen
{
    NSRect visible = screen.visibleFrame;
    CGFloat x = (_anchor == CameraOverlayTopLeft ||
                 _anchor == CameraOverlayBottomLeft)
        ? NSMinX(visible) + kOverlayMargin
        : NSMaxX(visible) - size.width - kOverlayMargin;
    CGFloat y = (_anchor == CameraOverlayTopLeft ||
                 _anchor == CameraOverlayTopRight)
        ? NSMaxY(visible) - size.height - kOverlayMargin
        : NSMinY(visible) + kOverlayMargin;
    return NSMakeRect(x, y, size.width, size.height);
}

- (void)placeAtAnchor
{
    if (!_window || _placing) return;
    if (!_targetScreen) return;

    _placing = YES;
    [_window setFrame:[self anchoredFrameForSize:_window.frame.size
                                         screen:_targetScreen]
              display:YES];
    _placing = NO;
}

- (instancetype)initWithSession:(AVCaptureSession *)session
                          width:(int)cameraWidth
                         height:(int)cameraHeight
                      displayID:(unsigned int)displayID
{
    self = [super init];
    if (!self) return nil;
    if (!session || cameraWidth <= 0 || cameraHeight <= 0) {
        [self release];
        return nil;
    }

    NSScreen *screen = screen_for_display_id(displayID);
    if (!screen) {
        [self release];
        return nil;
    }
    _targetScreen = [screen retain];

    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    NSNumber *savedAnchorValue = [defaults objectForKey:OverlayAnchorKey];
    NSInteger savedAnchor = savedAnchorValue.integerValue;
    _anchor = (savedAnchorValue && savedAnchor >= CameraOverlayTopLeft &&
               savedAnchor <= CameraOverlayBottomRight)
        ? (CameraOverlayAnchor)savedAnchor : CameraOverlayBottomRight;

    /* The camera source is 16:9, but the presenter surface is a square.  The
       preview layer's aspect-fill gravity supplies the centered left/right
       crop instead of stretching the camera image. */
    CGFloat width = [defaults doubleForKey:OverlayWidthKey];
    if (width < kOverlayMinWidth) width = 360.0;

    NSRect visible = screen.visibleFrame;
    CGFloat maxWidth = MIN(720.0, NSWidth(visible) * 0.55);
    maxWidth = MIN(maxWidth, NSHeight(visible) * 0.55);
    width = MIN(width, maxWidth);
    NSSize size = NSMakeSize(width, width);

    NSWindowStyleMask style = NSWindowStyleMaskBorderless |
                              NSWindowStyleMaskResizable |
                              NSWindowStyleMaskNonactivatingPanel;
    NSRect frame = [self anchoredFrameForSize:size screen:screen];
    _window = [[CameraOverlayPanel alloc] initWithContentRect:frame
                                         styleMask:style
                                           backing:NSBackingStoreBuffered
                                             defer:NO
                                            screen:screen];
    if (!_window) {
        [self release];
        return nil;
    }

    _window.delegate = self;
    _window.releasedWhenClosed = NO;
    _window.becomesKeyOnlyIfNeeded = YES;
    _window.hidesOnDeactivate = NO;
    /* Body dragging is explicit in CameraOverlayView so edge hit-testing can
       reliably reach the custom resize path. */
    _window.movableByWindowBackground = NO;
    _window.acceptsMouseMovedEvents = YES;
    /* Keep the borderless window itself transparent.  The content view owns
       the black fill and rounded clipping, so transparent window corners do
       not expose an opaque square behind the camera surface. */
    _window.opaque = NO;
    _window.backgroundColor = [NSColor clearColor];
    _window.hasShadow = NO;
    _window.level = NSFloatingWindowLevel;
    _window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                 NSWindowCollectionBehaviorFullScreenAuxiliary;
    _window.contentAspectRatio = NSMakeSize(1.0, 1.0);
    _window.contentMinSize = NSMakeSize(kOverlayMinWidth, kOverlayMinWidth);
    _window.contentMaxSize = NSMakeSize(maxWidth, maxWidth);

    CameraOverlayView *view = [[CameraOverlayView alloc]
        initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    view.wantsLayer = YES;
    view.layer.backgroundColor = [NSColor blackColor].CGColor;
    _window.contentView = view;

    _previewLayer = [[AVCaptureVideoPreviewLayer alloc] initWithSession:session];
    _previewLayer.videoGravity = AVLayerVideoGravityResizeAspectFill;
    [view.layer addSublayer:_previewLayer];
    [self updateSurfaceGeometry];
    [view release];

    [_window orderFrontRegardless];
    return self;
}

- (void)windowDidResize:(NSNotification *)notification
{
    (void)notification;
    [self updateSurfaceGeometry];
    if (_window.inLiveResize) [self placeAtAnchor];
}

- (void)updateSurfaceGeometry
{
    if (!_window || !_previewLayer) return;
    NSView *view = _window.contentView;
    CGFloat radius = MIN(NSWidth(view.bounds), NSHeight(view.bounds)) * 0.16;
    view.layer.cornerRadius = radius;
    view.layer.masksToBounds = YES;
    _previewLayer.frame = view.bounds;
    _previewLayer.cornerRadius = radius;
    _previewLayer.masksToBounds = YES;
}

- (void)windowDidEndLiveResize:(NSNotification *)notification
{
    (void)notification;
    [[NSUserDefaults standardUserDefaults]
        setDouble:NSWidth(_window.frame) forKey:OverlayWidthKey];
    [self placeAtAnchor];
}

- (void)overlayDidFinishGesture
{
    if (!_targetScreen) return;

    NSRect frame = _window.frame;
    NSRect visible = _targetScreen.visibleFrame;
    BOOL left = NSMidX(frame) < NSMidX(visible);
    BOOL bottom = NSMidY(frame) < NSMidY(visible);
    _anchor = left
        ? (bottom ? CameraOverlayBottomLeft : CameraOverlayTopLeft)
        : (bottom ? CameraOverlayBottomRight : CameraOverlayTopRight);
    [[NSUserDefaults standardUserDefaults]
        setInteger:_anchor forKey:OverlayAnchorKey];
    [self placeAtAnchor];
}

- (void)windowDidEndLiveMove:(NSNotification *)notification
{
    (void)notification;
    [self overlayDidFinishGesture];
}

- (void)close
{
    if (_window) {
        _window.delegate = nil;
        [_previewLayer removeFromSuperlayer];
        [_window orderOut:nil];
        [_window close];
        [_previewLayer release];
        _previewLayer = nil;
        [_window release];
        _window = nil;
    }
    [_targetScreen release];
    _targetScreen = nil;
}

- (void)dealloc
{
    [self close];
    [super dealloc];
}

@end

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
    void *overlay;          // CameraOverlayController (retained; main thread)
    dispatch_queue_t sessionQueue; // serial queue for session operations
    dispatch_queue_t captureQueue; // queue for frame delivery

    /* Signalled from the capture callback after the first frame arrives. */
    dispatch_semaphore_t firstFrameSem;

    /* Negotiated geometry, written once on the first frame. */
    int                width, height;
    enum AVPixelFormat av_fmt;

    /* Flags set under the capture queue. */
    atomic_int         firstFrameReceived;
    atomic_int         captureError;

    /* Session timeline — see avf_camera_start_session(). */
    atomic_llong       t0_us;
    atomic_int         session_started;

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

    /* Grab the pixel buffer.  It is handed to the optional C callback retained
       and untouched.  The presenter window uses the capture session directly,
       so displaying it does not copy or convert this buffer. */
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
    if (!atomic_load(&cam->firstFrameReceived)) {
        cam->width  = (int)CVPixelBufferGetWidth(cvbuf);
        cam->height = (int)CVPixelBufferGetHeight(cvbuf);
        cam->av_fmt = AV_PIX_FMT_NV12;
        atomic_store(&cam->firstFrameReceived, 1);
        dispatch_semaphore_signal(cam->firstFrameSem);
    }

    /* Deliver the frame (ownership passes to the callback). */
    CMTime cmpts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    int64_t pts = CMTIME_IS_VALID(cmpts)
        ? CMTimeConvertScale(cmpts, 1000000,
                             kCMTimeRoundingMethod_Default).value
        : 0;

    if (!atomic_load(&cam->session_started)) return;
    int64_t t0_us = atomic_load(&cam->t0_us);
    if (pts < t0_us) return;

    if (cam->on_frame)
        cam->on_frame(cam->user, (void *)CFRetain(cvbuf), pts - t0_us);
}

@end

#pragma mark - Camera device selection

void avf_camera_start_session(AvfCamera *cam, int64_t t0_us)
{
    if (!cam) return;
    atomic_store(&cam->t0_us, t0_us);
    atomic_store(&cam->session_started, 1);
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
    cam->session = session; /* owned by alloc; released in close */

    /* The preview is part of the recording, so it needs enough source detail
       to stay clean when the user makes the window large.  PreviewLayer keeps
       the path on the GPU; 720p does not add a CPU conversion. */
    if ([session canSetSessionPreset:AVCaptureSessionPreset1280x720])
        session.sessionPreset = AVCaptureSessionPreset1280x720;
    else if ([session canSetSessionPreset:AVCaptureSessionPresetHigh])
        session.sessionPreset = AVCaptureSessionPresetHigh;

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
    cam->input = [input retain];

    if (![session canAddInput:input]) {
        fprintf(stderr, "avf_camera: cannot add input\n");
        avf_camera_close(cam);
        return NULL;
    }
    [session addInput:input];

    /* --- output --- */
    AVCaptureVideoDataOutput *output =
        [[AVCaptureVideoDataOutput alloc] init];
    cam->output = output; /* owned by alloc; released in close */

    /* NV12 at the same 720p geometry as the preview.  The callback releases
       each buffer immediately in presenter mode; AVCaptureVideoPreviewLayer
       renders from the session without routing through that callback. */
    output.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferWidthKey:  @(1280),
        (id)kCVPixelBufferHeightKey: @(720),
    };
    output.alwaysDiscardsLateVideoFrames = YES;

    /* --- delegate --- */
    AvfCameraHelper *helper = [[AvfCameraHelper alloc] init];
    helper.cam = cam;
    cam->helper = helper; /* owned by alloc; released in close */

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

    if (sig != 0 || !atomic_load(&cam->firstFrameReceived)) {
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

int avf_camera_show_overlay(AvfCamera *cam, unsigned int display_id)
{
    if (!cam || !cam->session) return -1;
    if (![NSThread isMainThread]) {
        fprintf(stderr, "avf_camera: overlay must be created on the main thread\n");
        return -1;
    }
    if (cam->overlay) return 0;

    AVCaptureSession *session = (AVCaptureSession *)cam->session;
    CameraOverlayController *overlay = [[CameraOverlayController alloc]
        initWithSession:session
                  width:cam->width
                 height:cam->height
              displayID:display_id];
    if (!overlay) {
        fprintf(stderr, "avf_camera: could not create presenter window\n");
        return -1;
    }
    cam->overlay = overlay;
    return 0;
}

void avf_camera_close(AvfCamera *cam)
{
    if (!cam) return;

    /* AppKit objects must leave on the main thread, before their capture
       session stops.  The recorder calls close from its main thread. */
    if (cam->overlay) {
        CameraOverlayController *overlay = (CameraOverlayController *)cam->overlay;
        if ([NSThread isMainThread]) {
            [overlay close];
            [overlay release];
        } else {
            dispatch_sync(dispatch_get_main_queue(), ^{
                [overlay close];
                [overlay release];
            });
        }
        cam->overlay = NULL;
    }

    /* Stop the session on its queue, then drain the capture queue so no
       more callbacks fire. */
    void *session = cam->session;
    if (session) {
        AVCaptureSession *s = (AVCaptureSession *)session;
        dispatch_sync(cam->sessionQueue, ^{
            [s stopRunning];
        });
    }

    /* Barrier on the capture queue ensures pending callbacks complete. */
    if (cam->captureQueue)
        dispatch_sync(cam->captureQueue, ^{});

    /* Release bridged Objective-C objects. */
    if (cam->helper) {
        AvfCameraHelper *h = (AvfCameraHelper *)cam->helper;
        h.cam = nil; // break the weak back-reference
        [h release];
        cam->helper = NULL;
    }
    if (cam->output)
        [(AVCaptureVideoDataOutput *)cam->output release];
    if (cam->input)
        [(AVCaptureDeviceInput *)cam->input release];
    if (cam->session)
        [(AVCaptureSession *)cam->session release];

    if (cam->sessionQueue)
        dispatch_release(cam->sessionQueue);
    if (cam->captureQueue)
        dispatch_release(cam->captureQueue);
    if (cam->firstFrameSem)
        dispatch_release(cam->firstFrameSem);

    free(cam);
}
