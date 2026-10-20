#include "shader_status.h"
#include "nvn_query_msl.h"
static id<MTLLibrary> z_query_library;
static id<MTL4ArgumentTable> z_query_args;
static int z_query_first = -1;
static void zelda_query_init(void) {
    if (z_query_library) return;
    NSError *error = nil;
    z_query_library = ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:@(zelda_query_msl) options:nil error:&error]);
    MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 2;
    z_query_args = [g_dev newArgumentTableWithDescriptor:d error:&error];
    if (!z_query_library || !z_query_args) MDIE("GPU query pipeline initialization failed: %s", error.localizedDescription.UTF8String);
}
// Canonical 16-byte destinations of the counter writes posted since the last flush, and their bounds for a quick reject.
enum { Z_QUERY_PENDING_MAX = 256 };
static uint64_t z_query_destination[Z_QUERY_PENDING_MAX], z_query_low = UINT64_MAX, z_query_high;
static int zelda_gpu_counter(uint64_t address, unsigned type) {
    if (type != NVN_COUNTER_TYPE_TIMESTAMP && type != NVN_COUNTER_TYPE_TIMESTAMP_TOP && type != NVN_COUNTER_TYPE_SAMPLES_PASSED) return 0;
    if (z_query_n == Z_QUERY_PENDING_MAX) zelda_query_flush();
    if (z_query_first < 0) z_query_first = g_npost;
    post((post_op){.kind = type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 5 : 6, .addr = address,
        .vis = type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? vis_snapshot() : NULL});
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED) g_vis_cur = -1;
    uint64_t destination = nvn_canonical_address(address, 16);
    if (!destination) { zelda_query_flush(); return 1; }   // Unknown aliasing: write it now rather than track it.
    z_query_destination[z_query_n++] = destination;
    z_query_low = MIN(z_query_low, destination); z_query_high = MAX(z_query_high, destination + 16);
    return 1;
}
static int zelda_query_overlaps(uint64_t address, uint64_t length) {
    if (!z_query_n || !length) return 0;
    uint64_t source = nvn_canonical_address(address, length);
    if (!source) return 1;
    if (source >= z_query_high || source + length <= z_query_low) return 0;
    for (int i = 0; i < z_query_n; i++)
        if (source < z_query_destination[i] + 16 && z_query_destination[i] < source + length) return 1;
    return 0;
}
static int zelda_query_targets_overlap(const p_set_rt *targets) {
    for (int i = 0; i < targets->n && i < 8; i++) if (targets->color[i] &&
        zelda_query_overlaps((uint64_t)targets->color[i]->mem, targets->color[i]->L.size)) return 1;
    return targets->depth && zelda_query_overlaps((uint64_t)targets->depth->mem, targets->depth->L.size);
}
// Commands that cannot read or overwrite a pending counter destination before it is written. A barrier alone does not
// force the write: anything after it that could observe the memory is checked here or in zelda_query_prepare_draw.
static int zelda_query_passthrough(unsigned op, const void *data) {
    static int lazy = -1;
    if (lazy < 0) { const char *value = getenv("ZELDA_LAZY_QUERY"); lazy = !value || atoi(value); }   // 0: old flush points
    if (!lazy) return !(op == OP_BARRIER || op == OP_DISPATCH || op == OP_COPY_B2B || op == OP_COPY_T2B || op == OP_UPDATE_UB);
    switch (op) {
        case OP_BEGIN: case OP_END: case OP_JUMP: case OP_CALL: case OP_BARRIER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE:
        case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_DRAW: case OP_DRAW_IDX: return 1;
        case OP_CLEAR_COLOR: case OP_CLEAR_DS: return !zelda_query_targets_overlap(&R.rt);
        case OP_SET_RT: return !zelda_query_targets_overlap(data);
        case OP_REPORT_COUNTER: {
            uint64_t type, address; memcpy(&type, data, 8); memcpy(&address, (const uint8_t *)data + 8, 8);
            // Posted counters keep stream order among themselves; other counter types are written at once.
            return type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP ||
                type == NVN_COUNTER_TYPE_SAMPLES_PASSED || !zelda_query_overlaps(address, 16);
        }
        case OP_SIGNAL_EVENT: { uint64_t address; memcpy(&address, data, 8); return !zelda_query_overlaps(address, 4); }
        case OP_UPDATE_UB: {
            uint64_t address, length; memcpy(&address, data, 8); memcpy(&length, (const uint8_t *)data + 8, 8);
            return !zelda_query_overlaps(address, length);
        }
        case OP_COPY_B2B: {
            uint64_t source, destination, length; memcpy(&source, data, 8); memcpy(&destination, (const uint8_t *)data + 8, 8);
            memcpy(&length, (const uint8_t *)data + 16, 8);
            return !zelda_query_overlaps(source, length) && !zelda_query_overlaps(destination, length);
        }
        default: return 0;   // Dispatches, texture copies/clears and unknown commands write pending counters first.
    }
}
static void zelda_query_prepare_draw(NvProg *program, const p_draw_idx *indices) {
    if (!z_query_n) return;
    int conflict = !program || zelda_query_targets_overlap(&R.rt);
    for (int i = 0; i < 16 && !conflict; i++) conflict = S.vbuf[i].address && zelda_query_overlaps(S.vbuf[i].address, S.vbuf[i].size);
    if (!conflict && indices)
        conflict = zelda_query_overlaps(indices->ibuf, (uint64_t)indices->count * (indices->itype == 0 ? 1 : indices->itype == 1 ? 2 : 4));
    pthread_mutex_lock(&g_rmu);
    for (int slot = 0; slot < 5 && !conflict; slot++) {
        __unsafe_unretained NvShader *shader = program->sh[slot];
        if (!shader) continue;
        int stage = shader->stage;
        if (stage < 0 || stage > 4) { conflict = 1; break; }
        for (int i = 0; i < shader->rf.nb && !conflict; i++) {
            const nvn_rbind *binding = &shader->rf.b[i]; int reg = binding->reg;
            if (binding->kind == NVN_RB_CBUF || binding->kind == NVN_RB_UBOP || binding->kind == NVN_RB_SBUFP) {
                if (reg < 0 || reg >= 16) { conflict = 1; break; }
                const p_bind_buf *buffer = binding->kind == NVN_RB_SBUFP ? &S.ssbo[stage][reg] : &S.ubo[stage][reg];
                conflict = buffer->addr && zelda_query_overlaps(buffer->addr, buffer->size);
            } else if (binding->kind == NVN_RB_TEX || binding->kind == NVN_RB_TBUF) {
                unsigned tid = reg >= 0 && reg < 128 ? (unsigned)(S.tex[stage][reg] & 0xfffff) : 0;
                if (tid && tid < (unsigned)g_ntreg) conflict = zelda_query_overlaps((uint64_t)g_treg[tid].t.mem, g_treg[tid].t.L.size);
            } else if (binding->kind == NVN_RB_IMG || binding->kind == ZELDA_RB_SBUF_CB) {
                conflict = 1;   // Image views and uniform-held storage addresses are not resolved here; write first.
            }
        }
    }
    pthread_mutex_unlock(&g_rmu);
    if (conflict) { z_query_read_flushes++; zelda_query_flush(); }
}
static void zelda_query_flush(void) {
    z_query_n = 0; z_query_low = UINT64_MAX; z_query_high = 0;
    if (z_query_first < 0) return;
    rp_flush();
    zelda_query_init();
    static id<MTLComputePipelineState> pipeline;
    if (!pipeline) {
        NSError *error = nil;
        pipeline = ZELDA_SHADER_BUILD([g_dev newComputePipelineStateWithFunction:[z_query_library newFunctionWithName:@"query_sum"] error:&error]);
        if (!pipeline) MDIE("GPU counter pipeline failed: %s", error.localizedDescription.UTF8String);
    }
    // Metal 4's copy encoder can also dispatch; keep adjacent query work in this encoder.
    id<MTL4ComputeCommandEncoder> encoder = blit();
    [encoder barrierAfterEncoderStages:(MTLStageDispatch | MTLStageBlit) beforeEncoderStages:(MTLStageDispatch | MTLStageBlit) visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder setArgumentTable:z_query_args];
    for (int operation = z_query_first; operation < g_npost; operation++) {
        post_op *op = &g_post[operation];
        if (op->kind != 5 && op->kind != 6) continue;
        uint64_t address = op->addr;
        zelda_buffer_invalidate(address, 16);
        void *destination; uint64_t offset, gpu;
        if ((address & 7) || !mtl_gpu_addr(address, 16, &destination, &offset, &gpu)) MDIE("GPU counter outside memory pool");
        void dm_guest_write_prepare(void *, size_t); dm_guest_write_prepare((void *)(uintptr_t)address, 16);
        vis_list *list = op->vis;
        unsigned count = list ? (unsigned)list->n : 0;
        if (count > 65535) MDIE("GPU counter sample list exceeds arena bounds");
        if (!count) {
            // Timestamp payloads and empty visibility queries are zero; no sum shader is needed.
            [encoder fillBuffer:(__bridge id<MTLBuffer>)destination range:NSMakeRange(offset, 8) value:0];
            [encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:(MTLStageDispatch | MTLStageBlit) visibilityOptions:MTL4VisibilityOptionDevice];
            free(list); op->vis = NULL; op->kind = 4;
            continue;
        }
        id<MTLBuffer> parameters; size_t start = arena_alloc((count + 1) * sizeof(uint64_t), &parameters);
        uint64_t *values = (uint64_t *)((uint8_t *)parameters.contents + start); values[0] = count;
        for (unsigned i = 0; i < count; i++) {
            id<MTLBuffer> source = (__bridge id<MTLBuffer>)list->s[i].buf;
            values[i + 1] = source.gpuAddress + (uint64_t)list->s[i].slot * 8;
            for (int r = 0; r < g_vis_ring_n; r++) if (g_vis_ring[r].buf == source && g_vis_ring[r].fence < g_fence + 1)
                g_vis_ring[r].fence = g_fence + 1;
            CFRelease(list->s[i].buf);
        }
        free(list); op->vis = NULL; op->kind = 4;
        [z_query_args setAddress:gpu atIndex:0]; [z_query_args setAddress:parameters.gpuAddress + start atIndex:1];
        enc_c_pso(encoder, pipeline); enc_c_dispatch(encoder, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
        [encoder barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:(MTLStageDispatch | MTLStageBlit) visibilityOptions:MTL4VisibilityOptionDevice];
        // Only the approximate timestamp stays on the CPU; conditional rendering reads the GPU-written payload.
    }
    z_query_first = -1;
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
}
// Predicate words that pending counter writes will set: a timestamp report zeroes its counter word. Any other pending
// write to the two 16-byte reports (or unknown aliasing) means the counters must be written before the predicate is read.
static int zelda_query_forward(uint64_t address, uint32_t *known) {
    *known = 0;
    if (z_query_first < 0) return 1;
    uint64_t base = nvn_canonical_address(address, 32);
    if (!base) return 0;
    for (int i = z_query_first; i < g_npost; i++) {
        const post_op *op = &g_post[i];
        if (op->kind != 5 && op->kind != 6) continue;
        uint64_t destination = nvn_canonical_address(op->addr, 16);
        if (!destination) return 0;
        if (destination + 16 <= base || destination >= base + 32) continue;
        if (op->kind == 6 && destination == base) *known |= 1;
        else if (op->kind == 6 && destination == base + 16) *known |= 2;
        else return 0;
    }
    return 1;
}
// A conditional clear in a pass of its own, after every earlier command.
static void zelda_color_clear_alone(MTLRenderPassColorAttachmentDescriptor *attachment, const zelda_color_clear_t *c) {
    zelda_query_init();
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pipelines;
    if (!pipelines) pipelines = [NSMutableDictionary dictionary];
    id<MTLTexture> target = attachment.texture;
    NSNumber *key = @(target.pixelFormat);
    id<MTLRenderPipelineState> pipeline = pipelines[key];
    if (!pipeline) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [z_query_library newFunctionWithName:@"clear_vertex"];
        d.fragmentFunction = [z_query_library newFunctionWithName:@"clear_if"];
        d.colorAttachments[0].pixelFormat = target.pixelFormat;
        NSError *error = nil; pipeline = ZELDA_SHADER_BUILD([g_dev newRenderPipelineStateWithDescriptor:d error:&error]);
        if (!pipeline) MDIE("Conditional clear pipeline failed: %s", error.localizedDescription.UTF8String);
        pipelines[key] = pipeline;
    }
    id<MTLBuffer> parameters; size_t start = arena_alloc(32, &parameters);
    uint8_t *p = (uint8_t *)parameters.contents + start;
    memcpy(p, c->rgba, 16); memcpy(p + 16, &c->mode, 4); memcpy(p + 20, &c->known, 4);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    PD("c %llu standalone conditional clear %ldx%ld known=%u", g_stat_pass, c->width, c->height, c->known);
    MTL4RenderPassDescriptor *pass = [MTL4RenderPassDescriptor new];
    attachment.loadAction = MTLLoadActionLoad; attachment.storeAction = MTLStoreActionStore;
    pass.colorAttachments[0] = attachment;
    id<MTL4RenderCommandEncoder> encoder = enc_r_begin(pass);
    [encoder setArgumentTable:z_query_args atStages:(MTLRenderStageVertex | MTLRenderStageFragment)];
    [z_query_args setAddress:c->query atIndex:0]; [z_query_args setAddress:parameters.gpuAddress + start atIndex:1];
    enc_r_pso(encoder, pipeline); enc_r_viewport(encoder, (MTLViewport){0, 0, c->width, c->height, 0, 1});
    enc_r_scissor(encoder, (MTLScissorRect){c->x, c->y, c->right - c->x, c->bottom - c->y});
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_r_end(encoder);
    zelda_flush_block_targets(); args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    z_color_clear_alone++;
}
static int zelda_gpu_conditional_clear(const p_clear_color *clear) {
    int partial = zelda_inline_partial_clear(clear);
    if ((!z_render.address && !partial) || clear->mask != 15 || clear->index < 0 || clear->index >= R.rt.n || !R.rt.color[clear->index]) return 0;
    MTLRenderPassColorAttachmentDescriptor *attachment = [MTLRenderPassColorAttachmentDescriptor new];
    attach(attachment, R.rt.color[clear->index], &R.rt.cview[clear->index], R.rt.has_cview[clear->index], 1);
    id<MTLTexture> target = attachment.texture;
    // shortcut: integer color, multisample and 3D clears retain the CPU fallback until exercised by the game.
    if (!target || zelda_fragment_class(target.pixelFormat) || target.sampleCount != 1 || target.textureType == MTLTextureType3D) return 0;
    void *query; uint64_t offset, gpu;
    if (partial) gpu = dummy_buf_gpu();
    else if (!mtl_gpu_addr(z_render.address, 32, &query, &offset, &gpu)) MDIE("Conditional clear outside memory pool");
    int64_t width = MAX(target.width >> attachment.level, 1), height = MAX(target.height >> attachment.level, 1);
    int64_t x = MAX(R.sc.x, 0), y = MAX(R.sc.y, 0);
    int64_t right = MIN((int64_t)R.sc.x + R.sc.w, width), bottom = MIN((int64_t)R.sc.y + R.sc.h, height);
    if (right <= x || bottom <= y) return 1;
    zelda_color_clear_t c = {.index = clear->index, .mode = partial ? 0 : z_render.mode, .predicate = z_render.address, .query = gpu,
        .x = (long)x, .y = (long)y, .right = (long)right, .bottom = (long)bottom, .width = (long)width, .height = (long)height};
    memcpy(c.rgba, clear->rgba, 16);
    if (zelda_color_clear_join(&c)) { if (partial) z_partial_clear_inline++; return 1; }
    rp_flush();
    zelda_query_flush();
    c.known = partial ? 3 : 0;
    zelda_color_clear_alone(attachment, &c);
    return 1;
}

