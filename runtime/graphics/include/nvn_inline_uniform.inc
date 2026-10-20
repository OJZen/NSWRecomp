// Inline uniform updates arriving inside an open render pass are written after the pass instead of ending it.
// Until then, draws read each constant-buffer range covering a deferred write from a per-draw snapshot built only
// from bytes already known in this queue submission. Any other reader, or any unchecked command, writes them first.
#include "nvn_pointer_storage.h"
enum { Z_INLINE_MAX = 32, Z_INLINE_SNAPSHOT_MAX = 4096 };
static unsigned z_inline_count;
static struct { uint64_t source, destination, canonical, length; } z_inline[Z_INLINE_MAX];
static uint64_t z_inline_override[5][16];
static int z_inline_overridden;
static uint64_t z_inline_deferred, z_inline_snapshots, z_inline_flushes, z_inline_breaks;

static int zelda_inline_overlaps(uint64_t address, uint64_t length) {
    if (!z_inline_count) return 0;
    if (!length) return 1;
    uint64_t source = nvn_canonical_address(address, length);
    if (!source) return 1;
    for (unsigned i = 0; i < z_inline_count; i++)
        if (source < z_inline[i].canonical + z_inline[i].length && z_inline[i].canonical < source + length) return 1;
    return 0;
}
static int zelda_inline_targets_overlap(const p_set_rt *targets) {
    for (int i = 0; i < targets->n && i < 8; i++) if (targets->color[i] &&
        zelda_inline_overlaps((uint64_t)targets->color[i]->mem, targets->color[i]->L.size)) return 1;
    return targets->depth && zelda_inline_overlaps((uint64_t)targets->depth->mem, targets->depth->L.size);
}
static void zelda_inline_clear_overrides(void) {
    if (!z_inline_overridden) return;
    memset(z_inline_override, 0, sizeof z_inline_override); z_inline_overridden = 0; args_dirty_all();
}
static void zelda_inline_flush(void) {
    zelda_inline_clear_overrides();
    if (!z_inline_count) return;
    unsigned count = z_inline_count; z_inline_count = 0; z_inline_flushes++; z_inline_breaks += g_enc != nil;
    // In stream order, so overlapping updates leave the last value; the first copy ends any open pass. After the
    // pass has ended they wait for the next one like other updates between passes (nvn_pass_writes.inc).
    for (unsigned i = 0; i < count; i++)
        if (g_enc || !zelda_write_defer(z_inline[i].source, z_inline[i].destination, z_inline[i].length))
            mtl_sub_copy(z_inline[i].source, z_inline[i].destination, z_inline[i].length);
}
static int zelda_inline_defer(const void *data) {
    uint64_t address, length; memcpy(&address, data, 8); memcpy(&length, (const uint8_t *)data + 8, 8);
    // Outside a pass the copy costs no pass break; the cube snapshot keeps its own deferred words in order.
    if (!g_enc || tbdr_on() || z_uniform_pending_count || z_inline_count == Z_INLINE_MAX || !length) return 0;
    uint64_t canonical = nvn_canonical_address(address, length);
    if (!canonical) return 0;
    z_inline[z_inline_count++] = (typeof(z_inline[0])){(uint64_t)(uintptr_t)((const uint8_t *)data + 16), address, canonical, length};
    if (g_npend < PEND_MAX) { g_pend_lo[g_npend] = address; g_pend_hi[g_npend++] = address + length; }
    else g_pend_over = 1;
    z_inline_deferred++; return 1;
}
static int zelda_inline_passthrough(unsigned op, const void *data) {
    switch (op) {
        case OP_BEGIN: case OP_END: case OP_JUMP: case OP_CALL: case OP_UPDATE_UB: case OP_BARRIER: case OP_RESET_COUNTER:
        case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_DRAW: case OP_DRAW_IDX: return 1;
        case OP_CLEAR_COLOR: case OP_CLEAR_DS: return !zelda_inline_targets_overlap(&R.rt);
        case OP_SET_RT: return !zelda_inline_targets_overlap(data);
        case OP_REPORT_COUNTER: { uint64_t address; memcpy(&address, (const uint8_t *)data + 8, 8); return !zelda_inline_overlaps(address, 16); }
        case OP_SIGNAL_EVENT: { uint64_t address; memcpy(&address, data, 8); return !zelda_inline_overlaps(address, 4); }
        case OP_RENDER_ENABLE: { const p_render_enable *r = data; return !r->address || !zelda_inline_overlaps(r->address, 32); }
        default: return 0;
    }
}
static uint64_t zelda_inline_snapshot(uint64_t address, uint64_t length) {
    uint64_t source = length <= Z_INLINE_SNAPSHOT_MAX ? nvn_canonical_address(address, length) : 0;
    if (!source) return 0;
    zelda_buffer_cache_sync();
    // Cached entries are disjoint and invalidated by every other possible write, so full coverage is exact content.
    for (int pass = 0; pass < 2; pass++) {
        id<MTLBuffer> buffer = nil; size_t offset = 0;
        if (pass) offset = arena_alloc((size_t)length, &buffer);
        for (uint64_t at = 0; at < length;) {
            uint64_t take = 0;
            for (unsigned j = 0; j < z_uniform_count && !take; j++) {
                uint64_t start = z_uniform_cache[j].source, inside = source + at - start;
                if (source + at < start || inside >= z_uniform_cache[j].length) continue;
                take = MIN(length - at, z_uniform_cache[j].length - inside);
                if (pass) memcpy((uint8_t *)buffer.contents + offset + at, z_uniform_cache[j].bytes + inside, (size_t)take);
            }
            if (!take) return 0;
            at += take;
        }
        if (pass) return buffer.gpuAddress + offset;
    }
    return 0;
}
static void zelda_inline_prepare_draw(NvProg *program, const p_draw_idx *indices) {
    zelda_inline_clear_overrides();
    if (!z_inline_count) return;
    uint64_t snapshots[5][16] = {{0}}; int any = 0, conflict = !program || zelda_inline_targets_overlap(&R.rt);
    for (int i = 0; i < 16 && !conflict; i++) conflict = S.vbuf[i].address && zelda_inline_overlaps(S.vbuf[i].address, S.vbuf[i].size);
    if (!conflict && indices)
        conflict = zelda_inline_overlaps(indices->ibuf, (uint64_t)indices->count * (indices->itype == 0 ? 1 : indices->itype == 1 ? 2 : 4));
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
                if (!buffer->addr || !zelda_inline_overlaps(buffer->addr, buffer->size)) continue;
                if (binding->kind != NVN_RB_CBUF) conflict = 1;
                else if (!snapshots[stage][reg] && !(snapshots[stage][reg] = zelda_inline_snapshot(buffer->addr, buffer->size))) conflict = 1;
                else any = 1;
            } else if (binding->kind == NVN_RB_TEX || binding->kind == NVN_RB_TBUF) {
                unsigned tid = reg >= 0 && reg < 128 ? (unsigned)(S.tex[stage][reg] & 0xfffff) : 0;
                if (tid && tid < (unsigned)g_ntreg) conflict = zelda_inline_overlaps((uint64_t)g_treg[tid].t.mem, g_treg[tid].t.L.size);
            } else if (binding->kind == ZELDA_RB_SBUF_CB) {
                static int enabled = -1;
                if (enabled < 0) { const char *e = getenv("ZELDA_INLINE_POINTER_STORAGE"); enabled = !e || atoi(e); }
                void *buffer = NULL;
                uint64_t gpu = enabled ? zelda_pointer_storage(stage, binding, shader->key, &buffer) : 0;
                id<MTLBuffer> storage = (__bridge id<MTLBuffer>)buffer;
                // The shader's read extent is unknown: protect the whole backing buffer, including aliases.
                conflict = !gpu || !storage.contents ||
                    zelda_inline_overlaps((uint64_t)(uintptr_t)storage.contents, storage.length);
            } else if (binding->kind == NVN_RB_IMG) {
                conflict = 1;   // Image views are not resolved here; write first.
            }
        }
    }
    pthread_mutex_unlock(&g_rmu);
    if (conflict) { zelda_inline_flush(); return; }
    if (!any) return;
    memcpy(z_inline_override, snapshots, sizeof snapshots); z_inline_overridden = 1;
    z_inline_snapshots++; args_dirty_all();
}
