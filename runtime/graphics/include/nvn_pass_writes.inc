// Constant-buffer updates between render passes. Each one used to open its own copy encoder, and every
// copy -> render transition idles the GPU (~30 us). Instead the update waits for the next render pass and is written
// there by a rasterization-free vertex draw, followed by an in-pass vertex -> vertex/fragment barrier, before any of
// that pass's draws. Render encoders begin with a full queue barrier, so all earlier work has finished reading the old
// bytes. Commands that could observe the destination before that pass first write the pending updates with copies.
// ZELDA_PASS_WRITES=0 restores one copy per update.
enum { Z_WRITE_MAX = 64 };
typedef struct { uint64_t source, destination, bytes; } zelda_write_t;
static zelda_write_t z_write[Z_WRITE_MAX];
static int z_write_n, z_write_keep;
static uint64_t z_write_deferred, z_write_in_pass, z_write_copied, z_write_replaced;

static void zelda_write_register(uint64_t destination, uint64_t bytes) {
    if (g_npend < PEND_MAX) { g_pend_lo[g_npend] = destination; g_pend_hi[g_npend++] = destination + bytes; } else g_pend_over = 1;
}
static int zelda_write_overlaps(uint64_t address, uint64_t bytes) {
    for (int i = 0; i < z_write_n; i++)
        if (address < z_write[i].destination + z_write[i].bytes && z_write[i].destination < address + bytes) return 1;
    return 0;
}
// Queue an update issued outside a render pass. On 0 the caller copies it now; the pending updates were copied
// first, so stream order holds (two pending writes of one word would race inside a pass, so they never coexist).
static int zelda_write_defer(uint64_t source, uint64_t destination, uint64_t bytes) {
    static int enabled = -1;
    if (enabled < 0) { const char *value = getenv("ZELDA_PASS_WRITES"); enabled = !value || atoi(value); }
    void *buffer; uint64_t offset, gpu;
    // A later update of exactly the same range replaces the pending one (pending ranges never overlap each other).
    if (enabled && !g_enc && !((source | destination | bytes) & 3))
        for (int i = 0; i < z_write_n; i++)
            if (z_write[i].destination == destination && z_write[i].bytes == bytes && mtl_gpu_addr(source, bytes, &buffer, &offset, &gpu)) {
                z_write[i].source = source; z_write_deferred++; z_write_replaced++; return 1;
            }
    if (!enabled || g_enc || tbdr_on() || !bytes || ((source | destination | bytes) & 3) || bytes > 65536 ||
        z_write_n == Z_WRITE_MAX || zelda_write_overlaps(destination, bytes) || zelda_uniform_overlaps(destination, bytes) ||
        !mtl_gpu_addr(source, bytes, &buffer, &offset, &gpu) || !mtl_gpu_addr(destination, bytes, &buffer, &offset, &gpu)) {
        zelda_write_flush(); return 0;
    }
    z_write[z_write_n++] = (zelda_write_t){source, destination, bytes};
    zelda_write_register(destination, bytes);
    z_write_deferred++;
    return 1;
}
// Write pending updates with copies at this point in the stream (already registered as pending GPU writes).
static void zelda_write_flush(void) {
    if (!z_write_n || z_write_keep) return;
    for (int i = 0; i < z_write_n; i++) {
        void *source, *destination; uint64_t source_offset, destination_offset;
        if (!mtl_addr(z_write[i].source, z_write[i].bytes, &source, &source_offset) ||
            !mtl_addr(z_write[i].destination, z_write[i].bytes, &destination, &destination_offset)) MDIE("Pending constant update left its pool");
        enc_b_copy_buffer(blit(), (__bridge id<MTLBuffer>)source, source_offset, (__bridge id<MTLBuffer>)destination, destination_offset, z_write[i].bytes);
    }
    z_write_copied += z_write_n; z_write_n = 0;
}
// Updates still waiting for the next render pass (a flush keeps them while a submit slice carries them over).
static int zelda_write_pending(void) { return z_write_n; }
// A submit slice keeps the updates for the next command buffer's first pass; they are pending there now.
static void zelda_write_carry(void) {
    for (int i = 0; i < z_write_n; i++) zelda_write_register(z_write[i].destination, z_write[i].bytes);
}
// Commands that cannot observe a pending destination before the next pass's draws run.
static int zelda_write_passthrough(unsigned op, const void *data) {
    switch (op) {
        case OP_BEGIN: case OP_END: case OP_JUMP: case OP_CALL: case OP_BARRIER: case OP_RENDER_ENABLE: case OP_SET_RT:
        case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_DRAW: case OP_UPDATE_UB: return 1;
        // Predicated clears read their counter words, possibly before the next pass.
        case OP_CLEAR_COLOR: case OP_CLEAR_DS: return !z_render.address;
        case OP_DRAW_IDX: {   // Index expansion may read the index buffer before the pass begins.
            const p_draw_idx *draw = data; unsigned size = draw->itype == 0 ? 1 : draw->itype == 1 ? 2 : 4;
            return draw->count >= 0 && !zelda_write_overlaps(draw->ibuf, (uint64_t)draw->count * size);
        }
        case OP_REPORT_COUNTER: case OP_RESET_COUNTER: {
            uint64_t address; memcpy(&address, (const uint8_t *)data + 8, 8);
            return op == OP_RESET_COUNTER || !zelda_write_overlaps(address, 16);
        }
        default: return 0;
    }
}
// Right after a render encoder begins: write every pending update before the pass's own draws.
static void zelda_write_encode(void) {
    if (!z_write_n) return;
    zelda_query_init();
    static id<MTLFunction> function;
    if (!function) function = [z_query_library newFunctionWithName:@"buffer_write"];
    id<MTLRenderPipelineState> pipeline = zelda_vertex_write_pipeline(function);
    enc_r_pso(g_enc, pipeline);
    for (int i = 0; i < z_write_n; i++) {
        void *buffer; uint64_t offset, source, destination;
        if (!mtl_gpu_addr(z_write[i].source, z_write[i].bytes, &buffer, &offset, &source) ||
            !mtl_gpu_addr(z_write[i].destination, z_write[i].bytes, &buffer, &offset, &destination)) MDIE("Pending constant update left its pool");
        [z_query_args setAddress:destination atIndex:0]; [z_query_args setAddress:source atIndex:1];
        [g_enc setArgumentTable:z_query_args atStages:MTLRenderStageVertex];
        [g_enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:(NSUInteger)(z_write[i].bytes / 4)];
    }
    [g_enc barrierAfterEncoderStages:MTLStageVertex beforeEncoderStages:(MTLStageVertex | MTLStageFragment) visibilityOptions:MTL4VisibilityOptionDevice];
    PD("w %llu inline uniform updates=%d", g_stat_pass, z_write_n);
    [g_enc setArgumentTable:g_vt atStages:MTLRenderStageVertex];
    g_enc_pso = nil; args_dirty_all(); g_pso_dirty = 1;
    z_write_in_pass += z_write_n; z_write_n = 0;
}
