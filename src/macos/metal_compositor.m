// metal_compositor.m — GPU compositing for the macOS capture path
//
// Replaces four full-frame CPU passes (capture memcpy, BGRA→RGBA swizzle,
// RGBA→YUV420P conversion, software upload into VideoToolbox) with a single
// GPU pass that reads the capture buffers as textures and writes NV12 planes
// directly.  The pixels never touch the CPU.
//
// NV12 has two planes at different resolutions, and a fragment shader writes
// one texel per target pixel, so this is two render passes over the same
// scene: luma at full resolution, chroma at half.  Both call the same
// composite() to decide the colour at a point, so they cannot disagree.
//
// The shader is compiled from source at startup rather than built into a
// .metallib, which keeps the Makefile free of a Metal toolchain step.  It is
// a few hundred microseconds once per recording.

#import "metal_compositor.h"

#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <simd/simd.h>
#import <stdio.h>
#import <stdlib.h>

/* ── shader ────────────────────────────────────────────────── */

static const char *kShaderSource =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct Params {\n"
"    float2 canvasSize;\n"
"    float2 overlayOrigin;\n"   /* top-left of the overlay, canvas pixels */
"    float  overlaySize;\n"     /* square side, canvas pixels */
"    float  cornerRadius;\n"
"    float2 camCropOrigin;\n"   /* normalised crop rect in the camera texture */
"    float2 camCropSize;\n"
"    int    mode;\n"            /* 2 = webcam fills canvas, 3 = screen + inset */
"    int    hasCam;\n"
"};\n"
"\n"
"struct VOut {\n"
"    float4 pos [[position]];\n"
"    float2 uv;\n"
"};\n"
"\n"
/* Full-screen triangle: three vertices covering the viewport, no buffer. */
"vertex VOut vmain(uint vid [[vertex_id]])\n"
"{\n"
"    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
"    VOut o;\n"
"    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
"    o.uv  = p;\n"
"    return o;\n"
"}\n"
"\n"
"constexpr sampler smp(filter::linear, address::clamp_to_edge);\n"
"\n"
/* BT.709 video range: the capture stream is configured to match and the
   encoder is told the same, so this is the only conversion in the pipeline. */
"static float3 yuv2rgb(float y, float2 uv)\n"
"{\n"
"    float yy = (y - 16.0/255.0) * (255.0/219.0);\n"
"    float u  = (uv.x - 128.0/255.0) * (255.0/224.0);\n"
"    float v  = (uv.y - 128.0/255.0) * (255.0/224.0);\n"
"    return float3(yy + 1.5748 * v,\n"
"                  yy - 0.1873 * u - 0.4681 * v,\n"
"                  yy + 1.8556 * u);\n"
"}\n"
"\n"
"static float3 rgb2yuv(float3 c)\n"
"{\n"
"    float y = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;\n"
"    float u = (c.b - y) / 1.8556;\n"
"    float v = (c.r - y) / 1.5748;\n"
"    return float3(clamp(y, 0.0, 1.0) * (219.0/255.0) + 16.0/255.0,\n"
"                  clamp(u, -0.5, 0.5) * (224.0/255.0) + 128.0/255.0,\n"
"                  clamp(v, -0.5, 0.5) * (224.0/255.0) + 128.0/255.0);\n"
"}\n"
"\n"
"static float3 sampleNV12(texture2d<float> y, texture2d<float> uv, float2 c)\n"
"{\n"
"    return yuv2rgb(y.sample(smp, c).r, uv.sample(smp, c).rg);\n"
"}\n"
"\n"
/* Signed distance to a rounded square: an analytic anti-aliased corner,
   replacing the precomputed float mask the CPU compositor needed. */
"static float roundedAlpha(float2 p, float side, float radius)\n"
"{\n"
"    float2 h = float2(side * 0.5);\n"
"    float2 q = abs(p - h) - (h - float2(radius));\n"
"    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;\n"
"    return clamp(0.5 - d, 0.0, 1.0);\n"
"}\n"
"\n"
"static float3 composite(float2 uv,\n"
"                        texture2d<float> scrY, texture2d<float> scrUV,\n"
"                        texture2d<float> camY, texture2d<float> camUV,\n"
"                        constant Params &P)\n"
"{\n"
"    if (P.mode == 2) {\n"
"        if (P.hasCam == 0) return float3(0.0);\n"
"        return sampleNV12(camY, camUV, P.camCropOrigin + uv * P.camCropSize);\n"
"    }\n"
"\n"
"    float3 base = sampleNV12(scrY, scrUV, uv);\n"
"    if (P.hasCam == 0) return base;\n"
"\n"
"    float2 rel = uv * P.canvasSize - P.overlayOrigin;\n"
"    if (rel.x < 0.0 || rel.y < 0.0 ||\n"
"        rel.x >= P.overlaySize || rel.y >= P.overlaySize)\n"
"        return base;\n"
"\n"
"    float a = roundedAlpha(rel, P.overlaySize, P.cornerRadius);\n"
"    if (a <= 0.0) return base;\n"
"\n"
"    float2 c = P.camCropOrigin + (rel / P.overlaySize) * P.camCropSize;\n"
"    return mix(base, sampleNV12(camY, camUV, c), a);\n"
"}\n"
"\n"
"fragment float4 fluma(VOut in [[stage_in]],\n"
"                      texture2d<float> scrY  [[texture(0)]],\n"
"                      texture2d<float> scrUV [[texture(1)]],\n"
"                      texture2d<float> camY  [[texture(2)]],\n"
"                      texture2d<float> camUV [[texture(3)]],\n"
"                      constant Params &P     [[buffer(0)]])\n"
"{\n"
"    float3 c = composite(in.uv, scrY, scrUV, camY, camUV, P);\n"
"    return float4(rgb2yuv(c).x, 0.0, 0.0, 1.0);\n"
"}\n"
"\n"
"fragment float4 fchroma(VOut in [[stage_in]],\n"
"                        texture2d<float> scrY  [[texture(0)]],\n"
"                        texture2d<float> scrUV [[texture(1)]],\n"
"                        texture2d<float> camY  [[texture(2)]],\n"
"                        texture2d<float> camUV [[texture(3)]],\n"
"                        constant Params &P     [[buffer(0)]])\n"
"{\n"
"    float3 c = composite(in.uv, scrY, scrUV, camY, camUV, P);\n"
"    float3 y = rgb2yuv(c);\n"
"    return float4(y.y, y.z, 0.0, 1.0);\n"
"}\n";

/* Mirrors the shader's Params. */
typedef struct {
    simd_float2 canvasSize;
    simd_float2 overlayOrigin;
    float       overlaySize;
    float       cornerRadius;
    simd_float2 camCropOrigin;
    simd_float2 camCropSize;
    int32_t     mode;
    int32_t     hasCam;
} CompositorParams;

/* ── state ─────────────────────────────────────────────────── */

struct MetalCompositor {
    void *device;         /* id<MTLDevice> */
    void *queue;          /* id<MTLCommandQueue> */
    void *pipeLuma;       /* id<MTLRenderPipelineState> */
    void *pipeChroma;     /* id<MTLRenderPipelineState> */
    CVMetalTextureCacheRef texCache;
    CVPixelBufferPoolRef   pool;

    int canvas_w, canvas_h;
    int overlay_size, overlay_x, overlay_y;
    long frames;          /* renders since creation; paces the cache flush */
};

/* ── texture helpers ───────────────────────────────────────── */

/*
 * Wrap one plane of a CVPixelBuffer as a Metal texture.  The CVMetalTexture
 * must outlive the command buffer, so the caller keeps it alive until the GPU
 * has finished; that is what the `keep` array is for at the call site.
 */
static id<MTLTexture> plane_texture(MetalCompositor *mc, CVPixelBufferRef pb,
                                     int plane, MTLPixelFormat fmt,
                                     NSMutableArray *keep)
{
    if (!pb) return nil;

    size_t w = CVPixelBufferGetWidthOfPlane(pb, plane);
    size_t h = CVPixelBufferGetHeightOfPlane(pb, plane);

    CVMetalTextureRef ref = NULL;
    CVReturn cv = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, mc->texCache, pb, NULL, fmt, w, h, plane, &ref);
    if (cv != kCVReturnSuccess || !ref) {
        if (ref) CFRelease(ref);
        return nil;
    }

    id<MTLTexture> tex = CVMetalTextureGetTexture(ref);
    [keep addObject:(__bridge id)ref];
    CFRelease(ref);
    return tex;
}

