static id<MTLBuffer> zj_bound_buffer(id<MTLBuffer> original, uint64_t gpu) {
    if (gpu >= original.gpuAddress && gpu - original.gpuAddress < original.length) return original;
    for (NvArena *arena in g_sub_arenas)
        if (gpu >= arena->gpu && gpu - arena->gpu < arena->used) return arena->buf;
    return nil;
}
static uint64_t zj_bound_length(const nvn_rbind *binding, unsigned stage) {
    if (stage >= 6 || binding->reg >= 16) return 0;
    if (binding->kind == NVN_RB_CBUF || binding->kind == NVN_RB_UBOP) return S.ubo[stage][binding->reg].size;
    if (binding->kind == NVN_RB_SBUFP) return S.ssbo[stage][binding->reg].size;
    return 0;
}
