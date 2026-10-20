// Submit only at an existing barrier, with no deferred pass. Every Metal encoder already starts after a queue barrier,
// so a slice lets the GPU start earlier frame work while later passes are still being translated.
static void zelda_submit_slice(void) {
    static uint64_t frame = UINT64_MAX, pass;
    static unsigned slices;
    static int budget = -1, first, second, limit;
    if (budget < 0) {
        const char *e = getenv("ZELDA_SUBMIT_PASSES"), *f = getenv("ZELDA_SUBMIT_FIRST"), *s = getenv("ZELDA_SUBMIT_SECOND"),
            *n = getenv("ZELDA_SUBMIT_SLICES");
        // Feed the GPU again before its short first slice drains; subsequent batches keep their lower CPU overhead.
        budget = e ? MAX(atoi(e), 0) : 32; first = f ? MAX(atoi(f), 1) : 4; limit = n ? MAX(atoi(n), 0) : 12;
        second = s ? MAX(atoi(s), 1) : 12;
    }
    uint64_t current = atomic_load(&g_cur_frame);
    if (frame != current) { frame = current; pass = g_stat_pass; slices = 0; }
    zelda_timeline(TL_BARRIER, zelda_timeline_now(), g_stat_pass - pass, slices);
    if (!budget || slices >= (unsigned)limit || g_stat_pass == pass || tbdr_on()) return;
    // The first slice decides when the GPU can start this frame; later ones go out early once it has drained.
    if (g_stat_pass - pass < (unsigned)(slices == 0 ? first : slices == 1 ? second : budget)) {
        if (!slices) return;
        pthread_mutex_lock(&g_mu); int idle = g_done >= g_fence; pthread_mutex_unlock(&g_mu);
        if (!idle) return;
    }
    int pending = g_npend, overflow = g_pend_over;
    // Occlusion counts may span the slice: later slices keep the same slot buffer and allocation cursor.
    z_slice_vis_buffer = g_vis; z_slice_vis.ring = g_vis_ring_i; z_slice_vis.used = g_vis_n; z_slice_vis.keep = 1;
    z_write_keep = 1; mtl_sub_commit(); z_write_keep = 0; zelda_write_carry();
    mtl_sub_begin();
    z_slice_vis.keep = 0; z_slice_vis_buffer = nil;
    // Earlier slices may still be writing these ranges; CPU reads must keep treating them as pending.
    g_npend = pending; g_pend_over = overflow;
    args_dirty_all();
    pass = g_stat_pass; slices++;
}
// Pass ends without a barrier: only slice when nothing pending would turn into an extra clear pass.
static void zelda_slice_between_passes(void) {
    if (g_enc || R.clear_d || R.clear_s || z_depth_clear.pending || z_color_clear.count) return;
    for (int i = 0; i < 8; i++) if (R.clear_c[i]) return;
    zelda_submit_slice();
}
