// Reflection identifies direct storage ranges. Image, pointer-held or unclassified writes remain conservative.
static void zelda_buffer_shader_written(const nvn_refl *reflection, int stage) {
    if (!z_index_count && !z_uniform_count) return;
    if (!reflection || stage < 0 || stage > 5) { zelda_buffer_forget(); return; }
    for (int i = 0; i < reflection->nb; i++) {
        const nvn_rbind *binding = &reflection->b[i];
        if (binding->kind == NVN_RB_IMG || (binding->rw && binding->kind != NVN_RB_SBUFP)) {
            zelda_buffer_forget(); return;
        }
        if (binding->kind == NVN_RB_SBUFP && binding->rw) {
            if (!zelda_index_reuse_enabled()) { zelda_buffer_forget(); return; }
            if (binding->reg >= 16) { zelda_buffer_forget(); return; }
            const p_bind_buf *buffer = &S.ssbo[stage][binding->reg];
            if (!buffer->addr || !buffer->size) { zelda_buffer_forget(); return; }
            zelda_buffer_invalidate(buffer->addr, buffer->size);
        }
    }
}
