static size_t arena_alloc(size_t size, id<MTLBuffer> *buffer);
typedef struct {
    __strong id<MTLTexture> base, view;
    __strong id<MTLBuffer> buffer;
    NSUInteger offset, row, image, level, slice;
    MTLOrigin origin;
    MTLSize pixels;
} ZeldaBlockTarget;
static ZeldaBlockTarget z_block_targets[8];
static unsigned z_block_count;
static void zelda_block_barrier(id encoder) {
    if ([encoder conformsToProtocol:@protocol(MTL4CommandEncoder)])
        [encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
}

static void zelda_flush_block_targets(void) {
    unsigned count = z_block_count; z_block_count = 0;
    for (unsigned i = 0; i < count; i++) {
        ZeldaBlockTarget *t = &z_block_targets[i]; id encoder = blit();
        enc_b_copy_texture_buffer(encoder, t->view, 0, 0, MTLOriginMake(0, 0, 0),
            MTLSizeMake(t->view.width, t->view.height, 1), t->buffer, t->offset, t->row, t->image);
        zelda_block_barrier(encoder);
        enc_b_copy_buffer_texture(encoder, t->buffer, t->offset, t->row, t->image,
            t->pixels, t->base, t->slice, t->level, t->origin);
        defer_obj_release(t->view);
        t->base = nil; t->view = nil; t->buffer = nil;
    }
}

static void zelda_attach_view(MTLRenderPassAttachmentDescriptor *a, const tex_t *t, const view_t *v, int has_v) {
    if (!has_v || !v->set_format || v->format == t->d.format) return;
    uint32_t format; uint8_t swizzle[4];
    if (!nvn_fmt_to_mtl(v->format, &format, swizzle) || !format) MDIE("Unsupported render target view format");
    id<MTLTexture> base = a.texture;
    if (!base) MDIE("Render target texture is unavailable");
    if (!fmt_compressed(t->d.format)) {
        if (tex_need_pfv(base.pixelFormat, format) && !(base.usage & MTLTextureUsagePixelFormatView))
            base = tex_upgrade_pfv(t, base, format);
        id<MTLTexture> view = [base newTextureViewWithPixelFormat:format];
        if (!view) MDIE("Cannot create render target format view");
        res_add(view); defer_obj_release(view); a.texture = view; return;
    }
    int bw, bh, vw, vh;
    int bytes = nvn_fmt_bpb(t->d.format, &bw, &bh), view_bytes = nvn_fmt_bpb(v->format, &vw, &vh);
    if (!bytes || bytes != view_bytes || vw != 1 || vh != 1 || base.sampleCount != 1 ||
        z_block_count == 8 || a.level >= base.mipmapLevelCount || (v->set_layers && v->nlayers != 1))
        MDIE("Unsupported compressed render target view layout");
    NSUInteger width = MAX(base.width >> a.level, 1), height = MAX(base.height >> a.level, 1);
    NSUInteger depth = MAX(base.depth >> a.level, 1), layer = base.textureType == MTLTextureType3D ? a.depthPlane : a.slice;
    NSUInteger layers = base.textureType == MTLTextureType3D ? depth : base.arrayLength *
        (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    if (layer >= layers) MDIE("Compressed render target layer out of bounds");
    NSUInteger blocks_x = (width + bw - 1) / bw, blocks_y = (height + bh - 1) / bh;
    ZeldaBlockTarget *r = &z_block_targets[z_block_count++];
    r->base = base; r->level = a.level; r->slice = base.textureType == MTLTextureType3D ? 0 : layer;
    r->origin = MTLOriginMake(0, 0, base.textureType == MTLTextureType3D ? layer : 0);
    r->pixels = MTLSizeMake(width, height, 1);
    r->row = (blocks_x * bytes + 255) & ~(NSUInteger)255; r->image = r->row * blocks_y;
    // shortcut: staging fits the shared 4 MiB arena; larger aliases need a dedicated allocation.
    if (r->image > (4u << 20)) MDIE("Compressed render target staging exceeds 4 MiB");
    id<MTLBuffer> storage; r->offset = arena_alloc(r->image, &storage); r->buffer = storage;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
        width:blocks_x height:blocks_y mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate; descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    r->view = [g_dev newTextureWithDescriptor:descriptor];
    if (!r->view) MDIE("Cannot create compressed-block render target");
    res_add(r->view);
    id encoder = blit();
    enc_b_copy_texture_buffer(encoder, base, r->slice, r->level, r->origin, r->pixels,
        r->buffer, r->offset, r->row, r->image);
    zelda_block_barrier(encoder);
    enc_b_copy_buffer_texture(encoder, r->buffer, r->offset, r->row, r->image,
        MTLSizeMake(blocks_x, blocks_y, 1), r->view, 0, 0, MTLOriginMake(0, 0, 0));
    a.texture = r->view; a.level = a.slice = a.depthPlane = 0;
}
