#include "shader_status.h"
#include "nvn_conditional_draw.h"
static uint64_t z_conditional_draws, z_conditional_batches, z_conditional_fallbacks;
static unsigned z_conditional_max_batch;
enum { Z_CONDITIONAL_MAX = 64 };
static zelda_conditional_input z_conditional_inputs[Z_CONDITIONAL_MAX];
static const uint8_t *z_conditional_commands[Z_CONDITIONAL_MAX];
typedef struct {
    const prog_t *program;
    p_render_enable predicate;
    poly_st polygon;
    float line_width;
    uint32_t restart, restart_index;
} zelda_conditional_state;
static int z_conditional_mode_value = -1;
static int zelda_conditional_mode(void) {
    if (z_conditional_mode_value < 0) { const char *v = getenv("ZELDA_GPU_CONDITIONAL"); z_conditional_mode_value = v ? atoi(v) : 2; }
    return z_conditional_mode_value;
}
// GPU predicates must not outrun writes that only happen in a CPU completion callback.
typedef struct { uint64_t address, size, fence; } zelda_conditional_cpu_write;
static zelda_conditional_cpu_write z_conditional_cpu[128];
static unsigned z_conditional_cpu_count;
static uint64_t z_conditional_cpu_overflow;
static zelda_conditional_cpu_write zelda_conditional_post_range(const post_op *op) {
    uint64_t address = op->kind == 1 ? (uint64_t)op->val : op->addr;
    uint64_t size = op->kind == 1 ? 4 : 16;
    if (op->kind >= 4 && op->kind <= 6) { address += 8; size = 8; }
    address = nvn_canonical_address(address, size);
    // Predicates read aligned words at +0/+16, never the intervening timestamp words.
    if (address && size == 8 && (address & 15) == 8) return (zelda_conditional_cpu_write){0};
    return (zelda_conditional_cpu_write){.address=address,.size=size};
}
static int zelda_conditional_cpu_overlap(zelda_conditional_cpu_write write, uint64_t predicate) {
    return write.size && (!write.address || (write.address < predicate+8 && predicate < write.address+write.size) ||
        (write.address < predicate+24 && predicate+16 < write.address+write.size));
}
static void zelda_conditional_post_commit(const post_op *ops, int count, uint64_t fence) {
    if (!zelda_conditional_mode()) return;
    pthread_mutex_lock(&g_mu); uint64_t done = g_done; pthread_mutex_unlock(&g_mu);
    unsigned kept = 0;
    for (unsigned i=0;i<z_conditional_cpu_count;i++) if (z_conditional_cpu[i].fence > done) z_conditional_cpu[kept++]=z_conditional_cpu[i];
    z_conditional_cpu_count=kept;
    for (int i=0;i<count;i++) {
        zelda_conditional_cpu_write write=zelda_conditional_post_range(&ops[i]);
        if (!write.size) continue;
        if (z_conditional_cpu_count == 128 || !write.address) { z_conditional_cpu_overflow=fence; continue; }
        write.fence=fence;z_conditional_cpu[z_conditional_cpu_count++]=write;
    }
}
static int zelda_conditional_cpu_pending(uint64_t address) {
    uint64_t q=nvn_canonical_address(address,32);
    if (!q || (q&15)) return 1;
    pthread_mutex_lock(&g_mu); uint64_t done=g_done; pthread_mutex_unlock(&g_mu);
    if (z_conditional_cpu_overflow>done) return 1;
    for (unsigned i=0;i<z_conditional_cpu_count;i++)
        if (z_conditional_cpu[i].fence>done && zelda_conditional_cpu_overlap(z_conditional_cpu[i],q)) return 1;
    for (int i=0;i<g_npost;i++) if (zelda_conditional_cpu_overlap(zelda_conditional_post_range(&g_post[i]),q)) return 1;
    return 0;
}
static int zelda_conditional_input_make(const p_draw *dw, const p_draw_idx *di, const zelda_conditional_state *state, zelda_conditional_input *p) {
    int primitive = dw ? dw->prim : di->prim;
    int count = dw ? dw->count : di->count, instances = dw ? dw->instances : di->instances;
    int base = dw ? dw->base_instance : di->base_instance;
    // shortcut: U8 expansion, wide-line expansion and non-native topologies keep the CPU path until covered.
    if (primitive != 0 && primitive != 1 && primitive != 3 && primitive != 4 && primitive != 5) return 0;
    if (count < 0 || instances < 0 || base < 0 || (dw && dw->first < 0) ||
        (state->line_width != 1 && zelda_uses_line_width(primitive, state->polygon.mode))) return 0;
    if (di && ((di->itype != 1 && di->itype != 2) ||
        (state->restart && state->restart_index != (di->itype == 1 ? 0xffffu : 0xffffffffu)))) return 0;
    *p = (zelda_conditional_input){.predicate = state->predicate.address, .mode = state->predicate.mode, .indexed = di != NULL,
        .args = {(uint32_t)count, (uint32_t)instances, dw ? (uint32_t)dw->first : 0,
                 di ? (uint32_t)di->base_vertex : (uint32_t)base, di ? (uint32_t)base : 0}};
    return 1;
}
// A batched predicate may move before draws only when they cannot write memory outside the attachments.
static int zelda_conditional_readonly(const prog_t *program) {
    NvProg *p = program ? prog_get(program) : nil;
    if (!p || !p->sh[0]) return 0;
    for (int stage = 0; stage < 6; stage++) if (p->sh[stage]) {
        const nvn_refl *r = &p->sh[stage]->rf;
        if (stage > 1 || !r->route_b) return 0;
        for (int i = 0; i < r->nb; i++) {
            if (r->b[i].rw) return 0;
            switch (r->b[i].kind) {
                case NVN_RB_CBUF: case NVN_RB_C1: case NVN_RB_SUPPORT:
                case NVN_RB_TEX: case NVN_RB_TBUF: case NVN_RB_SAMP: case NVN_RB_SAMPT: break;
                default: return 0;
            }
        }
    }
    return 1;
}
static int zelda_conditional_target_safe(uint64_t predicate) {
    uint64_t q = nvn_canonical_address(predicate, 32);
    if (!q) return 0;
    for (int i = 0; i <= 8; i++) {
        const tex_t *texture = i == 8 ? R.rt.depth : i < R.rt.n ? R.rt.color[i] : NULL;
        if (!texture) continue;
        uint64_t size = texture->L.size, address = nvn_canonical_address((uint64_t)texture->mem, size);
        if (!address || (address < q + 32 && q < address + size)) return 0;
    }
    return 1;
}
static unsigned zelda_conditional_collect(const zelda_conditional_input *first, zelda_conditional_state state) {
    z_conditional_commands[0] = g_exec_cur; z_conditional_inputs[0] = *first;
    unsigned count = 1;
    if (zelda_conditional_mode() < 2 || !zelda_conditional_readonly(state.program) || !zelda_conditional_target_safe(first->predicate)) return count;
    const uint8_t *p = g_exec_next;
    // shortcut: collect only this linear command segment; CALL/JUMP/END stop until nested batches are needed.
    for (unsigned guard = 0; p && guard < 4096 && count < Z_CONDITIONAL_MAX; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (!nvn_command_valid(p, h)) break;
        const void *a = p + 8;
        switch (h.op) {
            case OP_RENDER_ENABLE:
                if (h.size < 8 + sizeof(p_render_enable)) return count;
                state.predicate = *(const p_render_enable *)a; break;
            case OP_BIND_PROGRAM:
                if (h.size < 8 + sizeof(p_bind_program)) return count;
                state.program = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_STATE: {
                if (h.size < 8 + sizeof(p_bind_state)) return count;
                const p_bind_state *b = a;
                if (b->kind > 9) return count;
                if (b->kind == 4) memcpy(&state.polygon, b->bytes, sizeof state.polygon);
                if (b->kind == 8) { state.restart = b->bytes[0]; memcpy(&state.restart_index, b->bytes + 4, 4); }
                if (b->kind == 9) memcpy(&state.line_width, b->bytes, 4);
                break;
            }
            case OP_DRAW: case OP_DRAW_IDX: {
                if (h.size < 8 + (h.op == OP_DRAW ? sizeof(p_draw) : sizeof(p_draw_idx)) || !zelda_conditional_readonly(state.program)) return count;
                if (state.predicate.address) {
                    zelda_conditional_input input;
                    if (!zelda_conditional_target_safe(state.predicate.address) || zelda_conditional_cpu_pending(state.predicate.address) ||
                        !zelda_conditional_input_make(h.op == OP_DRAW ? a : NULL, h.op == OP_DRAW_IDX ? a : NULL, &state, &input)) return count;
                    z_conditional_commands[count] = p; z_conditional_inputs[count++] = input;
                }
                break;
            }
            case OP_BEGIN: case OP_SCISSOR: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
            case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
            case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BLEND_COLOR:
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
            default: return count;
        }
        p += h.size;
    }
    return count;
}
static uint64_t zelda_conditional_encode(const zelda_conditional_input *input, unsigned count) {
    rp_flush(); zelda_inline_flush(); zelda_uniform_flush(); zelda_write_flush(); zelda_query_flush();
    id<MTLBuffer> parameters, output;
    size_t pstart = arena_alloc(sizeof(*input) * count, &parameters), ostart = arena_alloc(sizeof(zelda_conditional_output) * count, &output);
    zelda_conditional_input *p = (void *)((uint8_t *)parameters.contents + pstart);
    memcpy(p, input, sizeof(*input) * count);
    for (unsigned i = 0; i < count; i++) {
        void *buffer; uint64_t offset, gpu;
        if ((p[i].predicate & 7) || !mtl_gpu_addr(p[i].predicate, 32, &buffer, &offset, &gpu))
            MDIE("Conditional draw predicate is outside a memory pool");
        p[i].predicate = gpu;
    }
    static id<MTLComputePipelineState> pipeline;
    static id<MTL4ArgumentTable> table;
    if (!pipeline) {
        NSError *error = nil;
        id<MTLLibrary> library = ZELDA_SHADER_BUILD([g_dev newLibraryWithSource:@(zelda_conditional_msl) options:nil error:&error]);
        pipeline = ZELDA_SHADER_BUILD([g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"conditional_draw"] error:&error]);
        MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 2;
        table = [g_dev newArgumentTableWithDescriptor:d error:&error];
        if (!pipeline || !table) MDIE("Conditional draw pipeline failed: %s", error.localizedDescription.UTF8String);
    }
    id<MTL4ComputeCommandEncoder> encoder = blit();
    [encoder barrierAfterEncoderStages:(MTLStageDispatch | MTLStageBlit) beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    [table setAddress:parameters.gpuAddress + pstart atIndex:0]; [table setAddress:output.gpuAddress + ostart atIndex:1];
    [encoder setArgumentTable:table]; enc_c_pso(encoder, pipeline);
    enc_c_dispatch(encoder, MTLSizeMake(1, 1, 1), MTLSizeMake(count, 1, 1));
    [encoder barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:(MTLStageDispatch | MTLStageBlit) visibilityOptions:MTL4VisibilityOptionDevice];
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1; z_conditional_batches++;
    static int remaining = -1;
    if (remaining < 0) { const char *v = getenv("ZELDA_CONDITIONAL_CHECK"); remaining = v ? MIN(MAX(atoi(v), 0), 4096) : 0; }
    unsigned checked = MIN(count, (unsigned)remaining); remaining -= checked;
    if (checked) {
        uint64_t frame = atomic_load(&g_cur_frame);
        NSData *original = [NSData dataWithBytes:input length:count * sizeof(*input)];
        [g_commit_blocks addObject:^{
            const zelda_conditional_input *expected = (const void *)((const uint8_t *)parameters.contents + pstart);
            const zelda_conditional_output *result = (const void *)((const uint8_t *)output.contents + ostart);
            for (unsigned i = 0; i < checked; i++) {
                uint64_t counters[4] = {result[i].left, 0, result[i].right, 0};
                p_render_enable predicate = {.address = 1, .mode = expected[i].mode};
                uint32_t words[5]; memcpy(words, expected[i].args, sizeof words);
                int enabled = nvn_render_test(&predicate, counters); if (!enabled) words[0] = 0;
                if (result[i].enabled != (unsigned)enabled || memcmp(words, result[i].args, sizeof words))
                    MDIE("GPU conditional draw differs from CPU predicate");
                const zelda_conditional_input *guest = original.bytes;
                MLOG("Conditional check frame=%llu predicate=%#llx mode=%u values=%llu,%llu enabled=%d count=%u indexed=%u",
                    frame, guest[i].predicate, expected[i].mode, counters[0], counters[2], enabled, words[0], expected[i].indexed);
            }
        }];
    }
    return output.gpuAddress + ostart;
}
static int zelda_conditional_draw(const p_draw *dw, const p_draw_idx *di) {
    z_conditional_gpu = 0;
    if (!z_render.address) return z_render.enabled;
    int requested = zelda_conditional_mode_take();
    if (requested >= 0) {
        // Mode switches are diagnostics only; finish old CPU callbacks before changing the predicate path.
        uint64_t fence = mtl_sub_commit();
        if (!mtl_wait(fence, 5000000000ull)) MDIE("Conditional mode switch GPU wait timed out");
        mtl_sub_begin(); args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
        z_conditional_mode_value = requested;
        MLOG("Conditional mode applied=%d frame=%llu", requested, atomic_load(&g_cur_frame));
    }
    static uint64_t serial, base;
    static unsigned next, count;
    zelda_conditional_state state = {.program = S.prog, .predicate = z_render, .polygon = S.poly,
        .line_width = S.line_width, .restart = S.restart, .restart_index = S.restart_index};
    zelda_conditional_input input;
    int supported = zelda_conditional_mode() && !tbdr_on() && zelda_conditional_input_make(dw, di, &state, &input);
    int cached = supported && serial == g_sub_serial && next < count && z_conditional_commands[next] == g_exec_cur &&
        !memcmp(&input, &z_conditional_inputs[next], sizeof input);
    if (supported && !cached && zelda_conditional_cpu_pending(z_render.address)) supported = 0;
    if (supported) {
        if (!cached) {
            count = zelda_conditional_collect(&input, state); next = 0;
            base = zelda_conditional_encode(z_conditional_inputs, count); serial = g_sub_serial;
            z_conditional_max_batch = MAX(z_conditional_max_batch, count);
        }
        z_conditional_gpu = base + next++ * sizeof(zelda_conditional_output); z_conditional_draws++;
    } else { count = next = 0; z_conditional_fallbacks++; }
    static uint64_t last;
    uint64_t frame = atomic_load(&g_cur_frame);
    if (frame >= last + 300) {
        last = frame;
        MLOG("Conditional draws mode=%d gpu=%llu batches=%llu max_batch=%u fallback=%llu", zelda_conditional_mode(),
            z_conditional_draws, z_conditional_batches, z_conditional_max_batch, z_conditional_fallbacks);
    }
    return supported || zelda_render_enabled();
}
