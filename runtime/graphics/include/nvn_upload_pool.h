// Each slot belongs to an upload allocator, which up_begin waits to become idle before reuse.
static id<MTLBuffer> z_upload_buffers[3];
static uint64_t z_upload_buffers_created;
static int zelda_upload_is_pooled(id object) {
    for (unsigned i = 0; i < 3; i++) if (object && object == z_upload_buffers[i]) return 1;
    return 0;
}
static id<MTLBuffer> zelda_upload_buffer(unsigned slot, size_t size) {
    if (slot >= 3) MDIE("Invalid upload allocator slot");
    if (size > (4u << 20)) return nil;
    if (!z_upload_buffers[slot]) {
        id<MTLBuffer> buffer = [g_dev newBufferWithLength:4u << 20 options:MTLResourceStorageModePrivate];
        if (!buffer) MDIE("Cannot allocate upload reuse buffer");
        res_add(buffer); z_upload_buffers[slot] = buffer; z_upload_buffers_created++;
    }
    return z_upload_buffers[slot];
}
