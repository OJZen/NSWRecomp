// Only fully uploaded CPU textures enter this pool; the existing retirement fence protects readers.
enum { Z_TEXTURE_REUSE_COUNT = 64, Z_TEXTURE_REUSE_BYTES = 16 << 20 };
static struct { id<MTLTexture> texture; uint64_t fence; } z_texture_reuse[Z_TEXTURE_REUSE_COUNT];
static unsigned z_texture_reuse_cursor;
static uint64_t z_texture_reuse_bytes, z_texture_reuse_hits;
static void zelda_texture_evict(unsigned i) {
    id<MTLTexture> texture = z_texture_reuse[i].texture;
    if (!texture) return;
    z_texture_reuse_bytes -= texture.allocatedSize;
    defer_obj_release(texture); z_texture_reuse[i].texture = nil;
}
static void zelda_texture_retire(id<MTLTexture> texture) {
    if (!texture || !texture.allocatedSize || texture.allocatedSize > Z_TEXTURE_REUSE_BYTES ||
        texture.parentTexture || texture.buffer || texture.heap || texture.storageMode != MTLStorageModePrivate) {
        defer_obj_release(texture); return;
    }
    unsigned i = z_texture_reuse_cursor++ % Z_TEXTURE_REUSE_COUNT;
    zelda_texture_evict(i);
    while (z_texture_reuse_bytes + texture.allocatedSize > Z_TEXTURE_REUSE_BYTES) {
        i = z_texture_reuse_cursor++ % Z_TEXTURE_REUSE_COUNT; zelda_texture_evict(i);
    }
    z_texture_reuse[i].texture = texture; z_texture_reuse[i].fence = g_fence + 1;
    z_texture_reuse_bytes += texture.allocatedSize;
}
static id<MTLTexture> zelda_texture_reuse(MTLTextureDescriptor *d) {
    pthread_mutex_lock(&g_mu); uint64_t done = g_done; pthread_mutex_unlock(&g_mu);
    for (unsigned i = 0; i < Z_TEXTURE_REUSE_COUNT; i++) {
        id<MTLTexture> t = z_texture_reuse[i].texture;
        if (!t || z_texture_reuse[i].fence > done || t.textureType != d.textureType ||
            t.pixelFormat != d.pixelFormat || t.width != d.width || t.height != d.height || t.depth != d.depth ||
            t.arrayLength != d.arrayLength || t.mipmapLevelCount != d.mipmapLevelCount ||
            t.sampleCount != d.sampleCount || t.usage != d.usage || t.storageMode != d.storageMode) continue;
        z_texture_reuse_bytes -= t.allocatedSize; z_texture_reuse[i].texture = nil; z_texture_reuse_hits++;
        return t;
    }
    return nil;
}
