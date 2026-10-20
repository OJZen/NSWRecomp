// The descriptor owns its view under g_rmu; cached validation need not transfer it through an Objective-C return ABI.
static uint64_t z_texture_lookup_hits;
static int zelda_cached_texture(int tid, void **out) {
    static int enabled = -1;
    if (enabled < 0) { const char *value = getenv("ZELDA_TEXTURE_LOOKUP_CACHE"); enabled = !value || atoi(value); }
    if (!enabled || (g_tpool_n && tid >= g_tpool_n) || tid < 0 || tid >= g_ntreg ||
        !g_treg[tid].ok || !g_treg[tid].view) return 0;
    treg_t *entry = &g_treg[tid];
    uint64_t serial = g_ser_gen, dirty = nvn_dirty_now();
    if (entry->view_ser != serial || entry->view_dirty != dirty) {
        __unsafe_unretained id<MTLTexture> view = (__bridge id<MTLTexture>)entry->view;
        __unsafe_unretained id<MTLTexture> current = tex_mtl(&entry->t);
        if (!zelda_texture_view_current(view, current)) return 0;
        entry->view_ser = serial; entry->view_dirty = dirty;
    }
    *out = entry->view; z_texture_lookup_hits++; return 1;
}
