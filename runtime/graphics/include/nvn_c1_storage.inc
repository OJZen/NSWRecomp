static NSMapTable<NSData *, id<MTLBuffer>> *z_c1_storage;
static uint64_t z_c1_hits;
// Called under g_c1_lock; equal zero-padded constants can share the full declared range.
static id<MTLBuffer> zelda_c1_buffer(const uint8_t *data, size_t length) {
    if (length > sizeof(((nvn_refl *)0)->c1)) MDIE("Zelda c1 exceeds reflection storage");
    static int enabled = -1;
    if (enabled < 0) { const char *e = getenv("ZELDA_C1_REUSE"); enabled = !e || atoi(e); }
    NSData *key = nil;
    if (enabled) {
        while (length && !data[length - 1]) length--;
        key = [NSData dataWithBytes:data length:length];
        if (!z_c1_storage) z_c1_storage = [NSMapTable strongToWeakObjectsMapTable];
        id<MTLBuffer> shared = [z_c1_storage objectForKey:key];
        if (shared) { z_c1_hits++; return shared; }
    }
    id<MTLBuffer> buffer = [g_dev newBufferWithLength:C1_DECL_SIZE options:MTLResourceStorageModeShared];
    if (!buffer) MDIE("c1 buffer allocation failed");
    memset(buffer.contents, 0, C1_DECL_SIZE);
    memcpy(buffer.contents, data, length);
    res_add(buffer);
    g_c1_alloc_bytes += C1_DECL_SIZE;
    g_c1_exclusive++;
    if (key) [z_c1_storage setObject:buffer forKey:key];
    return buffer;
}
