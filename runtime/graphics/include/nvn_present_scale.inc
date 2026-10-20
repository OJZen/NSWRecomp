// Optional MetalFX spatial upscale of the presented image. Mode 1 (window) scales to the fitted drawable size when it
// is larger than the game output (full screen or a large window on a Retina display), and the usual present draw then
// copies it 1:1 (keeping letterboxing). Mode 2 (4K) scales to 3840x2160 (fitted to the image's aspect) whenever the
// drawable is larger than the game image, and the present draw then resamples that to the drawable when sizes differ. Metal 4 resources are untracked: an empty encoder with
// a queue barrier updates the fence the scaler waits on, so it reads the finished game frame; the present render
// encoder already starts with a queue barrier. Without MetalFX, a cropped input or a smaller output, nothing changes.
#import <MetalFX/MetalFX.h>
static _Atomic int z_scale_mode;
static id<MTL4FXSpatialScaler> z_scaler;
static id<MTLTexture> z_scaled;
static id<MTLFence> z_scale_fence;
static uint64_t z_scale_frames, z_scale_failures;
// Outputs replaced after a resize stay resident until the presents that used them have finished (the present ring
// waits for the command buffer three presents back before reusing its slot).
static struct { __strong id<MTLTexture> texture; uint64_t frame; } z_scale_retired[8];
static void zelda_scale_retire(void) {
    for (int i = 0; i < 8; i++)
        if (z_scale_retired[i].texture && z_scale_frames >= z_scale_retired[i].frame + 4) {
            res_remove(z_scale_retired[i].texture); z_scale_retired[i].texture = nil;
        }
}
static int zelda_scale_retire_slot(void) {
    for (int i = 0; i < 8; i++) if (!z_scale_retired[i].texture) return i;
    return -1;
}

void zelda_present_metalfx(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    atomic_store(&z_scale_mode, mode); MLOG("MetalFX present upscale %s", (const char *[]){"off", "to window", "to 4K"}[mode]);
}

int zelda_present_metalfx_mode(void) { return atomic_load(&z_scale_mode); }

static id<MTLTexture> zelda_present_upscale(id<MTL4CommandBuffer> cb, id<MTLTexture> src, NSUInteger cx, NSUInteger cy,
                                            NSUInteger cw, NSUInteger ch, NSUInteger out_w, NSUInteger out_h) {
    int mode = atomic_load(&z_scale_mode);
    if (mode == 2 && out_w > cw && out_h > ch) {   // Only when the drawable is larger than the game image.
        double scale = fmin(3840.0 / cw, 2160.0 / ch);
        out_w = (NSUInteger)lround(cw * scale); out_h = (NSUInteger)lround(ch * scale);
    }
    if (!mode || cx || cy || out_w <= cw || out_h <= ch || out_w > 16384 || out_h > 16384) return nil;
    static int unsupported = -1;
    if (unsupported < 0) {
        unsupported = ![MTLFXSpatialScalerDescriptor supportsDevice:g_dev];
        if (unsupported) MLOG("ERROR: MetalFX spatial scaling unsupported on this device");
    }
    if (unsupported) return nil;
    zelda_scale_retire();
    if (!z_scaler || z_scaler.inputWidth != src.width || z_scaler.inputHeight != src.height || z_scaler.colorTextureFormat != src.pixelFormat ||
        z_scaled.width != out_w || z_scaled.height != out_h) {
        int slot = zelda_scale_retire_slot();
        if (z_scaled && slot < 0) return nil;   // Many resizes in flight: stay bilinear until outputs retire.
        MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
        d.inputWidth = src.width; d.inputHeight = src.height; d.outputWidth = out_w; d.outputHeight = out_h;
        d.colorTextureFormat = src.pixelFormat; d.outputTextureFormat = src.pixelFormat;
        d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;   // 8-bit gamma-encoded game output
        static id<MTL4Compiler> compiler;
        NSError *error = nil;
        if (!compiler) compiler = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
        id<MTL4FXSpatialScaler> scaler = compiler ? [d newSpatialScalerWithDevice:g_dev compiler:compiler] : nil;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:out_w height:out_h mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = (scaler ? scaler.outputTextureUsage : 0) | MTLTextureUsageShaderRead;
        id<MTLTexture> output = scaler ? [g_dev newTextureWithDescriptor:td] : nil;
        if (!scaler || !output || (src.usage & scaler.colorTextureUsage) != scaler.colorTextureUsage) {
            if (!z_scale_failures++) MLOG("ERROR: MetalFX scaler unavailable for %lux%lu -> %lux%lu format %lu: %s", (unsigned long)src.width,
                (unsigned long)src.height, (unsigned long)out_w, (unsigned long)out_h, (unsigned long)src.pixelFormat,
                error ? error.localizedDescription.UTF8String : "scaler, output or input usage");
            return nil;
        }
        if (z_scaled) { z_scale_retired[slot].texture = z_scaled; z_scale_retired[slot].frame = z_scale_frames; }
        res_add(output); res_commit();
        if (!z_scale_fence) z_scale_fence = [g_dev newFence];
        z_scaler = scaler; z_scaled = output; z_scaler.fence = z_scale_fence;
        MLOG("MetalFX present upscale %lux%lu -> %lux%lu", (unsigned long)cw, (unsigned long)ch, (unsigned long)out_w, (unsigned long)out_h);
    }
    id<MTL4ComputeCommandEncoder> order = [cb computeCommandEncoder];
    [order barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [order updateFence:z_scale_fence afterEncoderStages:MTLStageDispatch];
    [order endEncoding];
    z_scaler.colorTexture = src; z_scaler.outputTexture = z_scaled;
    z_scaler.inputContentWidth = cw; z_scaler.inputContentHeight = ch;
    [z_scaler encodeToCommandBuffer:cb];
    z_scale_frames++;
    return z_scaled;
}
