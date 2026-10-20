static void zelda_clear_region(MTLRenderPassAttachmentDescriptor *attachment, p_scissor scissor, MTLClearColor color) {
    id<MTLTexture> target = attachment.texture;
    if (!target || target.sampleCount != 1) MDIE("Partial clear requires a single-sample target");
    int64_t width = MAX(target.width >> attachment.level, 1), height = MAX(target.height >> attachment.level, 1);
    int64_t x = MAX(scissor.x, 0), y = MAX(scissor.y, 0);
    int64_t end_x = MIN((int64_t)scissor.x + scissor.w, width), end_y = MIN((int64_t)scissor.y + scissor.h, height);
    if (end_x <= x || end_y <= y) return;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:target.pixelFormat
        width:end_x - x height:end_y - y mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget; descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> patch = [g_dev newTextureWithDescriptor:descriptor];
    if (!patch) MDIE("Partial clear texture allocation failed");
    res_add(patch);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = patch;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = color;
    enc_r_end(enc_r_begin(pass));
    enc_b_copy_texture(blit(), patch, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(end_x - x, end_y - y, 1),
        target, attachment.slice, attachment.level, MTLOriginMake(x, y, target.textureType == MTLTextureType3D ? attachment.depthPlane : 0));
    defer_obj_release(patch);
}