/* ── lifecycle ─────────────────────────────────────────────── */

MetalCompositor *metal_compositor_create(int canvas_w, int canvas_h,
                                          int overlay_size,
                                          int overlay_x, int overlay_y)
{
    if (canvas_w <= 0 || canvas_h <= 0) return NULL;

    MetalCompositor *mc = calloc(1, sizeof(*mc));
    if (!mc) return NULL;

    mc->canvas_w     = canvas_w;
    mc->canvas_h     = canvas_h;
    mc->overlay_size = overlay_size;
    mc->overlay_x    = overlay_x;
    mc->overlay_y    = overlay_y;

    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            fprintf(stderr, "metal: no Metal device\n");
            free(mc);
            return NULL;
        }

        NSError *err = nil;
        id<MTLLibrary> lib =
            [dev newLibraryWithSource:@(kShaderSource) options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "metal: shader compile failed: %s\n",
                    err ? err.localizedDescription.UTF8String : "unknown");
            free(mc);
            return NULL;
        }

        id<MTLFunction> vfn = [lib newFunctionWithName:@"vmain"];
        id<MTLFunction> lfn = [lib newFunctionWithName:@"fluma"];
        id<MTLFunction> cfn = [lib newFunctionWithName:@"fchroma"];
        if (!vfn || !lfn || !cfn) {
            fprintf(stderr, "metal: shader entry point missing\n");
            free(mc);
            return NULL;
        }

        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vfn;
        d.fragmentFunction = lfn;
        d.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        id<MTLRenderPipelineState> pl =
            [dev newRenderPipelineStateWithDescriptor:d error:&err];

        d.fragmentFunction = cfn;
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRG8Unorm;
        id<MTLRenderPipelineState> pc =
            [dev newRenderPipelineStateWithDescriptor:d error:&err];

        if (!pl || !pc) {
            fprintf(stderr, "metal: pipeline creation failed: %s\n",
                    err ? err.localizedDescription.UTF8String : "unknown");
            free(mc);
            return NULL;
        }

        CVMetalTextureCacheRef cache = NULL;
        if (CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, dev, NULL,
                                      &cache) != kCVReturnSuccess) {
            fprintf(stderr, "metal: texture cache creation failed\n");
            free(mc);
            return NULL;
        }

        /* Output pool: NV12, IOSurface-backed so VideoToolbox can take it by
           reference, Metal-compatible so it can be rendered into. */
        NSDictionary *px = @{
            (id)kCVPixelBufferPixelFormatTypeKey :
                @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (id)kCVPixelBufferWidthKey  : @(canvas_w),
            (id)kCVPixelBufferHeightKey : @(canvas_h),
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
            (id)kCVPixelBufferMetalCompatibilityKey  : @YES,
        };
        NSDictionary *poolAttrs = @{
            (id)kCVPixelBufferPoolMinimumBufferCountKey : @(6),
        };
        CVPixelBufferPoolRef pool = NULL;
        if (CVPixelBufferPoolCreate(kCFAllocatorDefault,
                                    (__bridge CFDictionaryRef)poolAttrs,
                                    (__bridge CFDictionaryRef)px,
                                    &pool) != kCVReturnSuccess) {
            fprintf(stderr, "metal: pixel buffer pool creation failed\n");
            CFRelease(cache);
            free(mc);
            return NULL;
        }

        mc->device     = (void *)CFBridgingRetain(dev);
        mc->queue      = (void *)CFBridgingRetain([dev newCommandQueue]);
        mc->pipeLuma   = (void *)CFBridgingRetain(pl);
        mc->pipeChroma = (void *)CFBridgingRetain(pc);
        mc->texCache   = cache;
        mc->pool       = pool;

        fprintf(stderr, "[REC] Compositor: Metal on %s\n",
                dev.name.UTF8String);
    }

    return mc;
}