static uint64_t z_depth_color_joined, z_depth_color_kept;
// Disjoint writes commute; never move a clear across a target or counter alias.
static int zelda_depth_color_join(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *value = getenv("ZELDA_COLOR_DEPTH_CLEAR"); enabled = !value || atoi(value); }
    const tex_t *depth = R.rt.depth;
    if (!enabled || g_enc || z_depth_clear.pending || !z_color_clear.count || !depth ||
        !depth->mem || !depth->L.size || zelda_pending_clear_conflicts(0xff) ||
        zelda_color_clear_touches((uint64_t)depth->mem, depth->L.size)) return 0;
    uint64_t d = nvn_canonical_address((uint64_t)depth->mem, depth->L.size), q = z_render.address ? nvn_canonical_address(z_render.address, 32) : 0;
    if (!d || (z_render.address && !q) || (q && d < q + 32 && q < d + depth->L.size)) return 0;
    for (int i = 0; i < R.rt.n && i < 8; i++) {
        const tex_t *color = R.rt.color[i]; if (!color) continue;
        uint64_t c = nvn_canonical_address((uint64_t)color->mem, color->L.size);
        if (!c || !color->L.size || (q && c < q + 32 && q < c + color->L.size) ||
            (c < d + depth->L.size && d < c + color->L.size)) return 0;
    }
    return 1;
}
static uint64_t z_depth_query_forwards, z_depth_reports_kept;
static int zelda_lazy_depth_clear(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *value = getenv("ZELDA_LAZY_DEPTH_CLEAR"); enabled = !value || atoi(value); }
    return enabled;
}
static int zelda_depth_report_deferred(const void *data) {
    uint64_t type; memcpy(&type, data, sizeof type);
    if (!zelda_lazy_depth_clear() || (type != NVN_COUNTER_TYPE_TIMESTAMP && type != NVN_COUNTER_TYPE_TIMESTAMP_TOP &&
        type != NVN_COUNTER_TYPE_SAMPLES_PASSED)) return 0;
    z_depth_reports_kept++;
    return 1;
}
static int zelda_gpu_depth_clear(const p_clear_ds *clear) {
    static int enabled = -1;
    if (enabled < 0) { const char *value = getenv("ZELDA_GPU_DEPTH"); enabled = !value || atoi(value); }
    if (!enabled || !z_render.address || !R.rt.depth || !isfinite(clear->depth)) return 0;
    MTLRenderPassDepthAttachmentDescriptor *attachment = [MTLRenderPassDepthAttachmentDescriptor new];
    attach(attachment, R.rt.depth, &R.rt.dview, R.rt.has_dview, 0);
    id<MTLTexture> texture = attachment.texture;
    // shortcut: multisample and unusual depth formats use the synchronous fallback until exercised.
    if (texture.sampleCount != 1 || (texture.pixelFormat != MTLPixelFormatDepth32Float &&
        texture.pixelFormat != MTLPixelFormatDepth32Float_Stencil8)) return 0;
    if (!clear->depth_mask && !clear->stencil_mask) return 1;
    void *query; uint64_t offset, gpu;
    if (!mtl_gpu_addr(z_render.address, 32, &query, &offset, &gpu)) MDIE("Conditional depth clear outside memory pool");
    int join = zelda_depth_color_join();
    if (!join) rp_flush();
    uint32_t known = 0;
    if (zelda_lazy_depth_clear() && zelda_query_forward(z_render.address, &known)) {
        if (z_query_first >= 0) z_depth_query_forwards++;
    } else { rp_flush(); zelda_query_flush(); known = 0; join = 0; }
    zelda_query_init();
    id<MTLBuffer> parameters; size_t start = arena_alloc(32, &parameters);
    float depth = fminf(1, fmaxf(0, clear->depth));
    memcpy((uint8_t *)parameters.contents + start, &depth, 4);
    memcpy((uint8_t *)parameters.contents + start + 16, &z_render.mode, 4);
    memcpy((uint8_t *)parameters.contents + start + 20, &known, 4);
    z_depth_clear = (typeof(z_depth_clear)){.pending=1, .has_scissor=R.has_sc, .clear=*clear,
        .scissor=R.sc, .parameters=parameters.gpuAddress + start, .query=gpu};
    if (join) z_depth_color_joined++;
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    return 1;
}

