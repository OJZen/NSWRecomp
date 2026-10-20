NVN(nvnTextureBuilderSetPackagedTextureData, void, NVNtextureBuilder *builder, const void *data) {
    ((tex_builder_t *)builder)->packaged_data = data;
}
NVN(nvnTextureBuilderGetPackagedTextureData, const void *, const NVNtextureBuilder *builder) {
    return ((const tex_builder_t *)builder)->packaged_data;
}
static int zelda_packaged_storage_valid(const tex_builder_t *builder, const tex_t *texture) {
    if (!builder->packaged_data) return 1;
    const pool_t *pool = builder->pool;
    if (!pool || !pool->mem || builder->off < 0 || (uint64_t)builder->off > pool->size ||
        texture->L.size > pool->size - (uint64_t)builder->off) return 0;
    // shortcut: accept packaged data already mapped into storage; a separate initial-data blob needs an explicit upload path.
    return builder->packaged_data == texture->mem || builder->packaged_data == pool->mem + builder->off;
}