/* ── render ────────────────────────────────────────────────── */

void *metal_compositor_render(MetalCompositor *mc, int mode,
                               void *screen_pixbuf, void *cam_pixbuf)
{
    if (!mc) return NULL;

    CVPixelBufferRef screen = (CVPixelBufferRef)screen_pixbuf;
    CVPixelBufferRef cam    = (CVPixelBufferRef)cam_pixbuf;
    CVPixelBufferRef out    = NULL;

    @autoreleasepool {
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, mc->pool,
                                               &out) != kCVReturnSuccess || !out)
            return NULL;

        NSMutableArray *keep = [NSMutableArray array];

        id<MTLTexture> outY = plane_texture(mc, out, 0,
                                            MTLPixelFormatR8Unorm, keep);
        id<MTLTexture> outUV = plane_texture(mc, out, 1,
                                             MTLPixelFormatRG8Unorm, keep);
        /* Mode 2 fills the canvas with the camera and never samples the
           screen, so binding it would mean wrapping two textures per frame
           that the shader is guaranteed to discard. */
        BOOL wantScreen = (mode != 2);
        id<MTLTexture> scrY = wantScreen
            ? plane_texture(mc, screen, 0, MTLPixelFormatR8Unorm, keep) : nil;
        id<MTLTexture> scrUV = wantScreen
            ? plane_texture(mc, screen, 1, MTLPixelFormatRG8Unorm, keep) : nil;
        id<MTLTexture> camY = plane_texture(mc, cam, 0,
                                            MTLPixelFormatR8Unorm, keep);
        id<MTLTexture> camUV = plane_texture(mc, cam, 1,
                                             MTLPixelFormatRG8Unorm, keep);

        if (!outY || !outUV) { CVPixelBufferRelease(out); return NULL; }

        /*
         * Every texture slot must be bound even when the shader will not read
         * it.  Alias missing inputs onto the other input rather than onto the
         * render target: binding a colour attachment as a fragment texture in
         * the same pass is a read-write hazard, and mode 2 leaves the screen
         * slots empty on every single frame.
         */
        int hasCam = (camY && camUV) ? 1 : 0;
        if (!scrY || !scrUV) {
            scrY  = hasCam ? camY  : outY;
            scrUV = hasCam ? camUV : outUV;
        }
        if (!hasCam) { camY = scrY; camUV = scrUV; }

        /* Largest centre-crop of the camera that matches the target aspect:
           the square overlay in mode 3, the canvas in mode 2. */
        CompositorParams P = {0};
        P.canvasSize    = (simd_float2){ (float)mc->canvas_w, (float)mc->canvas_h };
        P.overlayOrigin = (simd_float2){ (float)mc->overlay_x, (float)mc->overlay_y };
        P.overlaySize   = (float)mc->overlay_size;
        P.cornerRadius  = (float)mc->overlay_size / 8.0f;
        P.mode          = mode;
        P.hasCam        = hasCam;
        P.camCropOrigin = (simd_float2){ 0.0f, 0.0f };
        P.camCropSize   = (simd_float2){ 1.0f, 1.0f };

        if (hasCam) {
            float cw = (float)CVPixelBufferGetWidth(cam);
            float chh = (float)CVPixelBufferGetHeight(cam);
            float target = (mode == 2)
                ? (float)mc->canvas_w / (float)mc->canvas_h
                : 1.0f;   /* square overlay */
            float srcAspect = cw / chh;
            float fw = 1.0f, fh = 1.0f;
            if (srcAspect > target) fw = target / srcAspect;  /* crop sides */
            else                    fh = srcAspect / target;  /* crop top/bottom */
            P.camCropSize   = (simd_float2){ fw, fh };
            P.camCropOrigin = (simd_float2){ (1.0f - fw) * 0.5f,
                                             (1.0f - fh) * 0.5f };
        }

        id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)mc->queue;
        id<MTLCommandBuffer> cb = [q commandBuffer];

        id<MTLTexture> targets[2] = { outY, outUV };
        void *pipes[2] = { mc->pipeLuma, mc->pipeChroma };

        for (int pass = 0; pass < 2; pass++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture     = targets[pass];
            rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;

            id<MTLRenderCommandEncoder> e =
                [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:
                (__bridge id<MTLRenderPipelineState>)pipes[pass]];
            [e setFragmentTexture:scrY  atIndex:0];
            [e setFragmentTexture:scrUV atIndex:1];
            [e setFragmentTexture:camY  atIndex:2];
            [e setFragmentTexture:camUV atIndex:3];
            [e setFragmentBytes:&P length:sizeof(P) atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle
                  vertexStart:0 vertexCount:3];
            [e endEncoding];
        }

        [cb commit];
        /* VideoToolbox reads this buffer next, so the render must be finished
           before it is handed over. */
        [cb waitUntilCompleted];

        [keep removeAllObjects];

        /*
         * Flushing every frame emptied the cache every frame, which is the one
         * thing it exists to prevent: capture, camera and output buffers all
         * cycle through a small fixed set of IOSurfaces, so the wrapper for
         * each is worth keeping and was instead being rebuilt six times a
         * frame.  Flush occasionally to bound the cache when a buffer pool
         * does get replaced — a mode switch, a resolution change.
         */
        if (++mc->frames % 300 == 0)
            CVMetalTextureCacheFlush(mc->texCache, 0);
    }

    return out;
}

void metal_compositor_release_frame(void *pixbuf)
{
    if (pixbuf) CVPixelBufferRelease((CVPixelBufferRef)pixbuf);
}

void metal_compositor_destroy(MetalCompositor *mc)
{
    if (!mc) return;
    if (mc->pipeLuma)   CFBridgingRelease(mc->pipeLuma);
    if (mc->pipeChroma) CFBridgingRelease(mc->pipeChroma);
    if (mc->queue)      CFBridgingRelease(mc->queue);
    if (mc->device)     CFBridgingRelease(mc->device);
    if (mc->texCache)   CFRelease(mc->texCache);
    if (mc->pool)       CVPixelBufferPoolRelease(mc->pool);
    free(mc);
}
