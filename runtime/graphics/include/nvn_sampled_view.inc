static uint64_t zelda_sampled_texture(int tid, int type, void **out) {
    uint64_t address = tex_fast_rid(tid, out);
    __unsafe_unretained id<MTLTexture> texture = (__bridge id<MTLTexture>)*out;
    if (texture && ((type == MTLTextureType2DArray && texture.textureType == MTLTextureType2D) ||
            (type == MTLTextureType2D && texture.textureType == MTLTextureType2DArray && texture.arrayLength == 1))) {
        // Metal requires the shader's type even when 2D and single-layer array views address the same texels.
        treg_t *entry = &g_treg[tid];
        if (!entry->array_view) {
            id<MTLTexture> view = [texture newTextureViewWithPixelFormat:texture.pixelFormat
                textureType:type levels:NSMakeRange(0, texture.mipmapLevelCount)
                slices:NSMakeRange(0, 1) swizzle:texture.swizzle];
            if (!view) MDIE("Cannot create a compatible single-layer sampled view");
            entry->array_view = (void *)CFBridgingRetain(view);
            entry->array_rid = view.gpuResourceID._impl;
        }
        *out = entry->array_view; address = entry->array_rid;
    }
    return address;
}
