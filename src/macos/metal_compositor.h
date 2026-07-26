// metal_compositor.h — GPU compositing for the macOS capture path
#ifndef METAL_COMPOSITOR_H
#define METAL_COMPOSITOR_H

#include <stdint.h>

/*
 * Composites the screen and the webcam into one NV12 frame on the GPU.
 *
 * Every buffer in this path is a CVPixelBufferRef, passed as void * so the
 * header stays pure C.  Nothing is ever read back to the CPU: ScreenCaptureKit
 * and AVFoundation hand over IOSurface-backed buffers, the compositor renders
 * into another IOSurface-backed buffer, and VideoToolbox encodes that one
 * directly.  The pixels stay in GPU memory from capture to encode.
 *
 * `display` mode needs no compositing at all — the caller passes the capture
 * buffer straight to the encoder and never comes here.
 */

typedef struct MetalCompositor MetalCompositor;

/*
 * Build a compositor for a fixed canvas.  Overlay geometry is fixed too: the
 * output stream's dimensions cannot change mid-recording, and neither does
 * where the webcam sits.
 *
 * Returns NULL if Metal is unavailable or the shaders fail to build, in which
 * case the caller records without compositing rather than failing outright.
 */
MetalCompositor *metal_compositor_create(int canvas_w, int canvas_h,
                                          int overlay_size,
                                          int overlay_x, int overlay_y);

/*
 * Composite one frame and return a retained NV12 CVPixelBufferRef drawn from
 * an internal pool.  The caller owns the result and must release it via
 * metal_compositor_release_frame().
 *
 * mode 2 (webcam) scales the camera to fill the canvas; mode 3 (both) draws
 * the screen with the camera inset as a rounded square.  cam_pixbuf may be
 * NULL, in which case mode 3 degrades to the screen alone and mode 2 to black.
 *
 * Returns NULL if no buffer was available or the render failed.
 */
void *metal_compositor_render(MetalCompositor *mc, int mode,
                               void *screen_pixbuf, void *cam_pixbuf);

/* Release a frame returned by metal_compositor_render(). */
void metal_compositor_release_frame(void *pixbuf);

void metal_compositor_destroy(MetalCompositor *mc);

#endif /* METAL_COMPOSITOR_H */
