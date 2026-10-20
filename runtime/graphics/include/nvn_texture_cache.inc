static void zelda_sampled_view_forget(treg_t *entry) {
    if (!entry->array_view) return;
    // Recorded GPU commands keep the old view alive until their completion fence.
    defer_obj_unref((__bridge id)entry->array_view);
    CFRelease(entry->array_view); entry->array_view = NULL; entry->array_rid = 0;
}
static int zelda_texture_view_current(id<MTLTexture> view, id<MTLTexture> current) {
    if (!current) return 0;
    id<MTLTexture> ancestor = view;
    while (ancestor && ancestor != current) ancestor = ancestor.parentTexture;
    return ancestor != nil;
}
static void zelda_trim_texture_views(void) {
    pthread_mutex_lock(&g_rmu);
    for (int i = 0; i < g_ntreg; i++) {
        treg_t *r = &g_treg[i];
        if (!r->view) continue;
        id owner = serial_get(r->t.serial);
        id<MTLTexture> current = [owner isKindOfClass:[NvPkg class]] ? ((NvPkg *)owner)->tex : owner;
        if (zelda_texture_view_current((__bridge id<MTLTexture>)r->view, current)) continue;
        zelda_sampled_view_forget(r);
        // Unused descriptor slots otherwise retain every old upload until the slot is reused.
        defer_obj_unref((__bridge id)r->view);
        CFRelease(r->view); r->view = NULL; r->view_rid = 0;
    }
    pthread_mutex_unlock(&g_rmu);
}
