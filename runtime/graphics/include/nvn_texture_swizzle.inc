static id<MTLTexture> zelda_texture_swizzle(id<MTLTexture> view, int format, unsigned requested) {
    uint8_t native[4];
    nvn_fmt_to_mtl(format, NULL, native);
    MTLTextureSwizzle channels[4];
    int changed = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned component = requested ? (requested >> (3 * i)) & 7 : i + 2;
        if (component > NVN_SWZ_A) MDIE("Invalid texture component swizzle");
        // Guest selectors address NVN channels, after the native format's channel conversion.
        channels[i] = (MTLTextureSwizzle)(component < NVN_SWZ_R ? component : native[component - NVN_SWZ_R]);
        changed |= channels[i] != i + NVN_SWZ_R;
    }
    if (!requested && !changed) return view;
    NSUInteger slices = view.textureType == MTLTextureType3D ? 1 : view.arrayLength *
        (view.textureType == MTLTextureTypeCube || view.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    id<MTLTexture> result = [view newTextureViewWithPixelFormat:view.pixelFormat textureType:view.textureType
        levels:NSMakeRange(0, view.mipmapLevelCount) slices:NSMakeRange(0, slices)
        swizzle:MTLTextureSwizzleChannelsMake(channels[0], channels[1], channels[2], channels[3])];
    if (!result) MDIE("Zelda texture component swizzle could not be created");
    return result;
}
