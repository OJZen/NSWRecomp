// Complete source hashes prove these c3 footprints; unknown programs keep ordinary GPU copies.
static unsigned z_inline_count; // Inline uniform writes deferred in an open pass (nvn_inline_uniform.inc).
static const unsigned z_cube_words[] = {1, 3, 4, 5, 10, 11};
static const unsigned z_mip_words[] = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 64};
static const unsigned *z_uniform_words;
static unsigned z_uniform_word_count;
static struct { uint64_t address, canonical; uint32_t value; } z_uniform_pending[18];
static unsigned z_uniform_pending_count;
static uint64_t z_uniform_gpu, z_uniform_source, z_uniform_submit, z_uniform_pool, z_uniform_program_serial;
static const prog_t *z_uniform_program;
static uint64_t z_uniform_snapshots, z_uniform_deferred;

static void zelda_uniform_flush(void) {
    int changed = z_uniform_source || z_uniform_pending_count;
    z_uniform_gpu = z_uniform_source = 0;
    for (unsigned i = 0; i < z_uniform_pending_count; i++) {
        uint64_t address = z_uniform_pending[i].address; void *destination; uint64_t offset;
        if (!mtl_addr(address, 4, &destination, &offset)) MDIE("Deferred uniform outside memory pool");
        id<MTLBuffer> source; size_t start = arena_alloc(4, &source);
        memcpy((uint8_t *)source.contents + start, &z_uniform_pending[i].value, 4);
        enc_b_copy_buffer(blit(), source, start, (__bridge id<MTLBuffer>)destination, offset, 4);
    }
    z_uniform_pending_count = 0; if (changed) args_dirty_all();
}
static int zelda_uniform_defer(const void *data, unsigned record_size) {
    uint64_t address, length; memcpy(&address, data, 8); memcpy(&length, (const uint8_t *)data + 8, 8);
    if (length != 4 || record_size < 28 || !z_uniform_source || z_inline_count ||
        z_uniform_submit != g_sub_serial || z_uniform_pool != atomic_load(&z_pool_generation) ||
        S.prog != z_uniform_program || S.prog->serial != z_uniform_program_serial) return 0;
    uint64_t canonical = nvn_canonical_address(address, 4);
    int word = 0;
    for (unsigned i = 0; i < z_uniform_word_count; i++) word |= canonical == z_uniform_source + 4 * z_uniform_words[i];
    if (!canonical || !word) return 0;
    unsigned i;
    for (i = 0; i < z_uniform_pending_count; i++) if (z_uniform_pending[i].canonical == canonical) break;
    if (i == z_uniform_pending_count) {
        if (i == 18) return 0;
        z_uniform_pending_count++;
        if (g_npend < PEND_MAX) { g_pend_lo[g_npend] = address; g_pend_hi[g_npend++] = address + 4; }
        else g_pend_over = 1;
    }
    z_uniform_pending[i].address = address; z_uniform_pending[i].canonical = canonical;
    memcpy(&z_uniform_pending[i].value, (const uint8_t *)data + 16, 4);
    z_uniform_deferred++; args_dirty_all(); return 1;
}
static int zelda_uniform_overlaps(uint64_t address, uint64_t length) {
    if (!z_uniform_pending_count) return 0;
    if (!length) return 1;
    uint64_t source = nvn_canonical_address(address, length);
    if (!source) return 1;
    for (unsigned i = 0; i < z_uniform_pending_count; i++)
        if (source < z_uniform_pending[i].canonical + 4 && z_uniform_pending[i].canonical < source + length) return 1;
    return 0;
}
static int zelda_uniform_passthrough(unsigned op, const void *data) {
    if (!z_uniform_pending_count) return 1;
    switch (op) {
        case OP_BEGIN: case OP_END: case OP_JUMP: case OP_CALL: case OP_UPDATE_UB:
        case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
        case OP_DRAW: case OP_DRAW_IDX: return 1;
        case OP_REPORT_COUNTER: {
            uint32_t type; uint64_t address; memcpy(&type, data, 4); memcpy(&address, (const uint8_t *)data + 8, 8);
            return (type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) &&
                !zelda_uniform_overlaps(address, 16);
        }
        case OP_SET_RT: {
            const p_set_rt *r = data;
            for (int i = 0; i < r->n && i < 8; i++) if (r->color[i] &&
                zelda_uniform_overlaps((uint64_t)r->color[i]->mem, r->color[i]->L.size)) return 0;
            return !r->depth || !zelda_uniform_overlaps((uint64_t)r->depth->mem, r->depth->L.size);
        }
        default: return 0;
    }
}
static void zelda_uniform_prepare_draw(NvProg *program, const p_draw_idx *indices) {
    id<MTLBuffer> snapshot = nil;
    z_uniform_gpu = 0;
    if (tbdr_on() || !program || !program->sh[0] || !program->sh[1] ||
        !program->sh[0]->z_cube_uniform || program->sh[0]->z_cube_uniform > 2 ||
        program->sh[0]->z_cube_uniform != program->sh[1]->z_cube_uniform) goto fallback;
    for (int stage = 2; stage < 6; stage++) if (program->sh[stage]) goto fallback;
    for (int i = 0; i < program->sh[0]->rf.nv; i++) {
        unsigned attribute = program->sh[0]->rf.v[i].nvn;
        if (attribute >= 16 || zelda_vertex_input_class(S.attr[attribute].fmt)) goto fallback;
    }
    if (!zelda_uniform_linked_source(program->sh[0]->z_cube_uniform)) goto fallback;
    unsigned count = program->sh[0]->z_cube_uniform == 1 ? 6 : 18;
    const unsigned *layout = count == 6 ? z_cube_words : z_mip_words;
    unsigned extent = 4 * (layout[count - 1] + 1);
    uint64_t base = nvn_canonical_address(S.ubo[0][0].addr, extent);
    if (!base || S.ubo[0][0].size < extent || S.ubo[1][0].size < extent ||
        nvn_canonical_address(S.ubo[1][0].addr, extent) != base) goto fallback;
    if (!zelda_uniform_passthrough(OP_SET_RT, &R.rt)) goto fallback;
    int aliases = 0;
    pthread_mutex_lock(&g_rmu);
    for (int stage = 0; stage < 2; stage++) for (int i = 0; i < program->sh[stage]->rf.nb; i++) {
        const nvn_rbind *binding = &program->sh[stage]->rf.b[i];
        if (binding->kind != NVN_RB_TEX || binding->reg >= 128) continue;
        unsigned tid = S.tex[stage][binding->reg] & 0xfffff;
        if (tid && tid < (unsigned)g_ntreg) aliases |= zelda_uniform_overlaps((uint64_t)g_treg[tid].t.mem, g_treg[tid].t.L.size);
    }
    pthread_mutex_unlock(&g_rmu);
    if (aliases) goto fallback;
    for (int i = 0; i < 16; i++) if (S.vbuf[i].address &&
        zelda_uniform_overlaps(S.vbuf[i].address, S.vbuf[i].size)) goto fallback;
    if (indices && zelda_uniform_overlaps(indices->ibuf, (uint64_t)indices->count * (indices->itype == 0 ? 1 : indices->itype == 1 ? 2 : 4))) goto fallback;
    zelda_buffer_cache_sync();
    uint32_t words[18];
    for (unsigned i = 0; i < count; i++) {
        uint64_t address = base + 4 * layout[i]; int found = 0;
        for (unsigned j = 0; j < z_uniform_count; j++) {
            uint64_t offset = address - z_uniform_cache[j].source;
            if (address >= z_uniform_cache[j].source && offset <= z_uniform_cache[j].length &&
                4 <= z_uniform_cache[j].length - offset) {
                memcpy(&words[i], z_uniform_cache[j].bytes + offset, 4); found = 1; break;
            }
        }
        if (!found) goto fallback;
    }
    size_t offset = arena_alloc(65536, &snapshot);
    memset((uint8_t *)snapshot.contents + offset, 0, extent);
    for (unsigned i = 0; i < count; i++) memcpy((uint8_t *)snapshot.contents + offset + 4 * layout[i], &words[i], 4);
    z_uniform_gpu = snapshot.gpuAddress + offset; z_uniform_source = base;
    z_uniform_words = layout; z_uniform_word_count = count;
    z_uniform_submit = g_sub_serial; z_uniform_pool = atomic_load(&z_pool_generation);
    z_uniform_program = S.prog; z_uniform_program_serial = S.prog->serial;
    z_uniform_snapshots++; args_dirty_all(); return;
fallback:
    zelda_uniform_flush();
}
