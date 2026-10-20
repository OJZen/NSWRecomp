// Uniform bytes are submission-local; index results own storage and survive slices of that submission.
static int zelda_index_reuse_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *e = getenv("ZELDA_INDEX_REUSE"); enabled = !e || atoi(e) != 0; }
    return enabled;
}
static struct { uint64_t source, length, expanded; id<MTLBuffer> buffer; } z_index_cache[64];
static struct { uint64_t source, length; uint8_t bytes[256]; } z_uniform_cache[64];
static unsigned z_index_count, z_uniform_count;
static uint64_t z_buffer_submit, z_buffer_queue, z_buffer_registry;
static void zelda_index_remove(unsigned i) {
    defer_obj_release(z_index_cache[i].buffer);
    z_index_cache[i] = z_index_cache[--z_index_count];
    z_index_cache[z_index_count].buffer = nil;
}
static void zelda_index_forget(void) { while (z_index_count) zelda_index_remove(z_index_count - 1); }
static void zelda_buffer_forget(void) { zelda_index_forget(); z_uniform_count = 0; }
static void zelda_buffer_cache_sync(void) {
    uint64_t registry = atomic_load(&z_pool_generation);
    // A new guest submit may contain CPU writes; a slice only changes command-buffer scratch storage.
    if (z_buffer_queue != z_queue_submit || z_buffer_registry != registry) zelda_buffer_forget();
    if (z_buffer_submit != g_sub_serial) {
        for (unsigned i = 0; i < z_index_count;) {
            if (!z_index_cache[i].buffer) zelda_index_remove(i); else i++;
        }
    }
    z_buffer_submit = g_sub_serial; z_buffer_queue = z_queue_submit; z_buffer_registry = registry;
}
static void zelda_buffer_invalidate(uint64_t address, uint64_t length) {
    zelda_buffer_cache_sync();
    if (!length) return;
    uint64_t source = nvn_canonical_address(address, length);
    if (!source) { zelda_buffer_forget(); return; }
    for (unsigned i = 0; i < z_index_count;) {
        if (source < z_index_cache[i].source + z_index_cache[i].length && z_index_cache[i].source < source + length)
            zelda_index_remove(i);
        else i++;
    }
    for (unsigned i = 0; i < z_uniform_count;) {
        if (source < z_uniform_cache[i].source + z_uniform_cache[i].length && z_uniform_cache[i].source < source + length)
            z_uniform_cache[i] = z_uniform_cache[--z_uniform_count];
        else i++;
    }
}
static void zelda_buffer_texture_written(const tex_t *texture) {
    if (!texture || !texture->mem || !texture->L.size) zelda_buffer_forget();
    else zelda_buffer_invalidate((uint64_t)texture->mem, texture->L.size);
}
static int zelda_buffer_write_op(unsigned op, const void *data) {
    uint64_t address, length;
    switch (op) {
        case OP_UPDATE_UB:
            memcpy(&address, data, 8); memcpy(&length, (const uint8_t *)data + 8, 8); break;
        case OP_COPY_B2B:
            memcpy(&address, (const uint8_t *)data + 8, 8); memcpy(&length, (const uint8_t *)data + 16, 8); break;
        case OP_REPORT_COUNTER:
            memcpy(&address, (const uint8_t *)data + 8, 8); length = 16; break;
        case OP_SIGNAL_EVENT:
            memcpy(&address, data, 8); length = 4; break;
        case OP_COPY_B2T:
            zelda_buffer_texture_written(((const p_copy_b2t *)data)->tex); return 0;
        case OP_COPY_T2T:
            zelda_buffer_texture_written(((const p_copy_t2t *)data)->dst); return 0;
        case OP_CLEAR_TEX:
            zelda_buffer_texture_written(((const p_clear_tex *)data)->tex); return 0;
        case OP_DISPATCH:
            if (!zelda_index_reuse_enabled()) zelda_buffer_forget();
            z_uniform_count = 0; return 0; // Actual shader writes are checked at dispatch.
        case OP_COPY_T2B:
            zelda_buffer_forget(); return 0;
        default: return 0;
    }
    zelda_buffer_cache_sync();
    uint64_t source = nvn_canonical_address(address, length);
    const uint8_t *bytes = op == OP_UPDATE_UB ? (const uint8_t *)data + 16 : NULL;
    if (op == OP_UPDATE_UB && source && length && length <= 256)
        for (unsigned i = 0; i < z_uniform_count; i++)
            if (source >= z_uniform_cache[i].source && source - z_uniform_cache[i].source <= z_uniform_cache[i].length &&
                length <= z_uniform_cache[i].length - (source - z_uniform_cache[i].source) &&
                !memcmp(z_uniform_cache[i].bytes + source - z_uniform_cache[i].source, bytes, length)) return 1;
    zelda_buffer_invalidate(address, length);
    if (op == OP_UPDATE_UB && source && length && length <= 256 && z_uniform_count < 64) {
        unsigned i = z_uniform_count++;
        z_uniform_cache[i].source = source; z_uniform_cache[i].length = length;
        memcpy(z_uniform_cache[i].bytes, bytes, length);
    }
    return 0;
}
