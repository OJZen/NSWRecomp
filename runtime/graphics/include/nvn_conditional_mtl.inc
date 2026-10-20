static p_render_enable z_render = {.enabled = 1};
static int zelda_render_enabled(void) {
    if (!z_render.address) return z_render.enabled;
    // Unsupported draws and clear formats retain the CPU fallback.
    uint64_t fence = mtl_sub_commit();
    if (!mtl_wait(fence, 5000000000ull)) MDIE("Conditional render GPU wait timed out");
    void *buffer; uint64_t offset;
    if (!mtl_addr(z_render.address, 32, &buffer, &offset)) MDIE("Conditional render address is not in a memory pool");
    const void *contents = [(__bridge id<MTLBuffer>)buffer contents];
    if (!contents) MDIE("Conditional render buffer is not CPU visible");
    uint64_t counters[4]; memcpy(counters, (const uint8_t *)contents + offset, sizeof counters);
    int enabled = nvn_render_test(&z_render, counters);
    PD("W test addr=%#llx mode=%d values=%llu,%llu render=%d", (unsigned long long)z_render.address, z_render.mode,
       (unsigned long long)counters[0], (unsigned long long)counters[2], enabled);
    mtl_sub_begin(); args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    return enabled;
}
